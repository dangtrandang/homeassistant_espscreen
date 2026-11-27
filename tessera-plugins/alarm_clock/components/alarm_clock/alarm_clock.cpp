#include "alarm_clock.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esphome/components/audio/audio.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::alarm_clock {

static const char *const TAG = "alarm_clock";

void AlarmClock::setup() {
  pref_ = global_preferences->make_preference<AlarmStore>(ALARM_NVS_HASH);
  load_store_();
  add_card(
      "manager",
      [this]() {
        AlarmCardOps ops;
        ops.is_ringing = [this]() { return is_ringing(); };
        ops.revision = [this]() { return store_revision(); };
        ops.list = [this]() {
          std::vector<AlarmItemView> out;
          for (const auto &a : store_.alarms) {
            if (a.id == 0 || a.deleted != 0) continue;
            out.push_back({a.id, a.hour, a.minute, a.repeat_days, a.enabled != 0, a.label});
          }
          std::sort(out.begin(), out.end(), [](const AlarmItemView &x, const AlarmItemView &y) {
            if (x.hour != y.hour) return x.hour < y.hour;
            if (x.minute != y.minute) return x.minute < y.minute;
            return x.id < y.id;
          });
          return out;
        };
        ops.upsert = [this](int id, int hour, int minute, int repeat_days, bool enabled, const std::string &label) {
          return upsert_alarm(id, hour, minute, repeat_days, enabled, label);
        };
        ops.remove = [this](int id) { return delete_alarm(id); };
        ops.toggle = [this](int id, bool enabled) { return set_enabled(id, enabled); };
        ops.default_time = [this](int &hour, int &minute) {
          if (time_ != nullptr && time_->now().is_valid()) {
            const uint32_t next_ep = static_cast<uint32_t>(time_->now().timestamp) + 60UL;
            const auto t = ESPTime::from_epoch_local(next_ep);
            hour = t.hour;
            minute = t.minute;
          } else {
            hour = 7;
            minute = 0;
          }
        };
        return new AlarmManagerCard(std::move(ops));
      },
      true);
  add_bar_item("next", [this]() { return bar_item_next_(); });
  ESP_LOGI(TAG, "AlarmClock initialized (NVS key=0x%08X, max=%u, wav=%u bytes @ %u Hz)",
           static_cast<unsigned>(ALARM_NVS_HASH), static_cast<unsigned>(MAX_ALARMS),
           static_cast<unsigned>(alarm_sound_.length), static_cast<unsigned>(alarm_sound_.rate));
  dump_to_log();
}

void AlarmClock::on_ready() {
  ESP_LOGI(TAG, "AlarmClock plugin ready");
}

bool AlarmClock::settings(tessera::SettingsPage &page) {
  page.icon = "\U000F0020";
  page.card("Quản lý báo thức", "\U000F0020", "manager");
  page.number(
      "Báo lại sau", 1, 30, 1, " p",
      [this]() { return static_cast<int>(store_.snooze_minutes); },
      [this](int v) { set_snooze_minutes(v); });
  page.number(
      "Âm lượng chuông", 10, 100, 10, "%",
      [this]() { return static_cast<int>(store_.alarm_volume_pct); },
      [this](int v) { set_alarm_volume_pct(v); });
  return true;
}

void AlarmClock::set_snooze_minutes(int minutes) {
  if (minutes < 1) minutes = 1;
  if (minutes > 30) minutes = 30;
  if (store_.snooze_minutes == static_cast<uint8_t>(minutes)) return;
  store_.snooze_minutes = static_cast<uint8_t>(minutes);
  save_store_();
  notify_changed_();
  ESP_LOGI(TAG, "Snooze duration set to %u min", store_.snooze_minutes);
}

void AlarmClock::set_alarm_volume_pct(int pct) {
  if (pct < 10) pct = 10;
  if (pct > 100) pct = 100;
  if (store_.alarm_volume_pct == static_cast<uint8_t>(pct)) return;
  store_.alarm_volume_pct = static_cast<uint8_t>(pct);
  save_store_();
  notify_changed_();
  ESP_LOGI(TAG, "Alarm ringtone volume ceiling set to %u%%", store_.alarm_volume_pct);
}

void AlarmClock::before_update() {
  if (state_ == AlarmState::RINGING) {
    ESP_LOGI(TAG, "OTA update starting: stopping active alarm audio");
    state_ = AlarmState::IDLE;
    active_alarm_id_ = 0;
    close_popup_();
    stop_audio_and_release_();
  }
}

void AlarmClock::load_store_() {
  AlarmStore loaded{};
  if (pref_.load(&loaded) && loaded.magic == ALARM_STORE_MAGIC && loaded.version == ALARM_STORE_VERSION) {
    store_ = loaded;
    if (store_.snooze_minutes < 1 || store_.snooze_minutes > 30) store_.snooze_minutes = 5;
    if (store_.alarm_volume_pct < 10 || store_.alarm_volume_pct > 100) store_.alarm_volume_pct = 80;
    for (auto &a : store_.alarms) {
      a.label[MAX_LABEL_LEN - 1] = '\0';
      if (a.id > MAX_ALARMS || a.hour > 23 || a.minute > 59) {
        a = AlarmEntry{};
      }
    }
    ESP_LOGI(TAG, "Loaded alarm store from NVS");
    return;
  }
  store_ = AlarmStore{};
  ESP_LOGI(TAG, "Initialized fresh alarm store (no prior NVS record)");
}

bool AlarmClock::save_store_() {
  store_.magic = ALARM_STORE_MAGIC;
  store_.version = ALARM_STORE_VERSION;
  const bool ok = pref_.save(&store_);
  if (ok) {
    global_preferences->sync();
  } else {
    ESP_LOGE(TAG, "Failed to save alarm store to NVS");
  }
  return ok;
}

uint32_t AlarmClock::current_epoch_() const {
  if (time_ != nullptr) {
    const auto now = time_->now();
    if (now.is_valid()) return static_cast<uint32_t>(now.timestamp);
  }
  return 0;
}

void AlarmClock::notify_changed_() {
  ++store_rev_;
  tessera::refresh();
  publish_snapshot();
}

tessera::BarItem AlarmClock::bar_item_next_() const {
  tessera::BarItem item;
  if (time_ == nullptr) return item;
  const auto now = time_->now();
  if (!now.is_valid()) return item;

  static constexpr uint32_t ICON_ALARM = 0xF0020;  // MDI alarm
  char hhmm[8];

  // 1. Ringing: show active alarm time in ALERT tone.
  if (state_ == AlarmState::RINGING) {
    const AlarmEntry *a = find_by_id_(active_alarm_id_);
    if (a != nullptr) {
      std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", a->hour, a->minute);
      item.shown = true;
      item.icon = ICON_BELL_RING;
      item.text = hhmm;
      item.tone = tessera::Tone::ALERT;
      return item;
    }
  }

  // 2. Snoozed: prioritize actual snooze target time in BUSY tone (distinct from original schedule).
  if (state_ == AlarmState::SNOOZED && snooze_until_epoch_ > 0) {
    const auto st = ESPTime::from_epoch_local(snooze_until_epoch_);
    std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", st.hour, st.minute);
    item.shown = true;
    item.icon = ICON_ALARM;
    item.text = hhmm;
    item.tone = tessera::Tone::BUSY;
    return item;
  }

  // 3. Nearest upcoming enabled alarm in ICT local time (respecting weekday repeat_days).
  const uint32_t epoch = static_cast<uint32_t>(now.timestamp);
  const uint32_t cur_min = epoch / 60UL;
  const int now_min_of_day = now.hour * 60 + now.minute;
  int best_delta_min = 8 * 1440;
  const AlarmEntry *best = nullptr;

  for (const auto &a : store_.alarms) {
    if (a.id == 0 || a.deleted != 0 || !a.enabled) continue;
    const int a_min = a.hour * 60 + a.minute;
    int delta_days = -1;

    if (a.repeat_days == 0) {
      // One-shot alarm: today if still ahead, otherwise tomorrow.
      const bool passed_today = (a_min < now_min_of_day) ||
                                (a_min == now_min_of_day && (now.second > 0 || a.last_fired_minute == cur_min));
      delta_days = passed_today ? 1 : 0;
    } else {
      for (int d = 0; d <= 7; ++d) {
        const uint8_t dow = static_cast<uint8_t>(((now.day_of_week - 1 + d) % 7) + 1);
        if (!matches_weekday_(a.repeat_days, dow)) continue;
        if (d == 0) {
          const bool passed_today = (a_min < now_min_of_day) ||
                                    (a_min == now_min_of_day && (now.second > 0 || a.last_fired_minute == cur_min));
          if (passed_today) continue;
        }
        delta_days = d;
        break;
      }
    }

    if (delta_days < 0) continue;
    const int delta_min = delta_days * 1440 + a_min - now_min_of_day;
    if (delta_min < best_delta_min) {
      best_delta_min = delta_min;
      best = &a;
    }
  }

  if (best == nullptr) return item;
  std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", best->hour, best->minute);
  item.shown = true;
  item.icon = ICON_ALARM;
  item.text = hhmm;
  item.tone = tessera::Tone::ACCENT;
  return item;
}

void AlarmClock::publish_snapshot() {
  if (snapshot_cb_) {
    snapshot_cb_(build_snapshot_json(), build_status_text());
  }
}

void AlarmClock::on_api_connected() {
  ESP_LOGI(TAG, "HA API connected: publishing alarm snapshot (rev=%u)", static_cast<unsigned>(store_rev_));
  publish_snapshot();
}

std::string AlarmClock::escape_json_(const char *s) {
  std::string out;
  if (!s) return out;
  for (; *s; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c >= 0x20) {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

std::string AlarmClock::serialize_alarm_json_(const AlarmEntry &a) {
  char buf[160];
  const std::string lbl = escape_json_(a.label);
  std::snprintf(buf, sizeof(buf),
                "{\"id\":%u,\"h\":%u,\"m\":%u,\"r\":%u,\"e\":%u,\"d\":%u,\"u\":%u,\"l\":\"%s\"}",
                a.id, a.hour, a.minute, a.repeat_days, a.enabled, a.deleted,
                static_cast<unsigned>(a.updated_at), lbl.c_str());
  return std::string(buf);
}

std::string AlarmClock::build_snapshot_json() const {
  const char *st = state_ == AlarmState::RINGING ? "RINGING" : (state_ == AlarmState::SNOOZED ? "SNOOZED" : "IDLE");
  char head[96];
  std::snprintf(head, sizeof(head), "{\"s\":\"%s\",\"act\":%u,\"rev\":%u,\"snz\":%u,\"vol\":%u,\"a\":[",
                st, active_alarm_id_, static_cast<unsigned>(store_rev_),
                store_.snooze_minutes, store_.alarm_volume_pct);
  std::string out(head);
  bool first = true;
  for (const auto &a : store_.alarms) {
    if (a.id == 0) continue;
    if (!first) out.push_back(',');
    first = false;
    out += serialize_alarm_json_(a);
  }
  out += "]}";
  return out;
}

std::string AlarmClock::build_status_text() const {
  int total = 0, enabled = 0;
  for (const auto &a : store_.alarms) {
    if (a.id == 0 || a.deleted != 0) continue;
    ++total;
    if (a.enabled) ++enabled;
  }
  char buf[64];
  if (state_ == AlarmState::RINGING) {
    const AlarmEntry *a = find_by_id_(active_alarm_id_);
    if (a != nullptr) {
      std::snprintf(buf, sizeof(buf), "RINGING #%u %02u:%02u", a->id, a->hour, a->minute);
      return std::string(buf);
    }
    return "RINGING";
  }
  if (state_ == AlarmState::SNOOZED) {
    std::snprintf(buf, sizeof(buf), "SNOOZED #%u (%d/%d on)", active_alarm_id_, enabled, total);
    return std::string(buf);
  }
  std::snprintf(buf, sizeof(buf), "IDLE (%d/%d on)", enabled, total);
  return std::string(buf);
}

void AlarmClock::emit_ack_(const std::string &req_id, int alarm_id, bool ok, const std::string &error,
                           const AlarmEntry *entry) const {
  if (!ack_cb_) return;
  const uint32_t upd = entry != nullptr ? entry->updated_at : 0;
  const std::string json = entry != nullptr ? serialize_alarm_json_(*entry) : "{}";
  ack_cb_(req_id, alarm_id, ok, error, upd, json);
}

AlarmEntry *AlarmClock::find_by_id_(uint8_t id, bool include_deleted) {
  if (id == 0) return nullptr;
  for (auto &a : store_.alarms) {
    if (a.id == id && (include_deleted || a.deleted == 0)) return &a;
  }
  return nullptr;
}

const AlarmEntry *AlarmClock::find_by_id_(uint8_t id, bool include_deleted) const {
  if (id == 0) return nullptr;
  for (const auto &a : store_.alarms) {
    if (a.id == id && (include_deleted || a.deleted == 0)) return &a;
  }
  return nullptr;
}

uint8_t AlarmClock::allocate_id_() const {
  // Prefer completely unused IDs first.
  for (uint8_t candidate = 1; candidate <= MAX_ALARMS; ++candidate) {
    if (find_by_id_(candidate, true) == nullptr) return candidate;
  }
  // If all IDs exist, reuse the oldest deleted tombstone.
  uint8_t oldest_id = 0;
  uint32_t oldest_ts = UINT32_MAX;
  for (const auto &a : store_.alarms) {
    if (a.id != 0 && a.deleted != 0 && a.updated_at <= oldest_ts) {
      oldest_ts = a.updated_at;
      oldest_id = a.id;
    }
  }
  return oldest_id;
}

bool AlarmClock::matches_weekday_(uint8_t repeat_days, uint8_t esphome_day_of_week) {
  if (repeat_days == 0) return true;  // One-shot alarm matches any day.
  if (esphome_day_of_week < 1 || esphome_day_of_week > 7) return false;
  const uint8_t bit = static_cast<uint8_t>(1u << (esphome_day_of_week - 1));
  return (repeat_days & bit) != 0;
}

int AlarmClock::upsert_alarm(int id, int hour, int minute, int repeat_days, bool enabled,
                             const std::string &label, uint32_t updated_at) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    ESP_LOGW(TAG, "Rejected invalid alarm time %02d:%02d", hour, minute);
    return -1;
  }
  if (id < 0 || id > static_cast<int>(MAX_ALARMS)) {
    ESP_LOGW(TAG, "Rejected invalid alarm id %d", id);
    return -1;
  }

  AlarmEntry *slot = nullptr;
  uint8_t target_id = static_cast<uint8_t>(id);
  if (target_id == 0) {
    target_id = allocate_id_();
    if (target_id == 0) {
      ESP_LOGW(TAG, "Alarm store full (max %u)", static_cast<unsigned>(MAX_ALARMS));
      return -1;
    }
    slot = find_by_id_(target_id, true);
  } else {
    slot = find_by_id_(target_id, true);
  }

  if (slot == nullptr) {
    for (auto &a : store_.alarms) {
      if (a.id == 0) {
        slot = &a;
        break;
      }
    }
  }
  if (slot == nullptr) return -1;

  const uint32_t now_epoch = current_epoch_();
  const bool schedule_changed = (slot->id == 0 || slot->deleted != 0 || slot->hour != hour || slot->minute != minute ||
                                 slot->repeat_days != static_cast<uint8_t>(repeat_days & 0x7F));
  slot->id = target_id;
  slot->hour = static_cast<uint8_t>(hour);
  slot->minute = static_cast<uint8_t>(minute);
  slot->repeat_days = static_cast<uint8_t>(repeat_days & 0x7F);
  slot->enabled = enabled ? 1 : 0;
  slot->deleted = 0;
  slot->updated_at = updated_at != 0 ? updated_at : now_epoch;
  if (schedule_changed) slot->last_fired_minute = 0;
  std::memset(slot->label, 0, sizeof(slot->label));
  if (!label.empty()) {
    std::strncpy(slot->label, label.c_str(), MAX_LABEL_LEN - 1);
  }

  if (!save_store_()) return -1;
  notify_changed_();
  ESP_LOGI(TAG, "Saved alarm #%u: %02u:%02u repeat=0x%02X enabled=%u label='%s'",
           slot->id, slot->hour, slot->minute, slot->repeat_days, slot->enabled, slot->label);
  return slot->id;
}

bool AlarmClock::delete_alarm(int id, uint32_t updated_at) {
  if (id <= 0 || id > static_cast<int>(MAX_ALARMS)) return false;
  AlarmEntry *slot = find_by_id_(static_cast<uint8_t>(id), false);
  if (slot == nullptr) return false;
  if (active_alarm_id_ == slot->id) {
    state_ = AlarmState::IDLE;
    active_alarm_id_ = 0;
    snooze_until_epoch_ = 0;
    close_popup_();
    stop_audio_and_release_();
  }
  // Keep LWW tombstone in NVS so offline delete is not resurrected by an older HA copy.
  slot->enabled = 0;
  slot->deleted = 1;
  slot->updated_at = updated_at != 0 ? updated_at : current_epoch_();
  if (!save_store_()) return false;
  notify_changed_();
  ESP_LOGI(TAG, "Deleted alarm #%d (tombstone updated_at=%u)", id, static_cast<unsigned>(slot->updated_at));
  return true;
}

bool AlarmClock::set_enabled(int id, bool enabled, uint32_t updated_at) {
  if (id <= 0 || id > static_cast<int>(MAX_ALARMS)) return false;
  AlarmEntry *slot = find_by_id_(static_cast<uint8_t>(id), false);
  if (slot == nullptr) return false;
  slot->enabled = enabled ? 1 : 0;
  if (!enabled && active_alarm_id_ == slot->id) {
    state_ = AlarmState::IDLE;
    active_alarm_id_ = 0;
    snooze_until_epoch_ = 0;
    close_popup_();
    stop_audio_and_release_();
  }
  slot->updated_at = updated_at != 0 ? updated_at : current_epoch_();
  if (!save_store_()) return false;
  notify_changed_();
  ESP_LOGI(TAG, "Alarm #%d enabled=%d", id, enabled ? 1 : 0);
  return true;
}

void AlarmClock::ha_upsert(const std::string &req_id, int id, int hour, int minute, int repeat_days,
                           bool enabled, const std::string &label, uint32_t updated_at) {
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || id < 0 || id > static_cast<int>(MAX_ALARMS)) {
    emit_ack_(req_id, id, false, "invalid_params", nullptr);
    return;
  }
  // Per-alarm_id LWW check: never overwrite a strictly newer local record/tombstone.
  if (id > 0 && updated_at > 0) {
    const AlarmEntry *existing = find_by_id_(static_cast<uint8_t>(id), true);
    if (existing != nullptr && existing->updated_at > updated_at) {
      ESP_LOGI(TAG, "LWW skip HA upsert #%d (ha=%u < local=%u)", id,
               static_cast<unsigned>(updated_at), static_cast<unsigned>(existing->updated_at));
      emit_ack_(req_id, id, true, "skipped_older", existing);
      return;
    }
  }
  const int saved_id = upsert_alarm(id, hour, minute, repeat_days, enabled, label, updated_at);
  if (saved_id <= 0) {
    emit_ack_(req_id, id, false, "nvs_save_failed", nullptr);
    return;
  }
  emit_ack_(req_id, saved_id, true, "", find_by_id_(static_cast<uint8_t>(saved_id), true));
}

void AlarmClock::ha_delete(const std::string &req_id, int id, uint32_t updated_at) {
  if (id <= 0 || id > static_cast<int>(MAX_ALARMS)) {
    emit_ack_(req_id, id, false, "invalid_id", nullptr);
    return;
  }
  const AlarmEntry *existing = find_by_id_(static_cast<uint8_t>(id), true);
  if (existing == nullptr) {
    emit_ack_(req_id, id, true, "already_absent", nullptr);
    return;
  }
  if (updated_at > 0 && existing->updated_at > updated_at) {
    ESP_LOGI(TAG, "LWW skip HA delete #%d (ha=%u < local=%u)", id,
             static_cast<unsigned>(updated_at), static_cast<unsigned>(existing->updated_at));
    emit_ack_(req_id, id, true, "skipped_older", existing);
    return;
  }
  if (existing->deleted != 0) {
    emit_ack_(req_id, id, true, "", existing);
    return;
  }
  const bool ok = delete_alarm(id, updated_at);
  emit_ack_(req_id, id, ok, ok ? "" : "nvs_save_failed", find_by_id_(static_cast<uint8_t>(id), true));
}

void AlarmClock::ha_toggle(const std::string &req_id, int id, bool enabled, uint32_t updated_at) {
  if (id <= 0 || id > static_cast<int>(MAX_ALARMS)) {
    emit_ack_(req_id, id, false, "invalid_id", nullptr);
    return;
  }
  const AlarmEntry *existing = find_by_id_(static_cast<uint8_t>(id), true);
  if (existing == nullptr || existing->deleted != 0) {
    emit_ack_(req_id, id, false, "not_found", existing);
    return;
  }
  if (updated_at > 0 && existing->updated_at > updated_at) {
    ESP_LOGI(TAG, "LWW skip HA toggle #%d (ha=%u < local=%u)", id,
             static_cast<unsigned>(updated_at), static_cast<unsigned>(existing->updated_at));
    emit_ack_(req_id, id, true, "skipped_older", existing);
    return;
  }
  const bool ok = set_enabled(id, enabled, updated_at);
  emit_ack_(req_id, id, ok, ok ? "" : "nvs_save_failed", find_by_id_(static_cast<uint8_t>(id), true));
}

int AlarmClock::set_in_minutes(int offset_minutes, const std::string &label) {
  if (time_ == nullptr) return -1;
  const auto now = time_->now();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "Cannot set relative alarm: SNTP clock not valid yet");
    return -1;
  }
  if (offset_minutes < 1) offset_minutes = 1;
  if (offset_minutes > 1440) offset_minutes = 1440;
  const uint32_t target_epoch = static_cast<uint32_t>(now.timestamp) + static_cast<uint32_t>(offset_minutes) * 60UL;
  const auto target = ESPTime::from_epoch_local(target_epoch);
  return upsert_alarm(0, target.hour, target.minute, 0, true, label, static_cast<uint32_t>(now.timestamp));
}

void AlarmClock::on_interval(uint32_t now_ms) {
  // Safety auto-stop if ringing unattended past MAX_RING_MS.
  if (state_ == AlarmState::RINGING && (now_ms - ring_started_ms_) >= MAX_RING_MS) {
    ESP_LOGW(TAG, "Alarm #%u auto-stopped after %u min", active_alarm_id_,
             static_cast<unsigned>(MAX_RING_MS / 60000UL));
    dismiss();
    return;
  }
  check_schedule_();
}

void AlarmClock::check_schedule_() {
  if (time_ == nullptr) return;
  const auto now = time_->now();
  if (!now.is_valid()) {
    return;
  }
  const uint32_t epoch = static_cast<uint32_t>(now.timestamp);
  if (!time_was_valid_) {
    time_was_valid_ = true;
    ESP_LOGI(TAG, "SNTP clock valid: %04d-%02d-%02d %02d:%02d:%02d (dow=%u)",
             now.year, now.month, now.day_of_month, now.hour, now.minute, now.second, now.day_of_week);
  }
  // Evaluate schedule at most once per second.
  if (epoch == last_check_epoch_) return;
  last_check_epoch_ = epoch;

  if (state_ == AlarmState::RINGING) return;

  const uint32_t cur_min = epoch / 60UL;

  // 1. Check active snooze first.
  if (state_ == AlarmState::SNOOZED && snooze_until_epoch_ != 0 && epoch >= snooze_until_epoch_) {
    const uint32_t late_sec = epoch - snooze_until_epoch_;
    AlarmEntry *alarm = find_by_id_(active_alarm_id_);
    snooze_until_epoch_ = 0;
    if (alarm == nullptr || !alarm->enabled) {
      state_ = AlarmState::IDLE;
      active_alarm_id_ = 0;
      return;
    }
    if (late_sec <= MISSED_CATCHUP_SEC) {
      start_ringing_(*alarm, cur_min, true);
      return;
    }
    ESP_LOGW(TAG, "Snoozed alarm #%u missed by %u s (> %u s), skipping",
             alarm->id, static_cast<unsigned>(late_sec), static_cast<unsigned>(MISSED_CATCHUP_SEC));
    state_ = AlarmState::IDLE;
    active_alarm_id_ = 0;
  }

  // 2. Check all enabled alarms (exact match + <= 2-minute missed catch-up window).
  const uint32_t sec_of_day = static_cast<uint32_t>(now.hour) * 3600UL +
                              static_cast<uint32_t>(now.minute) * 60UL +
                              static_cast<uint32_t>(now.second);
  const uint32_t day_start_epoch = epoch - sec_of_day;

  for (auto &a : store_.alarms) {
    if (a.id == 0 || a.deleted != 0 || !a.enabled) continue;
    if (!matches_weekday_(a.repeat_days, now.day_of_week)) continue;

    const uint32_t sched_epoch = day_start_epoch + static_cast<uint32_t>(a.hour) * 3600UL +
                                 static_cast<uint32_t>(a.minute) * 60UL;
    const uint32_t sched_min = sched_epoch / 60UL;

    if (epoch < sched_epoch) continue;
    if (a.last_fired_minute == sched_min) continue;

    const uint32_t late_sec = epoch - sched_epoch;
    if (late_sec <= MISSED_CATCHUP_SEC) {
      if (late_sec > 59) {
        ESP_LOGI(TAG, "Catch-up firing alarm #%u (%02u:%02u, late by %u s)",
                 a.id, a.hour, a.minute, static_cast<unsigned>(late_sec));
      }
      start_ringing_(a, sched_min, false);
      return;
    }
  }
}

void AlarmClock::start_ringing_(AlarmEntry &alarm, uint32_t fire_minute, bool from_snooze) {
  alarm.last_fired_minute = fire_minute;
  save_store_();

  state_ = AlarmState::RINGING;
  active_alarm_id_ = alarm.id;
  snooze_until_epoch_ = 0;
  ring_started_ms_ = millis();
  sound_offset_ = 0;
  stream_configured_ = false;
  last_audio_push_ms_ = millis();
  reopen_popup_ = false;

  // Wake screen from any standby/Sleep state and display the ringing popup.
  tessera::wake(this);
  show_popup_();

  // Claim BOTH sides at DOORBELL rank so wake_word / voice session pauses cleanly and regains on release.
  using tessera::audio::Role;
  using tessera::audio::Side;
  audio_claimed_ = tessera::audio::claim(
      Side::BOTH, Role::DOORBELL, ALARM_AUDIO_CLAIM_ID,
      [this]() {
        // Another DOORBELL-rank claim took audio; keep state tidy.
        audio_claimed_ = false;
        stream_configured_ = false;
      });

  ESP_LOGI(TAG, "RINGING alarm #%u (%02u:%02u, %s, label='%s', audio_claimed=%d)",
           alarm.id, alarm.hour, alarm.minute, from_snooze ? "snooze" : "scheduled",
           alarm.label, audio_claimed_ ? 1 : 0);
  notify_changed_();
}

void AlarmClock::show_popup_() {
  const AlarmEntry *alarm = find_by_id_(active_alarm_id_);
  AlarmPopupView view;
  char hhmm[8] = "--:--";
  if (alarm != nullptr) {
    std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", alarm->hour, alarm->minute);
    view.label_text = alarm->label[0] != '\0' ? std::string(alarm->label) : std::string("Báo thức");
  } else {
    view.label_text = "Báo thức";
  }
  view.time_text = hhmm;
  char snz[24];
  std::snprintf(snz, sizeof(snz), "Báo lại %up", static_cast<unsigned>(store_.snooze_minutes));
  view.snooze_text = snz;
  view.dismiss_text = "Tắt";

  tessera::show_popup(
      this,
      [this, view]() {
        return new AlarmPopup(
            view,
            [this]() { snooze(0); },
            [this]() { dismiss(); },
            [this]() {
              // Accidental close (tap outside card, close X key, Back): NEVER stop the alarm.
              if (state_ == AlarmState::RINGING) {
                reopen_popup_ = true;
                ESP_LOGI(TAG, "Alarm popup closed externally while RINGING; scheduling reopen");
              }
            });
      },
      tessera::PopupWidth::ALERT);
}

void AlarmClock::close_popup_() {
  reopen_popup_ = false;
  tessera::close_popup(this);
}

void AlarmClock::stop_audio_and_release_() {
  stream_configured_ = false;
  sound_offset_ = 0;
  if (audio_claimed_) {
    audio_claimed_ = false;
    tessera::audio::release(tessera::audio::Side::BOTH, tessera::audio::Role::DOORBELL, ALARM_AUDIO_CLAIM_ID);
  }
}

void AlarmClock::snooze(int minutes) {
  if (state_ != AlarmState::RINGING) {
    ESP_LOGI(TAG, "Snooze ignored (state=%u)", static_cast<unsigned>(state_));
    return;
  }
  const int mins = (minutes >= 1 && minutes <= 30) ? minutes : static_cast<int>(store_.snooze_minutes);

  if (time_ != nullptr && time_->now().is_valid()) {
    snooze_until_epoch_ = static_cast<uint32_t>(time_->now().timestamp) + static_cast<uint32_t>(mins) * 60UL;
    state_ = AlarmState::SNOOZED;
    ESP_LOGI(TAG, "Alarm #%u SNOOZED for %d min (until epoch %u)",
             active_alarm_id_, mins, static_cast<unsigned>(snooze_until_epoch_));
  } else {
    state_ = AlarmState::IDLE;
    active_alarm_id_ = 0;
    snooze_until_epoch_ = 0;
    ESP_LOGW(TAG, "Snooze fallback to IDLE: clock invalid");
  }
  close_popup_();
  stop_audio_and_release_();
  tessera::wake(this);
  notify_changed_();
}

void AlarmClock::dismiss() {
  if (state_ == AlarmState::IDLE) return;
  const uint8_t id = active_alarm_id_;
  state_ = AlarmState::IDLE;
  active_alarm_id_ = 0;
  snooze_until_epoch_ = 0;
  close_popup_();
  stop_audio_and_release_();
  tessera::wake(this);

  AlarmEntry *alarm = find_by_id_(id);
  if (alarm != nullptr && alarm->repeat_days == 0 && alarm->enabled) {
    // One-shot alarm policy: disable after dismiss, keep record in NVS for future UI/HA sync.
    alarm->enabled = 0;
    const uint32_t now_ep = current_epoch_();
    if (now_ep != 0) alarm->updated_at = now_ep;
    save_store_();
    ESP_LOGI(TAG, "Alarm #%u DISMISSED (one-shot disabled)", id);
  } else {
    ESP_LOGI(TAG, "Alarm #%u DISMISSED (repeating kept enabled)", id);
  }
  notify_changed_();
}

void AlarmClock::loop() {
  if (state_ == AlarmState::RINGING) {
    if (reopen_popup_ && !tessera::popup_shown(this)) {
      reopen_popup_ = false;
      show_popup_();
    }
    feed_audio_(millis());
  }
}

void AlarmClock::feed_audio_(uint32_t now_ms) {
  if (speaker_ == nullptr || alarm_sound_.data == nullptr || alarm_sound_.length < 2) return;
  if (!stream_configured_) {
    speaker_->set_audio_stream_info(
        audio::AudioStreamInfo(16, alarm_sound_.channels, alarm_sound_.rate));
    stream_configured_ = true;
  }

  // Compute smooth software gain ramp: 25% -> alarm_volume_pct% over FADE_IN_MS.
  // Scaling PCM in software keeps ts_media_player.volume & saved_volume completely untouched!
  const uint32_t elapsed = now_ms - ring_started_ms_;
  const float max_gain = static_cast<float>(store_.alarm_volume_pct) / 100.0f;
  const float min_gain = std::min(0.25f, max_gain);
  const float progress = elapsed >= FADE_IN_MS ? 1.0f : static_cast<float>(elapsed) / static_cast<float>(FADE_IN_MS);
  const float gain = min_gain + (max_gain - min_gain) * progress;

  // Push scaled 16-bit PCM chunks non-blockingly (exact same loop contract as plugin_sound.h).
  int16_t scaled[256];
  constexpr size_t CHUNK_BYTES = sizeof(scaled);
  for (int iter = 0; iter < 4; ++iter) {
    if (sound_offset_ >= alarm_sound_.length) {
      sound_offset_ = 0;  // Seamless loop
    }
    size_t avail = alarm_sound_.length - sound_offset_;
    size_t to_send = std::min(avail, CHUNK_BYTES) & ~static_cast<size_t>(1);
    if (to_send == 0) {
      sound_offset_ = 0;
      continue;
    }
    const size_t samples = to_send / 2;
    const auto *src = reinterpret_cast<const int16_t *>(alarm_sound_.data + sound_offset_);
    for (size_t i = 0; i < samples; ++i) {
      int32_t v = static_cast<int32_t>(static_cast<float>(src[i]) * gain);
      if (v > 32767) v = 32767;
      else if (v < -32768) v = -32768;
      scaled[i] = static_cast<int16_t>(v);
    }
    const size_t taken = speaker_->play(reinterpret_cast<const uint8_t *>(scaled), to_send, 0);
    if (taken == 0) {
      break;
    }
    // Keep offset sample-aligned (even byte boundary).
    sound_offset_ += (taken & ~static_cast<size_t>(1));
    last_audio_push_ms_ = now_ms;
    if (taken < to_send) break;
  }
}

void AlarmClock::dump_to_log() const {
  int count = 0;
  for (const auto &a : store_.alarms) {
    if (a.id == 0) continue;
    ++count;
    ESP_LOGI(TAG, "  [#%u] %02u:%02u repeat=0x%02X enabled=%u last_min=%u updated=%u label='%s'",
             a.id, a.hour, a.minute, a.repeat_days, a.enabled,
             static_cast<unsigned>(a.last_fired_minute),
             static_cast<unsigned>(a.updated_at), a.label);
  }
  ESP_LOGI(TAG, "Total alarms: %d (state=%u, active=#%u, snooze=%umin, vol=%u%%)",
           count, static_cast<unsigned>(state_), active_alarm_id_,
           store_.snooze_minutes, store_.alarm_volume_pct);
}

}  // namespace esphome::alarm_clock
