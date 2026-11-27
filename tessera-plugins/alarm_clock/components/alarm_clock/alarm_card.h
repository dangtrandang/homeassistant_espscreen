#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "esphome/core/hal.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::alarm_clock {

class AlarmClock;
struct AlarmEntry;

// Snapshot item passed between AlarmClock and AlarmManagerCard.
struct AlarmItemView {
  uint8_t id{0};
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t repeat_days{0};
  bool enabled{false};
  std::string label;
};

struct AlarmCardOps {
  std::function<bool()> is_ringing;
  std::function<uint32_t()> revision;
  std::function<std::vector<AlarmItemView>()> list;
  std::function<int(int id, int hour, int minute, int repeat_days, bool enabled, const std::string &label)> upsert;
  std::function<bool(int id)> remove;
  std::function<bool(int id, bool enabled)> toggle;
  std::function<void(int &hour, int &minute)> default_time;
};

// Full-screen alarm management card opened from Settings -> Báo thức -> Quản lý báo thức.
// Supports List View (up to 10 alarms, inline toggle, add) and Editor View (hour/minute steppers,
// 7-day repeat selector, quick preset label switcher, save, and 2-tap confirmed delete).
class AlarmManagerCard : public tessera::Card {
 public:
  explicit AlarmManagerCard(AlarmCardOps ops) : ops_(std::move(ops)) {}

  void open(const tessera::CardContext &c) override {
    root_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    mode_ = Mode::LIST;
    last_rev_ = ops_.revision ? ops_.revision() : 0;
    render_();
  }

  void on_theme() override { render_(); }

  void on_tick(uint32_t) override {
    // Auto-refresh List View whenever alarm store changes (e.g., Popup Dismiss or HA Sync).
    if (mode_ == Mode::LIST && ops_.revision) {
      const uint32_t rev = ops_.revision();
      if (rev != last_rev_) {
        last_rev_ = rev;
        render_();
        return;
      }
    }
    // Expire 2-tap delete confirmation after 4 seconds.
    if (mode_ == Mode::EDIT && confirm_delete_ && (esphome::millis() - confirm_delete_ms_) > 4000) {
      confirm_delete_ = false;
      update_delete_btn_();
    }
  }

  bool on_back() override {
    if (mode_ == Mode::EDIT) {
      mode_ = Mode::LIST;
      confirm_delete_ = false;
      render_();
      return true;  // Stay inside card, return to list view.
    }
    return false;
  }

 private:
  enum class Mode : uint8_t { LIST, EDIT };

  static constexpr size_t PRESET_COUNT = 6;
  static const char *preset_label_(size_t i) {
    static constexpr std::array<const char *, PRESET_COUNT> kPresets = {
        "Báo thức", "Thức dậy", "Đi làm", "Uống thuốc", "Họp", "Tập thể dục",
    };
    return kPresets[i % PRESET_COUNT];
  }

  static std::string repeat_summary_(uint8_t mask) {
    if (mask == 0) return "1 lần";
    if ((mask & 0x7F) == 0x7F) return "Hàng ngày";
    if ((mask & 0x7F) == 0x3E) return "T2–T6";
    if ((mask & 0x7F) == 0x41) return "Cuối tuần";
    static constexpr const char *kDays[7] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};
    std::string out;
    for (int d = 1; d <= 7; ++d) {
      const int bit = d % 7;  // Mon..Sat, then Sun
      if (mask & (1u << bit)) {
        if (!out.empty()) out += " ";
        out += kDays[bit];
      }
    }
    return out;
  }

  bool busy_ringing_() const { return ops_.is_ringing && ops_.is_ringing(); }

  void clear_root_() {
    if (root_ != nullptr) lv_obj_clean(root_);
    // Reset dynamic widget pointers before rebuilding the active view.
    hour_lbl_ = minute_lbl_ = label_btn_lbl_ = repeat_info_lbl_ = nullptr;
    delete_btn_ = delete_lbl_ = nullptr;
    for (auto &b : day_btns_) b = nullptr;
    for (auto &l : day_lbls_) l = nullptr;
    row_actions_.clear();
  }

  void render_() {
    if (root_ == nullptr) return;
    clear_root_();
    if (mode_ == Mode::LIST) render_list_();
    else render_edit_();
  }

  // ---- LIST VIEW ----
  struct RowAction {
    AlarmManagerCard *self;
    uint8_t id;
    bool is_toggle;
  };

  static void on_row_click_(lv_event_t *e) {
    auto *act = static_cast<RowAction *>(lv_event_get_user_data(e));
    if (!act || !act->self || act->self->busy_ringing_()) return;
    if (act->is_toggle) {
      act->self->toggle_alarm_(act->id);
    } else {
      act->self->open_editor_(act->id);
    }
  }

  static void on_add_click_(lv_event_t *e) {
    auto *self = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!self || self->busy_ringing_()) return;
    self->open_editor_(0);
  }

  void toggle_alarm_(uint8_t id) {
    if (!ops_.list || !ops_.toggle) return;
    for (const auto &it : ops_.list()) {
      if (it.id == id) {
        ops_.toggle(id, !it.enabled);
        break;
      }
    }
    render_();
  }

  void open_editor_(uint8_t id) {
    confirm_delete_ = false;
    edit_id_ = id;
    if (id == 0) {
      int h = 7, m = 0;
      if (ops_.default_time) ops_.default_time(h, m);
      edit_hour_ = h;
      edit_minute_ = m;
      edit_repeat_ = 0;
      edit_enabled_ = true;
      edit_label_ = preset_label_(0);
    } else if (ops_.list) {
      for (const auto &it : ops_.list()) {
        if (it.id == id) {
          edit_hour_ = it.hour;
          edit_minute_ = it.minute;
          edit_repeat_ = it.repeat_days;
          edit_enabled_ = it.enabled;
          edit_label_ = it.label.empty() ? preset_label_(0) : it.label;
          break;
        }
      }
    }
    mode_ = Mode::EDIT;
    render_();
  }

  void render_list_() {
    namespace ui = tessera::ui;
    using tessera::Font;

    const auto items = ops_.list ? ops_.list() : std::vector<AlarmItemView>{};
    const bool large = ui::large();
    const int add_h = ui::px(large ? 44 : 36);
    const int gap = ui::px(large ? 10 : 6);
    const int list_h = std::max(1, height_ - add_h - gap);

    // Scrollable list container
    lv_obj_t *list = lv_obj_create(root_);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, width_, list_h);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);

    if (items.empty()) {
      lv_obj_t *empty = ui::label(list, Font::BODY_LARGE, theme::INK_SOFT);
      lv_obj_set_width(empty, width_);
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(empty);
      ui::set_text(empty, "Chưa có báo thức");
    } else {
      const int row_h = ui::px(large ? 58 : 46);
      const int row_gap = ui::px(large ? 8 : 6);
      const int tog_w = ui::px(large ? 64 : 52);
      const int tog_h = ui::px(large ? 36 : 30);
      const int pad_x = ui::px(large ? 12 : 8);
      row_actions_.reserve(items.size() * 2);

      int y = 0;
      for (const auto &it : items) {
        lv_obj_t *row = ui::block(list, theme::CARD);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_size(row, width_, row_h);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);

        // Tap left/body area to edit
        row_actions_.push_back({this, it.id, false});
        lv_obj_add_event_cb(row, on_row_click_, LV_EVENT_CLICKED, &row_actions_.back());

        const int text_w = std::max(1, width_ - tog_w - pad_x * 3);
        char hhmm[8];
        std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", it.hour, it.minute);
        lv_obj_t *t_lbl = ui::label(row, Font::TITLE, it.enabled ? theme::INK : theme::MUTED);
        lv_obj_set_pos(t_lbl, pad_x, ui::px(4));
        lv_obj_set_width(t_lbl, text_w);
        ui::set_text(t_lbl, hhmm);

        std::string sub = repeat_summary_(it.repeat_days);
        if (!it.label.empty()) {
          sub += " · ";
          sub += it.label;
        }
        lv_obj_t *s_lbl = ui::label(row, Font::BODY, theme::INK_SOFT);
        lv_obj_set_pos(s_lbl, pad_x, row_h - ui::line_height(Font::BODY) - ui::px(4));
        lv_obj_set_width(s_lbl, text_w);
        ui::set_text(s_lbl, sub);

        // Right inline toggle pill
        lv_obj_t *tog = ui::block(row, it.enabled ? theme::ACCENT : theme::TRACK);
        if (it.enabled) ui::set_fill(tog, tessera::Tone::ACCENT, theme::ACCENT);
        lv_obj_set_size(tog, tog_w, tog_h);
        lv_obj_set_pos(tog, width_ - tog_w - pad_x, (row_h - tog_h) / 2);
        lv_obj_set_style_radius(tog, tog_h / 2, 0);
        lv_obj_add_flag(tog, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(tog, LV_OBJ_FLAG_EVENT_BUBBLE);
        row_actions_.push_back({this, it.id, true});
        lv_obj_add_event_cb(tog, on_row_click_, LV_EVENT_CLICKED, &row_actions_.back());

        lv_obj_t *tog_lbl = ui::label(tog, Font::BODY, it.enabled ? theme::ON_ACCENT : theme::INK_SOFT);
        lv_obj_set_width(tog_lbl, tog_w);
        lv_obj_set_style_text_align(tog_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(tog_lbl);
        ui::set_text(tog_lbl, it.enabled ? "Bật" : "Tắt");

        y += row_h + row_gap;
      }
    }

    // Bottom "+ Thêm báo thức" button
    const bool full = items.size() >= 10;
    lv_obj_t *add_btn = ui::block(root_, full ? theme::TRACK : theme::ACCENT);
    if (!full) ui::set_fill(add_btn, tessera::Tone::ACCENT, theme::ACCENT);
    lv_obj_set_pos(add_btn, 0, height_ - add_h);
    lv_obj_set_size(add_btn, width_, add_h);
    if (!full) {
      lv_obj_add_flag(add_btn, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(add_btn, on_add_click_, LV_EVENT_CLICKED, this);
    }
    lv_obj_t *add_lbl = ui::label(add_btn, Font::BODY_LARGE, full ? theme::INK_SOFT : theme::ON_ACCENT);
    lv_obj_set_width(add_lbl, width_ - ui::px(8));
    lv_obj_set_style_text_align(add_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(add_lbl);
    ui::set_text(add_lbl, full ? "Đã đạt tối đa 10 báo thức" : "+ Thêm báo thức");
  }

  // ---- EDITOR VIEW ----
  static void on_hour_minus_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_()) return;
    s->edit_hour_ = (s->edit_hour_ + 23) % 24;
    s->update_time_labels_();
  }
  static void on_hour_plus_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_()) return;
    s->edit_hour_ = (s->edit_hour_ + 1) % 24;
    s->update_time_labels_();
  }
  static void on_min_minus_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_()) return;
    s->edit_minute_ = (s->edit_minute_ + 59) % 60;
    s->update_time_labels_();
  }
  static void on_min_plus_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_()) return;
    s->edit_minute_ = (s->edit_minute_ + 1) % 60;
    s->update_time_labels_();
  }
  static void on_label_cycle_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_()) return;
    size_t next = 0;
    for (size_t i = 0; i < PRESET_COUNT; ++i) {
      if (s->edit_label_ == preset_label_(i)) {
        next = (i + 1) % PRESET_COUNT;
        break;
      }
    }
    s->edit_label_ = preset_label_(next);
    if (s->label_btn_lbl_) tessera::ui::set_text(s->label_btn_lbl_, s->edit_label_);
  }
  static void on_day_click_(lv_event_t *e) {
    auto *act = static_cast<RowAction *>(lv_event_get_user_data(e));
    if (!act || !act->self || act->self->busy_ringing_()) return;
    act->self->edit_repeat_ ^= static_cast<uint8_t>(1u << act->id);
    act->self->update_day_buttons_();
  }
  static void on_save_click_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_() || !s->ops_.upsert) return;
    const int saved = s->ops_.upsert(s->edit_id_, s->edit_hour_, s->edit_minute_,
                                     s->edit_repeat_, s->edit_enabled_, s->edit_label_);
    if (saved > 0) {
      s->mode_ = Mode::LIST;
      s->confirm_delete_ = false;
      s->render_();
    }
  }
  static void on_cancel_click_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s) return;
    s->mode_ = Mode::LIST;
    s->confirm_delete_ = false;
    s->render_();
  }
  static void on_delete_click_(lv_event_t *e) {
    auto *s = static_cast<AlarmManagerCard *>(lv_event_get_user_data(e));
    if (!s || s->busy_ringing_() || s->edit_id_ == 0 || !s->ops_.remove) return;
    if (!s->confirm_delete_) {
      s->confirm_delete_ = true;
      s->confirm_delete_ms_ = esphome::millis();
      s->update_delete_btn_();
      return;
    }
    s->ops_.remove(s->edit_id_);
    s->mode_ = Mode::LIST;
    s->confirm_delete_ = false;
    s->render_();
  }

  lv_obj_t *make_key_(lv_obj_t *parent, int x, int y, int w, int h, const char *txt,
                      tessera::Font font, lv_event_cb_t cb, void *ud) {
    namespace ui = tessera::ui;
    lv_obj_t *b = ui::block(parent, theme::KEY);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ui::label(b, font, theme::INK);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    ui::set_text(l, txt);
    return b;
  }

  void render_edit_() {
    namespace ui = tessera::ui;
    using tessera::Font;

    const bool large = ui::large();
    const bool wide_row = width_ >= ui::px(250);
    const int gap = ui::px(large ? 8 : 5);
    const int time_h = ui::px(large ? 48 : 38);
    const int key_w = ui::px(large ? 36 : 28);
    const int num_w = ui::px(large ? 44 : 34);
    const int colon_w = ui::px(10);

    // Row 1: Hour stepper + ':' + Minute stepper (+ Label preset on right if landscape)
    int x = 0;
    make_key_(root_, x, 0, key_w, time_h, "-", Font::TITLE, on_hour_minus_, this);
    x += key_w + 2;
    hour_lbl_ = ui::label(root_, Font::HEADLINE, theme::INK);
    lv_obj_set_size(hour_lbl_, num_w, time_h);
    lv_obj_set_pos(hour_lbl_, x, (time_h - ui::line_height(Font::HEADLINE)) / 2);
    lv_obj_set_style_text_align(hour_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    x += num_w + 2;
    make_key_(root_, x, 0, key_w, time_h, "+", Font::TITLE, on_hour_plus_, this);
    x += key_w + 2;

    lv_obj_t *colon = ui::label(root_, Font::HEADLINE, theme::INK_SOFT);
    lv_obj_set_size(colon, colon_w, time_h);
    lv_obj_set_pos(colon, x, (time_h - ui::line_height(Font::HEADLINE)) / 2);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(colon, ":");
    x += colon_w + 2;

    make_key_(root_, x, 0, key_w, time_h, "-", Font::TITLE, on_min_minus_, this);
    x += key_w + 2;
    minute_lbl_ = ui::label(root_, Font::HEADLINE, theme::INK);
    lv_obj_set_size(minute_lbl_, num_w, time_h);
    lv_obj_set_pos(minute_lbl_, x, (time_h - ui::line_height(Font::HEADLINE)) / 2);
    lv_obj_set_style_text_align(minute_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    x += num_w + 2;
    make_key_(root_, x, 0, key_w, time_h, "+", Font::TITLE, on_min_plus_, this);
    x += key_w + gap;

    int next_y = time_h + gap;
    // Label preset selector
    const int lbl_x = wide_row ? x : 0;
    const int lbl_y = wide_row ? 0 : next_y;
    const int lbl_w = wide_row ? std::max(ui::px(60), width_ - x) : width_;
    const int lbl_h = wide_row ? time_h : ui::px(34);
    lv_obj_t *lbl_btn = ui::block(root_, theme::KEY);
    lv_obj_set_pos(lbl_btn, lbl_x, lbl_y);
    lv_obj_set_size(lbl_btn, lbl_w, lbl_h);
    lv_obj_add_flag(lbl_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_btn, on_label_cycle_, LV_EVENT_CLICKED, this);
    label_btn_lbl_ = ui::label(lbl_btn, Font::BODY, theme::INK);
    lv_obj_set_width(label_btn_lbl_, std::max(1, lbl_w - ui::px(8)));
    lv_obj_set_style_text_align(label_btn_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label_btn_lbl_);
    ui::set_text(label_btn_lbl_, edit_label_);
    if (!wide_row) next_y += lbl_h + gap;

    // Row 2: 7-day repeat selector (T2..T7, CN)
    const int day_h = ui::px(large ? 40 : 34);
    const int day_gap = ui::px(4);
    const int day_w = std::max(1, (width_ - day_gap * 6) / 7);
    static constexpr const char *kDayTxt[7] = {"T2", "T3", "T4", "T5", "T6", "T7", "CN"};
    static constexpr uint8_t kDayBit[7] = {1, 2, 3, 4, 5, 6, 0};  // bit 0 = Sun, 1 = Mon .. 6 = Sat
    row_actions_.reserve(7);
    for (int i = 0; i < 7; ++i) {
      const int dx = i * (day_w + day_gap);
      const int dw = (i == 6) ? (width_ - dx) : day_w;
      lv_obj_t *db = ui::block(root_, theme::KEY);
      lv_obj_set_pos(db, dx, next_y);
      lv_obj_set_size(db, dw, day_h);
      lv_obj_add_flag(db, LV_OBJ_FLAG_CLICKABLE);
      row_actions_.push_back({this, kDayBit[i], false});
      lv_obj_add_event_cb(db, on_day_click_, LV_EVENT_CLICKED, &row_actions_.back());
      lv_obj_t *dl = ui::label(db, Font::BODY, theme::INK_SOFT);
      lv_obj_set_width(dl, dw);
      lv_obj_set_style_text_align(dl, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(dl);
      ui::set_text(dl, kDayTxt[i]);
      day_btns_[kDayBit[i]] = db;
      day_lbls_[kDayBit[i]] = dl;
    }
    next_y += day_h + gap;

    // Row 3: Repeat summary text
    repeat_info_lbl_ = ui::label(root_, Font::BODY, theme::INK_SOFT);
    lv_obj_set_pos(repeat_info_lbl_, 0, next_y);
    lv_obj_set_width(repeat_info_lbl_, width_);

    // Row 4: Bottom action buttons (Delete/Cancel | Save)
    const int act_h = ui::px(large ? 46 : 38);
    const int act_y = std::max(next_y + ui::line_height(Font::BODY) + gap, height_ - act_h);
    const int act_gap = ui::px(8);
    const int left_w = (width_ - act_gap) * 44 / 100;
    const int right_x = left_w + act_gap;
    const int right_w = width_ - right_x;

    if (edit_id_ > 0) {
      delete_btn_ = ui::block(root_, theme::KEY);
      lv_obj_set_pos(delete_btn_, 0, act_y);
      lv_obj_set_size(delete_btn_, left_w, act_h);
      lv_obj_add_flag(delete_btn_, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(delete_btn_, on_delete_click_, LV_EVENT_CLICKED, this);
      delete_lbl_ = ui::label(delete_btn_, Font::BODY_LARGE, theme::INK);
      lv_obj_set_width(delete_lbl_, std::max(1, left_w - ui::px(6)));
      lv_obj_set_style_text_align(delete_lbl_, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(delete_lbl_);
      update_delete_btn_();
    } else {
      make_key_(root_, 0, act_y, left_w, act_h, "Hủy", Font::BODY_LARGE, on_cancel_click_, this);
    }

    lv_obj_t *save_btn = ui::block(root_, theme::ACCENT);
    ui::set_fill(save_btn, tessera::Tone::ACCENT, theme::ACCENT);
    lv_obj_set_pos(save_btn, right_x, act_y);
    lv_obj_set_size(save_btn, right_w, act_h);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, on_save_click_, LV_EVENT_CLICKED, this);
    lv_obj_t *save_lbl = ui::label(save_btn, Font::BODY_LARGE, theme::ON_ACCENT);
    lv_obj_set_width(save_lbl, std::max(1, right_w - ui::px(6)));
    lv_obj_set_style_text_align(save_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(save_lbl);
    ui::set_text(save_lbl, "Lưu");

    update_time_labels_();
    update_day_buttons_();
  }

  void update_time_labels_() {
    char buf[4];
    if (hour_lbl_) {
      std::snprintf(buf, sizeof(buf), "%02d", edit_hour_);
      tessera::ui::set_text(hour_lbl_, buf);
    }
    if (minute_lbl_) {
      std::snprintf(buf, sizeof(buf), "%02d", edit_minute_);
      tessera::ui::set_text(minute_lbl_, buf);
    }
  }

  void update_day_buttons_() {
    namespace ui = tessera::ui;
    for (int bit = 0; bit < 7; ++bit) {
      if (!day_btns_[bit] || !day_lbls_[bit]) continue;
      const bool on = (edit_repeat_ & (1u << bit)) != 0;
      if (on) {
        ui::set_fill(day_btns_[bit], tessera::Tone::ACCENT, theme::ACCENT);
        ui::set_color(day_lbls_[bit], theme::ON_ACCENT);
      } else {
        lv_obj_set_style_bg_color(day_btns_[bit], ui::color(theme::KEY), 0);
        ui::set_color(day_lbls_[bit], theme::INK_SOFT);
      }
    }
    if (repeat_info_lbl_) {
      tessera::ui::set_text(repeat_info_lbl_, "Lặp: " + repeat_summary_(edit_repeat_));
    }
  }

  void update_delete_btn_() {
    namespace ui = tessera::ui;
    if (!delete_btn_ || !delete_lbl_) return;
    if (confirm_delete_) {
      ui::set_fill(delete_btn_, tessera::Tone::ALERT, theme::ACCENT);
      ui::set_color(delete_lbl_, theme::ON_ACCENT);
      ui::set_text(delete_lbl_, "Chạm lại?");
    } else {
      lv_obj_set_style_bg_color(delete_btn_, ui::color(theme::KEY), 0);
      ui::set_tone(delete_lbl_, tessera::Tone::ALERT, theme::INK);
      ui::set_text(delete_lbl_, "Xóa");
    }
  }

  AlarmCardOps ops_;
  lv_obj_t *root_{nullptr};
  int width_{0}, height_{0};
  Mode mode_{Mode::LIST};
  uint32_t last_rev_{0};

  uint8_t edit_id_{0};
  int edit_hour_{7}, edit_minute_{0};
  uint8_t edit_repeat_{0};
  bool edit_enabled_{true};
  std::string edit_label_;
  bool confirm_delete_{false};
  uint32_t confirm_delete_ms_{0};

  lv_obj_t *hour_lbl_{nullptr}, *minute_lbl_{nullptr}, *label_btn_lbl_{nullptr}, *repeat_info_lbl_{nullptr};
  lv_obj_t *delete_btn_{nullptr}, *delete_lbl_{nullptr};
  std::array<lv_obj_t *, 7> day_btns_{};
  std::array<lv_obj_t *, 7> day_lbls_{};
  std::vector<RowAction> row_actions_;
};

}  // namespace esphome::alarm_clock
