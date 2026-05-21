#pragma once

/* Solar tracker bridge -- ESP-side of the Phase 3 protocol + Phase 4 mesh.
 *
 * Wire frame:   \xAA \x55  <ASCII payload>  <crcHi> <crcLo>  \n
 *   - payload: printable ASCII, no '\n'
 *   - crcHi/crcLo: CRC8 of the payload, uppercase 2 hex chars
 *   - CRC: poly 0x07, init 0x00, MSB-first, no reflection, no final XOR
 *     (CRC-8/SMBUS).  Computed over the payload bytes ONLY.
 *
 * Behavior:
 *   - Every 2 s: transmit a poll frame with payload "?".
 *   - In loop(): drain UART byte-by-byte through the frame parser.
 *   - On a valid frame whose payload starts with "az=", parse the
 *     k=v fields (az, el, wind, mode) and publish to ESPHome sensors.
 *   - Discard any other payload (own poll self-echo on half-duplex
 *     bus has payload "?"; anything not "az=" is noise / future).
 *
 * The parser is byte-identical to the STC firmware's
 * (uart_frame_feed / crc8_update in EcoWorthyFirmware/src/main.c).
 * If either side changes the algorithm, both must change.
 *
 * Phase 4: ESP-NOW mesh broadcast with AES-128-GCM authenticated
 * encryption.  mesh_tx_/mesh_rx_/mesh_dispatch_ live in this file.
 * Set `mesh: test_broadcast: true` in YAML to emit a 2-byte test
 * packet 3 s after boot (for bench validation with a listener node);
 * leave unset / false in production.
 */

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/core/preferences.h"

/* ESP-NOW C API -- wrap in extern "C" on the ESP8266 NONOS SDK to
 * avoid C++ name-mangling issues with the SDK's C headers. */
extern "C" {
#include <espnow.h>
}

#include <Crypto.h>
#include <AES.h>
#include <GCM.h>
#include <SHA256.h>

#include <string>
#include <cstdlib>
#include <map>
#include <array>

namespace esphome {
namespace tracker_bridge {

static const char *const TAG = "tracker_bridge";

class TrackerBridge : public Component, public uart::UARTDevice {
 public:
  void set_az_sensor(sensor::Sensor *s)        { az_ = s; }
  void set_el_sensor(sensor::Sensor *s)        { el_ = s; }
  void set_wind_sensor(sensor::Sensor *s)      { wind_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s) { mode_ = s; }

  /* --- Phase 4: mesh config setters (called from __init__.py to_code) --- */
  void set_mesh_channel(uint8_t ch) { mesh_channel_ = ch; mesh_enabled_ = true; }
  void set_mesh_psk(const std::string &psk) { mesh_psk_ = psk; }
  void set_tracker_id(const std::string &id) { tracker_id_ = id; }
  void set_test_broadcast(bool v) { test_broadcast_ = v; }
  /* local_role: 1 = primary (owns wind sensor), 2 = secondary */
  void set_local_role(uint8_t r) { local_role_ = r; }

  /* Bench-helper write path for the wind cache.  Used by the optional
   * WindOverrideNumber to inject synthetic wind values on a node with
   * no STC attached.  On a node WITH an STC, the next status poll (~2 s
   * later) will overwrite this -- don't wire both up at once. */
  void set_wind_override(uint8_t v) { local_wind_used_ = v; }

  void setup() override {
    /* Poll the STC every 2 s.  ESPHome's set_interval handles timing
     * and survives WiFi/HA disconnects -- the bridge keeps polling
     * regardless of upstream state. */
    this->set_interval("poll", 2000, [this]() { this->send_poll_(); });
    ESP_LOGCONFIG(TAG, "tracker_bridge configured");

    if (mesh_enabled_) {
      mesh_setup_();
    }

    if (mesh_enabled_ && test_broadcast_) {
      /* Bench validation: broadcast a 2-byte test packet every 5 s.
       * Periodic (not one-shot) so a slow WiFi/channel settle doesn't
       * leave the window closed before the radio is ready, and so the
       * receiver gets many chances to log a match. */
      this->set_interval("mesh_test", 5000, [this]() {
        uint8_t test[] = {0xDE, 0xAD};
        mesh_tx_(99, test, 2);  /* type 99 = test, not an assigned type */
        ESP_LOGI(TAG, "test broadcast sent (ctr=%u)", (unsigned) tx_counter_);
      });
    }
  }

  void loop() override {
    uint8_t b;
    while (this->available()) {
      if (this->read_byte(&b)) this->feed_byte_(b);
    }
  }

  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  /* --- CRC8/SMBUS, mirror of the STC's crc8_update() --- */
  static uint8_t crc8_step_(uint8_t crc, uint8_t b) {
    crc ^= b;
    for (uint8_t i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07)
                         : uint8_t(crc << 1);
    }
    return crc;
  }
  static char hex_digit_(uint8_t nib) {
    nib &= 0x0F;
    return char(nib < 10 ? '0' + nib : 'A' + (nib - 10));
  }
  static int hex_val_(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  }

  /* --- Outbound poll: AA 55 '?' <2 hex CRC> '\n' --- */
  void send_poll_() {
    const uint8_t payload = uint8_t('?');
    uint8_t crc = crc8_step_(0, payload);
    this->write_byte(0xAA);
    this->write_byte(0x55);
    this->write_byte(payload);
    this->write_byte(uint8_t(hex_digit_(crc >> 4)));
    this->write_byte(uint8_t(hex_digit_(crc & 0x0F)));
    this->write_byte(uint8_t('\n'));
  }

  /* --- Inbound frame parser: state machine matching STC's uart_frame_feed --- */
  uint8_t pstate_{0};   /* 0=WAIT_AA  1=WAIT_55  2=PAYLOAD */
  std::string pbuf_;
  static constexpr size_t MAX_PAYLOAD = 64;

  void feed_byte_(uint8_t b) {
    switch (pstate_) {
      case 0:
        if (b == 0xAA) pstate_ = 1;
        break;
      case 1:
        if (b == 0x55) { pstate_ = 2; pbuf_.clear(); }
        else if (b == 0xAA) pstate_ = 1;
        else pstate_ = 0;
        break;
      default:
        if (b == '\n') {
          /* Need >= 1 payload char + 2 CRC chars before terminator. */
          if (pbuf_.size() >= 3) {
            int hi = hex_val_(pbuf_[pbuf_.size() - 2]);
            int lo = hex_val_(pbuf_[pbuf_.size() - 1]);
            if (hi >= 0 && lo >= 0) {
              uint8_t want = uint8_t((hi << 4) | lo);
              uint8_t crc = 0;
              size_t plen = pbuf_.size() - 2;
              for (size_t i = 0; i < plen; i++)
                crc = crc8_step_(crc, uint8_t(pbuf_[i]));
              if (crc == want) {
                this->handle_payload_(pbuf_.substr(0, plen));
              } else {
                ESP_LOGD(TAG, "CRC fail want=%02X got=%02X", want, crc);
              }
            }
          }
          pstate_ = 0;
          pbuf_.clear();
        } else if (b == 0xAA) {
          /* Resync: new prefix mid-payload abandons this frame. */
          pstate_ = 1;
          pbuf_.clear();
        } else if (b >= 0x20 && b <= 0x7E && pbuf_.size() < MAX_PAYLOAD) {
          pbuf_.push_back(char(b));
        } else {
          pstate_ = 0;
          pbuf_.clear();
        }
        break;
    }
  }

  void handle_payload_(const std::string &p) {
    /* "?" is our own poll heard back on the half-duplex bus. */
    if (p == "?") return;
    /* v1 only consumes status replies. */
    if (p.compare(0, 3, "az=") != 0) {
      ESP_LOGD(TAG, "ignored payload: %s", p.c_str());
      return;
    }

    int az = -1, el = -1, wind = -1;
    std::string mode;
    size_t i = 0;
    while (i < p.size()) {
      while (i < p.size() && p[i] == ' ') i++;
      if (i >= p.size()) break;
      size_t kstart = i;
      while (i < p.size() && p[i] != '=' && p[i] != ' ') i++;
      if (i >= p.size() || p[i] != '=') break;
      std::string key = p.substr(kstart, i - kstart);
      i++;  /* skip '=' */
      size_t vstart = i;
      while (i < p.size() && p[i] != ' ') i++;
      std::string val = p.substr(vstart, i - vstart);
      if (key == "az")        az   = std::atoi(val.c_str());
      else if (key == "el")   el   = std::atoi(val.c_str());
      else if (key == "wind") wind = std::atoi(val.c_str());
      else if (key == "mode") mode = val;
    }
    if (az_   && az   >= 0) az_->publish_state(float(az));
    if (el_   && el   >= 0) el_->publish_state(float(el));
    if (wind_ && wind >= 0) wind_->publish_state(float(wind));
    if (mode_ && !mode.empty()) mode_->publish_state(mode);

    /* Cache for mesh broadcasts (Task 8). */
    if (az   >= 0) local_az_pct_   = (uint8_t)(az   > 255 ? 255 : az);
    if (el   >= 0) local_el_pct_   = (uint8_t)(el   > 255 ? 255 : el);
    if (wind >= 0) local_wind_used_ = (uint8_t)(wind > 255 ? 255 : wind);
    if (!mode.empty()) local_mode_ = mode;
  }

  sensor::Sensor *az_{nullptr};
  sensor::Sensor *el_{nullptr};
  sensor::Sensor *wind_{nullptr};
  text_sensor::TextSensor *mode_{nullptr};

  /* ================================================================
   * Phase 4: ESP-NOW mesh -- AES-128-GCM authenticated broadcast
   * ================================================================ */

  /* --- Message types (Phase 4 Task 8) --- */
  static constexpr uint8_t MSG_WIND       = 1;
  static constexpr uint8_t MSG_TELEMETRY  = 2;
  static constexpr uint8_t MSG_GATEWAY_HB = 3;
  static constexpr uint8_t MSG_COMMAND    = 4;
  static constexpr uint8_t MSG_CONFIG     = 5;

  /* Role constants: primary owns the wind sensor and broadcasts WIND packets;
   * secondary nodes receive WIND from the primary and forward to their STC. */
  static constexpr uint8_t ROLE_PRIMARY   = 1;
  static constexpr uint8_t ROLE_SECONDARY = 2;

  /* --- Cached STC state, populated by handle_payload_ --- */
  uint8_t local_az_pct_{0};
  uint8_t local_el_pct_{0};
  uint8_t local_wind_used_{0};
  std::string local_mode_{};
  /* ROLE_PRIMARY or ROLE_SECONDARY (default secondary until set via YAML) */
  uint8_t local_role_{ROLE_SECONDARY};

  /* --- Mesh config (populated by YAML via setters above) --- */
  uint8_t mesh_channel_{0};
  std::string mesh_psk_{};
  uint8_t mesh_key_[16]{};      /* derived from psk_ via SHA-256 trunc (deterministic) */
  std::string tracker_id_{};
  bool mesh_enabled_{false};
  bool test_broadcast_{false};

  /* --- TX counter + flash persistence --- */
  uint32_t tx_counter_{0};
  ESPPreferenceObject pref_tx_counter_;
  static constexpr uint32_t CTR_FLUSH_EVERY = 100;

  /* --- Per-peer last-accepted counter (replay protection) --- */
  std::map<std::array<uint8_t, 6>, uint32_t> peer_last_ctr_;

  /* Singleton pointer -- ESP-NOW C callback has no user-data parameter. */
  static TrackerBridge *instance_;

  /* ---- mesh_setup_: derive key, restore counter, init ESP-NOW ---- */
  void mesh_setup_() {
    /* Derive a 16-byte AES key from the passphrase via SHA-256 truncation.
     * Deterministic: operators can use a human-readable string in YAML. */
    SHA256 sha;
    sha.update(mesh_psk_.data(), mesh_psk_.size());
    uint8_t digest[32];
    sha.finalize(digest, sizeof(digest));
    memcpy(mesh_key_, digest, 16);

    /* Restore TX counter from flash; advance by a safety margin to
     * guarantee monotonicity across power cycles.  Fixed preference
     * key -- there's one tracker_bridge per device in practice
     * (MULTI_CONF + the singleton already preclude two instances). */
    pref_tx_counter_ = global_preferences->make_preference<uint32_t>(0xC0DEC0DE);
    if (!pref_tx_counter_.load(&tx_counter_)) tx_counter_ = 0;
    tx_counter_ += CTR_FLUSH_EVERY;
    pref_tx_counter_.save(&tx_counter_);
    global_preferences->sync();

    /* Init ESP-NOW.  WiFi must already be associated on mesh_channel_. */
    if (esp_now_init() != 0) {
      ESP_LOGE(TAG, "esp_now_init failed");
      return;
    }
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);

    /* Set instance_ BEFORE registering the callback so there is no window
     * in which the callback fires with a null instance_. */
    instance_ = this;
    esp_now_register_recv_cb(recv_cb_);

    /* Register broadcast peer (FF:FF:FF:FF:FF:FF). */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_add_peer(bcast, ESP_NOW_ROLE_SLAVE, mesh_channel_, NULL, 0);
    ESP_LOGI(TAG, "Mesh enabled: channel=%u tracker_id=%s role=%u",
             mesh_channel_, tracker_id_.c_str(), (unsigned)local_role_);

    /* Every 5 s: TELEMETRY (always), WIND (if primary), GATEWAY_HB (if WiFi up) */
    this->set_interval("mesh_broadcasts", 5000, [this]() {
      this->mesh_tx_telemetry_();
      if (this->local_role_ == ROLE_PRIMARY) this->mesh_tx_wind_();
      if (WiFi.isConnected()) this->mesh_tx_gateway_hb_();
    });
  }

  /* ---- recv_cb_: IRAM-resident ESP-NOW receive callback entry point ----
   * ESP8266 NONOS SDK fires this from an interrupt-adjacent context that
   * requires the function to reside in IRAM (ICACHE_RAM_ATTR).  A capturing
   * lambda cannot carry that attribute and would be placed in flash,
   * risking a cache-stall panic.  This thin stub lives in IRAM and
   * dispatches to mesh_rx_() which may remain in flash. */
  static void IRAM_ATTR recv_cb_(uint8_t *mac, uint8_t *data, uint8_t len) {
    if (TrackerBridge::instance_)
      TrackerBridge::instance_->mesh_rx_(mac, data, len);
  }

  /* ---- mesh_tx_: encrypt + broadcast a single mesh frame ---- */
  /*
   * Frame layout (binary):
   *   [0]      type       (1 byte)
   *   [1..6]   src MAC    (6 bytes)
   *   [7..10]  ctr        (4 bytes, big-endian)
   *   [11..]   ciphertext (plen bytes)
   *   [..+8]   AES-GCM tag (8 bytes)
   *
   * AAD covers bytes [0..10] (type + header), so a tampered type or
   * counter is detected by the tag check.
   */
  void mesh_tx_(uint8_t type, const uint8_t *payload_in, size_t plen) {
    /* Guard: pkt is sized for at most 64 bytes of plaintext. */
    if (plen > 64) {
      ESP_LOGE(TAG, "mesh_tx_ plen %u > 64", (unsigned)plen);
      return;
    }
    /* Maximum ESP-NOW payload is 250 bytes; 11 header + 8 tag = 19 bytes
     * of overhead, leaving 231 bytes for plaintext.  64 is well within. */
    uint8_t pkt[1 + 6 + 4 + 64 + 8];

    /* Header */
    pkt[0] = type;
    uint8_t mac[6];
    WiFi.macAddress(mac);
    memcpy(pkt + 1, mac, 6);
    uint32_t ctr = ++tx_counter_;
    pkt[7]  = (ctr >> 24) & 0xFF;
    pkt[8]  = (ctr >> 16) & 0xFF;
    pkt[9]  = (ctr >>  8) & 0xFF;
    pkt[10] =  ctr        & 0xFF;

    /* Persist counter every CTR_FLUSH_EVERY ticks */
    if ((ctr % CTR_FLUSH_EVERY) == 0) {
      pref_tx_counter_.save(&tx_counter_);
      global_preferences->sync();
    }

    /* AES-128-GCM encrypt */
    GCM<AES128> gcm;
    gcm.setKey(mesh_key_, 16);
    /* Nonce: src(6) || ctr(4) || 0x0000 = 12 bytes */
    uint8_t nonce[12];
    memcpy(nonce, mac, 6);
    memcpy(nonce + 6, pkt + 7, 4);
    nonce[10] = 0;
    nonce[11] = 0;
    gcm.setIV(nonce, 12);
    /* AAD: type byte + src MAC + counter.  Tag size is fixed at 8 via
     * the computeTag/checkTag length argument; GCM has no separate
     * setTagSize() in this library. */
    gcm.addAuthData(pkt, 1 + 6 + 4);
    /* Encrypt payload into pkt at byte 11; tag follows at pkt[11+plen] */
    gcm.encrypt(pkt + 11, payload_in, plen);
    uint8_t tag[8];
    gcm.computeTag(tag, 8);
    memcpy(pkt + 11 + plen, tag, 8);

    /* Broadcast */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(bcast, pkt, 11 + plen + 8);
  }

  /* ---- mesh_rx_: decrypt + auth + replay-check an incoming frame ---- */
  void mesh_rx_(uint8_t *mac, uint8_t *data, uint8_t len) {
    /* Minimum frame: 1+6+4 header + 0 ciphertext + 8 tag = 19 bytes */
    if (len < 1 + 6 + 4 + 0 + 8) return;

    uint8_t type = data[0];
    uint8_t src[6];
    memcpy(src, data + 1, 6);
    uint32_t ctr = ((uint32_t)data[7]  << 24)
                 | ((uint32_t)data[8]  << 16)
                 | ((uint32_t)data[9]  <<  8)
                 |  (uint32_t)data[10];
    size_t plen = (size_t)len - (1 + 6 + 4 + 8);

    /* Replay protection: reject any frame with a non-advancing counter. */
    std::array<uint8_t, 6> peer_key{src[0], src[1], src[2],
                                    src[3], src[4], src[5]};
    auto it = peer_last_ctr_.find(peer_key);
    if (it != peer_last_ctr_.end() && ctr <= it->second) {
      ESP_LOGD(TAG, "replay drop src=%02X..%02X ctr=%u last=%u",
               src[0], src[5], ctr, it->second);
      return;
    }

    /* AES-128-GCM decrypt */
    GCM<AES128> gcm;
    gcm.setKey(mesh_key_, 16);
    uint8_t nonce[12];
    memcpy(nonce, src, 6);
    memcpy(nonce + 6, data + 7, 4);
    nonce[10] = 0;
    nonce[11] = 0;
    gcm.setIV(nonce, 12);
    gcm.addAuthData(data, 1 + 6 + 4);
    uint8_t plaintext[64];
    if (plen > sizeof(plaintext)) {
      ESP_LOGD(TAG, "oversized payload %u from %02X..%02X", (unsigned)plen,
               src[0], src[5]);
      return;
    }
    gcm.decrypt(plaintext, data + 11, plen);
    if (!gcm.checkTag(data + 11 + plen, 8)) {
      ESP_LOGD(TAG, "AES auth fail src=%02X..%02X", src[0], src[5]);
      return;
    }

    /* Authenticated -- update replay table and dispatch. */
    peer_last_ctr_[peer_key] = ctr;
    mesh_dispatch_(type, src, plaintext, plen);
  }

  /* ---- mesh_dispatch_: route inbound frames (Task 8+) ---- */
  void mesh_dispatch_(uint8_t type, uint8_t src[6],
                      const uint8_t *p, size_t plen) {
    ESP_LOGI(TAG, "mesh rx type=%u from %02X:%02X:%02X:%02X:%02X:%02X plen=%u",
             type,
             src[0], src[1], src[2], src[3], src[4], src[5],
             (unsigned)plen);
    switch (type) {
      case MSG_WIND:
        if (plen < 2) return;
        /* The primary is the sender of WIND broadcasts.  ESP-NOW broadcasts
         * loop back to the sender on ESP8266 ROLE_COMBO, so the primary
         * receives its own packet -- skip the forward to avoid a redundant
         * !wind= command to the primary's own STC. */
        if (local_role_ == ROLE_PRIMARY) break;
        /* Forward wind value to local STC via the !wind=NN framed command.
         * Secondary nodes relay primary's wind reading to their STC so the
         * STC's own storm_check can trip without a local wind sensor. */
        send_command_frame_("!wind=", p[0]);
        break;
      case MSG_TELEMETRY:
      case MSG_GATEWAY_HB:
        /* Handled in Tasks 9 + 10. */
        break;
      default:
        /* type=99 is the bench test type; others are future. */
        break;
    }
  }

  /* ---- Periodic mesh broadcast helpers (Task 8) ---- */

  void mesh_tx_telemetry_() {
    uint8_t p[4] = {
      local_az_pct_, local_el_pct_, local_wind_used_,
      mode_to_code_(local_mode_)
    };
    mesh_tx_(MSG_TELEMETRY, p, 4);
    ESP_LOGD(TAG, "tx TELEMETRY az=%u el=%u wind=%u mode=%u",
             p[0], p[1], p[2], p[3]);
  }

  void mesh_tx_wind_() {
    uint8_t p[2] = { local_wind_used_, 0 };
    /* Flag bit 0: storm active on primary */
    if (local_mode_ == "storm") p[1] |= 0x01;
    mesh_tx_(MSG_WIND, p, 2);
    ESP_LOGD(TAG, "tx WIND wind=%u flags=0x%02X", p[0], p[1]);
  }

  void mesh_tx_gateway_hb_() {
    int8_t rssi = (int8_t)WiFi.RSSI();
    uint8_t p[1] = { (uint8_t)rssi };
    mesh_tx_(MSG_GATEWAY_HB, p, 1);
    ESP_LOGD(TAG, "tx GATEWAY_HB rssi=%d", (int)rssi);
  }

  /* Map STC mode string (from uart_mode_str in main.c) to a compact code.
   * Strings are the authority -- pulled directly from STC firmware. */
  static uint8_t mode_to_code_(const std::string &m) {
    if (m == "idle")   return 0;
    if (m == "track")  return 1;
    if (m == "storm")  return 2;
    if (m == "jog")    return 3;
    if (m == "cal")    return 4;
    if (m == "menu")   return 5;
    if (m == "set")    return 6;   /* ST_SETTINGS / ST_SETTINGS_EDIT */
    if (m == "nocal")  return 7;
    if (m == "btest")  return 8;
    if (m == "ver")    return 9;
    return 255;  /* 255 = unknown mode sentinel (STC sent '?' or a new mode we don't know) */
  }

  /* ---- send_command_frame_: build and write a !cmd=NN framed UART packet ----
   * Format: AA 55 <ASCII cmd string> <2 hex CRC8> LF
   * CRC covers only the ASCII payload bytes -- byte-identical to the STC's
   * uart_send_frame / crc8_update (see EcoWorthyFirmware/src/main.c). */
  void send_command_frame_(const char *prefix, uint8_t value) {
    char buf[16];
    int sn = snprintf(buf, sizeof(buf), "%s%u", prefix, (unsigned)value);
    /* sn == 0 is an empty payload (shouldn't happen); sn >= sizeof(buf) means
     * snprintf would have written more than fits -- truncation occurred and the
     * buffer contains a garbage-truncated string.  Bail in either case. */
    if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
      ESP_LOGE(TAG, "send_command_frame_ snprintf truncation (sn=%d)", sn);
      return;
    }
    size_t len = (size_t)sn;
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++)
      crc = crc8_step_(crc, (uint8_t)buf[i]);
    this->write_byte(0xAA);
    this->write_byte(0x55);
    for (size_t i = 0; i < len; i++)
      this->write_byte((uint8_t)buf[i]);
    this->write_byte((uint8_t)hex_digit_(crc >> 4));
    this->write_byte((uint8_t)hex_digit_(crc & 0x0F));
    this->write_byte(uint8_t('\n'));
    ESP_LOGD(TAG, "cmd->STC: %s (crc=%02X)", buf, crc);
  }
};

/* Out-of-class definition of the static singleton pointer. */
TrackerBridge *TrackerBridge::instance_ = nullptr;

/* Bench-helper: HA-controllable wind value for nodes WITHOUT an STC.
 * Lets you flash a spare ESP-01S as a "secondary" or "primary" without
 * physical hardware behind it and inject test wind readings from HA's
 * UI -- proves end-to-end mesh + remote-wind plumbing with only one
 * real STC on the bench. */
class WindOverrideNumber : public number::Number, public Component {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }

 protected:
  void control(float value) override {
    if (parent_ == nullptr) return;
    uint8_t v = (value < 0.0f) ? 0 : (value > 99.0f) ? 99 : (uint8_t) value;
    parent_->set_wind_override(v);
    this->publish_state(float(v));
  }

  TrackerBridge *parent_{nullptr};
};

}  // namespace tracker_bridge
}  // namespace esphome
