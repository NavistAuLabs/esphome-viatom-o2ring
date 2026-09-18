#include "viatom_o2ring.h"

#ifdef USE_ESP32

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"

namespace esphome {
namespace viatom_o2ring {

static const char *const TAG = "viatom_o2ring";

// service 14839ac4-7d7e-415c-9a42-167340cf2339
// write   8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3   (write without response)
// notify  0734594a-a8e7-4b1a-a6b1-cd5243059a57
static const espbt::ESPBTUUID SERVICE_UUID = espbt::ESPBTUUID::from_raw("14839ac4-7d7e-415c-9a42-167340cf2339");
static const espbt::ESPBTUUID WRITE_CHAR_UUID = espbt::ESPBTUUID::from_raw("8b00ace7-eb0b-49b0-bbe9-9aee0a26e1a3");
static const espbt::ESPBTUUID NOTIFY_CHAR_UUID = espbt::ESPBTUUID::from_raw("0734594a-a8e7-4b1a-a6b1-cd5243059a57");

static const uint8_t CMD_INFO = 0x14;
static const uint8_t CMD_FILE_OPEN = 0x03;
static const uint8_t CMD_FILE_READ = 0x04;
static const uint8_t CMD_FILE_CLOSE = 0x05;

// Fragment size and inter-fragment gap used to write a request. Whether they
// reflect a real device limit or just an untuned MTU is unknown; the values
// the ring is known to accept are used as-is rather than re-derived.
static const size_t WRITE_FRAGMENT_SIZE = 20;
static const uint32_t WRITE_FRAGMENT_GAP_MS = 20;
static const uint32_t REPLY_TIMEOUT_MS = 10000;

// Safety bound on FILE_READ block count. The ring is expected to signal
// completion by the running byte
// count reaching the FILE_OPEN-declared size well before this; it exists so
// a misbehaving reply sequence can't spin forever rather than to reflect any
// known file-size limit.
static const uint16_t MAX_FILE_READ_BLOCKS = 8000;

// The sync trigger is seeing the device advertise after a timeout. The
// timeout must be at least 20s: that is how long a connection attempt takes,
// and the scanner hears no advertisements during one -- anything shorter
// fires the trigger after every failed attempt.
static const uint32_t PRESENCE_TIMEOUT_MS = 60000;

// ble_client.connect fails roughly half the time with ESP_GATTC_DISCONNECT_EVT
// reason 0x100 (cancelled locally, not refused by the ring) -- cause unknown,
// and not bluetooth_proxy slot contention or scan duty cycle: changing either
// makes no difference to the failure rate. So absorb it rather than chase it,
// and retry. 5 total attempts (1 + 4 retries) failing together is ~3% at
// even odds, well inside the minutes-long window a charging ring stays awake.
static const uint8_t MAX_CONNECT_RETRIES = 4;
static const uint32_t CONNECT_RETRY_DELAY_MS = 3000;

// Fixed mount point of the sd_mmc_card component (not configurable there),
// and so the root path anything serving the card has to be pointed at.
// Downloads land under <SD_ROOT>/o2ring/<sn>/ once complete; a transfer in
// progress lives under <SD_ROOT>/.partial/o2ring/<sn>/ until it is proven
// whole.
static const char *const SD_ROOT = "/sdcard";

static std::string partial_dir(const std::string &sn) {
  return std::string(SD_ROOT) + "/.partial/o2ring/" + sn;
}
static std::string partial_path(const std::string &sn, const std::string &name) {
  return partial_dir(sn) + "/" + name + ".vld";
}
static std::string completed_dir(const std::string &sn) { return std::string(SD_ROOT) + "/o2ring/" + sn; }
static std::string completed_path(const std::string &sn, const std::string &name) {
  return completed_dir(sn) + "/" + name + ".vld";
}

// POSIX mkdir() is not recursive and this SD card's FAT driver has no
// mkdir -p equivalent, so each path component is created in turn. EEXIST is
// expected and not an error -- most calls are re-creating a directory tree
// that is already there.
static void mkdir_p(const std::string &path) {
  size_t pos = 1;  // skip the leading '/'
  while (true) {
    pos = path.find('/', pos);
    const std::string component = pos == std::string::npos ? path : path.substr(0, pos);
    if (mkdir(component.c_str(), 0777) != 0 && errno != EEXIST) {
      ESP_LOGW(TAG, "mkdir(%s) failed: errno=%d (%s)", component.c_str(), errno, strerror(errno));
    }
    if (pos == std::string::npos)
      return;
    pos++;
  }
}

uint8_t viatom_crc8(const uint8_t *data, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) {
    const uint8_t k = c ^ data[i];
    c = 0;
    if (k & 0x01)
      c = 0x07;
    if (k & 0x02)
      c ^= 0x0E;
    if (k & 0x04)
      c ^= 0x1C;
    if (k & 0x08)
      c ^= 0x38;
    if (k & 0x10)
      c ^= 0x70;
    if (k & 0x20)
      c ^= 0xE0;
    if (k & 0x40)
      c ^= 0xC7;
    if (k & 0x80)
      c ^= 0x89;
  }
  return c;
}

std::vector<uint8_t> ViatomPacketCodec::encode(uint8_t cmd, uint16_t block, const std::vector<uint8_t> &data) {
  std::vector<uint8_t> pkt;
  pkt.reserve(7 + data.size() + 1);
  pkt.push_back(0xAA);
  pkt.push_back(cmd);
  pkt.push_back(cmd ^ 0xFF);
  pkt.push_back(block & 0xFF);
  pkt.push_back((block >> 8) & 0xFF);
  pkt.push_back(data.size() & 0xFF);
  pkt.push_back((data.size() >> 8) & 0xFF);
  pkt.insert(pkt.end(), data.begin(), data.end());
  pkt.push_back(viatom_crc8(pkt.data(), pkt.size()));
  return pkt;
}

void ViatomPacketCodec::sync_() {
  // Requests start with 0xAA, replies start with 0x55.
  //
  // Request (CMD_INFO, block 0, empty payload):
  //   AA 14 EB 00 00 00 00 ..
  //   ^  ^  ^  ^^^^^ ^^^^^ ^
  //   |  |  |  block len LE CRC8
  //   |  |  cmd complement
  //   |  cmd
  //   request marker
  //
  // Reply:
  //   55 00 ff 00 00 02 7b 22 52 ...       header, then payload
  //   ^  ^  ^  ^^^^^ ^^^^^
  //   |  |  |  block len=512 LE
  //   |  |  status complement
  //   |  status (0x00 = OK)
  //   reply marker
  //
  // Byte 1 of a reply is a STATUS, not an echo of the command. Resync on the
  // marker plus a valid status/complement pair; the complement check matters
  // because 0x55 occurs in payload data.
  size_t i = 0;
  while (i + 3 <= this->buffer_.size()) {
    if (this->buffer_[i] == REPLY_MARKER && (this->buffer_[i + 1] ^ this->buffer_[i + 2]) == 0xFF)
      break;
    ++i;
  }
  if (i > 0) {
    ESP_LOGV(TAG, "Resync: discarded %u byte(s) before header", (unsigned) i);
    this->buffer_.erase(this->buffer_.begin(), this->buffer_.begin() + i);
  }
}

bool ViatomPacketCodec::feed(const uint8_t *data, size_t len) {
  this->buffer_.insert(this->buffer_.end(), data, data + len);
  this->sync_();
  if (this->buffer_.size() < 7)
    return false;
  const uint16_t payload_len = this->buffer_[5] | (static_cast<uint16_t>(this->buffer_[6]) << 8);
  return this->buffer_.size() >= 7u + payload_len + 1u;
}

ViatomPacket ViatomPacketCodec::take_packet() {
  ViatomPacket pkt;
  this->sync_();
  if (this->buffer_.size() < 7)
    return pkt;
  const uint16_t payload_len = this->buffer_[5] | (static_cast<uint16_t>(this->buffer_[6]) << 8);
  const size_t total = 7u + payload_len + 1u;
  if (this->buffer_.size() < total)
    return pkt;

  const uint8_t expected_crc = viatom_crc8(this->buffer_.data(), total - 1);
  const uint8_t actual_crc = this->buffer_[total - 1];
  if (expected_crc != actual_crc) {
    ESP_LOGW(TAG, "Reply CRC mismatch: expected 0x%02x got 0x%02x", expected_crc, actual_crc);
  }

  pkt.status = this->buffer_[1];
  pkt.block = this->buffer_[3] | (static_cast<uint16_t>(this->buffer_[4]) << 8);
  pkt.data.assign(this->buffer_.begin() + 7, this->buffer_.begin() + 7 + payload_len);

  // Consume only this packet, not the whole buffer: an unsolicited packet may
  // already be queued behind it and discarding that would resync onto a
  // fragment next time.
  this->buffer_.erase(this->buffer_.begin(), this->buffer_.begin() + total);
  return pkt;

}

void ViatomO2Ring::dump_config() {
  ESP_LOGCONFIG(TAG, "Viatom O2Ring:");
  ESP_LOGCONFIG(TAG, "  MAC Address: %s", this->parent()->address_str());
}

void ViatomO2Ring::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      this->state_ = State::IDLE;
      this->write_handle_ = 0;
      this->notify_handle_ = 0;
      this->codec_.reset();
      this->cancel_timeout("write");
      this->cancel_timeout("reply");
      // A disconnect mid-transfer is exactly the "short or failed transfer"
      // case .partial/ exists for: this closes the handle and drops the
      // in-flight fields, but never touches the bytes already written.
      this->reset_download_state_();
      // A raw connect failure can already show IDLE here (see
      // maybe_retry_connect_()'s comment for why more than one event needs
      // this same check).
      this->maybe_retry_connect_();
      break;
    }

    // Neither carries protocol state of ours -- both exist purely as extra
    // places a raw connect failure can surface the parent's state going back
    // to IDLE (see maybe_retry_connect_()).
    case ESP_GATTC_OPEN_EVT:
    case ESP_GATTC_CLOSE_EVT: {
      this->maybe_retry_connect_();
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Reached regardless of what's found below: this is "the connection
      // itself survived past the flaky open" for maybe_retry_connect_'s
      // purposes, not "the right device answered".
      this->reached_search_cmpl_ = true;
      auto *write_chr = this->parent()->get_characteristic(SERVICE_UUID, WRITE_CHAR_UUID);
      auto *notify_chr = this->parent()->get_characteristic(SERVICE_UUID, NOTIFY_CHAR_UUID);
      if (write_chr == nullptr || notify_chr == nullptr) {
        ESP_LOGW(TAG, "[%s] O2Ring service/characteristics not found -- wrong device?",
                 this->parent()->address_str());
        break;
      }
      this->write_handle_ = write_chr->handle;
      this->notify_handle_ = notify_chr->handle;
      this->state_ = State::SUBSCRIBING;

      // register_for_notify() (the BLEClientBase helper, not the raw
      // esp_ble_gattc_* call) so the parent holds the GATT cache release
      // until this completes -- see ble_client.h's BLEClientNode comment on
      // node_state sequencing.
      auto status = this->parent()->register_for_notify(this->notify_handle_);
      if (status != ESP_OK) {
        ESP_LOGW(TAG, "[%s] register_for_notify failed, status=%d", this->parent()->address_str(), status);
      }
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.handle != this->notify_handle_)
        break;
      // Safe now: both handles were resolved from the search above, so
      // nothing here still needs the GATT cache the parent is about to free.
      this->node_state = espbt::ClientState::ESTABLISHED;
      this->state_ = State::AWAITING_INFO;
      this->send_request_(CMD_INFO);
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.handle != this->notify_handle_)
        break;
      if (!this->codec_.feed(param->notify.value, param->notify.value_len))
        break;
      this->cancel_timeout("reply");
      auto pkt = this->codec_.take_packet();
      // Which request this reply belongs to is tracked by state_, set right
      // before each request went out -- there is only ever one in flight.
      switch (this->state_) {
        case State::AWAITING_INFO:
          if (pkt.status == STATUS_OK) {
            this->handle_info_reply_(pkt.data);
          } else {
            ESP_LOGW(TAG, "[%s] INFO reply status 0x%02x", this->parent()->address_str(), pkt.status);
            this->finish_();
          }
          break;
        case State::AWAITING_OPEN:
          // FILE_OPEN's status is unreliable -- it reports OK with a size
          // even for a filename that doesn't exist, echoing the previous
          // open. The name only ever comes from FileList here, so status is
          // ignored rather than used to gate anything.
          this->handle_open_reply_(pkt.data);
          break;
        case State::AWAITING_READ:
          this->handle_read_reply_(pkt.data);
          break;
        case State::AWAITING_CLOSE:
          this->handle_close_reply_(pkt.data);
          break;
        default:
          break;
      }
      break;
    }

    default:
      break;
  }
}

void ViatomO2Ring::send_request_(uint8_t cmd, uint16_t block, const std::vector<uint8_t> &data) {
  this->codec_.reset();
  this->write_buffer_ = ViatomPacketCodec::encode(cmd, block, data);
  this->write_offset_ = 0;
  this->write_next_fragment_();
  this->set_timeout("reply", REPLY_TIMEOUT_MS, [this, cmd]() {
    ESP_LOGW(TAG, "[%s] Timed out waiting for cmd 0x%02x reply", this->parent()->address_str(), cmd);
  });
}

void ViatomO2Ring::write_next_fragment_() {
  const size_t remaining = this->write_buffer_.size() - this->write_offset_;
  const size_t chunk = remaining < WRITE_FRAGMENT_SIZE ? remaining : WRITE_FRAGMENT_SIZE;
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                          this->write_handle_, chunk, this->write_buffer_.data() + this->write_offset_,
                                          ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status != ESP_OK) {
    ESP_LOGW(TAG, "[%s] esp_ble_gattc_write_char failed, status=%d", this->parent()->address_str(), status);
    return;
  }
  this->write_offset_ += chunk;
  if (this->write_offset_ < this->write_buffer_.size()) {
    this->set_timeout("write", WRITE_FRAGMENT_GAP_MS, [this]() { this->write_next_fragment_(); });
  }
}

void ViatomO2Ring::handle_info_reply_(const std::vector<uint8_t> &payload) {
  // The reply payload isn't pure JSON -- find the outermost {...} in it.
  const std::string text(payload.begin(), payload.end());
  const size_t start = text.find('{');
  const size_t end = text.rfind('}');
  if (start == std::string::npos || end == std::string::npos || end < start) {
    ESP_LOGW(TAG, "[%s] INFO reply had no JSON payload (%u bytes)", this->parent()->address_str(),
             (unsigned) payload.size());
    this->finish_();
    return;
  }
  const std::string json_text = text.substr(start, end - start + 1);

  const bool ok = json::parse_json(json_text, [this](JsonObject root) -> bool {
    const std::string sn = root["SN"].is<const char *>() ? root["SN"].as<std::string>() : "";
    const std::string cur_bat = root["CurBAT"].is<const char *>() ? root["CurBAT"].as<std::string>() : "";
    const std::string cur_state = root["CurState"].is<const char *>() ? root["CurState"].as<std::string>() : "";
    const std::string cur_batstate =
        root["CurBatState"].is<const char *>() ? root["CurBatState"].as<std::string>() : "";
    const std::string cur_time = root["CurTIME"].is<const char *>() ? root["CurTIME"].as<std::string>() : "";
    const std::string file_list = root["FileList"].is<const char *>() ? root["FileList"].as<std::string>() : "";

    ESP_LOGI(TAG, "[%s] INFO: SN=%s battery=%s state=%s batstate=%s time=%s files=[%s]",
             this->parent()->address_str(), sn.c_str(), cur_bat.c_str(), cur_state.c_str(), cur_batstate.c_str(),
             cur_time.c_str(), file_list.c_str());

    if (this->battery_level_sensor_ != nullptr) {
      // "98%" -> 98; strtof() stops at the '%' on its own.
      this->battery_level_sensor_->publish_state(cur_bat.empty() ? NAN : strtof(cur_bat.c_str(), nullptr));
    }
    if (this->serial_number_text_sensor_ != nullptr)
      this->serial_number_text_sensor_->publish_state(sn);
    if (this->state_text_sensor_ != nullptr)
      this->state_text_sensor_->publish_state(cur_state);
    if (this->file_list_text_sensor_ != nullptr)
      this->file_list_text_sensor_->publish_state(file_list);

    // CurTIME is the ring's own clock at this INFO read; last_sync_text_sensor_
    // only gets it once the whole walk below finishes without a failure (see
    // start_next_download_()).
    this->cur_time_ = cur_time;
    this->walk_had_failure_ = false;
    this->pending_sn_ = sn;
    this->build_pending_downloads_(sn, file_list);
    // Sets state_ itself (DONE if there is nothing pending, AWAITING_OPEN if
    // a download is starting) -- nothing after this point may touch state_.
    this->start_next_download_();
    return true;
  });

  if (!ok) {
    ESP_LOGW(TAG, "[%s] INFO JSON failed to parse: %s", this->parent()->address_str(), json_text.c_str());
    this->finish_();
  }
}

void ViatomO2Ring::build_pending_downloads_(const std::string &sn, const std::string &file_list) {
  std::vector<std::string> names;
  size_t pos = 0;
  while (pos < file_list.size()) {
    const size_t comma = file_list.find(',', pos);
    const std::string name = file_list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!name.empty())
      names.push_back(name);
    if (comma == std::string::npos)
      break;
    pos = comma + 1;
  }

  // Oldest first, deliberately: the ring's four slots evict the oldest
  // recording when a new session STARTS (not when it's saved), so the
  // oldest file is the one closest to being lost if this walk gets cut
  // short. Names are fixed-width timestamps, so lexicographic order is time
  // order -- no need to parse them as dates.
  std::sort(names.begin(), names.end());

  this->pending_files_.clear();
  struct stat st;
  for (const auto &name : names) {
    const std::string done_path = completed_path(sn, name);
    if (stat(done_path.c_str(), &st) == 0) {
      ESP_LOGD(TAG, "[%s] %s already downloaded (%s)", this->parent()->address_str(), name.c_str(),
               done_path.c_str());
      continue;
    }
    this->pending_files_.push_back(name);
  }
}

void ViatomO2Ring::start_next_download_() {
  if (this->pending_files_.empty()) {
    if (this->walk_had_failure_) {
      ESP_LOGW(TAG, "[%s] FileList walk finished with at least one failed transfer -- last-sync not advanced",
               this->parent()->address_str());
    } else {
      ESP_LOGI(TAG, "[%s] FileList walk finished clean", this->parent()->address_str());
      if (this->last_sync_text_sensor_ != nullptr)
        this->last_sync_text_sensor_->publish_state(this->cur_time_);
    }
    this->finish_();
    return;
  }

  const std::string name = this->pending_files_.front();
  this->pending_files_.erase(this->pending_files_.begin());
  ESP_LOGI(TAG, "[%s] Downloading %s (%u more queued)", this->parent()->address_str(), name.c_str(),
           (unsigned) this->pending_files_.size());
  this->download_sn_ = this->pending_sn_;
  this->download_name_ = name;
  std::vector<uint8_t> data(name.begin(), name.end());
  data.push_back(0x00);  // FILE_OPEN's filename is NUL-terminated.
  this->send_request_(CMD_FILE_OPEN, 0, data);
  this->state_ = State::AWAITING_OPEN;
}

void ViatomO2Ring::handle_open_reply_(const std::vector<uint8_t> &payload) {
  if (payload.size() < 4) {
    ESP_LOGW(TAG, "[%s] FILE_OPEN reply too short (%u bytes)", this->parent()->address_str(),
             (unsigned) payload.size());
    this->download_failed_ = true;
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
    return;
  }

  this->download_size_ = payload[0] | (static_cast<uint32_t>(payload[1]) << 8) |
                          (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
  this->download_written_ = 0;
  this->download_block_ = 0;
  this->download_failed_ = false;

  mkdir_p(partial_dir(this->download_sn_));
  const std::string path = partial_path(this->download_sn_, this->download_name_);
  this->download_file_ = fopen(path.c_str(), "wb");
  if (this->download_file_ == nullptr) {
    ESP_LOGW(TAG, "[%s] fopen(%s) failed: errno=%d (%s)", this->parent()->address_str(), path.c_str(), errno,
             strerror(errno));
    this->download_failed_ = true;
  } else {
    ESP_LOGI(TAG, "[%s] %s: opening, %u bytes declared", this->parent()->address_str(),
             this->download_name_.c_str(), (unsigned) this->download_size_);
  }

  if (!this->download_failed_ && this->download_size_ > 0) {
    this->send_request_(CMD_FILE_READ, this->download_block_);
    this->state_ = State::AWAITING_READ;
  } else {
    // Nothing to read (zero-length file) or the local open already failed --
    // either way go straight to FILE_CLOSE so the device's handle is freed.
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
  }
}

void ViatomO2Ring::handle_read_reply_(const std::vector<uint8_t> &payload) {
  if (payload.empty()) {
    ESP_LOGW(TAG, "[%s] %s: empty FILE_READ reply at %u/%u bytes -- short transfer",
             this->parent()->address_str(), this->download_name_.c_str(), (unsigned) this->download_written_,
             (unsigned) this->download_size_);
    this->download_failed_ = true;
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
    return;
  }

  const size_t written = fwrite(payload.data(), 1, payload.size(), this->download_file_);
  this->download_written_ += written;
  if (written != payload.size()) {
    ESP_LOGW(TAG, "[%s] %s: short fwrite (%u/%u bytes at offset %u) errno=%d (%s)", this->parent()->address_str(),
             this->download_name_.c_str(), (unsigned) written, (unsigned) payload.size(),
             (unsigned) this->download_written_, errno, strerror(errno));
    this->download_failed_ = true;
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
    return;
  }

  ESP_LOGD(TAG, "[%s] %s: %u/%u bytes", this->parent()->address_str(), this->download_name_.c_str(),
           (unsigned) this->download_written_, (unsigned) this->download_size_);

  if (this->download_written_ >= this->download_size_) {
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
    return;
  }

  this->download_block_++;
  if (this->download_block_ >= MAX_FILE_READ_BLOCKS) {
    ESP_LOGW(TAG, "[%s] %s: hit the %u-block safety cap without reaching the declared size -- aborting",
             this->parent()->address_str(), this->download_name_.c_str(), (unsigned) MAX_FILE_READ_BLOCKS);
    this->download_failed_ = true;
    this->send_request_(CMD_FILE_CLOSE);
    this->state_ = State::AWAITING_CLOSE;
    return;
  }

  this->send_request_(CMD_FILE_READ, this->download_block_);
}

void ViatomO2Ring::handle_close_reply_(const std::vector<uint8_t> & /*payload*/) {
  // FILE_CLOSE's reply carries nothing this component needs -- completion is
  // judged from the running byte count, not from anything in this payload.
  //
  // Flush and close before touching the path: renaming while the FAT driver
  // still holds buffered writes for this handle would race the promotion.
  if (this->download_file_ != nullptr) {
    fclose(this->download_file_);
    this->download_file_ = nullptr;
  }

  const bool complete = !this->download_failed_ && this->download_written_ == this->download_size_;
  const std::string src = partial_path(this->download_sn_, this->download_name_);

  // A truncated file that looks complete is worse than an obviously
  // incomplete one, because a periodic HTTP puller would publish it as real
  // data. So promotion is gated strictly on the written
  // count matching what FILE_OPEN declared; anything else is left in
  // .partial/ rather than guessed at.
  if (complete) {
    mkdir_p(completed_dir(this->download_sn_));
    const std::string dst = completed_path(this->download_sn_, this->download_name_);
    if (rename(src.c_str(), dst.c_str()) == 0) {
      ESP_LOGI(TAG, "[%s] %s: downloaded %u bytes -> %s", this->parent()->address_str(),
               this->download_name_.c_str(), (unsigned) this->download_written_, dst.c_str());
    } else {
      ESP_LOGW(TAG, "[%s] %s: rename to %s failed, errno=%d (%s) -- left in %s", this->parent()->address_str(),
               this->download_name_.c_str(), dst.c_str(), errno, strerror(errno), src.c_str());
      this->walk_had_failure_ = true;
    }
  } else {
    ESP_LOGW(TAG, "[%s] %s: incomplete transfer (%u/%u bytes) -- left in %s", this->parent()->address_str(),
             this->download_name_.c_str(), (unsigned) this->download_written_, (unsigned) this->download_size_,
             src.c_str());
    this->walk_had_failure_ = true;
  }

  this->reset_download_state_();
  // One file down (or given up on) -- continue oldest-first through
  // whatever's left in pending_files_ rather than stopping at the first
  // failure, so one bad file doesn't cost the rest of the walk.
  this->start_next_download_();
}

void ViatomO2Ring::reset_download_state_() {
  if (this->download_file_ != nullptr) {
    fclose(this->download_file_);
    this->download_file_ = nullptr;
  }
  this->download_sn_.clear();
  this->download_name_.clear();
  this->download_size_ = 0;
  this->download_written_ = 0;
  this->download_block_ = 0;
  this->download_failed_ = false;
}

bool ViatomO2Ring::parse_device(const ble_device_base::ESPBTDevice &device) {
  if (device.address_uint64() != this->parent()->get_address())
    return false;

  // VERBOSE, not VERY_VERBOSE: the device logger runs at VERBOSE, and a
  // VERY_VERBOSE statement is compiled out entirely at that level, which reads
  // as "no advertisements" and is indistinguishable from the listener never
  // being called. The tracker logs nothing per-advertisement of its own, so
  // this is the only way to answer "is this listener even reached?" from
  // outside.
  ESP_LOGV(TAG, "[%s] advertisement seen (present=%d)", this->parent()->address_str(), (int) this->ring_present_);

  const bool was_present = this->ring_present_;
  this->ring_present_ = true;
  // Rescheduled on every advertisement seen; PRESENCE_TIMEOUT_MS after the
  // last one, this flips back to false and the next sighting is an edge
  // again -- this is the "gone quiet" detector, entirely separate from the
  // sync rate limit in maybe_start_sync_().
  this->cancel_timeout("presence");
  this->set_timeout("presence", PRESENCE_TIMEOUT_MS, [this]() { this->ring_present_ = false; });

  if (!was_present) {
    // Absent -> present: the ring was fully silent and just started
    // advertising, which after a night's wear means it's being docked (see
    // the capture-window sequence in this component's README). This is the trigger;
    // maybe_start_sync_() applies the actual rate limit.
    ESP_LOGD(TAG, "[%s] Started advertising -- likely just woke", this->parent()->address_str());
    this->maybe_start_sync_(millis());
  }
  return true;
}

void ViatomO2Ring::maybe_start_sync_(uint32_t now) {
  if (this->parent()->state() != espbt::ClientState::IDLE) {
    // A connection -- ours from an earlier retry, or the manual button's --
    // is already in flight. Let it run rather than stepping on it.
    //
    // Logged rather than returning silently: a client left stuck in a
    // non-IDLE state after a timed-out connect swallows every subsequent
    // edge, and an edge is a ONE-SHOT -- the ring advertises continuously
    // once awake, so a swallowed edge is not retried until it next goes
    // fully off the air. Silence here is indistinguishable from the listener
    // never firing.
    ESP_LOGD(TAG, "[%s] Edge ignored: client state %d, not IDLE", this->parent()->address_str(),
             (int) this->parent()->state());
    return;
  }
  if (this->has_synced_before_ && now - this->last_sync_attempt_ms_ < this->min_sync_interval_ms_) {
    // The rate limit exists to stop a long continuous advertising period
    // (e.g. a full charge cycle) from reconnecting repeatedly, not to
    // schedule discovery -- the edge above is what decides *when* to try.
    ESP_LOGD(TAG, "[%s] Skipping sync, last attempt %u ms ago (minimum %u ms)", this->parent()->address_str(),
             (unsigned) (now - this->last_sync_attempt_ms_), (unsigned) this->min_sync_interval_ms_);
    return;
  }

  ESP_LOGI(TAG, "[%s] Starting sync", this->parent()->address_str());
  this->has_synced_before_ = true;
  this->last_sync_attempt_ms_ = now;
  this->syncing_ = true;
  this->connect_attempts_ = 0;
  this->reached_search_cmpl_ = false;
  this->cancel_timeout("connect_retry");
  this->parent()->connect();
}

void ViatomO2Ring::maybe_retry_connect_() {
  // Only a sync this component started, and only while the connection
  // hasn't gotten anywhere yet -- a manual `O2Ring Connect` press, or a
  // disconnect after real protocol progress, are left to run their course
  // rather than retried here. Both DISCONNECT_EVT and OPEN_EVT/CLOSE_EVT can
  // be the event where the parent's state actually lands back on IDLE for a
  // cancelled connect (ESP_GATTC_DISCONNECT_EVT reason 0x100 followed by
  // OPEN_EVT status=133) -- retry_pending_
  // stops more than one of them from double-scheduling the same retry.
  if (!this->syncing_ || this->reached_search_cmpl_ || this->retry_pending_)
    return;
  if (this->parent()->state() != espbt::ClientState::IDLE)
    return;

  if (this->connect_attempts_ >= MAX_CONNECT_RETRIES) {
    ESP_LOGW(TAG, "[%s] Giving up after %u failed connect attempts", this->parent()->address_str(),
             (unsigned) this->connect_attempts_);
    this->syncing_ = false;
    return;
  }

  this->connect_attempts_++;
  this->retry_pending_ = true;
  ESP_LOGW(TAG, "[%s] Connect attempt failed before service discovery -- retry %u/%u in %ums",
           this->parent()->address_str(), (unsigned) this->connect_attempts_, (unsigned) MAX_CONNECT_RETRIES,
           (unsigned) CONNECT_RETRY_DELAY_MS);
  this->set_timeout("connect_retry", CONNECT_RETRY_DELAY_MS, [this]() {
    this->retry_pending_ = false;
    this->parent()->connect();
  });
}

void ViatomO2Ring::finish_() {
  this->state_ = State::DONE;
  this->syncing_ = false;
  // RELEASE THE LINK. A connected BLE peripheral stops advertising, so holding
  // it open makes the ring invisible to every later scan -- and to this
  // component's own reconnect attempts, which then fail against a device it is
  // already attached to. Worse, a held connection was observed preempting the
  // ring's countdown / "saving" / "END" display sequence, so keeping it open
  // risks the recording never being written at all.
  if (this->parent() != nullptr) {
    ESP_LOGD(TAG, "[%s] Sync finished, disconnecting", this->parent()->address_str());
    this->parent()->disconnect();
  }
}

}  // namespace viatom_o2ring
}  // namespace esphome

#endif  // USE_ESP32
