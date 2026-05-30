#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"

#include <vector>
#include <algorithm>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

static bool IsClockText(const char* text) {
    return text != nullptr &&
        text[0] >= '0' && text[0] <= '9' &&
        text[1] >= '0' && text[1] <= '9' &&
        text[2] == ':' &&
        text[3] >= '0' && text[3] <= '9' &&
        text[4] >= '0' && text[4] <= '9' &&
        text[5] == '\0';
}

static void FormatClockText(char* buffer, size_t size) {
    if (buffer == nullptr || size == 0) {
        return;
    }

    snprintf(buffer, size, "--:--");
    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    if (tm != nullptr && tm->tm_year >= 2025 - 1900) {
        strftime(buffer, size, "%H:%M", tm);
    }
}

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    SetPreviewImage(nullptr);
    
    // Clean up GIF controller
    if (gif_controller_) {
        gif_controller_->Stop();
        gif_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_HOR_RES * 0.8);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);
    
    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
        }
        return;
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF6A33A), 0);

    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_border_color(container_, lv_color_hex(0x743818), 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(0xF6A33A), 0);
    lv_obj_set_style_bg_grad_dir(container_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_scrollbar_mode(container_, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* top_shadow = lv_obj_create(container_);
    lv_obj_set_size(top_shadow, LV_HOR_RES, 58);
    lv_obj_align(top_shadow, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(top_shadow, 14, 0);
    lv_obj_set_style_bg_color(top_shadow, lv_color_hex(0x617C94), 0);
    lv_obj_set_style_border_width(top_shadow, 0, 0);
    lv_obj_clear_flag(top_shadow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(top_shadow, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* floor_panel = lv_obj_create(container_);
    lv_obj_set_size(floor_panel, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(floor_panel, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_radius(floor_panel, 10, 0);
    lv_obj_set_style_bg_color(floor_panel, lv_color_hex(0xDDF3FD), 0);
    lv_obj_set_style_bg_opa(floor_panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(floor_panel, 0, 0);
    lv_obj_clear_flag(floor_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(floor_panel, LV_OBJ_FLAG_HIDDEN);

    emoji_box_ = lv_obj_create(container_);
    lv_obj_set_size(emoji_box_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_clear_flag(emoji_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    robot_scene_ = lv_obj_create(emoji_box_);
    lv_obj_set_size(robot_scene_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_opa(robot_scene_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(robot_scene_, 0, 0);
    lv_obj_set_style_border_width(robot_scene_, 0, 0);
    lv_obj_clear_flag(robot_scene_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* halo = lv_obj_create(robot_scene_);
    lv_obj_set_size(halo, 106, 36);
    lv_obj_align(halo, LV_ALIGN_TOP_MID, -5, 4);
    lv_obj_set_style_radius(halo, 20, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(halo, 5, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(0x27B7D6), 0);
    lv_obj_set_style_outline_width(halo, 3, 0);
    lv_obj_set_style_outline_color(halo, lv_color_white(), 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(halo, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* wind_key = lv_obj_create(robot_scene_);
    lv_obj_set_size(wind_key, 48, 60);
    lv_obj_align(wind_key, LV_ALIGN_RIGHT_MID, -2, -6);
    lv_obj_set_style_bg_opa(wind_key, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wind_key, 0, 0);
    lv_obj_set_style_pad_all(wind_key, 0, 0);
    lv_obj_clear_flag(wind_key, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wind_key, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* key_stem = lv_obj_create(wind_key);
    lv_obj_set_size(key_stem, 28, 9);
    lv_obj_align(key_stem, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(key_stem, 2, 0);
    lv_obj_set_style_bg_color(key_stem, lv_color_hex(0xC9CFD2), 0);
    lv_obj_set_style_border_width(key_stem, 3, 0);
    lv_obj_set_style_border_color(key_stem, lv_color_hex(0x33312E), 0);
    lv_obj_clear_flag(key_stem, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t* loop = lv_obj_create(wind_key);
        lv_obj_set_size(loop, 34, 34);
        lv_obj_align(loop, LV_ALIGN_RIGHT_MID, 0, i == 0 ? -18 : 18);
        lv_obj_set_style_radius(loop, 18, 0);
        lv_obj_set_style_bg_opa(loop, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(loop, 5, 0);
        lv_obj_set_style_border_color(loop, lv_color_hex(0xF8F8F8), 0);
        lv_obj_set_style_outline_width(loop, 3, 0);
        lv_obj_set_style_outline_color(loop, lv_color_hex(0x33312E), 0);
        lv_obj_clear_flag(loop, LV_OBJ_FLAG_SCROLLABLE);
    }

    lv_obj_t* body = lv_obj_create(robot_scene_);
    lv_obj_set_size(body, 70, 74);
    lv_obj_align(body, LV_ALIGN_CENTER, -4, 56);
    lv_obj_set_style_radius(body, 7, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x263346), 0);
    lv_obj_set_style_border_width(body, 3, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x161A20), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t* leg = lv_obj_create(robot_scene_);
        lv_obj_set_size(leg, 12, 34);
        lv_obj_align(leg, LV_ALIGN_CENTER, i == 0 ? -32 : 24, 88);
        lv_obj_set_style_radius(leg, 3, 0);
        lv_obj_set_style_bg_color(leg, lv_color_hex(0x1A1C1D), 0);
        lv_obj_set_style_border_width(leg, 0, 0);
        lv_obj_clear_flag(leg, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(leg, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* foot = lv_obj_create(robot_scene_);
        lv_obj_set_size(foot, 38, 48);
        lv_obj_align(foot, LV_ALIGN_CENTER, i == 0 ? -52 : 44, 98);
        lv_obj_set_style_radius(foot, 3, 0);
        lv_obj_set_style_bg_color(foot, lv_color_hex(0xE7E7E5), 0);
        lv_obj_set_style_border_width(foot, 3, 0);
        lv_obj_set_style_border_color(foot, lv_color_hex(0x33312E), 0);
        lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(foot, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* sole = lv_obj_create(foot);
        lv_obj_set_size(sole, 14, 22);
        lv_obj_align(sole, LV_ALIGN_BOTTOM_MID, 0, -3);
        lv_obj_set_style_radius(sole, 7, 0);
        lv_obj_set_style_bg_color(sole, lv_color_hex(0x2E2E2E), 0);
        lv_obj_set_style_border_width(sole, 0, 0);
        lv_obj_clear_flag(sole, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* toe = lv_obj_create(foot);
        lv_obj_set_size(toe, 16, 18);
        lv_obj_align(toe, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(toe, 2, 0);
        lv_obj_set_style_bg_color(toe, lv_color_hex(0xD9934A), 0);
        lv_obj_set_style_bg_opa(toe, LV_OPA_80, 0);
        lv_obj_set_style_border_width(toe, 0, 0);
        lv_obj_clear_flag(toe, LV_OBJ_FLAG_SCROLLABLE);
    }

    for (int i = 0; i < 2; ++i) {
        lv_obj_t* arm = lv_obj_create(robot_scene_);
        lv_obj_set_size(arm, 18, 66);
        lv_obj_align(arm, LV_ALIGN_CENTER, i == 0 ? -62 : 58, 48);
        lv_obj_set_style_radius(arm, 5, 0);
        lv_obj_set_style_bg_color(arm, lv_color_hex(0xE3E3E1), 0);
        lv_obj_set_style_border_width(arm, 3, 0);
        lv_obj_set_style_border_color(arm, lv_color_hex(0x33312E), 0);
        lv_obj_clear_flag(arm, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(arm, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t* head_shadow = lv_obj_create(robot_scene_);
    lv_obj_set_size(head_shadow, 136, 92);
    lv_obj_align(head_shadow, LV_ALIGN_CENTER, 3, -17);
    lv_obj_set_style_radius(head_shadow, 12, 0);
    lv_obj_set_style_bg_color(head_shadow, lv_color_hex(0x898987), 0);
    lv_obj_set_style_border_width(head_shadow, 0, 0);
    lv_obj_clear_flag(head_shadow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(head_shadow, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* head = lv_obj_create(robot_scene_);
    lv_obj_set_size(head, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(head, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_shadow_width(head, 0, 0);
    lv_obj_set_style_shadow_color(head, lv_color_white(), 0);
    lv_obj_set_style_shadow_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    robot_face_ = lv_obj_create(head);
    lv_obj_set_size(robot_face_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(robot_face_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(robot_face_, 0, 0);
    lv_obj_set_style_bg_color(robot_face_, lv_color_hex(0xF6A33A), 0);
    lv_obj_set_style_bg_grad_dir(robot_face_, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(robot_face_, 0, 0);
    lv_obj_set_style_border_color(robot_face_, lv_color_hex(0x743818), 0);
    lv_obj_set_style_shadow_width(robot_face_, 0, 0);
    lv_obj_set_style_shadow_color(robot_face_, lv_color_white(), 0);
    lv_obj_set_style_shadow_opa(robot_face_, LV_OPA_40, 0);
    lv_obj_clear_flag(robot_face_, LV_OBJ_FLAG_SCROLLABLE);

    robot_eye_left_ = lv_obj_create(robot_face_);
    robot_eye_right_ = lv_obj_create(robot_face_);
    lv_obj_t* eyes[] = {robot_eye_left_, robot_eye_right_};
    for (auto eye : eyes) {
        lv_obj_set_size(eye, 36, 50);
        lv_obj_set_style_radius(eye, 18, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(0xFFE7C0), 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_set_style_shadow_width(eye, 4, 0);
        lv_obj_set_style_shadow_color(eye, lv_color_hex(0xFFF4DE), 0);
        lv_obj_set_style_shadow_opa(eye, LV_OPA_40, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_align(robot_eye_left_, LV_ALIGN_CENTER, -68, -24);
    lv_obj_align(robot_eye_right_, LV_ALIGN_CENTER, 68, -24);

    lv_obj_t* shine = lv_obj_create(robot_face_);
    lv_obj_set_size(shine, 15, 15);
    lv_obj_align(shine, LV_ALIGN_TOP_MID, 26, 62);
    lv_obj_set_style_radius(shine, 7, 0);
    lv_obj_set_style_bg_color(shine, lv_color_hex(0xFFE7C0), 0);
    lv_obj_set_style_bg_opa(shine, LV_OPA_90, 0);
    lv_obj_set_style_border_width(shine, 0, 0);
    lv_obj_clear_flag(shine, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(shine, LV_OBJ_FLAG_HIDDEN);

    robot_cheek_left_ = lv_obj_create(robot_face_);
    robot_cheek_right_ = lv_obj_create(robot_face_);
    lv_obj_t* cheeks[] = {robot_cheek_left_, robot_cheek_right_};
    for (auto cheek : cheeks) {
        lv_obj_set_size(cheek, 46, 18);
        lv_obj_set_style_radius(cheek, 9, 0);
        lv_obj_set_style_bg_color(cheek, lv_color_hex(0xFF7E79), 0);
        lv_obj_set_style_bg_opa(cheek, LV_OPA_50, 0);
        lv_obj_set_style_border_width(cheek, 0, 0);
        lv_obj_clear_flag(cheek, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_align(robot_cheek_left_, LV_ALIGN_CENTER, -108, 35);
    lv_obj_align(robot_cheek_right_, LV_ALIGN_CENTER, 108, 35);

    robot_mouth_ = lv_obj_create(robot_face_);
    lv_obj_set_size(robot_mouth_, 54, 9);
    lv_obj_align(robot_mouth_, LV_ALIGN_CENTER, 0, 54);
    lv_obj_set_style_radius(robot_mouth_, 5, 0);
    lv_obj_set_style_bg_color(robot_mouth_, lv_color_hex(0xFFE7C0), 0);
    lv_obj_set_style_border_width(robot_mouth_, 0, 0);
    lv_obj_set_style_transform_pivot_x(robot_mouth_, 27, 0);
    lv_obj_set_style_transform_pivot_y(robot_mouth_, 4, 0);
    lv_obj_set_style_transform_rotation(robot_mouth_, 0, 0);
    lv_obj_clear_flag(robot_mouth_, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* side_panel = lv_obj_create(head);
    lv_obj_set_size(side_panel, 24, 42);
    lv_obj_align(side_panel, LV_ALIGN_RIGHT_MID, -8, 8);
    lv_obj_set_style_radius(side_panel, 4, 0);
    lv_obj_set_style_bg_color(side_panel, lv_color_hex(0x666763), 0);
    lv_obj_set_style_border_width(side_panel, 3, 0);
    lv_obj_set_style_border_color(side_panel, lv_color_hex(0x33312E), 0);
    lv_obj_clear_flag(side_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(side_panel, LV_OBJ_FLAG_HIDDEN);

    robot_status_dot_ = lv_obj_create(robot_scene_);
    lv_obj_set_size(robot_status_dot_, 8, 8);
    lv_obj_align(robot_status_dot_, LV_ALIGN_CENTER, 42, -74);
    lv_obj_set_style_radius(robot_status_dot_, 4, 0);
    lv_obj_set_style_bg_color(robot_status_dot_, lv_color_hex(0x33C9E7), 0);
    lv_obj_set_style_border_width(robot_status_dot_, 0, 0);
    lv_obj_clear_flag(robot_status_dot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(robot_status_dot_, LV_OBJ_FLAG_HIDDEN);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lv_color_hex(0x243342), 0);
    lv_label_set_text(emoji_label_, "");
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    preview_image_ = lv_image_create(screen);
    lv_obj_set_size(preview_image_, width_ / 2, height_ / 2);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 8);
    lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);

    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, 34);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_hex(0xF6A33A), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_layout(top_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 12, 8);

    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_add_flag(mute_label_, LV_OBJ_FLAG_HIDDEN);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);
    lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right_icons, LV_OBJ_FLAG_HIDDEN);

    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, 92, 34);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);
    lv_obj_align(status_bar_, LV_ALIGN_TOP_RIGHT, -12, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, 92);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(0x4B2411), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_TOP_RIGHT, 0, 8);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, 92);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x4B2411), 0);
    lv_label_set_text(status_label_, "--:--");
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, 0, 8);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(bottom_bar_, lv_color_hex(0xFFE7C0), 0);
    lv_obj_set_style_pad_left(bottom_bar_, 12, 0);
    lv_obj_set_style_pad_right(bottom_bar_, 12, 0);
    lv_obj_set_style_pad_top(bottom_bar_, 4, 0);
    lv_obj_set_style_pad_bottom(bottom_bar_, 8, 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 24);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFE7C0), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
#else
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + 12);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(bottom_bar_, lv_color_hex(0xFFE7C0), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, 12, 0);
    lv_obj_set_style_pad_right(bottom_bar_, 12, 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - 24);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFE7C0), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (preview_image_ == nullptr) {
        ESP_LOGE(TAG, "Preview image is not initialized");
        return;
    }

    if (image == nullptr) {
        esp_timer_stop(preview_timer_);
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
        preview_image_cached_.reset();
        if (gif_controller_) {
            gif_controller_->Start();
        }
        return;
    }

    preview_image_cached_ = std::move(image);
    auto img_dsc = preview_image_cached_->image_dsc();
    lv_image_set_src(preview_image_, img_dsc);
    if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
        // zoom factor 0.5
        lv_image_set_scale(preview_image_, 128 * width_ / img_dsc->header.w);
    }

    // Hide emoji_box_
    if (gif_controller_) {
        gif_controller_->Stop();
    }
    lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(preview_image_, LV_OBJ_FLAG_HIDDEN);
    esp_timer_stop(preview_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() was called but label not created)", role, content);
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }

    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but label not created)", status);
        }
        return;
    }

    if (IsClockText(status)) {
        lv_label_set_text(status_label_, status);
    } else if (chat_message_label_ != nullptr && bottom_bar_ != nullptr) {
        lv_label_set_text(chat_message_label_, status != nullptr ? status : "");
        if (status == nullptr || status[0] == '\0' || hide_subtitle_) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    last_status_update_time_ = std::chrono::system_clock::now();
}

void LcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
    }

    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr || bottom_bar_ == nullptr) {
        return;
    }

    lv_label_set_text(chat_message_label_, notification != nullptr ? notification : "");
    if (notification == nullptr || notification[0] == '\0' || hide_subtitle_) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    if (notification_label_ != nullptr) {
        lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (status_label_ != nullptr) {
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LcdDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LcdDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);

    char time_str[16];
    FormatClockText(time_str, sizeof(time_str));

    DisplayLockGuard lock(this);
    if (status_label_ != nullptr) {
        lv_label_set_text(status_label_, time_str);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetEmotion(const char* emotion) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }
    // Stop any running GIF animation
    if (gif_controller_) {
        DisplayLockGuard lock(this);
        gif_controller_->Stop();
        // Hide image before destroying GIF controller to prevent LVGL from
        // accessing freed image data during rendering between lock scopes
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        gif_controller_.reset();
    }
    
    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    if (robot_face_ != nullptr && robot_eye_left_ != nullptr && robot_eye_right_ != nullptr && robot_mouth_ != nullptr) {
        const char* mood = emotion != nullptr ? emotion : "";
        lv_coord_t eye_w = 36;
        lv_coord_t eye_h = 50;
        lv_coord_t eye_y = -24;
        lv_coord_t eye_x = 68;
        lv_coord_t mouth_w = 54;
        lv_coord_t mouth_h = 9;
        lv_coord_t mouth_y = 54;
        int32_t mouth_rotation = 0;
        lv_opa_t cheek_opa = LV_OPA_50;
        lv_color_t face_color = lv_color_hex(0xF6A33A);
        lv_color_t dot_color = lv_color_hex(0x33C9E7);

        if (strstr(mood, "happy") || strstr(mood, "laugh") || strstr(mood, "smile")) {
            eye_w = 42;
            eye_h = 22;
            eye_y = -26;
            mouth_w = 76;
            mouth_h = 12;
            mouth_y = 54;
            mouth_rotation = 0;
            cheek_opa = LV_OPA_70;
            dot_color = lv_color_hex(0x63D46C);
        } else if (strstr(mood, "sad")) {
            eye_w = 30;
            eye_h = 42;
            eye_y = -16;
            mouth_w = 42;
            mouth_h = 7;
            mouth_y = 58;
            mouth_rotation = 80;
            cheek_opa = LV_OPA_30;
            face_color = lv_color_hex(0xE99A3A);
            dot_color = lv_color_hex(0x7AA9C7);
        } else if (strstr(mood, "angry")) {
            eye_w = 38;
            eye_h = 14;
            eye_y = -24;
            mouth_w = 50;
            mouth_h = 6;
            mouth_y = 56;
            mouth_rotation = -40;
            cheek_opa = LV_OPA_20;
            face_color = lv_color_hex(0xE88832);
            dot_color = lv_color_hex(0xF15A42);
        } else if (strstr(mood, "surprise") || strstr(mood, "shock")) {
            eye_w = 42;
            eye_h = 52;
            eye_y = -24;
            eye_x = 66;
            mouth_w = 30;
            mouth_h = 30;
            mouth_y = 52;
            mouth_rotation = 0;
            cheek_opa = LV_OPA_50;
            dot_color = lv_color_hex(0xF4D03F);
        } else if (strstr(mood, "thinking")) {
            eye_w = 30;
            eye_h = 38;
            eye_y = -20;
            eye_x = 66;
            mouth_w = 36;
            mouth_h = 7;
            mouth_y = 56;
            mouth_rotation = -80;
            cheek_opa = LV_OPA_40;
            dot_color = lv_color_hex(0x9B8CFF);
        } else if (strstr(mood, "speaking") || strstr(mood, "talk")) {
            eye_w = 36;
            eye_h = 48;
            eye_y = -24;
            mouth_w = 42;
            mouth_h = 24;
            mouth_y = 52;
            mouth_rotation = 0;
            cheek_opa = LV_OPA_60;
            dot_color = lv_color_hex(0x63D46C);
        } else if (strstr(mood, "listening")) {
            eye_w = 38;
            eye_h = 52;
            eye_y = -26;
            mouth_w = 48;
            mouth_h = 8;
            mouth_y = 54;
            cheek_opa = LV_OPA_50;
            dot_color = lv_color_hex(0x33C9E7);
        }

        DisplayLockGuard lock(this);
        if (emoji_image_) {
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (emoji_label_) {
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(robot_face_, face_color, 0);
        lv_obj_set_style_bg_grad_dir(robot_face_, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_size(robot_eye_left_, eye_w, eye_h);
        lv_obj_set_size(robot_eye_right_, eye_w, eye_h);
        lv_obj_set_style_radius(robot_eye_left_, eye_w / 2, 0);
        lv_obj_set_style_radius(robot_eye_right_, eye_w / 2, 0);
        lv_obj_align(robot_eye_left_, LV_ALIGN_CENTER, -eye_x, eye_y);
        lv_obj_align(robot_eye_right_, LV_ALIGN_CENTER, eye_x, eye_y);
        lv_obj_set_size(robot_mouth_, mouth_w, mouth_h);
        lv_obj_set_style_radius(robot_mouth_, mouth_h / 2, 0);
        lv_obj_set_style_transform_pivot_x(robot_mouth_, mouth_w / 2, 0);
        lv_obj_set_style_transform_pivot_y(robot_mouth_, mouth_h / 2, 0);
        lv_obj_set_style_transform_rotation(robot_mouth_, mouth_rotation, 0);
        lv_obj_align(robot_mouth_, LV_ALIGN_CENTER, 0, mouth_y);
        if (robot_cheek_left_ != nullptr && robot_cheek_right_ != nullptr) {
            lv_obj_set_style_bg_opa(robot_cheek_left_, cheek_opa, 0);
            lv_obj_set_style_bg_opa(robot_cheek_right_, cheek_opa, 0);
        }
        if (robot_status_dot_) {
            lv_obj_set_style_bg_color(robot_status_dot_, dot_color, 0);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    auto image = emoji_collection != nullptr ? emoji_collection->GetEmojiImage(emotion) : nullptr;
    if (image == nullptr) {
        const char* utf8 = font_awesome_get_utf8(emotion);
        if (utf8 != nullptr && emoji_label_ != nullptr) {
            DisplayLockGuard lock(this);
            lv_label_set_text(emoji_label_, utf8);
            lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    DisplayLockGuard lock(this);
    if (image->IsGif()) {
        // Create new GIF controller
        gif_controller_ = std::make_unique<LvglGif>(image->image_dsc());
        
        if (gif_controller_->IsLoaded()) {
            // Set up frame update callback
            gif_controller_->SetFrameCallback([this]() {
                lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            });
            
            // Set initial frame and start animation
            lv_image_set_src(emoji_image_, gif_controller_->image_dsc());
            gif_controller_->Start();
            
            // Show GIF, hide others
            lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            ESP_LOGE(TAG, "Failed to load GIF for emotion: %s", emotion);
            gif_controller_.reset();
        }
    } else {
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(emotion, "neutral") == 0 && child_count > 0) {
        // Stop GIF animation if running
        if (gif_controller_) {
            gif_controller_->Stop();
            gif_controller_.reset();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Set background image
    if (lvgl_theme->background_image() != nullptr) {
        lv_obj_set_style_bg_image_src(container_, lvgl_theme->background_image()->image_dsc(), 0);
    } else {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    }
    
    // Update top bar background color with 50% opacity
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    }
    
    // Update status bar elements
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(status_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(mute_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(battery_label_, lvgl_theme->text_color(), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);

    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF6A33A), 0);
    if (container_ != nullptr) {
        lv_obj_set_style_bg_image_src(container_, nullptr, 0);
        lv_obj_set_style_bg_color(container_, lv_color_hex(0xF6A33A), 0);
        lv_obj_set_style_bg_grad_dir(container_, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_border_color(container_, lv_color_hex(0x743818), 0);
    }
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(top_bar_, lv_color_hex(0xF6A33A), 0);
    }
    lv_obj_set_style_text_color(network_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_hex(0x4B2411), 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(0x4B2411), 0);

    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(0xFFE7C0), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lv_color_hex(0x4B2411), 0);
    }
    
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(0xF6A33A), 0);
        lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(0x743818), 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}
