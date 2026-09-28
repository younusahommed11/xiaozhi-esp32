#include "oled_display.h"
#include "assets/lang_config.h"
#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#include <algorithm>
#include <string>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#define TAG "OledDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_1);
LV_FONT_DECLARE(font_noto_emoji_30_1);

// অ্যানিমেশন এবং ফেস ভ্যারিয়েবল
uint32_t speak_last_update_ = 0;
int speak_mouth_target_ = 4;
int speak_mouth_current_ = 4;
int eye_size = 22; // চোখের সাইজ

OledDisplay::OledDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, bool mirror_x, bool mirror_y)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_1);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_1);

    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("dark", dark_theme);
    current_theme_ = dark_theme;

    ESP_LOGI(TAG, "Initialize LVGL");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.task_stack = 6144;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding OLED display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * height_),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = true,
        .rotation =
            {
                .swap_xy = false,
                .mirror_x = mirror_x,
                .mirror_y = mirror_y,
            },
        .flags =
            {
                .buff_dma = 1,
                .buff_spiram = 0,
                .sw_rotate = 0,
                .full_refresh = 0,
                .direct_mode = 0,
            },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }
}

void OledDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }

    Display::SetupUI();  
    if (height_ == 64) {
        SetupUI_128x64();
    } else {
        SetupUI_128x32();
    }

    // ফেস অ্যানিমেশন টাইমার (~16 FPS)
    lv_timer_create(
        [](lv_timer_t* t) {
            auto self = static_cast<OledDisplay*>(lv_timer_get_user_data(t));
            self->UpdateFace();
        },
        60, this);
}

OledDisplay::~OledDisplay() {
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }

    bool is_128x64_layout = (top_bar_ != nullptr);
    if (status_bar_ != nullptr && is_128x64_layout) {
        status_label_ = nullptr;
        notification_label_ = nullptr;
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        network_label_ = nullptr;
        mute_label_ = nullptr;
        battery_label_ = nullptr;
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        if (!is_128x64_layout) {
            status_label_ = nullptr;
            notification_label_ = nullptr;
            network_label_ = nullptr;
            mute_label_ = nullptr;
            battery_label_ = nullptr;
        }
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
    lvgl_port_deinit();
}

bool OledDisplay::Lock(int timeout_ms) { return lvgl_port_lock(timeout_ms); }

void OledDisplay::Unlock() { lvgl_port_unlock(); }

void OledDisplay::SetFaceState(FaceState state) { face_state_ = state; }

void OledDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    std::string content_str = content;
    std::replace(content_str.begin(), content_str.end(), '\n', ' ');

    lv_anim_delete(chat_message_label_, nullptr);
    if (content_right_ == nullptr) {
        lv_label_set_text(chat_message_label_, content_str.c_str());
    } else {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(chat_message_label_, content_str.c_str());
            lv_obj_remove_flag(content_right_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --- ফেস অ্যানিমেশন লজিক (ভিডিওর স্টাইল) ---
void OledDisplay::IdleBehavior(int base_eye_height) {
    if (rand() % 40 == 0) {
        idle_move_offset_x_ = (rand() % 5) - 2;
        idle_move_offset_y_ = (rand() % 3) - 1;
    }

    int eye_h = base_eye_height;
    int eye_w = eye_h * 0.75; 

    lv_obj_set_size(left_eye_, eye_w, eye_h);
    lv_obj_set_size(right_eye_, eye_w, eye_h);

    lv_obj_set_style_radius(left_eye_, eye_w / 2, 0);
    lv_obj_set_style_radius(right_eye_, eye_w / 2, 0);

    lv_obj_align(left_eye_, LV_ALIGN_CENTER, -24 + idle_move_offset_x_, -4 + idle_move_offset_y_);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, 24 + idle_move_offset_x_, -4 + idle_move_offset_y_);

    lv_obj_set_size(mouth_, 12, 3);
    lv_obj_set_style_radius(mouth_, 1, 0);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, idle_move_offset_x_, 14 + idle_move_offset_y_);
}

void OledDisplay::ListeningBehavior(int base_eye_height) {
    int right_h = base_eye_height;
    int right_w = right_h * 0.75;
    int left_h = base_eye_height;
    int left_w = left_h * 0.75;

    lv_obj_set_size(left_eye_, left_w, left_h);
    lv_obj_set_size(right_eye_, right_w, right_h);

    lv_obj_set_style_radius(left_eye_, left_w / 2, 0);
    lv_obj_set_style_radius(right_eye_, right_w / 2, 0);

    lv_obj_align(left_eye_, LV_ALIGN_CENTER, -24, -4);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, 24, -4);

    lv_obj_set_size(mouth_, 8, 2);
    lv_obj_set_style_radius(mouth_, 1, 0);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, 14);
}

void OledDisplay::SpeakingBehavior(int eye_height) {
    uint32_t now = lv_tick_get();
    int eye_w = eye_height * 0.75;

    lv_obj_set_size(left_eye_, eye_w, eye_height);
    lv_obj_set_size(right_eye_, eye_w, eye_height);

    lv_obj_set_style_radius(left_eye_, eye_w / 2, 0);
    lv_obj_set_style_radius(right_eye_, eye_w / 2, 0);

    lv_obj_align(left_eye_, LV_ALIGN_CENTER, -24, -4);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, 24, -4);

    if (now - speak_last_update_ > 80 + (rand() % 50)) {
        speak_last_update_ = now;
        int r = rand() % 100;
        if (r < 20)
            speak_mouth_target_ = 3;
        else if (r < 50)
            speak_mouth_target_ = 8;
        else if (r < 80)
            speak_mouth_target_ = 12;
        else
            speak_mouth_target_ = 16;
    }

    if (speak_mouth_current_ < speak_mouth_target_)
        speak_mouth_current_ += 3;
    else if (speak_mouth_current_ > speak_mouth_target_)
        speak_mouth_current_ -= 3;

    lv_obj_set_size(mouth_, 14, speak_mouth_current_);
    lv_obj_set_style_radius(mouth_, 6, 0);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, 14);
}

void OledDisplay::UpdateFace() {
    DisplayLockGuard lock(this);
    if (left_eye_ == nullptr || right_eye_ == nullptr || mouth_ == nullptr) {
        return;
    }

    if (blink_phase_ == 0) {
        if (rand() % 120 == 0) {
            blink_phase_ = 1;
        }
    }

    int eye_height = eye_size;
    switch (blink_phase_) {
        case 1:
            eye_height = 4;
            blink_phase_ = 2;
            break;
        case 2:
            eye_height = 1;
            blink_phase_ = 3;
            break;
        case 3:
            eye_height = 6;
            blink_phase_ = 0;
            break;
        default:
            eye_height = eye_size;
            break;
    }

    switch (face_state_) {
        case FaceState::Idle:
            IdleBehavior(eye_height);
            break;
        case FaceState::Listening:
            ListeningBehavior(eye_height);
            break;
        case FaceState::Speaking:
            SpeakingBehavior(eye_height);
            break;
    }
}

void OledDisplay::SetupUI_128x64() {
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Layer 1: Top bar */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);

    /* Layer 2: Status bar */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, 16);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content & Face Container */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_style_pad_all(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_size(content_, 128, 48);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    face_container_ = lv_obj_create(content_);
    lv_obj_set_size(face_container_, 128, 48);
    lv_obj_set_style_border_width(face_container_, 0, 0);
    lv_obj_set_style_bg_opa(face_container_, LV_OPA_TRANSP, 0);

    // চোখের উইজেট তৈরি
    left_eye_ = lv_obj_create(face_container_);
    right_eye_ = lv_obj_create(face_container_);

    lv_obj_set_style_bg_color(left_eye_, lv_color_black(), 0);
    lv_obj_set_style_bg_color(right_eye_, lv_color_black(), 0);
    lv_obj_set_style_border_width(left_eye_, 0, 0);
    lv_obj_set_style_border_width(right_eye_, 0, 0);

    lv_obj_set_size(left_eye_, eye_size * 0.75, eye_size);
    lv_obj_set_size(right_eye_, eye_size * 0.75, eye_size);

    lv_obj_set_style_radius(left_eye_, (eye_size * 0.75) / 2, 0);
    lv_obj_set_style_radius(right_eye_, (eye_size * 0.75) / 2, 0);

    lv_obj_align(left_eye_, LV_ALIGN_CENTER, -24, -4);
    lv_obj_align(right_eye_, LV_ALIGN_CENTER, 24, -4);

    // মুখের উইজেট তৈরি
    mouth_ = lv_obj_create(face_container_);
    lv_obj_set_style_bg_color(mouth_, lv_color_black(), 0);
    lv_obj_set_style_border_width(mouth_, 0, 0);
    lv_obj_set_style_radius(mouth_, 4, 0);
    lv_obj_set_size(mouth_, 12, 4);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, 0, 14);

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
    lv_obj_set_style_radius(low_battery_popup_, 10, 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void OledDisplay::SetupUI_128x32() {
    SetupUI_128x64(); // ছোট স্ক্রীনের ক্ষেত্রেও একই অ্যানিমেটেড ফেস ফলব্যাক হিসেবে কাজ করবে
}

void OledDisplay::SetEmotion(const char* emotion) {
    // অ্যানিমেশন ফেস চালু থাকায় ইমোশন লেবেল বাইপাস করা হলো
}

void OledDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto text_font = lvgl_theme->text_font()->font();
    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
}

void OledDisplay::SetPowerSaveMode(bool on) {
    if (panel_) {
        Settings settings("wifi", false);
        if (settings.GetBool("power_save_display_off", false)) {
            esp_lcd_panel_disp_on_off(panel_, !on);
        }
    }
    LvglDisplay::SetPowerSaveMode(on);
}
