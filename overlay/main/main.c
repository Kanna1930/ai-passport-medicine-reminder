#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bsp_audio.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "medicine_model.h"
#include "medicine_store.h"

#define INPUT_QUEUE_DEPTH 8
#define AUDIO_QUEUE_DEPTH 1
#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_CHUNK 160

#define COLOR_BG       0x101827
#define COLOR_PANEL    0x1B2A41
#define COLOR_ACCENT   0x67E8F9
#define COLOR_OK       0x86EFAC
#define COLOR_WARN     0xFDBA74
#define COLOR_ALARM    0x7F1D1D
#define COLOR_TEXT     0xF8FAFC
#define COLOR_MUTED    0x94A3B8

typedef enum {
    SCREEN_HOME = 0,
    SCREEN_SET_CLOCK_HOUR,
    SCREEN_SET_CLOCK_MINUTE,
    SCREEN_SET_REMINDER_HOUR,
    SCREEN_SET_REMINDER_MINUTE,
    SCREEN_ALARM,
} screen_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static const char *TAG = "medicine";

static medicine_model_t s_model;
static screen_mode_t s_mode;
static uint8_t s_edit_hour;
static uint8_t s_edit_minute;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_audio_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static bool s_button_ok;
static bool s_audio_ok;

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_big;
static lv_obj_t *s_sub;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static uint8_t s_last_minute = 0xFF;
static medicine_day_status_t s_last_day_status = (medicine_day_status_t)0xFF;

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            int x, int y, int width, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    return label;
}

static const char *status_text(medicine_day_status_t status) {
    switch (status) {
        case MEDICINE_DAY_TAKEN: return "TODAY  TAKEN";
        case MEDICINE_DAY_SKIPPED: return "TODAY  SKIPPED";
        case MEDICINE_DAY_WAITING:
        default: return "TODAY  WAITING";
    }
}

static void set_screen_background(uint32_t color) {
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
}

static void render_ui(void) {
    if (!s_screen) return;

    uint64_t now = now_ms();
    uint8_t hour = 0;
    uint8_t minute = 0;
    (void)medicine_model_clock(&s_model, now, &hour, &minute, NULL);

    lv_obj_set_style_text_color(s_title, lv_color_hex(COLOR_ACCENT), 0);
    lv_obj_set_style_text_color(s_big, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(COLOR_MUTED), 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_OK), 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(COLOR_MUTED), 0);

    switch (s_mode) {
        case SCREEN_HOME: {
            set_screen_background(COLOR_BG);
            lv_label_set_text(s_title, "MEDS");
            lv_label_set_text_fmt(s_big, "%02u:%02u", hour, minute);
            if (s_model.reminder_enabled) {
                lv_label_set_text_fmt(s_sub, "NEXT  %02u:%02u", s_model.reminder_hour,
                                      s_model.reminder_minute);
            } else {
                lv_label_set_text(s_sub, "REMINDER  OFF");
            }
            medicine_day_status_t status = medicine_model_day_status(&s_model, now);
            lv_label_set_text(s_status, status_text(status));
            if (!s_button_ok) {
                lv_label_set_text(s_hint, "BUTTON ERROR\nCHECK HARDWARE");
            } else {
                lv_label_set_text(s_hint,
                                  "HOLD OK  REMINDER\nHOLD UP  CLOCK   HOLD DOWN  ON/OFF");
            }
            break;
        }
        case SCREEN_SET_CLOCK_HOUR:
            set_screen_background(COLOR_PANEL);
            lv_label_set_text(s_title, "SET CURRENT HOUR");
            lv_label_set_text_fmt(s_big, "%02u:--", s_edit_hour);
            lv_label_set_text(s_sub, "UP / DOWN  CHANGE");
            lv_label_set_text(s_status, "OK  NEXT");
            lv_label_set_text(s_hint, "Clock is re-set after power loss");
            break;
        case SCREEN_SET_CLOCK_MINUTE:
            set_screen_background(COLOR_PANEL);
            lv_label_set_text(s_title, "SET CURRENT MINUTE");
            lv_label_set_text_fmt(s_big, "%02u:%02u", s_edit_hour, s_edit_minute);
            lv_label_set_text(s_sub, "UP / DOWN  CHANGE");
            lv_label_set_text(s_status, "OK  SAVE CLOCK");
            lv_label_set_text(s_hint, "Use the current local time");
            break;
        case SCREEN_SET_REMINDER_HOUR:
            set_screen_background(COLOR_PANEL);
            lv_label_set_text(s_title, "SET REMINDER HOUR");
            lv_label_set_text_fmt(s_big, "%02u:--", s_edit_hour);
            lv_label_set_text(s_sub, "UP / DOWN  CHANGE");
            lv_label_set_text(s_status, "OK  NEXT");
            lv_label_set_text(s_hint, "HOLD OK  CANCEL");
            break;
        case SCREEN_SET_REMINDER_MINUTE:
            set_screen_background(COLOR_PANEL);
            lv_label_set_text(s_title, "SET REMINDER MINUTE");
            lv_label_set_text_fmt(s_big, "%02u:%02u", s_edit_hour, s_edit_minute);
            lv_label_set_text(s_sub, "UP / DOWN  5 MIN");
            lv_label_set_text(s_status, "OK  SAVE REMINDER");
            lv_label_set_text(s_hint, "HOLD OK  CANCEL");
            break;
        case SCREEN_ALARM:
            set_screen_background(COLOR_ALARM);
            lv_obj_set_style_text_color(s_title, lv_color_hex(COLOR_WARN), 0);
            lv_label_set_text(s_title, "TIME TO TAKE MEDICINE");
            lv_label_set_text_fmt(s_big, "%02u:%02u", s_model.reminder_hour,
                                  s_model.reminder_minute);
            lv_obj_set_style_text_color(s_sub, lv_color_hex(COLOR_TEXT), 0);
            lv_label_set_text(s_sub, "OK  TAKEN");
            lv_obj_set_style_text_color(s_status, lv_color_hex(COLOR_WARN), 0);
            lv_label_set_text(s_status, "UP  SNOOZE 10 MIN");
            lv_obj_set_style_text_color(s_hint, lv_color_hex(COLOR_TEXT), 0);
            lv_label_set_text(s_hint, "DOWN  SKIP TODAY");
            break;
    }
}

static void build_ui(void) {
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_title = make_label(s_screen, &lv_font_montserrat_14, COLOR_ACCENT,
                         10, 22, 220, LV_TEXT_ALIGN_CENTER);
    s_big = make_label(s_screen, &lv_font_montserrat_20, COLOR_TEXT,
                       10, 94, 220, LV_TEXT_ALIGN_CENTER);
    s_sub = make_label(s_screen, &lv_font_montserrat_14, COLOR_MUTED,
                       10, 145, 220, LV_TEXT_ALIGN_CENTER);
    s_status = make_label(s_screen, &lv_font_montserrat_14, COLOR_OK,
                          10, 195, 220, LV_TEXT_ALIGN_CENTER);
    s_hint = make_label(s_screen, &lv_font_montserrat_14, COLOR_MUTED,
                        12, 252, 216, LV_TEXT_ALIGN_CENTER);

    render_ui();
    lv_screen_load(s_screen);
}

static void audio_write_note(int frequency, int duration_ms) {
    int16_t samples[AUDIO_CHUNK];
    int total = AUDIO_SAMPLE_RATE * duration_ms / 1000;
    int period = frequency > 0 ? AUDIO_SAMPLE_RATE / frequency : 1;
    int phase = 0;
    while (total > 0) {
        int count = total < AUDIO_CHUNK ? total : AUDIO_CHUNK;
        for (int i = 0; i < count; ++i) {
            samples[i] = frequency == 0 ? 0 : (phase < period / 2 ? 3800 : -3800);
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(samples, (size_t)count * sizeof(samples[0])) != ESP_OK) break;
        total -= count;
    }
}

static void audio_task(void *arg) {
    (void)arg;
    uint8_t event;
    if (bsp_audio_set_format(AUDIO_SAMPLE_RATE, 16, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Audio reminder disabled: format setup failed");
        s_audio_ok = false;
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(60);
    for (;;) {
        if (xQueueReceive(s_audio_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        (void)event;
        audio_write_note(880, 120);
        audio_write_note(0, 80);
        audio_write_note(1175, 140);
        audio_write_note(0, 80);
        audio_write_note(880, 180);
    }
}

static void play_alarm_tone(void) {
    if (!s_audio_ok || !s_audio_queue) return;
    uint8_t event = 1;
    (void)xQueueOverwrite(s_audio_queue, &event);
}

static uint8_t wrap_hour(int value) {
    while (value < 0) value += 24;
    while (value >= 24) value -= 24;
    return (uint8_t)value;
}

static uint8_t wrap_minute(int value) {
    while (value < 0) value += 60;
    while (value >= 60) value -= 60;
    return (uint8_t)value;
}

static bool handle_input_locked(const input_event_t *input) {
    bool save_config = false;
    uint64_t now = now_ms();

    if (s_mode == SCREEN_ALARM) {
        if (input->event != BSP_BTN_CLICK) return false;
        if (input->btn == BSP_BTN_OK && medicine_model_mark_taken(&s_model)) {
            s_mode = SCREEN_HOME;
        } else if (input->btn == BSP_BTN_UP && medicine_model_snooze(&s_model, now)) {
            s_mode = SCREEN_HOME;
        } else if (input->btn == BSP_BTN_DOWN && medicine_model_skip_today(&s_model)) {
            s_mode = SCREEN_HOME;
        }
        render_ui();
        return false;
    }

    if (s_mode == SCREEN_HOME) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            s_edit_hour = s_model.reminder_hour;
            s_edit_minute = s_model.reminder_minute;
            s_mode = SCREEN_SET_REMINDER_HOUR;
            render_ui();
        } else if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_UP) {
            uint8_t hour = 8, minute = 0;
            (void)medicine_model_clock(&s_model, now, &hour, &minute, NULL);
            s_edit_hour = hour;
            s_edit_minute = minute;
            s_mode = SCREEN_SET_CLOCK_HOUR;
            render_ui();
        } else if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_DOWN) {
            s_model.reminder_enabled = !s_model.reminder_enabled;
            s_model.snooze_active = false;
            s_model.alarm_active = false;
            save_config = true;
            render_ui();
        }
        return save_config;
    }

    if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK &&
        (s_mode == SCREEN_SET_REMINDER_HOUR || s_mode == SCREEN_SET_REMINDER_MINUTE) &&
        s_model.clock_valid) {
        s_mode = SCREEN_HOME;
        render_ui();
        return false;
    }

    if (input->event != BSP_BTN_CLICK && input->event != BSP_BTN_DOUBLE) return false;
    int step = input->event == BSP_BTN_DOUBLE ? 5 : 1;

    switch (s_mode) {
        case SCREEN_SET_CLOCK_HOUR:
            if (input->btn == BSP_BTN_UP) s_edit_hour = wrap_hour((int)s_edit_hour + step);
            else if (input->btn == BSP_BTN_DOWN) s_edit_hour = wrap_hour((int)s_edit_hour - step);
            else if (input->btn == BSP_BTN_OK) s_mode = SCREEN_SET_CLOCK_MINUTE;
            break;
        case SCREEN_SET_CLOCK_MINUTE:
            if (input->btn == BSP_BTN_UP) s_edit_minute = wrap_minute((int)s_edit_minute + step);
            else if (input->btn == BSP_BTN_DOWN) s_edit_minute = wrap_minute((int)s_edit_minute - step);
            else if (input->btn == BSP_BTN_OK) {
                medicine_model_set_clock(&s_model, s_edit_hour, s_edit_minute, now);
                s_mode = SCREEN_HOME;
            }
            break;
        case SCREEN_SET_REMINDER_HOUR:
            if (input->btn == BSP_BTN_UP) s_edit_hour = wrap_hour((int)s_edit_hour + step);
            else if (input->btn == BSP_BTN_DOWN) s_edit_hour = wrap_hour((int)s_edit_hour - step);
            else if (input->btn == BSP_BTN_OK) s_mode = SCREEN_SET_REMINDER_MINUTE;
            break;
        case SCREEN_SET_REMINDER_MINUTE: {
            int minute_step = input->event == BSP_BTN_DOUBLE ? 10 : 5;
            if (input->btn == BSP_BTN_UP) s_edit_minute = wrap_minute((int)s_edit_minute + minute_step);
            else if (input->btn == BSP_BTN_DOWN) s_edit_minute = wrap_minute((int)s_edit_minute - minute_step);
            else if (input->btn == BSP_BTN_OK) {
                medicine_model_set_reminder(&s_model, s_edit_hour, s_edit_minute, true);
                save_config = true;
                s_mode = SCREEN_HOME;
            }
            break;
        }
        case SCREEN_HOME:
        case SCREEN_ALARM:
            break;
    }
    render_ui();
    return save_config;
}

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;
        bool save_config = false;
        if (bsp_lvgl_lock(500)) {
            save_config = handle_input_locked(&input);
            bsp_lvgl_unlock();
        }
        if (save_config) {
            esp_err_t err = medicine_store_save(&s_model);
            if (err != ESP_OK) ESP_LOGW(TAG, "Reminder save failed: %s", esp_err_to_name(err));
        }
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t event, void *user) {
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = {.btn = btn, .event = event};
    (void)xQueueSend(s_input_queue, &input, 0);
}

static void timer_cb(lv_timer_t *timer) {
    (void)timer;
    uint64_t now = now_ms();
    medicine_event_t event = medicine_model_tick(&s_model, now);
    if (event == MEDICINE_EVENT_ALARM) {
        s_mode = SCREEN_ALARM;
        play_alarm_tone();
        render_ui();
        return;
    }

    if (s_mode == SCREEN_HOME) {
        uint8_t minute = 0;
        if (medicine_model_clock(&s_model, now, NULL, &minute, NULL)) {
            medicine_day_status_t status = medicine_model_day_status(&s_model, now);
            if (minute != s_last_minute || status != s_last_day_status) {
                s_last_minute = minute;
                s_last_day_status = status;
                render_ui();
            }
        }
    }
}

static esp_err_t input_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "medicine_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void audio_init_optional(void) {
    if (bsp_audio_init() != ESP_OK) {
        ESP_LOGW(TAG, "Audio unavailable; visual reminder will still work");
        return;
    }
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(uint8_t));
    s_audio_ok = s_audio_queue != NULL;
    if (!s_audio_queue || xTaskCreate(audio_task, "medicine_audio", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Audio worker unavailable; visual reminder will still work");
        s_audio_ok = false;
        if (s_audio_queue) {
            vQueueDelete(s_audio_queue);
            s_audio_queue = NULL;
        }
        return;
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "Medicine reminder starting");

    medicine_model_defaults(&s_model);
    esp_err_t store_err = medicine_store_init(&s_model);
    if (store_err != ESP_OK) {
        ESP_LOGW(TAG, "Persistent settings unavailable this boot: %s", esp_err_to_name(store_err));
    }

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "Display/LVGL init failed");
        return;
    }
    bsp_display_backlight(75);

    esp_err_t input_err = input_init();
    if (input_err == ESP_OK) {
        s_button_ok = bsp_button_init(on_key, NULL) == ESP_OK;
    }
    if (!s_button_ok) ESP_LOGE(TAG, "Button input unavailable");

    audio_init_optional();

    s_edit_hour = 8;
    s_edit_minute = 0;
    s_mode = SCREEN_SET_CLOCK_HOUR;
    if (bsp_lvgl_lock(1000)) {
        build_ui();
        s_timer = lv_timer_create(timer_cb, 250, NULL);
        bsp_lvgl_unlock();
    }

    s_input_ready = s_button_ok;
    ESP_LOGI(TAG, "Ready: reminder=%02u:%02u audio=%d buttons=%d",
             s_model.reminder_hour, s_model.reminder_minute, s_audio_ok, s_button_ok);
}
