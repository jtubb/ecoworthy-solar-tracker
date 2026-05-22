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
 *
 * P4-12: frame header grows from 11 to 45 bytes.  Layout:
 *   [0]      type       (1 byte)
 *   [1..6]   src MAC    (6 bytes)
 *   [7..8]   epoch      (2 bytes, big-endian, boot counter)
 *   [9..12]  ctr        (4 bytes, big-endian, per-boot frame counter)
 *   [13..44] node_name  (32 bytes, NUL-padded ASCII, from App.get_name())
 *   [45..]   ciphertext (plen bytes, AES-128-GCM)
 *   [..+8]   AES-GCM tag (8 bytes)
 * AAD covers all 45 header bytes.
 * Nonce: src(6) || epoch(2) || ctr(4) = 12 bytes.
 * Binding key for replay + peer lookup: node_name (32 bytes).
 */

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/preferences.h"
#include "esphome/core/application.h"

#ifdef USE_NUMBER
#include "esphome/components/number/number.h"
#endif

#ifdef USE_BUTTON
#include "esphome/components/button/button.h"
#endif

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
#include <cstring>
#include <map>
#include <array>
#include <algorithm>

namespace esphome {
namespace tracker_bridge {

static const char *const TAG = "tracker_bridge";

class TrackerBridge : public Component, public uart::UARTDevice {
 public:
  /* --- Local entity setters (peer_id == "") --- */
  void set_az_sensor(sensor::Sensor *s)        { az_ = s; }
  void set_el_sensor(sensor::Sensor *s)        { el_ = s; }
  void set_wind_sensor(sensor::Sensor *s)      { wind_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s) { mode_ = s; }

  /* --- Per-peer entity setters: empty peer_id → local, else route to peer_decls_ --- */
  void set_az_sensor_for(const std::string &peer_id, sensor::Sensor *s) {
    if (peer_id.empty()) { az_ = s; return; }
    auto key = make_name_key_(peer_id);
    auto it = peer_decls_.find(key);
    if (it != peer_decls_.end()) { it->second.az = s; return; }
    ESP_LOGW(TAG, "set_az_sensor_for: unknown peer_id '%s'", peer_id.c_str());
  }
  void set_el_sensor_for(const std::string &peer_id, sensor::Sensor *s) {
    if (peer_id.empty()) { el_ = s; return; }
    auto key = make_name_key_(peer_id);
    auto it = peer_decls_.find(key);
    if (it != peer_decls_.end()) { it->second.el = s; return; }
    ESP_LOGW(TAG, "set_el_sensor_for: unknown peer_id '%s'", peer_id.c_str());
  }
  void set_wind_sensor_for(const std::string &peer_id, sensor::Sensor *s) {
    if (peer_id.empty()) { wind_ = s; return; }
    auto key = make_name_key_(peer_id);
    auto it = peer_decls_.find(key);
    if (it != peer_decls_.end()) { it->second.wind = s; return; }
    ESP_LOGW(TAG, "set_wind_sensor_for: unknown peer_id '%s'", peer_id.c_str());
  }
  void set_mode_sensor_for(const std::string &peer_id, text_sensor::TextSensor *s) {
    if (peer_id.empty()) { mode_ = s; return; }
    auto key = make_name_key_(peer_id);
    auto it = peer_decls_.find(key);
    if (it != peer_decls_.end()) { it->second.mode = s; return; }
    ESP_LOGW(TAG, "set_mode_sensor_for: unknown peer_id '%s'", peer_id.c_str());
  }

  /* --- Peer registration (called from __init__.py to_code for each declared peer) --- */
  void register_peer(const std::string &name) {
    auto key = make_name_key_(name);
    /* Just ensure the slot exists; sensors are attached later by set_*_for(). */
    peer_decls_[key];  // default-constructs PeerDecl if not present
    ESP_LOGD(TAG, "registered peer '%s'", name.c_str());
  }

  /* --- Config slider write path: local STC or remote peer via mesh --- */
  void config_set_for_peer(const std::string &peer_id, uint8_t field_id, uint8_t value) {
    if (peer_id.empty()) {
      /* Local STC: send directly over UART */
      char buf[32];
      int sn = snprintf(buf, sizeof(buf), "!cfg set id=%u val=%u", field_id, value);
      if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
        ESP_LOGE(TAG, "config_set_for_peer snprintf truncation");
        return;
      }
      write_str_frame_(buf);
    } else {
      /* Remote peer: unicast MSG_CONFIG SET_REQ via mesh.
       * Payload: op(1) + target_name(32) + field_id(1) + value(1) = 35 bytes.
       * Receiver compares target_name against its own wire_id_ and silently
       * drops on mismatch -- only the named peer self-applies the SET. */
      auto target = make_name_key_(peer_id);
      uint8_t p[1 + 32 + 1 + 1];
      p[0] = CFG_OP_SET_REQ;
      memcpy(p + 1, target.data(), 32);
      p[33] = field_id;
      p[34] = value;
      mesh_tx_(MSG_CONFIG, p, sizeof(p));
      ESP_LOGD(TAG, "config_set peer='%s' fid=%u val=%u", peer_id.c_str(), field_id, value);
    }
  }

  /* --- Phase 4: mesh config setters (called from __init__.py to_code) --- */
  void set_mesh_channel(uint8_t ch) { mesh_channel_ = ch; mesh_enabled_ = true; }
  void set_mesh_psk(const std::string &psk) { mesh_psk_ = psk; }
  void set_test_broadcast(bool v) { test_broadcast_ = v; }
  /* local_role: 1 = primary (owns wind sensor), 2 = secondary */
  void set_local_role(uint8_t r) { local_role_ = r; }

  /* Bench-helper write path for the wind cache.  Used by the optional
   * WindOverrideNumber to inject synthetic wind values on a node with
   * no STC attached.  On a node WITH an STC, the next status poll (~2 s
   * later) will overwrite this -- don't wire both up at once. */
  void set_wind_override(uint8_t v) { local_wind_used_ = v; }

  /* HA->mesh broadcast methods (callable from buttons/numbers) */
  void broadcast_force_park()    { mesh_tx_command_(broadcast_mac_(), CMD_FORCE_PARK,    0, 0); }
  void broadcast_force_release() { mesh_tx_command_(broadcast_mac_(), CMD_FORCE_RELEASE, 0, 0); }
  void broadcast_stop()          { mesh_tx_command_(broadcast_mac_(), CMD_STOP,          0, 0); }
  void send_goto(const uint8_t target[6], uint8_t az, uint8_t el)            { mesh_tx_command_(target, CMD_GOTO,      az,     el); }
  void send_jog(const uint8_t target[6], uint8_t ax_dir, uint8_t dur_100ms) { mesh_tx_command_(target, CMD_JOG,       ax_dir, dur_100ms); }
  void send_calibrate(const uint8_t target[6])                               { mesh_tx_command_(target, CMD_CALIBRATE, 0,      0); }

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
    /* Half-duplex self-echo: our own outbound poll ("?") and forwarded
     * commands ("!wind=...", "!park", ...) loop back on the single-wire
     * bus.  All start-with-! and start-with-? frames originated here. */
    if (p == "?" || (!p.empty() && p[0] == '!')) return;
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

  /* --- Command codes (MSG_COMMAND payload byte 6) --- */
  static constexpr uint8_t CMD_FORCE_PARK    = 1;
  static constexpr uint8_t CMD_FORCE_RELEASE = 2;
  static constexpr uint8_t CMD_GOTO          = 3;
  static constexpr uint8_t CMD_JOG           = 4;
  static constexpr uint8_t CMD_STOP          = 5;
  static constexpr uint8_t CMD_CALIBRATE     = 6;

  /* --- CONFIG operation codes (MSG_CONFIG payload byte 0) --- */
  static constexpr uint8_t CFG_OP_GET_REQ    = 1;
  static constexpr uint8_t CFG_OP_GET_RESP   = 2;
  static constexpr uint8_t CFG_OP_SET_REQ    = 3;
  static constexpr uint8_t CFG_OP_SET_ACK    = 4;

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
  uint8_t my_mac_[6]{};         /* cached at mesh_setup_; used for gateway election */
  char wire_id_[32]{};          /* 32-byte NUL-padded node_name from App.get_name() */
  bool mesh_enabled_{false};
  bool test_broadcast_{false};

  /* --- TX counter + flash persistence --- */
  uint32_t tx_counter_{0};
  ESPPreferenceObject pref_tx_counter_;
  static constexpr uint32_t CTR_FLUSH_EVERY = 100;

  /* --- Boot epoch + flash persistence (P4-12 replay protection) --- */
  uint16_t boot_epoch_{0};
  ESPPreferenceObject pref_boot_epoch_;

  /* --- Per-peer last-accepted (epoch, ctr) tuple -- replay protection ---
   * Keyed by node_name (32-byte fixed-width ASCII, NUL-padded). */
  struct SessionKey {
    uint16_t epoch{0};
    uint32_t ctr{0};
  };
  std::map<std::array<char, 32>, SessionKey> peer_last_session_;

  /* --- Statically declared peer entities (populated at codegen from YAML) ---
   * Separate from peers_ (which tracks runtime broadcast state).
   * PeerDecl is keyed by node_name (32-byte fixed-width ASCII, NUL-padded). */
  struct PeerDecl {
    sensor::Sensor *az{nullptr};
    sensor::Sensor *el{nullptr};
    sensor::Sensor *wind{nullptr};
    text_sensor::TextSensor *mode{nullptr};
    /* Config number entities -- wired by number.py; used for future GET_RESP
     * read-back.  Pointers stored here so the dispatcher can publish_state
     * when a CFG_OP_GET_RESP arrives. */
#ifdef USE_NUMBER
    number::Number *wind_storm{nullptr};
    number::Number *wind_release{nullptr};
    number::Number *storm_dwell{nullptr};
    number::Number *track_thresh{nullptr};
#endif
  };
  std::map<std::array<char, 32>, PeerDecl> peer_decls_;

  /* --- Per-peer application state (gateway election + HA publishing) --- */
  struct PeerEntry {
    uint32_t last_gateway_hb_ms{0};   // when we last saw GATEWAY_HB
    uint32_t last_telemetry_ms{0};
    uint8_t az_pct{0}, el_pct{0}, wind_used{0};
    std::string mode{};
    uint8_t mac[6]{};             /* last-seen source MAC for this peer (unicast targeting) */
  };
  std::map<std::array<char, 32>, PeerEntry> peers_;

  static constexpr uint32_t HB_STALE_MS = 15000;   // 3 missed @ 5s

  /* Singleton pointer -- ESP-NOW C callback has no user-data parameter. */
  static TrackerBridge *instance_;

  /* ---- mesh_setup_: derive key, restore counter, init ESP-NOW ---- */
  void mesh_setup_() {
    /* Cache own MAC once; used by gateway election and MSG_COMMAND targeting. */
    WiFi.macAddress(my_mac_);

    /* Populate wire_id_ from App.get_name().  Truncate with a warning if
     * the name exceeds 31 chars (the 32nd byte is always NUL). */
    {
      const std::string &name = App.get_name();
      if (name.size() > 31) {
        ESP_LOGW(TAG, "esphome.name '%s' is %u chars, truncating to 31 for node_name",
                 name.c_str(), (unsigned)name.size());
      }
      memset(wire_id_, 0, sizeof(wire_id_));
      size_t copy_len = name.size() < 31 ? name.size() : 31;
      memcpy(wire_id_, name.c_str(), copy_len);
    }

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

    /* Restore and increment boot epoch for cross-boot replay protection. */
    pref_boot_epoch_ = global_preferences->make_preference<uint16_t>(0xC0DEEEEC);
    if (!pref_boot_epoch_.load(&boot_epoch_)) boot_epoch_ = 0;
    boot_epoch_++;
    pref_boot_epoch_.save(&boot_epoch_);

    /* One sync() flushes both preference writes to flash. */
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
    ESP_LOGI(TAG, "Mesh enabled: channel=%u name=%s epoch=%u role=%u",
             mesh_channel_, wire_id_, (unsigned)boot_epoch_, (unsigned)local_role_);

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
   * Frame layout (binary), P4-12:
   *   [0]      type       (1 byte)
   *   [1..6]   src MAC    (6 bytes)
   *   [7..8]   epoch      (2 bytes, big-endian)
   *   [9..12]  ctr        (4 bytes, big-endian)
   *   [13..44] node_name  (32 bytes, NUL-padded ASCII)
   *   [45..]   ciphertext (plen bytes, AES-128-GCM encrypted)
   *   [..+8]   AES-GCM tag (8 bytes)
   *
   * Header = 1+6+2+4+32 = 45 bytes.
   * AAD covers all 45 header bytes.
   * Nonce: src(6) || epoch(2) || ctr(4) = 12 bytes.
   */
  static constexpr size_t MESH_HDR = 45;   /* header size in bytes */

  void mesh_tx_(uint8_t type, const uint8_t *payload_in, size_t plen) {
    /* Guard: pkt is sized for at most 64 bytes of plaintext. */
    if (plen > 64) {
      ESP_LOGE(TAG, "mesh_tx_ plen %u > 64", (unsigned)plen);
      return;
    }
    /* Maximum ESP-NOW payload is 250 bytes; 45 header + 8 tag = 53 bytes
     * of overhead, leaving 197 bytes for plaintext.  64 is well within. */
    uint8_t pkt[MESH_HDR + 64 + 8];

    /* Header: type */
    pkt[0] = type;
    /* src MAC */
    memcpy(pkt + 1, my_mac_, 6);
    /* epoch (big-endian) */
    pkt[7] = (boot_epoch_ >> 8) & 0xFF;
    pkt[8] =  boot_epoch_       & 0xFF;
    /* ctr (big-endian) */
    uint32_t ctr = ++tx_counter_;
    pkt[9]  = (ctr >> 24) & 0xFF;
    pkt[10] = (ctr >> 16) & 0xFF;
    pkt[11] = (ctr >>  8) & 0xFF;
    pkt[12] =  ctr        & 0xFF;
    /* node_name (32 bytes, NUL-padded) */
    memcpy(pkt + 13, wire_id_, 32);

    /* Persist counter every CTR_FLUSH_EVERY ticks */
    if ((ctr % CTR_FLUSH_EVERY) == 0) {
      pref_tx_counter_.save(&tx_counter_);
      global_preferences->sync();
    }

    /* AES-128-GCM encrypt */
    GCM<AES128> gcm;
    gcm.setKey(mesh_key_, 16);
    /* Nonce: src(6) || epoch(2) || ctr(4) = 12 bytes */
    uint8_t nonce[12];
    memcpy(nonce,     pkt + 1, 6);   /* src MAC */
    memcpy(nonce + 6, pkt + 7, 2);   /* epoch */
    memcpy(nonce + 8, pkt + 9, 4);   /* ctr */
    gcm.setIV(nonce, 12);
    /* AAD: entire 45-byte header.  Tag size is fixed at 8 via the
     * computeTag/checkTag length argument. */
    gcm.addAuthData(pkt, MESH_HDR);
    /* Encrypt payload into pkt at byte MESH_HDR; tag follows */
    gcm.encrypt(pkt + MESH_HDR, payload_in, plen);
    uint8_t tag[8];
    gcm.computeTag(tag, 8);
    memcpy(pkt + MESH_HDR + plen, tag, 8);

    /* Broadcast */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(bcast, pkt, MESH_HDR + plen + 8);
  }

  /* ---- mesh_rx_: decrypt + auth + replay-check an incoming frame ---- */
  void mesh_rx_(uint8_t * /*mac*/, uint8_t *data, uint8_t len) {
    /* Minimum frame: MESH_HDR(45) + 0 ciphertext + 8 tag = 53 bytes */
    if (len < MESH_HDR + 8) return;

    uint8_t type = data[0];
    uint8_t src[6];
    memcpy(src, data + 1, 6);
    uint16_t epoch = ((uint16_t)data[7] << 8) | (uint16_t)data[8];
    uint32_t ctr   = ((uint32_t)data[9]  << 24)
                   | ((uint32_t)data[10] << 16)
                   | ((uint32_t)data[11] <<  8)
                   |  (uint32_t)data[12];
    /* node_name: 32 bytes at offset 13.  Force byte 31 to NUL after copy
     * so a malicious sender can't desync our key from make_name_key_'s
     * 31-char invariant and create a parallel session entry for the same
     * logical peer. */
    std::array<char, 32> name_key;
    memcpy(name_key.data(), data + 13, 32);
    name_key[31] = '\0';

    size_t plen = (size_t)len - (MESH_HDR + 8);

    /* Replay protection: accept only if (epoch, ctr) strictly advances. */
    auto sit = peer_last_session_.find(name_key);
    if (sit != peer_last_session_.end()) {
      const SessionKey &last = sit->second;
      bool advanced = (epoch > last.epoch) ||
                      (epoch == last.epoch && ctr > last.ctr);
      if (!advanced) {
        ESP_LOGD(TAG, "replay drop name=%.32s epoch=%u ctr=%u last=(%u,%u)",
                 name_key.data(), epoch, ctr, last.epoch, last.ctr);
        return;
      }
    }

    /* AES-128-GCM decrypt */
    GCM<AES128> gcm;
    gcm.setKey(mesh_key_, 16);
    /* Nonce: src(6) || epoch(2) || ctr(4) = 12 bytes */
    uint8_t nonce[12];
    memcpy(nonce,     data + 1,  6);  /* src MAC */
    memcpy(nonce + 6, data + 7,  2);  /* epoch */
    memcpy(nonce + 8, data + 9,  4);  /* ctr */
    gcm.setIV(nonce, 12);
    gcm.addAuthData(data, MESH_HDR);
    uint8_t plaintext[64];
    if (plen > sizeof(plaintext)) {
      ESP_LOGD(TAG, "oversized payload %u from %02X..%02X", (unsigned)plen,
               src[0], src[5]);
      return;
    }
    gcm.decrypt(plaintext, data + MESH_HDR, plen);
    if (!gcm.checkTag(data + MESH_HDR + plen, 8)) {
      ESP_LOGD(TAG, "AES auth fail src=%02X..%02X", src[0], src[5]);
      return;
    }

    /* Authenticated -- update replay table and dispatch. */
    peer_last_session_[name_key] = SessionKey{epoch, ctr};
    mesh_dispatch_(type, src, name_key, plaintext, plen);
  }

  /* ---- mesh_dispatch_: route inbound frames (Task 8+) ---- */
  void mesh_dispatch_(uint8_t type, uint8_t src[6],
                      const std::array<char, 32> &name_key,
                      const uint8_t *p, size_t plen) {
    ESP_LOGI(TAG, "mesh rx type=%u from %02X:%02X:%02X:%02X:%02X:%02X name=%.32s plen=%u",
             type,
             src[0], src[1], src[2], src[3], src[4], src[5],
             name_key.data(),
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
        {
          char buf[16];
          int sn = snprintf(buf, sizeof(buf), "!wind=%u", (unsigned)p[0]);
          if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
            ESP_LOGE(TAG, "MSG_WIND snprintf truncation");
            break;
          }
          write_str_frame_(buf);
        }
        break;
      case MSG_GATEWAY_HB: {
        /* Drop self-echo by node_name comparison. */
        if (std::memcmp(name_key.data(), wire_id_, 32) == 0) break;
        auto &e = peers_[name_key];
        e.last_gateway_hb_ms = millis();
        memcpy(e.mac, src, 6);
        break;
      }
      case MSG_TELEMETRY: {
        if (plen < 4) return;
        /* Drop self-echo by node_name comparison. */
        if (std::memcmp(name_key.data(), wire_id_, 32) == 0) break;
        auto &e = peers_[name_key];
        e.last_telemetry_ms = millis();
        e.az_pct = p[0]; e.el_pct = p[1]; e.wind_used = p[2];
        e.mode = mode_from_code_(p[3]);
        memcpy(e.mac, src, 6);
        /* If we're the acting gateway, publish to HA for this peer */
        if (is_acting_gateway_()) publish_peer_to_ha_(name_key, e);
        break;
      }
      case MSG_COMMAND: {
        if (plen < 9) return;
        /* Target occupies the first 6 bytes of the plaintext payload.
         * If target is not broadcast and not us, ignore this command. */
        bool is_bcast = std::all_of(p, p + 6, [](uint8_t b) { return b == 0xFF; });
        if (!is_bcast && std::memcmp(p, my_mac_, 6) != 0) return;
        uint8_t cmd = p[6], a1 = p[7], a2 = p[8];
        switch (cmd) {
          case CMD_FORCE_PARK:
            write_str_frame_("!park");
            break;
          case CMD_FORCE_RELEASE:
            /* !release clears operator-forced storm (storm_forced) only.
             * Wind-driven failsafe (wind_failsafe) is intentionally NOT
             * cleared here: a remote release must not override an active
             * wind sensor reading or watchdog failsafe. */
            write_str_frame_("!release");
            break;
          case CMD_STOP:
            write_str_frame_("!stop");
            break;
          case CMD_GOTO: {
            char buf[32];
            int sn = snprintf(buf, sizeof(buf), "!goto az=%u el=%u", a1, a2);
            if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
              ESP_LOGE(TAG, "CMD_GOTO snprintf truncation");
              break;
            }
            write_str_frame_(buf);
            break;
          }
          case CMD_JOG: {
            uint8_t axis   = (a1 >> 7) & 1;
            uint8_t dir    =  a1       & 1;
            uint16_t dur_ms = (uint16_t)a2 * 100;
            char buf[40];
            int sn = snprintf(buf, sizeof(buf), "!jog ax=%u dir=%c ms=%u",
                              axis, dir ? '+' : '-', (unsigned)dur_ms);
            if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
              ESP_LOGE(TAG, "CMD_JOG snprintf truncation");
              break;
            }
            write_str_frame_(buf);
            break;
          }
          case CMD_CALIBRATE:
            write_str_frame_("!cal");
            break;
          default:
            ESP_LOGD(TAG, "MSG_COMMAND unknown cmd=%u", cmd);
            break;
        }
        break;
      }
      case MSG_CONFIG: {
        if (plen < 1) return;
        uint8_t op = p[0];
        if (op == CFG_OP_SET_REQ) {
          /* SET_REQ payload: op(1) + target_name(32) + field_id(1) + val(1) = 35 bytes */
          if (plen < 1 + 32 + 1 + 1) return;
          /* Reject if target_name doesn't match our wire_id_ */
          if (memcmp(p + 1, wire_id_, 32) != 0) break;
          uint8_t field_id = p[33];
          uint8_t val      = p[34];
          char buf[32];
          int sn = snprintf(buf, sizeof(buf), "!cfg set id=%u val=%u", field_id, val);
          if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
            ESP_LOGE(TAG, "CFG_SET snprintf truncation");
            break;
          }
          write_str_frame_(buf);
        } else if (op == CFG_OP_GET_REQ) {
          /* GET_REQ payload: op(1) + field_id(1) = 2 bytes.
           * v1: broadcast-and-everyone-responds; P5-2 will add target_name
           * scoping here so non-target peers can ignore. */
          if (plen < 2) return;
          uint8_t field_id = p[1];
          char buf[20];
          int sn = snprintf(buf, sizeof(buf), "!cfg get id=%u", field_id);
          if (sn <= 0 || (size_t)sn >= sizeof(buf)) {
            ESP_LOGE(TAG, "CFG_GET snprintf truncation");
            break;
          }
          write_str_frame_(buf);
          /* STC reply arrives via UART RX path -> published to HA */
        }
        /* GET_RESP / SET_ACK: P5-2 */
        break;
      }
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
    ESP_LOGD(TAG, "tx TELEMETRY az=%u el=%u wind=%u mode=%u role=%u",
             p[0], p[1], p[2], p[3], (unsigned) local_role_);
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

  /* Inverse of mode_to_code_: decode a packed code back to the mode string. */
  static std::string mode_from_code_(uint8_t code) {
    switch (code) {
      case 0: return "idle";
      case 1: return "track";
      case 2: return "storm";
      case 3: return "jog";
      case 4: return "cal";
      case 5: return "menu";
      case 6: return "set";
      case 7: return "nocal";
      case 8: return "btest";
      case 9: return "ver";
      default: return "?";
    }
  }

  /* Gateway election: returns true iff this node has the lowest MAC among
   * all nodes currently broadcasting GATEWAY_HB within HB_STALE_MS AND
   * this node itself has WiFi connectivity.
   *
   * peers_ is now keyed by node_name, so we read each entry's stored .mac
   * field (populated from the src field of their last GATEWAY_HB) for the
   * MAC-lowest comparison. */
  bool is_acting_gateway_() {
    if (!WiFi.isConnected()) return false;
    uint32_t now = millis();
    /* Start with our own MAC as the candidate lowest. */
    std::array<uint8_t, 6> lowest{my_mac_[0],my_mac_[1],my_mac_[2],my_mac_[3],my_mac_[4],my_mac_[5]};
    for (const auto &kv : peers_) {
      if (now - kv.second.last_gateway_hb_ms > HB_STALE_MS) continue;
      /* kv.second.mac holds the last-seen source MAC for this peer. */
      std::array<uint8_t, 6> pmac;
      memcpy(pmac.data(), kv.second.mac, 6);
      if (pmac < lowest) lowest = pmac;
    }
    return std::memcmp(my_mac_, lowest.data(), 6) == 0;
  }

  /* Publish peer telemetry to HA.  Looks up peer_decls_ by node_name key
   * and calls publish_state on each wired entity. */
  void publish_peer_to_ha_(const std::array<char, 32> &name_key, const PeerEntry &e) {
    auto it = peer_decls_.find(name_key);
    if (it == peer_decls_.end()) {
      ESP_LOGD(TAG, "[gateway] no entity binding for peer '%.32s' (not declared in YAML)",
               name_key.data());
      return;
    }
    auto &d = it->second;
    ESP_LOGD(TAG, "[gateway] publish peer '%.32s' az=%u el=%u wind=%u mode=%s",
             name_key.data(), e.az_pct, e.el_pct, e.wind_used, e.mode.c_str());
    if (d.az)   d.az->publish_state(float(e.az_pct));
    if (d.el)   d.el->publish_state(float(e.el_pct));
    if (d.wind) d.wind->publish_state(float(e.wind_used));
    if (d.mode) d.mode->publish_state(e.mode);
  }

  /* ---- write_str_frame_: build and write a framed UART packet from a
   * complete ASCII payload string.  Replaces the old send_command_frame_
   * (prefix+value variant) -- all callers now build the string themselves
   * with snprintf before calling this helper.
   *
   * Format: AA 55 <ASCII payload> <2 hex CRC8> LF
   * CRC covers only the ASCII payload bytes -- byte-identical to the STC's
   * uart_send_frame / crc8_update (see EcoWorthyFirmware/src/main.c). */
  void write_str_frame_(const char *s) {
    uint8_t crc = 0;
    for (const char *q = s; *q; q++) crc = crc8_step_(crc, (uint8_t)*q);
    this->write_byte(0xAA);
    this->write_byte(0x55);
    for (const char *q = s; *q; q++) this->write_byte((uint8_t)*q);
    this->write_byte((uint8_t)hex_digit_(crc >> 4));
    this->write_byte((uint8_t)hex_digit_(crc & 0x0F));
    this->write_byte(uint8_t('\n'));
    ESP_LOGD(TAG, "cmd->STC: %s (crc=%02X)", s, crc);
  }

  /* ---- find_peer_mac_: look up a peer's last-seen MAC by node_name.
   * Returns pointer to static storage inside the peers_ map entry (valid
   * as long as no rehash occurs, which doesn't happen with std::map).
   * Returns nullptr if the peer has never broadcast. ---- */
  const uint8_t *find_peer_mac_(const std::string &peer_name) {
    auto key = make_name_key_(peer_name);
    auto it = peers_.find(key);
    if (it != peers_.end()) return it->second.mac;
    ESP_LOGW(TAG, "find_peer_mac_: peer '%s' not yet seen on mesh", peer_name.c_str());
    return nullptr;
  }

  /* ---- make_name_key_: build a 32-byte NUL-padded array from a string ----
   * Caps at 31 chars copied so the 32nd byte is always NUL.  Matches the
   * 31-char codegen-time cap in __init__.py and the runtime truncation in
   * mesh_setup_().  Two names that share their first 31 chars would
   * otherwise collide in peer_decls_ / peer_last_session_ if either filled
   * all 32 bytes. */
  static std::array<char, 32> make_name_key_(const std::string &name) {
    std::array<char, 32> key{};
    size_t copy_len = name.size() < 31 ? name.size() : 31;
    memcpy(key.data(), name.c_str(), copy_len);
    return key;
  }

  /* ---- broadcast_mac_: return a pointer to the FF:FF:FF:FF:FF:FF MAC ---- */
  static const uint8_t *broadcast_mac_() {
    static uint8_t b[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    return b;
  }

  /* ---- mesh_tx_command_: assemble and send a MSG_COMMAND mesh frame ---- */
  void mesh_tx_command_(const uint8_t target[6], uint8_t cmd,
                        uint8_t a1, uint8_t a2) {
    uint8_t payload[9];
    memcpy(payload, target, 6);
    payload[6] = cmd;
    payload[7] = a1;
    payload[8] = a2;
    mesh_tx_(MSG_COMMAND, payload, 9);
  }

};

/* Out-of-class definition of the static singleton pointer. */
TrackerBridge *TrackerBridge::instance_ = nullptr;

#ifdef USE_NUMBER
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

/* RW config slider: fires config_set_for_peer when the HA user moves a slider.
 * Works for both local (peer_id == "") and remote peers.  The slider is
 * optimistic -- published value tracks what was set, not an echo from the STC.
 * Future Task 12 can wire CFG_OP_GET_RESP → publish_state for read-back. */
class ConfigNumber : public number::Number {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
  void set_peer_id(const std::string &id) { peer_id_ = id; }
  void set_field_id(uint8_t fid) { field_id_ = fid; }

 protected:
  void control(float value) override {
    if (parent_ == nullptr) return;
    /* Clamp to uint8 range before sending; slider min/max enforced by ESPHome */
    uint8_t v = (value < 0.0f) ? 0 : (value > 255.0f) ? 255 : (uint8_t)value;
    parent_->config_set_for_peer(peer_id_, field_id_, v);
    this->publish_state(float(v));
  }

  TrackerBridge *parent_{nullptr};
  std::string peer_id_{};
  uint8_t field_id_{0};
};
#endif

#ifdef USE_BUTTON
/* HA command buttons that broadcast mesh commands to all trackers.
 * Each button class holds a pointer to the TrackerBridge that owns
 * the mesh TX path, and calls the corresponding broadcast method on
 * press.  Guarded by USE_BUTTON so the include only resolves when the
 * button framework is actually loaded (mirrors USE_NUMBER above). */
class ForceParkButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_force_park(); }
  TrackerBridge *parent_{nullptr};
};

class ForceReleaseButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_force_release(); }
  TrackerBridge *parent_{nullptr};
};

class StopButton : public button::Button {
 public:
  void set_parent(TrackerBridge *p) { parent_ = p; }
 protected:
  void press_action() override { if (parent_) parent_->broadcast_stop(); }
  TrackerBridge *parent_{nullptr};
};
#endif

}  // namespace tracker_bridge
}  // namespace esphome
