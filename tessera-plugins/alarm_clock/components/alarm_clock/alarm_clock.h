#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/smart_display/plugin_api.h"
#include "esphome/components/smart_display/plugin_sound.h"
#include "alarm_card.h"
#include "alarm_popup.h"

namespace esphome::alarm_clock {

static constexpr size_t MAX_ALARMS = 10;
static constexpr size_t MAX_LABEL_LEN = 32;
// Unique NVS preference key ("CLK1"), distinct from alarm_panel's "ALM1" (0x414C4D31).
static constexpr uint32_t ALARM_NVS_HASH = 0x434C4B31UL;
static constexpr uint32_t ALARM_STORE_MAGIC = 0x434C4B31UL;
static constexpr uint32_t ALARM_STORE_VERSION = 1;

// Unique audio_owner claim id under Role::DOORBELL so it never collides with real doorbell (id=0).
static constexpr uint32_t ALARM_AUDIO_CLAIM_ID = 100;

// Missed-alarm catch-up window: fire catch-up if <= 120s late, otherwise mark missed.
static constexpr uint32_t MISSED_CATCHUP_SEC = 120;
// Safety auto-stop after 10 minutes of continuous ringing.
static constexpr uint32_t MAX_RING_MS = 10UL * 60UL * 1000UL;
// Smooth software gain ramp duration (12 seconds, from 25% up to alarm_volume_pct).
static constexpr uint32_t FADE_IN_MS = 12000UL;

enum class AlarmState : uint8_t {
  IDLE = 0,
  RINGING = 1,
  SNOOZED = 2,
};

// Fixed POD record stored directly in NVS flash.
// repeat_days bitmask: bit 0 = Sun, bit 1 = Mon, ..., bit 6 = Sat. 0 = one-shot, 0x7F = daily.
struct AlarmEntry {
  uint8_t id{0};                  // 1..MAX_ALARMS, 0 = empty slot
  uint8_t hour{0};                // 0..23
  uint8_t minute{0};              // 0..59
  uint8_t repeat_days{0};         // bitmask (0 = one-shot)
  uint8_t enabled{0};             // 0 or 1
  uint8_t deleted{0};             // 0 = active, 1 = LWW tombstone (uses former reserved[0], NVS-compatible!)
  uint8_t reserved[2]{0, 0};
  uint32_t updated_at{0};         // epoch seconds when last modified
  uint32_t last_fired_minute{0};  // epoch / 60 of last trigger to prevent duplicate fire
  char label[MAX_LABEL_LEN]{};
};

struct AlarmStore {
  uint32_t magic{ALARM_STORE_MAGIC};
  uint32_t version{ALARM_STORE_VERSION};
  uint8_t snooze_minutes{5};
  uint8_t alarm_volume_pct{80};   // software gain ceiling (10..100%), never mutates saved_volume
  uint8_t reserved[2]{0, 0};
  AlarmEntry alarms[MAX_ALARMS]{};
};

class AlarmClock : public Component, public tessera::Plugin {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void on_ready() override;
  void on_interval(uint32_t now_ms) override;
  bool settings(tessera::SettingsPage &page) override;
  void before_update() override;

  void set_time(time::RealTimeClock *t) { time_ = t; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }
  void set_alarm_sound(tessera::Sound s) { alarm_sound_ = s; }

  // Alarm management API (local-first, persists immediately to NVS).
  // Pass id == 0 to auto-allocate a free slot; returns allocated/updated id or -1 on error.
  int upsert_alarm(int id, int hour, int minute, int repeat_days, bool enabled,
                   const std::string &label = "", uint32_t updated_at = 0);
  bool delete_alarm(int id, uint32_t updated_at = 0);
  bool set_enabled(int id, bool enabled, uint32_t updated_at = 0);
  // Convenience helper for testing: creates a one-shot alarm offset_minutes from now.
  int set_in_minutes(int offset_minutes, const std::string &label = "Báo thức thử");

  // Stage 4 HA Sync API (per-alarm_id LWW + NVS write + ACK event + snapshot publish).
  void ha_upsert(const std::string &req_id, int id, int hour, int minute, int repeat_days,
                 bool enabled, const std::string &label, uint32_t updated_at = 0);
  void ha_delete(const std::string &req_id, int id, uint32_t updated_at = 0);
  void ha_toggle(const std::string &req_id, int id, bool enabled, uint32_t updated_at = 0);
  void publish_snapshot();
  void on_api_connected();

  // Callbacks wired from plugin.yaml to emit Home Assistant events & update status sensor.
  using AckCallback = std::function<void(const std::string &req_id, int alarm_id, bool ok,
                                         const std::string &error, uint32_t updated_at,
                                         const std::string &saved_alarm_json)>;
  using SnapshotCallback = std::function<void(const std::string &snapshot_json, const std::string &status_text)>;
  void set_ack_callback(AckCallback cb) { ack_cb_ = std::move(cb); }
  void set_snapshot_callback(SnapshotCallback cb) { snapshot_cb_ = std::move(cb); }

  // Runtime ring controls (100% local, never depends on HA).
  void snooze(int minutes = 0);
  void dismiss();

  // Settings helpers (persist into existing AlarmStore NVS record).
  void set_snooze_minutes(int minutes);
  void set_alarm_volume_pct(int pct);

  // State inspection (used by standby hook, popup, card auto-refresh, and HA sync).
  AlarmState state() const { return state_; }
  bool is_ringing() const { return state_ == AlarmState::RINGING; }
  uint8_t active_alarm_id() const { return active_alarm_id_; }
  uint32_t store_revision() const { return store_rev_; }
  const AlarmStore &store() const { return store_; }
  std::string build_snapshot_json() const;
  std::string build_status_text() const;
  void dump_to_log() const;

 protected:
  void load_store_();
  bool save_store_();
  void notify_changed_();
  void emit_ack_(const std::string &req_id, int alarm_id, bool ok, const std::string &error,
                 const AlarmEntry *entry) const;
  static std::string serialize_alarm_json_(const AlarmEntry &a);
  static std::string escape_json_(const char *s);
  uint32_t current_epoch_() const;
  void check_schedule_();
  void start_ringing_(AlarmEntry &alarm, uint32_t fire_minute, bool from_snooze);
  void stop_audio_and_release_();
  void show_popup_();
  void close_popup_();
  tessera::BarItem bar_item_next_() const;
  void feed_audio_(uint32_t now_ms);
  AlarmEntry *find_by_id_(uint8_t id, bool include_deleted = false);
  const AlarmEntry *find_by_id_(uint8_t id, bool include_deleted = false) const;
  uint8_t allocate_id_() const;
  static bool matches_weekday_(uint8_t repeat_days, uint8_t esphome_day_of_week);

  time::RealTimeClock *time_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  tessera::Sound alarm_sound_{};

  ESPPreferenceObject pref_;
  AlarmStore store_{};
  uint32_t store_rev_{1};
  AckCallback ack_cb_;
  SnapshotCallback snapshot_cb_;

  AlarmState state_{AlarmState::IDLE};
  uint8_t active_alarm_id_{0};
  uint32_t ring_started_ms_{0};
  uint32_t snooze_until_epoch_{0};
  uint32_t last_check_epoch_{0};
  bool time_was_valid_{false};

  // Ringing popup lifecycle (Stage 2).
  bool reopen_popup_{false};

  // Looping + software-gain audio playback state.
  bool audio_claimed_{false};
  bool stream_configured_{false};
  size_t sound_offset_{0};
  uint32_t last_audio_push_ms_{0};
};

}  // namespace esphome::alarm_clock
