#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

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
#include "power_manager.h"
#include "time_sync.h"

#define HOME_IDLE_MS 30000ULL
#define CONFIRM_IDLE_MS 5000ULL
#define AUDIO_RATE 8000
#define AUDIO_CHUNK 160

typedef enum {
    UI_NETWORK = 0,
    UI_HOME,
    UI_SET_HOUR,
    UI_SET_MINUTE,
    UI_ALARM,
    UI_CONFIRM,
} ui_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} key_msg_t;

static const char *TAG = "medicine";
static medicine_model_t g_model;
static ui_mode_t g_mode = UI_NETWORK;
static uint8_t g_edit_hour;
static uint8_t g_edit_minute;
static QueueHandle_t g_keys;
static QueueHandle_t g_audio_q;
static bool g_audio_ok;
static volatile bool g_woke;
static uint64_t g_idle_deadline;
static const char *g_confirm = "已记录";

static lv_obj_t *g_screen;
static lv_obj_t *g_title;
static lv_obj_t *g_clock;
static lv_obj_t *g_body;
static lv_obj_t *g_hint;
static time_sync_state_t g_last_sync = (time_sync_state_t)-1;
static int g_last_minute = -1;

static uint64_t ms_now(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static bool local_now(time_t *epoch, int32_t *day, uint16_t *minute_of_day,
                      uint8_t *hour, uint8_t *minute) {
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    struct tm t;
    if (!localtime_r(&now, &t)) return false;
    if (epoch) *epoch = now;
    if (day) *day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    if (minute_of_day) *minute_of_day = (uint16_t)t.tm_hour * 60U + t.tm_min;
    if (hour) *hour = (uint8_t)t.tm_hour;
    if (minute) *minute = (uint8_t)t.tm_min;
    return true;
}

static time_t next_wake_time(void) {
    time_t now = time(NULL);
    if (g_model.snooze_active && g_model.snooze_epoch_minute > 0) {
        time_t t = (time_t)(g_model.snooze_epoch_minute * 60);
        if (t > now) return t;
    }

    struct tm t;
    if (!localtime_r(&now, &t)) return now + 60;
    int32_t day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    medicine_day_status_t status = medicine_model_day_status(&g_model, day);

    t.tm_hour = g_model.reminder_hour;
    t.tm_min = g_model.reminder_minute;
    t.tm_sec = 0;
    t.tm_isdst = -1;
    time_t wake = mktime(&t);
    if (wake <= now + 2 || status != MEDICINE_DAY_WAITING || g_model.last_trigger_day == day) {
        t.tm_mday += 1;
        t.tm_isdst = -1;
        wake = mktime(&t);
    }
    return wake > now ? wake : now + 60;
}

static void reset_idle(void) {
    g_idle_deadline = ms_now() + HOME_IDLE_MS;
}

static void label_style(lv_obj_t *obj, const lv_font_t *font, uint32_t color) {
    lv_obj_set_width(obj, 216);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
}

static const char *status_text(medicine_day_status_t s) {
    if (s == MEDICINE_DAY_TAKEN) return "今日状态：已服药";
    if (s == MEDICINE_DAY_SKIPPED) return "今日状态：已跳过";
    return "今日状态：待服药";
}

static void render(void) {
    if (!g_screen) return;
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(0xF4F7FB), 0);

    if (g_mode == UI_NETWORK) {
        time_sync_state_t s = time_sync_get_state();
        lv_label_set_text(g_title, "自动校时");
        lv_label_set_text(g_clock, "--:--");
        switch (s) {
        case TIME_SYNC_CONNECTING:
            lv_label_set_text(g_body, "正在连接已保存的 Wi-Fi\n联网仅用于校准时间");
            lv_label_set_text(g_hint, "校时完成后自动关闭网络");
            break;
        case TIME_SYNC_PROVISIONING:
            lv_label_set_text(g_body,
                "首次使用，请完成配网\n\n微信小程序：\n蓝牙配网-FoloToy AI PASSPORT\n\n设备：BLUFI_FoloPassport");
            lv_label_set_text(g_hint, "请选择 2.4GHz Wi-Fi");
            break;
        case TIME_SYNC_PHONE_CONNECTED:
            lv_label_set_text(g_body, "手机已连接设备\n请在小程序中发送 Wi-Fi 信息");
            lv_label_set_text(g_hint, "等待配网");
            break;
        case TIME_SYNC_WIFI_CONNECTING:
            lv_label_set_text(g_body, "正在连接 Wi-Fi");
            lv_label_set_text(g_hint, "连接后立即校准时间");
            break;
        case TIME_SYNC_SNTP:
            lv_label_set_text(g_body, "网络已连接\n正在自动校准时间");
            lv_label_set_text(g_hint, "完成后关闭 Wi-Fi 和蓝牙");
            break;
        case TIME_SYNC_DONE:
            lv_label_set_text(g_body, "校时成功");
            lv_label_set_text(g_hint, "网络已关闭");
            break;
        case TIME_SYNC_FAILED:
            lv_label_set_text(g_body, "校时失败\n请检查网络");
            lv_label_set_text(g_hint, "OK 重试  DOWN 重新配网");
            break;
        default:
            lv_label_set_text(g_body, "准备自动校时");
            lv_label_set_text(g_hint, "请稍候");
            break;
        }
        return;
    }

    if (g_mode == UI_SET_HOUR || g_mode == UI_SET_MINUTE) {
        bool hour_page = g_mode == UI_SET_HOUR;
        lv_label_set_text(g_title, hour_page ? "设置提醒小时" : "设置提醒分钟");
        if (hour_page) lv_label_set_text_fmt(g_clock, "%02u:--", g_edit_hour);
        else lv_label_set_text_fmt(g_clock, "%02u:%02u", g_edit_hour, g_edit_minute);
        lv_label_set_text(g_body, "UP / DOWN 调整");
        lv_label_set_text(g_hint, hour_page ? "OK 下一步  长按 OK 取消" : "OK 保存  长按 OK 取消");
        return;
    }

    if (g_mode == UI_ALARM) {
        lv_obj_set_style_bg_color(g_screen, lv_color_hex(0xFFF1F2), 0);
        lv_label_set_text(g_title, "该吃药了");
        lv_label_set_text_fmt(g_clock, "%02u:%02u", g_model.reminder_hour, g_model.reminder_minute);
        lv_label_set_text(g_body, "OK  已服药\n\nUP  延后 10 分钟\nDOWN  今日跳过");
        lv_label_set_text(g_hint, "确认后自动息屏待机");
        return;
    }

    if (g_mode == UI_CONFIRM) {
        lv_label_set_text(g_title, "操作完成");
        lv_label_set_text(g_clock, "OK");
        lv_label_set_text(g_body, g_confirm);
        lv_label_set_text(g_hint, "即将息屏待机");
        return;
    }

    uint8_t h = 0, m = 0;
    int32_t day = -1;
    (void)local_now(NULL, &day, NULL, &h, &m);
    lv_label_set_text(g_title, "服药提醒");
    lv_label_set_text_fmt(g_clock, "%02u:%02u", h, m);
    lv_label_set_text_fmt(g_body, "提醒时间  %02u:%02u\n\n%s",
                          g_model.reminder_hour, g_model.reminder_minute,
                          status_text(medicine_model_day_status(&g_model, day)));
    lv_label_set_text(g_hint, "长按 OK 修改提醒\n长按 UP 校时  长按 DOWN 重新配网");
}

static void ui_init(void) {
    g_screen = lv_obj_create(NULL);
    lv_obj_set_style_border_width(g_screen, 0, 0);
    lv_obj_set_style_pad_all(g_screen, 0, 0);
    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);

    g_title = lv_label_create(g_screen);
    label_style(g_title, &lv_font_source_han_sans_sc_16_cjk, 0x2563EB);
    lv_obj_set_pos(g_title, 12, 22);

    g_clock = lv_label_create(g_screen);
    label_style(g_clock, &lv_font_montserrat_20, 0x152033);
    lv_obj_set_pos(g_clock, 12, 63);

    g_body = lv_label_create(g_screen);
    label_style(g_body, &lv_font_source_han_sans_sc_16_cjk, 0x152033);
    lv_obj_set_pos(g_body, 12, 115);
    lv_obj_set_height(g_body, 118);

    g_hint = lv_label_create(g_screen);
    label_style(g_hint, &lv_font_source_han_sans_sc_16_cjk, 0x64748B);
    lv_obj_set_pos(g_hint, 12, 252);

    render();
    lv_screen_load(g_screen);
}

static void tone_write(int hz, int duration_ms) {
    int16_t samples[AUDIO_CHUNK];
    int left = AUDIO_RATE * duration_ms / 1000;
    int period = hz ? AUDIO_RATE / hz : 1;
    int phase = 0;
    while (left > 0) {
        int n = left < AUDIO_CHUNK ? left : AUDIO_CHUNK;
        for (int i = 0; i < n; ++i) {
            samples[i] = hz ? (phase < period / 2 ? 4200 : -4200) : 0;
            if (++phase >= period) phase = 0;
        }
        if (bsp_audio_write(samples, (size_t)n * sizeof(samples[0])) != ESP_OK) break;
        left -= n;
    }
}

static void audio_task(void *arg) {
    (void)arg;
    uint8_t v;
    if (bsp_audio_set_format(AUDIO_RATE, 16, 1) != ESP_OK) {
        g_audio_ok = false;
        vTaskDelete(NULL);
        return;
    }
    bsp_audio_set_volume(60);
    for (;;) {
        if (xQueueReceive(g_audio_q, &v, portMAX_DELAY) != pdTRUE) continue;
        tone_write(880, 140);
        tone_write(0, 80);
        tone_write(1175, 180);
        tone_write(0, 80);
        tone_write(880, 220);
    }
}

static void play_tone(void) {
    if (!g_audio_ok || !g_audio_q) return;
    uint8_t v = 1;
    (void)xQueueOverwrite(g_audio_q, &v);
}

static uint8_t wrap24(int v) {
    while (v < 0) v += 24;
    while (v >= 24) v -= 24;
    return (uint8_t)v;
}

static uint8_t wrap60(int v) {
    while (v < 0) v += 60;
    while (v >= 60) v -= 60;
    return (uint8_t)v;
}

static void network_start(bool reprovision) {
    g_mode = UI_NETWORK;
    g_last_sync = (time_sync_state_t)-1;
    esp_err_t err = time_sync_start(reprovision);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "time sync start: %s", esp_err_to_name(err));
    }
    render();
}

static bool handle_key(const key_msg_t *k) {
    bool save = false;

    if (g_mode == UI_NETWORK) {
        if (time_sync_get_state() == TIME_SYNC_FAILED && k->ev == BSP_BTN_CLICK) {
            if (k->btn == BSP_BTN_OK) network_start(false);
            else if (k->btn == BSP_BTN_DOWN) network_start(true);
        }
        return false;
    }

    if (g_mode == UI_ALARM && k->ev == BSP_BTN_CLICK) {
        if (k->btn == BSP_BTN_OK && medicine_model_mark_taken(&g_model)) {
            g_confirm = "已记录服药";
            save = true;
        } else if (k->btn == BSP_BTN_UP &&
                   medicine_model_snooze(&g_model, (int64_t)time(NULL) / 60)) {
            g_confirm = "已延后 10 分钟";
        } else if (k->btn == BSP_BTN_DOWN && medicine_model_skip_today(&g_model)) {
            g_confirm = "今日已跳过";
            save = true;
        } else {
            return false;
        }
        g_mode = UI_CONFIRM;
        g_idle_deadline = ms_now() + CONFIRM_IDLE_MS;
        render();
        return save;
    }

    if (g_mode == UI_HOME) {
        reset_idle();
        if (k->ev == BSP_BTN_LONG && k->btn == BSP_BTN_OK) {
            g_edit_hour = g_model.reminder_hour;
            g_edit_minute = g_model.reminder_minute;
            g_mode = UI_SET_HOUR;
            render();
        } else if (k->ev == BSP_BTN_LONG && k->btn == BSP_BTN_UP) {
            network_start(false);
        } else if (k->ev == BSP_BTN_LONG && k->btn == BSP_BTN_DOWN) {
            network_start(true);
        }
        return false;
    }

    if ((g_mode == UI_SET_HOUR || g_mode == UI_SET_MINUTE) &&
        k->ev == BSP_BTN_LONG && k->btn == BSP_BTN_OK) {
        g_mode = UI_HOME;
        reset_idle();
        render();
        return false;
    }

    if (k->ev != BSP_BTN_CLICK && k->ev != BSP_BTN_DOUBLE) return false;
    int step = k->ev == BSP_BTN_DOUBLE ? 5 : 1;
    if (g_mode == UI_SET_HOUR) {
        if (k->btn == BSP_BTN_UP) g_edit_hour = wrap24((int)g_edit_hour + step);
        else if (k->btn == BSP_BTN_DOWN) g_edit_hour = wrap24((int)g_edit_hour - step);
        else if (k->btn == BSP_BTN_OK) g_mode = UI_SET_MINUTE;
    } else if (g_mode == UI_SET_MINUTE) {
        int mstep = k->ev == BSP_BTN_DOUBLE ? 10 : 5;
        if (k->btn == BSP_BTN_UP) g_edit_minute = wrap60((int)g_edit_minute + mstep);
        else if (k->btn == BSP_BTN_DOWN) g_edit_minute = wrap60((int)g_edit_minute - mstep);
        else if (k->btn == BSP_BTN_OK) {
            medicine_model_set_reminder(&g_model, g_edit_hour, g_edit_minute, true);
            g_mode = UI_HOME;
            reset_idle();
            save = true;
        }
    }
    render();
    return save;
}

static void key_task(void *arg) {
    (void)arg;
    key_msg_t k;
    for (;;) {
        if (xQueueReceive(g_keys, &k, portMAX_DELAY) != pdTRUE) continue;
        bool save = false;
        if (bsp_lvgl_lock(500)) {
            save = handle_key(&k);
            bsp_lvgl_unlock();
        }
        if (save) (void)medicine_store_save(&g_model);
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    if (!g_keys) return;
    key_msg_t k = {.btn = btn, .ev = ev};
    (void)xQueueSend(g_keys, &k, 0);
}

static void on_wake(void *user) {
    (void)user;
    g_woke = true;
}

static void standby(void) {
    if (!time_sync_clock_valid() || time_sync_busy() || power_manager_busy()) return;
    esp_err_t err = power_manager_sleep_until(next_wake_time());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "standby request failed: %s", esp_err_to_name(err));
        reset_idle();
    }
}

static void tick(lv_timer_t *timer) {
    (void)timer;
    uint64_t now_ms = ms_now();

    if (g_woke) {
        g_woke = false;
        g_mode = UI_HOME;
        g_last_minute = -1;
        reset_idle();
        render();
    }

    if (g_mode == UI_NETWORK) {
        time_sync_state_t s = time_sync_get_state();
        if (s != g_last_sync) {
            g_last_sync = s;
            render();
        }
        if (s == TIME_SYNC_DONE && time_sync_clock_valid()) {
            g_mode = UI_HOME;
            reset_idle();
            render();
        }
        return;
    }

    time_t epoch;
    int32_t day;
    uint16_t minute_of_day;
    uint8_t minute;
    if (!local_now(&epoch, &day, &minute_of_day, NULL, &minute)) return;

    if (medicine_model_tick(&g_model, (int64_t)epoch / 60, day, minute_of_day) ==
        MEDICINE_EVENT_ALARM) {
        g_mode = UI_ALARM;
        bsp_display_backlight(100);
        play_tone();
        render();
        return;
    }

    if (g_mode == UI_CONFIRM) {
        if (now_ms >= g_idle_deadline) standby();
        return;
    }

    if (g_mode == UI_HOME) {
        if (minute != g_last_minute) {
            g_last_minute = minute;
            render();
        }
        if (now_ms >= g_idle_deadline) standby();
    }
}

void app_main(void) {
    setenv("TZ", "CST-8", 1);
    tzset();

    medicine_model_defaults(&g_model);
    (void)medicine_store_init(&g_model);

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    bsp_display_backlight(72);

    g_keys = xQueueCreate(8, sizeof(key_msg_t));
    if (g_keys && xTaskCreate(key_task, "med_keys", 4096, NULL, 5, NULL) == pdPASS) {
        (void)bsp_button_init(on_key, NULL);
    }

    if (bsp_audio_init() == ESP_OK) {
        g_audio_q = xQueueCreate(1, sizeof(uint8_t));
        g_audio_ok = g_audio_q &&
            xTaskCreate(audio_task, "med_audio", 3072, NULL, 4, NULL) == pdPASS;
    }

    (void)power_manager_init(on_wake, NULL);

    if (bsp_lvgl_lock(1000)) {
        ui_init();
        (void)lv_timer_create(tick, 250, NULL);
        bsp_lvgl_unlock();
    }

    esp_err_t err = time_sync_start(false);
    if (err != ESP_OK) ESP_LOGE(TAG, "initial time sync: %s", esp_err_to_name(err));
}
