#pragma once

// Wellue O2Ring pulse oximeter, connected as a ble_client peer. GATT
// surface, CRC8 and the AA-framed packet format were reverse-engineered
// against the device -- see __init__.py's docstring for provenance. No
// bonding, no encryption, no secret.

#ifdef USE_ESP32

#include <esp_gattc_api.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/ble_device_base/ble_device.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

namespace esphome {
namespace viatom_o2ring {

/// Requests are marked 0xAA; replies come back with its complement.
static constexpr uint8_t REQUEST_MARKER = 0xAA;
static constexpr uint8_t REPLY_MARKER = 0x55;
static constexpr uint8_t STATUS_OK = 0x00;

namespace espbt = esphome::esp32_ble_tracker;

/// CRC8 over a byte range. This is the ring's own bit-reflected polynomial
/// handling, not a stock CRC8 variant, so no library implementation may be
/// substituted for it.
uint8_t viatom_crc8(const uint8_t *data, size_t len);

/// One reassembled AA-framed packet, header and CRC stripped.
struct ViatomPacket {
  /// Byte 1 of a reply is a status, not the command. 0x00 is OK.
  uint8_t status{0xFF};
  uint16_t block{0};
  std::vector<uint8_t> data;
};

/// Builds request packets and reassembles notification fragments into
/// complete ones: `AA | CMD | ~CMD | BLOCK(2 LE) | LEN(2 LE) | DATA | CRC8`,
/// complete once 7 + LEN + 1 bytes have arrived. Deliberately has no BLE or
/// ESPHome dependency: framing and the connection state machine fail in
/// different ways, and keeping this half a plain byte-in/byte-out class is
/// what makes it possible to reason about -- and eventually test -- on its
/// own.
class ViatomPacketCodec {
 public:
  static std::vector<uint8_t> encode(uint8_t cmd, uint16_t block = 0, const std::vector<uint8_t> &data = {});

  /// Feed one notification fragment. Returns true once a full packet is
  /// ready (fetch it with take_packet()).
  bool feed(const uint8_t *data, size_t len);
  ViatomPacket take_packet();

  /// Drop any partial fragment -- call before sending a new request so a
  /// stale tail can't be mistaken for the start of the next reply.
  void reset() { this->buffer_.clear(); }

 protected:
  /// Drop bytes before the next plausible 0xAA header. Unsolicited live-data
  /// notifications share this characteristic, so the buffer can start mid-packet.
  void sync_();

 public:

 protected:
  std::vector<uint8_t> buffer_;
};

/// Connection sequence, advanced purely by gattc_event_handler() and
/// set_timeout() callbacks -- gattc_event_handler() runs on ESPHome's
/// cooperative main loop, so nothing here may block. After INFO, walks the
/// whole FileList oldest-first (FILE_OPEN -> repeated FILE_READ ->
/// FILE_CLOSE, one file at a time -- see start_next_download_()) and stops
/// at DONE once nothing pending remains.
enum class State : uint8_t {
  IDLE,
  SUBSCRIBING,    // registered for notify, waiting for ESP_GATTC_REG_FOR_NOTIFY_EVT
  AWAITING_INFO,  // INFO request written, waiting for the reassembled reply
  AWAITING_OPEN,  // FILE_OPEN written, waiting for the reply carrying the file size
  AWAITING_READ,  // FILE_READ written for the current block, waiting for its data
  AWAITING_CLOSE, // FILE_CLOSE written, waiting for the ack
  DONE,           // INFO reply parsed and any download finished (or skipped)
};

class ViatomO2Ring : public Component,
                     public ble_client::BLEClientNode,
                     public ble_device_base::ESPBTDeviceListener {
 public:
  // Purely event/timeout-driven; nothing needs a per-tick poll.
  void loop() override { this->disable_loop(); }
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_BLUETOOTH; }

  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                            esp_ble_gattc_cb_param_t *param) override;

  /// Advertisement-parsed path (registered directly with the tracker, not via
  /// the ble_client parent) -- this is how an absent->present edge is
  /// noticed even though the ring is otherwise not connected. See
  /// parse_device()'s comment in the .cpp for the edge/rate-limit split.
  bool parse_device(const ble_device_base::ESPBTDevice &device) override;

  void set_battery_level_sensor(sensor::Sensor *s) { this->battery_level_sensor_ = s; }
  void set_serial_number_text_sensor(text_sensor::TextSensor *s) { this->serial_number_text_sensor_ = s; }
  void set_state_text_sensor(text_sensor::TextSensor *s) { this->state_text_sensor_ = s; }
  void set_file_list_text_sensor(text_sensor::TextSensor *s) { this->file_list_text_sensor_ = s; }
  void set_last_sync_text_sensor(text_sensor::TextSensor *s) { this->last_sync_text_sensor_ = s; }
  void set_min_sync_interval(uint32_t ms) { this->min_sync_interval_ms_ = ms; }

 protected:
  /// Enter DONE and release the BLE link. Never just set the state.
  void finish_();
  void send_request_(uint8_t cmd, uint16_t block = 0, const std::vector<uint8_t> &data = {});
  void write_next_fragment_();
  void handle_info_reply_(const std::vector<uint8_t> &payload);

  /// Parses FileList into pending_files_, oldest first, skipping anything
  /// already in the completed tree. Called once per INFO reply.
  void build_pending_downloads_(const std::string &sn, const std::string &file_list);
  /// Pops the front of pending_files_ and issues its FILE_OPEN, or -- once
  /// the queue is empty -- publishes last_sync_ and settles at DONE.
  void start_next_download_();
  void handle_open_reply_(const std::vector<uint8_t> &payload);
  void handle_read_reply_(const std::vector<uint8_t> &payload);
  void handle_close_reply_(const std::vector<uint8_t> &payload);
  /// Closes any open handle and clears the in-flight transfer fields.
  /// Never touches the file on disk -- a partial file is left exactly where
  /// it landed, per the staging/promotion contract in the .cpp.
  void reset_download_state_();

  /// Called from maybe_start_sync_() and from every gattc event that could
  /// mark a raw connect failure (see .cpp for why more than one event can).
  /// No-op unless a sync campaign (syncing_) is in progress and hasn't yet
  /// reached service discovery.
  void maybe_retry_connect_();
  /// Absent->present edge handler: rate-limits, then starts a connect.
  void maybe_start_sync_(uint32_t now);

  ViatomPacketCodec codec_;
  State state_{State::IDLE};
  uint16_t write_handle_{0};
  uint16_t notify_handle_{0};

  // In-flight request, written in fragments with a gap between each -- see
  // send_request_() in the .cpp for why.
  std::vector<uint8_t> write_buffer_;
  size_t write_offset_{0};

  // In-flight download, one at a time.
  FILE *download_file_{nullptr};
  std::string download_sn_;
  std::string download_name_;
  uint32_t download_size_{0};
  uint32_t download_written_{0};
  uint16_t download_block_{0};
  bool download_failed_{false};

  // Rest of the walk, oldest-first -- built once from FileList, drained one
  // FILE_OPEN at a time by start_next_download_().
  std::vector<std::string> pending_files_;
  std::string pending_sn_;
  // CurTIME from the INFO reply that started the current walk, and whether
  // anything in it failed -- last_sync_text_sensor_ only advances to
  // cur_time_ if the whole walk finished clean. See handle_info_reply_() /
  // start_next_download_() in the .cpp for why CurTIME rather than uptime.
  std::string cur_time_;
  bool walk_had_failure_{false};

  // Advertisement presence edge (parse_device()) and the sync campaign it
  // starts (maybe_start_sync_() / maybe_retry_connect_()) -- see the .cpp
  // for the full state machine these drive.
  bool ring_present_{false};
  bool has_synced_before_{false};
  uint32_t last_sync_attempt_ms_{0};
  uint32_t min_sync_interval_ms_{30 * 60 * 1000};
  bool syncing_{false};
  bool reached_search_cmpl_{false};
  bool retry_pending_{false};
  uint8_t connect_attempts_{0};

  sensor::Sensor *battery_level_sensor_{nullptr};
  text_sensor::TextSensor *serial_number_text_sensor_{nullptr};
  text_sensor::TextSensor *state_text_sensor_{nullptr};
  text_sensor::TextSensor *file_list_text_sensor_{nullptr};
  text_sensor::TextSensor *last_sync_text_sensor_{nullptr};
};

}  // namespace viatom_o2ring
}  // namespace esphome

#endif  // USE_ESP32
