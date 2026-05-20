#pragma once

/* Solar tracker bridge -- ESP-side of the Phase 3 protocol.
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
 */

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <string>
#include <cstdlib>

namespace esphome {
namespace tracker_bridge {

static const char *const TAG = "tracker_bridge";

class TrackerBridge : public Component, public uart::UARTDevice {
 public:
  void set_az_sensor(sensor::Sensor *s)        { az_ = s; }
  void set_el_sensor(sensor::Sensor *s)        { el_ = s; }
  void set_wind_sensor(sensor::Sensor *s)      { wind_ = s; }
  void set_mode_sensor(text_sensor::TextSensor *s) { mode_ = s; }

  void setup() override {
    /* Poll the STC every 2 s.  ESPHome's set_interval handles timing
     * and survives WiFi/HA disconnects -- the bridge keeps polling
     * regardless of upstream state. */
    this->set_interval("poll", 2000, [this]() { this->send_poll_(); });
    ESP_LOGCONFIG(TAG, "tracker_bridge configured");
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
  }

  sensor::Sensor *az_{nullptr};
  sensor::Sensor *el_{nullptr};
  sensor::Sensor *wind_{nullptr};
  text_sensor::TextSensor *mode_{nullptr};
};

}  // namespace tracker_bridge
}  // namespace esphome
