#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::alarm_clock {

// MDI bell-ring glyph (0xF009E), confirmed present in Tessera's built-in icon font.
static constexpr uint32_t ICON_BELL_RING = 0xF009E;

struct AlarmPopupView {
  std::string time_text;    // e.g. "06:30"
  std::string label_text;   // e.g. "Đi làm" or "Báo thức"
  std::string snooze_text;  // e.g. "Báo lại 5p"
  std::string dismiss_text; // e.g. "Tắt"
};

// Ringing alarm popup on lv_layer_top() (plugin API 0.18).
// Shows alarm time, alarm label, and two large touch buttons: Snooze and Dismiss.
// Taps outside the two buttons do nothing; accidental close (X key / tap outside card)
// triggers `on_closed` which lets AlarmClock immediately reopen the popup while RINGING.
class AlarmPopup : public tessera::Popup {
 public:
  AlarmPopup(AlarmPopupView view, std::function<void()> on_snooze, std::function<void()> on_dismiss_btn,
             std::function<void()> on_closed)
      : view_(std::move(view)),
        on_snooze_(std::move(on_snooze)),
        on_dismiss_btn_(std::move(on_dismiss_btn)),
        on_closed_(std::move(on_closed)) {}

  void open(const tessera::PopupContext &c) override {
    namespace ui = tessera::ui;
    using tessera::Font;

    width_ = c.width;
    const bool large = ui::large();
    const int gap = ui::px(large ? 14 : 10);
    const int disc_sz = c.key > 0 ? c.key : ui::px(large ? 52 : 36);

    // 1. Header row: pulsing red bell disc + large alarm time (HH:MM)
    disc_ = lv_obj_create(c.parent);
    lv_obj_remove_style_all(disc_);
    lv_obj_remove_flag(disc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(disc_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(disc_, disc_sz, disc_sz);
    lv_obj_set_pos(disc_, 0, 0);
    lv_obj_set_style_radius(disc_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(disc_, LV_OPA_COVER, 0);
    ui::set_fill(disc_, tessera::Tone::ALERT, theme::ACCENT);
    ui::animate(disc_, tessera::Motion::PULSE, 1100);

    icon_ = ui::label(disc_, Font::ICON_SMALL, theme::ON_ACCENT);
    lv_obj_set_width(icon_, disc_sz);
    lv_obj_set_style_text_align(icon_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(icon_);
    ui::set_text(icon_, ui::icon(ICON_BELL_RING));

    const int time_x = disc_sz + gap;
    const int time_w = std::max(1, width_ - time_x - c.corner);
    Font time_font = Font::HEADLINE;
    if (ui::text_width(view_.time_text, Font::VALUE) <= time_w && ui::line_height(Font::VALUE) <= disc_sz + ui::px(8)) {
      time_font = Font::VALUE;
    }
    time_lbl_ = ui::label(c.parent, time_font, theme::INK);
    lv_obj_set_width(time_lbl_, time_w);
    const int time_h = ui::line_height(time_font);
    lv_obj_set_pos(time_lbl_, time_x, std::max(0, (disc_sz - time_h) / 2));
    ui::set_text(time_lbl_, view_.time_text);

    // 2. Label row: alarm name
    const int label_top = std::max(disc_sz, time_h) + gap;
    const std::string &lbl = view_.label_text.empty() ? std::string("Báo thức") : view_.label_text;
    Font lbl_font = Font::TITLE;
    if (ui::text_width(lbl, Font::TITLE) > width_) lbl_font = Font::BODY_LARGE;
    label_lbl_ = ui::label(c.parent, lbl_font, theme::INK_SOFT);
    lv_obj_set_width(label_lbl_, width_);
    lv_obj_set_pos(label_lbl_, 0, label_top);
    ui::set_text(label_lbl_, lbl);

    // 3. Action buttons row: two large touch targets side-by-side (Snooze | Dismiss)
    const int btn_top = label_top + ui::line_height(lbl_font) + ui::px(large ? 18 : 14);
    const int btn_h = std::max(ui::px(large ? 56 : 46), ui::mm(8));
    const int btn_gap = ui::px(large ? 12 : 10);
    const int btn_w = std::max(1, (width_ - btn_gap) / 2);

    // Left button: Snooze
    snooze_box_ = ui::block(c.parent, theme::KEY);
    lv_obj_set_pos(snooze_box_, 0, btn_top);
    lv_obj_set_size(snooze_box_, btn_w, btn_h);
    snooze_lbl_ = ui::label(snooze_box_, Font::BODY_LARGE, theme::INK);
    lv_obj_set_width(snooze_lbl_, btn_w - ui::px(8));
    lv_obj_set_style_text_align(snooze_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(snooze_lbl_);
    ui::set_text(snooze_lbl_, view_.snooze_text);
    snooze_rect_ = {0, btn_top, btn_w, btn_top + btn_h};

    // Right button: Dismiss (high-contrast alert red)
    const int dismiss_x = btn_w + btn_gap;
    dismiss_box_ = ui::block(c.parent, theme::ACCENT);
    ui::set_fill(dismiss_box_, tessera::Tone::ALERT, theme::ACCENT);
    lv_obj_set_pos(dismiss_box_, dismiss_x, btn_top);
    lv_obj_set_size(dismiss_box_, width_ - dismiss_x, btn_h);
    dismiss_lbl_ = ui::label(dismiss_box_, Font::BODY_LARGE, theme::ON_ACCENT);
    lv_obj_set_width(dismiss_lbl_, std::max(1, (width_ - dismiss_x) - ui::px(8)));
    lv_obj_set_style_text_align(dismiss_lbl_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(dismiss_lbl_);
    ui::set_text(dismiss_lbl_, view_.dismiss_text);
    dismiss_rect_ = {dismiss_x, btn_top, width_, btn_top + btn_h};
  }

  void on_theme() override {
    namespace ui = tessera::ui;
    ui::set_fill(disc_, tessera::Tone::ALERT, theme::ACCENT);
    ui::set_color(time_lbl_, theme::INK);
    ui::set_color(label_lbl_, theme::INK_SOFT);
    lv_obj_set_style_bg_color(snooze_box_, ui::color(theme::KEY), 0);
    ui::set_color(snooze_lbl_, theme::INK);
    ui::set_fill(dismiss_box_, tessera::Tone::ALERT, theme::ACCENT);
    ui::set_color(dismiss_lbl_, theme::ON_ACCENT);
  }

  void on_tap() override {
    const tessera::Point pt = tessera::tap_point();
    if (pt.x < 0 || pt.y < 0) return;
    if (hit_(snooze_rect_, pt)) {
      if (on_snooze_) on_snooze_();
      return;
    }
    if (hit_(dismiss_rect_, pt)) {
      if (on_dismiss_btn_) on_dismiss_btn_();
      return;
    }
    // Tap anywhere else on the card does nothing (prevents accidental alarm dismissal).
  }

  void on_dismiss() override {
    if (on_closed_) on_closed_();
  }

 private:
  struct Rect {
    int x1{0}, y1{0}, x2{0}, y2{0};
  };
  static bool hit_(const Rect &r, const tessera::Point &p) {
    return p.x >= r.x1 && p.x <= r.x2 && p.y >= r.y1 && p.y <= r.y2;
  }

  AlarmPopupView view_;
  std::function<void()> on_snooze_;
  std::function<void()> on_dismiss_btn_;
  std::function<void()> on_closed_;

  int width_{0};
  lv_obj_t *disc_{nullptr}, *icon_{nullptr}, *time_lbl_{nullptr}, *label_lbl_{nullptr};
  lv_obj_t *snooze_box_{nullptr}, *snooze_lbl_{nullptr};
  lv_obj_t *dismiss_box_{nullptr}, *dismiss_lbl_{nullptr};
  Rect snooze_rect_{}, dismiss_rect_{};
};

}  // namespace esphome::alarm_clock
