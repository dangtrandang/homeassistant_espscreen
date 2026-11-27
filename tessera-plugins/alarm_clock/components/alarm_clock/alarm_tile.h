#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::alarm_clock {

struct AlarmTileEntry {
  uint8_t hour{0};
  uint8_t minute{0};
  uint8_t repeat_days{0};
  std::string label;
  bool is_snooze{false};
};

struct AlarmTileState {
  bool ringing{false};
  bool snoozed{false};
  std::vector<AlarmTileEntry> upcoming;  // Sorted nearest-first (max ~10), snooze first if snoozed.
};

struct AlarmTileOps {
  std::function<bool()> is_ringing;
  std::function<uint32_t()> revision;
  std::function<AlarmTileState()> snapshot;
  std::function<void(int tile_index)> open_manager;
};

// Adaptive local-first Alarm Clock tile (plugin:alarm_clock.alarms).
// Reads directly from AlarmClock NVS/Engine (100% offline).
// Supports 1x1 (next time + count), 2x1 (next time + repeat + label), and 2x2+ (up to 4 upcoming alarms).
// Tapping anywhere on the tile opens AlarmManagerCard directly.
class AlarmTile : public tessera::Tile {
 public:
  explicit AlarmTile(AlarmTileOps ops) : ops_(std::move(ops)) {}

  void create(const tessera::TileContext &c) override {
    parent_ = c.parent;
    width_ = c.width;
    height_ = c.height;
    cols_ = c.columns;
    rows_ = c.rows;
    tile_index_ = c.tile;
    last_rev_ = UINT32_MAX;
    last_minute_ = UINT32_MAX;
    render_();
  }

  void on_tick(uint32_t epoch) override {
    const uint32_t rev = ops_.revision ? ops_.revision() : 0;
    const uint32_t cur_min = epoch / 60UL;
    if (rev != last_rev_ || cur_min != last_minute_) {
      last_rev_ = rev;
      last_minute_ = cur_min;
      render_();
    }
  }

  void on_theme() override { render_(); }

  void on_tap() override {
    if (ops_.is_ringing && ops_.is_ringing()) return;
    if (ops_.open_manager) ops_.open_manager(tile_index_);
  }

 private:
  static constexpr uint32_t ICON_ALARM = 0xF0020;
  static constexpr uint32_t ICON_BELL_RING = 0xF009E;

  static std::string repeat_short_(uint8_t mask) {
    if (mask == 0) return "1 lần";
    if ((mask & 0x7F) == 0x7F) return "Hàng ngày";
    if ((mask & 0x7F) == 0x3E) return "T2–T6";
    if ((mask & 0x7F) == 0x7E) return "T2–T7";
    if ((mask & 0x7F) == 0x41) return "Cuối tuần";
    static constexpr const char *kDays[7] = {"CN", "T2", "T3", "T4", "T5", "T6", "T7"};
    std::string out;
    for (int d = 1; d <= 7; ++d) {
      const int bit = d % 7;
      if (mask & (1u << bit)) {
        if (!out.empty()) out += " ";
        out += kDays[bit];
      }
    }
    return out;
  }

  static tessera::Tone state_tone_(const AlarmTileState &st) {
    if (st.ringing) return tessera::Tone::ALERT;
    if (st.snoozed) return tessera::Tone::BUSY;
    if (!st.upcoming.empty()) return tessera::Tone::ACCENT;
    return tessera::Tone::NORMAL;
  }

  void render_() {
    if (parent_ == nullptr) return;
    lv_obj_clean(parent_);
    const AlarmTileState st = ops_.snapshot ? ops_.snapshot() : AlarmTileState{};
    if (rows_ >= 2) {
      render_list_(st);
    } else if (cols_ >= 2 && width_ > height_ * 3 / 2) {
      render_wide_(st);
    } else {
      render_compact_(st);
    }
  }

  // 1x1: Icon + next alarm time + remaining count / label.
  void render_compact_(const AlarmTileState &st) {
    namespace ui = tessera::ui;
    using tessera::Font;

    const tessera::Tone tone = state_tone_(st);
    const uint32_t glyph = st.ringing ? ICON_BELL_RING : ICON_ALARM;
    const int icon_h = ui::line_height(Font::ICON);

    std::string main_txt = "Không có";
    std::string sub_txt = "Chạm để đặt";
    if (!st.upcoming.empty()) {
      const auto &first = st.upcoming[0];
      char hhmm[8];
      std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", first.hour, first.minute);
      main_txt = hhmm;
      if (first.is_snooze) {
        sub_txt = "Báo lại";
      } else if (st.upcoming.size() > 1) {
        char more[24];
        std::snprintf(more, sizeof(more), "+%u báo thức", static_cast<unsigned>(st.upcoming.size() - 1));
        sub_txt = more;
      } else {
        sub_txt = !first.label.empty() ? first.label : repeat_short_(first.repeat_days);
      }
    }

    Font main_font = Font::HEADLINE;
    for (Font f : {Font::HEADLINE, Font::TITLE, Font::BODY_LARGE, Font::BODY}) {
      main_font = f;
      if (ui::text_width(main_txt, f) <= width_) break;
    }
    const int main_h = ui::line_height(main_font);
    const int sub_h = ui::line_height(Font::BODY);
    const int total_h = icon_h + main_h + sub_h + ui::px(4);
    const int top = std::max(0, (height_ - total_h) / 2);

    lv_obj_t *ic = ui::label(parent_, Font::ICON, theme::MUTED);
    lv_obj_set_pos(ic, 0, top);
    lv_obj_set_size(ic, width_, icon_h);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(ic, ui::icon(glyph));
    ui::set_tone(ic, tone, theme::MUTED);

    lv_obj_t *ml = ui::label(parent_, main_font, st.upcoming.empty() ? theme::MUTED : theme::INK);
    lv_obj_set_pos(ml, 0, top + icon_h + ui::px(2));
    lv_obj_set_size(ml, width_, main_h);
    lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(ml, main_txt);

    lv_obj_t *sl = ui::label(parent_, Font::BODY, theme::INK_SOFT);
    lv_obj_set_pos(sl, 0, top + icon_h + main_h + ui::px(4));
    lv_obj_set_size(sl, width_, sub_h);
    lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(sl, sub_txt);
  }

  // 2x1: Left icon + Right next time, repeat schedule, and label.
  void render_wide_(const AlarmTileState &st) {
    namespace ui = tessera::ui;
    using tessera::Font;

    const tessera::Tone tone = state_tone_(st);
    const uint32_t glyph = st.ringing ? ICON_BELL_RING : ICON_ALARM;
    const int icon_h = ui::line_height(Font::ICON);
    const int gap = ui::px(10);
    const int text_x = icon_h + gap;
    const int text_w = std::max(1, width_ - text_x);

    lv_obj_t *ic = ui::label(parent_, Font::ICON, theme::MUTED);
    lv_obj_set_pos(ic, 0, (height_ - icon_h) / 2);
    lv_obj_set_size(ic, icon_h, icon_h);
    lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_text(ic, ui::icon(glyph));
    ui::set_tone(ic, tone, theme::MUTED);

    std::string main_txt = "Không có báo thức";
    std::string line2 = "Chạm để thêm";
    if (!st.upcoming.empty()) {
      const auto &first = st.upcoming[0];
      char hhmm[8];
      std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", first.hour, first.minute);
      main_txt = hhmm;
      if (first.is_snooze) {
        line2 = "Đang báo lại";
      } else {
        line2 = repeat_short_(first.repeat_days);
        if (!first.label.empty()) {
          line2 += " · ";
          line2 += first.label;
        }
      }
      if (st.upcoming.size() > 1) {
        char more[16];
        std::snprintf(more, sizeof(more), " (+%u)", static_cast<unsigned>(st.upcoming.size() - 1));
        line2 += more;
      }
    }

    Font main_font = Font::HEADLINE;
    for (Font f : {Font::HEADLINE, Font::TITLE, Font::BODY_LARGE}) {
      main_font = f;
      if (ui::text_width(main_txt, f) <= text_w) break;
    }
    const int main_h = ui::line_height(main_font);
    const int sub_h = ui::line_height(Font::BODY);
    const int top = std::max(0, (height_ - main_h - sub_h - ui::px(4)) / 2);

    lv_obj_t *ml = ui::label(parent_, main_font, st.upcoming.empty() ? theme::MUTED : theme::INK);
    lv_obj_set_pos(ml, text_x, top);
    lv_obj_set_size(ml, text_w, main_h);
    ui::set_text(ml, main_txt);

    lv_obj_t *sl = ui::label(parent_, Font::BODY, theme::INK_SOFT);
    lv_obj_set_pos(sl, text_x, top + main_h + ui::px(4));
    lv_obj_set_size(sl, text_w, sub_h);
    ui::set_text(sl, line2);
  }

  // 2x2+: Header + list of up to 4 nearest upcoming enabled alarms.
  void render_list_(const AlarmTileState &st) {
    namespace ui = tessera::ui;
    using tessera::Font;

    const tessera::Tone tone = state_tone_(st);
    const uint32_t glyph = st.ringing ? ICON_BELL_RING : ICON_ALARM;
    const int hdr_h = std::max(ui::line_height(Font::ICON_SMALL), ui::line_height(Font::TITLE));
    const int ic_w = ui::line_height(Font::ICON_SMALL);

    lv_obj_t *ic = ui::label(parent_, Font::ICON_SMALL, theme::MUTED);
    lv_obj_set_pos(ic, 0, (hdr_h - ic_w) / 2);
    lv_obj_set_size(ic, ic_w, ic_w);
    ui::set_text(ic, ui::icon(glyph));
    ui::set_tone(ic, tone, theme::MUTED);

    char hdr_txt[40];
    if (st.upcoming.empty()) {
      std::snprintf(hdr_txt, sizeof(hdr_txt), "Báo thức");
    } else if (st.snoozed) {
      std::snprintf(hdr_txt, sizeof(hdr_txt), "Báo lại · %u bật", static_cast<unsigned>(st.upcoming.size()));
    } else {
      std::snprintf(hdr_txt, sizeof(hdr_txt), "Báo thức · %u bật", static_cast<unsigned>(st.upcoming.size()));
    }
    lv_obj_t *hl = ui::label(parent_, Font::TITLE, theme::INK);
    lv_obj_set_pos(hl, ic_w + ui::px(6), (hdr_h - ui::line_height(Font::TITLE)) / 2);
    lv_obj_set_width(hl, std::max(1, width_ - ic_w - ui::px(6)));
    ui::set_text(hl, hdr_txt);

    const int body_top = hdr_h + ui::px(6);
    const int avail_h = std::max(1, height_ - body_top);

    if (st.upcoming.empty()) {
      lv_obj_t *el = ui::label(parent_, Font::BODY_LARGE, theme::INK_SOFT);
      lv_obj_set_width(el, width_);
      lv_obj_set_style_text_align(el, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(el, 0, body_top + std::max(0, (avail_h - ui::line_height(Font::BODY_LARGE)) / 2));
      ui::set_text(el, "Chạm để đặt báo thức");
      return;
    }

    const int row_h = std::max(ui::line_height(Font::BODY_LARGE), ui::px(22)) + ui::px(4);
    const size_t max_rows = std::min<size_t>(4, std::max<size_t>(1, static_cast<size_t>(avail_h / row_h)));
    const size_t count = std::min(st.upcoming.size(), max_rows);
    const int time_w = ui::text_width("00:00", Font::BODY_LARGE) + ui::px(8);

    int y = body_top;
    for (size_t i = 0; i < count; ++i) {
      const auto &a = st.upcoming[i];
      char hhmm[8];
      std::snprintf(hhmm, sizeof(hhmm), "%02u:%02u", a.hour, a.minute);
      lv_obj_t *tl = ui::label(parent_, Font::BODY_LARGE, theme::INK);
      lv_obj_set_pos(tl, 0, y);
      lv_obj_set_width(tl, time_w);
      if (a.is_snooze) ui::set_tone(tl, tessera::Tone::BUSY, theme::INK);
      ui::set_text(tl, hhmm);

      std::string desc = a.is_snooze ? std::string("Báo lại") : repeat_short_(a.repeat_days);
      if (!a.label.empty()) {
        desc += " · ";
        desc += a.label;
      }
      lv_obj_t *dl = ui::label(parent_, Font::BODY, theme::INK_SOFT);
      lv_obj_set_pos(dl, time_w, y + (ui::line_height(Font::BODY_LARGE) - ui::line_height(Font::BODY)) / 2);
      lv_obj_set_width(dl, std::max(1, width_ - time_w));
      ui::set_text(dl, desc);

      y += row_h;
    }
  }

  AlarmTileOps ops_;
  lv_obj_t *parent_{nullptr};
  int width_{0}, height_{0}, tile_index_{-1};
  uint8_t cols_{1}, rows_{1};
  uint32_t last_rev_{UINT32_MAX};
  uint32_t last_minute_{UINT32_MAX};
};

}  // namespace esphome::alarm_clock
