#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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

#define INPUT_QUEUE_DEPTH 8
#define AUDIO_QUEUE_DEPTH 1
#define AUDIO_SAMPLE_RATE 8000
#define AUDIO_CHUNK 160
#define HOME_IDLE_STANDBY_MS 30000ULL
#define CONFIRM_STANDBY_MS 5000ULL

#define COLOR_BG       0xF4F7FB
#define COLOR_CARD     0xFFFFFF
#define COLOR_TEXT     0x152033
#define COLOR_MUTED    0x64748B
#define COLOR_ACCENT   0x2563EB
#define COLOR_OK       0x16A34A
#define COLOR_WARN     0xD97706
#define COLOR_DANGER   0xDC2626
#define COLOR_SOFTBLUE 0xE8F0FF
#define COLOR_SOFTGREEN 0xEAF8EF
#define COLOR_SOFTRED  0xFDECEC

typedef enum {
    SCREEN_NETWORK = 0,
    SCREEN_HOME,
    SCREEN_SET_REMINDER_HOUR,
    SCREEN_SET_REMINDER_MINUTE,
    SCREEN_ALARM,
    SCREEN_CONFIRM,
} screen_mode_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static const char *TAG = "medicine";

static medicine_model_t s_model;
static screen_mode_t s_mode = SCREEN_NETWORK;
static uint8_t s_edit_hour;
static uint8_t s_edit_minute;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_audio_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;
static bool s_button_ok;
static bool s_audio_ok;
static volatile bool s_wake_pending;

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_time;
static lv_obj_t *s_card;
static lv_obj_t *s_primary;
static lv_obj_t *s_secondary;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;

static uint64_t s_home_idle_deadline_ms;
static uint64_t s_confirm_deadline_ms;
static const char *s_confirm_text = "е·Іи®°еЅ•";
static time_sync_state_t s_last_sync_state = (time_sync_state_t)-1;
static int s_last_display_minute = -1;
static medicine_day_status_t s_last_day_status = (medicine_day_status_t)-1;

static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static bool local_snapshot(time_t *epoch_out, int32_t *day_out,
                           uint16_t *minute_of_day_out,
                           uint8_t *hour_out, uint8_t *minute_out) {
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    struct tm tm_now;
    if (!localtime_r(&now, &tm_now)) return false;
    if (epoch_out) *epoch_out = now;
    if (day_out) {
        *day_out = (tm_now.tm_year + 1900) * 10000 +
                   (tm_now.tm_mon + 1) * 100 + tm_now.tm_mday;
    }
    uint16_t minute_of_day = (uint16_t)tm_now.tm_hour * 60U + tm_now.tm_min;
    if (minute_of_day_out) *minute_of_day_out = minute_of_day;
    if (hour_out) *hour_out = (uint8_t)tm_now.tm_hour;
    if (minute_out) *minute_out = (uint8_t)tm_now.tm_min;
    return true;
}

static int64_t epoch_minute_now(void) {
    time_t now = time(NULL);
    return now >= 0 ? (int64_t)now / 60 : -1;
}

static time_t next_wake_epoch(void) {
    time_t now = time(NULL);
    if (s_model.snooze_active && s_model.snooze_epoch_minute > 0) {
        time_t snooze = (time_t)(s_model.snooze_epoch_minute * 60);
        if (snooze > now) return snooze;
    }

    struct tm local;
    if (!localtime_r(&now, &local)) return now + 60;
    int32_t day = (local.tm_year + 1900) * 10000 + (local.tm_mon + 1) * 100 + local.tm_mday;
    medicine_day_status_t status = medicine_model_day_status(&s_model, day);

    struct tm target = local;
    target.tm_hour = s_model.reminder_hour;
    target.tm_min = s_model.reminder_minute;
    target.tm_sec = 0;
    time_t wake = mktime(&target);
    if (wake <= now + 2 || status != MEDICINE_DAY_WAITING || s_model.last_trigger_day == day) {
        target.tm_mday += 1;
        target.tm_isdst = -1;
        wake = mktime(&target);
    }
    return wake > now ? wake : now + 60;
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font,
                              uint32_t color, int width, lv_text_align_t align) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

static lv_obj_t *card_create(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void reset_home_idle(void) {
    s_home_idle_deadline_ms = now_ms() + HOME_IDLE_STANDBY_MS;
}

static const char *day_status_text(medicine_day_status_t status) {
    switch (status) {
        case MEDICINE_DAY_TAKEN: return "д»Љж—ҐзЉ¶жЂЃпјље·ІжњЌиЌЇ";
        case MEDICINE_DAY_SKIPPED: return "д»Љж—ҐзЉ¶жЂЃпјље·Іи·іиї‡";
        case MEDICINE_DAY_WAITING:
        default: return "д»Љж—ҐзЉ¶жЂЃпјљеѕ…жњЌиЌЇ";
    }
}

static void render_network(void) {
    time_sync_state_t state = time_sync_get_state();
    lv_label_set_text(s_title, "и‡ЄеЉЁж Ўж—¶");
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_bg_color(s_card, lv_color_hex(COLOR_SOFTBLUE), 0);
    lv_obj_set_style_text_color(s_primary, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(s_secondary, lv_color_hex(COLOR_MUTED), 0);

    switch (state) {
        case TIME_SYNC_CONNECTING:
            lv_label_set_text(s_primary, "ж­ЈењЁиїћжЋҐе·Ідїќе­зљ„ Wi-Fi");
            lv_label_set_text(s_secondary, "иЃ”зЅ‘д»…з”ЁдєЋж Ўе‡†ж—¶й—ґ\nж Ўж—¶е®Њж€ђеђЋдјљи‡ЄеЉЁе…ій—­зЅ‘з»њ");
            lv_label_set_text(s_hint, "иЇ·зЁЌеЂ™");
            break;
        case TIME_SYNC_PROVISIONING:
            lv_label_set_text(s_primary, "й¦–ж¬ЎдЅїз”ЁпјЊиЇ·е®Њж€ђй…ЌзЅ‘");
            lv_label_set_text(s_secondary,
                              "ж‰‹жњєж‰“ејЂеѕ®дїЎе°ЏзЁ‹еєЏ\nвЂњи“ќз‰™й…ЌзЅ‘-FoloToy AI PASSPORTвЂќ\nйЂ‰ж‹© BLUFI_FoloPassport\nиїћжЋҐ 2.4GHz Wi-Fi");
            lv_label_set_text(s_hint, "й…ЌзЅ‘ж€ђеЉџеђЋдјљи‡ЄеЉЁж Ўж—¶");
            break;
        case TIME_SYNC_PHONE_CONNECTED:
            lv_label_set_text(s_primary, "ж‰‹жњєе·ІиїћжЋҐи®ѕе¤‡");
            lv_label_set_text(s_secondary, "иЇ·ењЁе°ЏзЁ‹еєЏдё­йЂ‰ж‹© 2.4GHz Wi-Fi\nе№¶еЏ‘йЂЃеЇ†з Ѓ");
            lv_label_set_text(s_hint, "з­‰еѕ… Wi-Fi дїЎжЃЇ");
            break;
        case TIME_SYNC_WIFI_CONNECTING:
            lv_label_set_text(s_primary, "ж­ЈењЁиїћжЋҐ Wi-Fi");
            lv_label_set_text(s_secondary, "иїћжЋҐж€ђеЉџеђЋе°†з«‹еЌіж Ўе‡†ж—¶й—ґ");
            lv_label_set_text(s_hint, "иЇ·зЁЌеЂ™");
            break;
        case TIME_SYNC_SNTP:
            lv_label_set_text(s_primary, "зЅ‘з»ње·ІиїћжЋҐ");
            lv_label_set_text(s_secondary, "ж­ЈењЁд»ЋзЅ‘з»њи‡ЄеЉЁж Ўе‡†ж—¶й—ґ");
            lv_label_set_text(s_hint, "ж Ўж—¶еђЋи‡ЄеЉЁе…ій—­ Wi-Fi е’Њи“ќз‰™");
            break;
        case TIME_SYNC_DONE:
            lv_label_set_text(s_primary, "ж Ўж—¶ж€ђеЉџ");
            lv_label_set_text(s_secondary, "зЅ‘з»ње·Іе…ій—­пјЊиї›е…ҐзњЃз”µиїђиЎЊ");
            lv_label_set_text(s_hint, "еЌіе°‡иї›е…ҐжЏђй†’йЎµйќў");
            break;
        case TIME_SYNC_FAILED:
            lv_obj_set_style_bg_color(s_card, lu_color_hex(COLOR_SOFTRED), 0);
            lv_label_set_text(s_primary, "ж Ўж—¶е¤±иґҐ");
            lv_label_set_text(s_secondary, "иЇ·жЈЂжџҐзЅ‘з»њеђЋй‡ЌиЇ•");
            lv_label_set_text(s_hint, "OK й‡ЌиЇ•   DOWN й‡Ќж–°й…ЌзЅ‘");
            break;
        case TIME_SYNC_IDLE:
        default:
            lv_label_set_text(s_primary, "е‡†е¤‡и‡ЄеЉЁж Ўж—¶");
            lv_label_set_text(s_secondary, "з”µзЅ‘еЏЄз”ЁдєЋиЋ·еЏ–е‡†зЎ®ж—¶й—ґ");
            lv_label_set_text(s_hint, "иЇ·зЁЌеЂ™");
            break;
    }
}

static void render_home(void) {
    uint8_t hour = 0, minute = 0;
    int32_t day = -1;
    (void)local_snapshot(NULL, &day, NULL, &hour, &minute);
    medicine_day_status_t status = medicine_model_day_status(&s_model, day);

    lv_label_set_text(s_title, "жњЌиЌЇ йЂљйЃ“");
    lu_label_set_text_fmt(s_time, "%02u:%02u", hour, minute);
    lv_obj_set_style_bg_color(s_card, lu_color_hex(COLOR_CARD), 0);
    lv_obj_set_style_text_color(s_primary, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(s_secondary, lv_color_hex(status == MEDICINE_DAY_TAKEN ? COLOR_OK : COLOR_MUTED), 0);
    lu_label_set_text_fmt(s_primary, "жЏђй†’ж—¶й—ґ  %02u:%02u", s_model.reminder_hour, s_model.reminder_minute);
    lv_label_set_text(s_secondary, day_status_text(status));
    lu_label_set_text(s_hint, "й•їжЊ‰ OK дї®ж”№жЏђй†’ж—¶й—ґ\nй•їжЊ‰ UP ж Ўж—¶   й•їжЊ‰ DOWN й‡Ќж–°й…ЌзЅ‘");
}

static void render_setting(void) {
    bool hour_page = s_mode == SCREEN_SET_REMINDER_HOUR;
    lv_label_set_text(s_title, hour_page ? "и®®зЅ®жЏђй†’е°Џж—¶" : "и®ѕе®љжЏђй†’е€†й’џв");
    lv_label_set_text_fmt(s_time, hour_page ? "%02u:--" : "%02u:%02u", s_edit_hour, s_edit_minute);
    lu_obj_set_style_bg_color(s_card, lv_color_hex(COLOR_SOFTBLUE), 0);
    lu_obj_set_style_text_color(s_primary, lu_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_color(s_secondary, lu_color_hex(COLOR_MUTED), 0);
    lv_label_set_text(s_primary, "UP / DOWN ијѓж•ґ");
    lv_label_set_text(s_secondary, hour_page ? "OK дё‹дёЂйЎ№" : "OK дїќе­");
    lv_label_set_text(s_hint, "й•їжЊ‰ OK еЏ–ж¶€ИЉNВџB‚њЭ]XИ›ЪY™[™\—Ш[\›J›ЪY
HВ€WЫШљ—ЬЩ]ЬЭ[WШ™ЧШЫЫЬЉЧЬШЬ™Y[‹WШЫЫЬ—Ъ^
‘‘ђQЌ
K
NВ€—ЫX™[ЬЩ]Э^
ЧЭ]Kє+йyd ъ#kщ.ўИЉNВ€—ЫX™[ЬЩ]Э^Щ›]
ЧЭ[YK‰LќN‰LќH‹ЧЫ[Щ[њ™[Z[™\—ЪЭ\‹ЧЫ[Щ[њ™[Z[™\—ЫZ[ќ]JNВ€—ЫШљ—ЬЩ]ЬЭ[WШ™ЧШЫЫЬЉЧШШ\™WШЫЫЬ—Ъ^
УУФ—ФУС•‘Q
K
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЬљ[X\ћK—ШЫЫЬ—Ъ^
УУФ—СS‘СTЉK
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЬЩXЫЫ™\ћKWШЫЫЬ—Ъ^
УУФ—ХV
K
NВ€—ЫX™[ЬЩ]Э^
ЧЬљ[X\ћK“ТИ9mм№§#z#kИЉNВ€—ЫX™[ЬЩ]Э^
ЧЬЩXЫЫ™\ћK•T9nн№d#€L9b!єd§Ч‘ХУ€9.в№Ґйz-мъ/бИЉNВ€—ЫX™[ЬЩ]Э^
ЧЪ[ќ№иkє+©9d#є!к№bЄ9 kщlcщoЎy§.€ЉNВџB‚њЭ]XИ›ЪY™[™\—ШЫЫ™љ\›J›ЪY
HВ€WЫX™[ЬЩ]Э^
ЧЭ]K№¤гy/g9kЈ9ў$ЉNВ€WЫX™[ЬЩ]Э^
ЧЭ[YK“ТИЉNВ€—ЫШљ—ЬЩ]ЬЭ[WШ™ЧШЫЫЬЉЧШШ\™—ШЫЫЬ—Ъ^
УУФ—ФУС•Ф‘QSЉK
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЬљ[X\ћK—ШЫЫЬ—Ъ^
УУФ—УТКK
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЬЩXЫЫ™\ћKWШЫЫЬ—Ъ^
УУФ—УUUQ
K
NВ€—ЫX™[ЬЩ]Э^
ЧЬљ[X\ћKЧШЫЫ™љ\›WЭ^
NВ€Z[ќЌЭ™[XZ[€HЧШЫЫ™љ\›WЩXY[™WЫ\И€›ЭЧЫ\К
B€И
ЧШЫЫ™љ\›WЩXY[™WЫ\ИH›ЭЧЫ\К
H
ИNNJHИL€€В€—ЫX™[ЬЩ]Э^Щ›]
ЧЬЩXЫЫ™\ћK‰[H9йд№d#є!к№bЄ9 kщlcщoЎy§.€‹€
[њЪYЫ™YЫ™ИЫ™К\™[XZ[ЉNВ€WЫX™[ЬЩ]Э^
ЧЪ[ќ№o!y§.№Ґн€ЪKQљKъ$зyвfy/зyЈ yalъeлHЉNВџB‚њЭ]XИ›ЪY™[™\—ЭZJ›ЪY
HВ€Y€
\ЧЬШЬ™Y[ЉH™]\›ЋВ€WЫШљ—ЬЩ]ЬЭ[WШ™ЧШЫЫЬЉЧЬШЬ™Y[‹WШЫЫЬ—Ъ^
УУФ—Р‘КK
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЭ]KWШЫЫЬ—Ъ^
УУФ—РPРСS•
K
NВ€WЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЭ[YKWШЫЫЬ—Ъ^
УУФ—ХV
K
NВ€—ЫШљ—ЬЩ]ЬЭ[WЭ^ШЫЫЬЉЧЪ[ќ—ШЫЫЬ—Ъ^
УУФ—УUUQ
K
NВ‚€ЭЪ]Ъ
ЧЫ[ЩJHВ€Ш\ЩHРФ‘QS—У‘UУФ’О€™[™\—Ы™]ЫЬљК
NИњ™XZОВ€Ш\ЩHРФ‘QS—ТУQN€™[™\—ЪЫYJ
NИњ™XZОВ€Ш\ЩHРФ‘QS—ФСUФ‘SRS‘T—ТХTЋ‚€Ш\ЩHРФ‘QS—ФСUФ‘SRS‘T—УRS•UN€™[™\—ЬЩ][™К
NИњ™XZОВ€Ш\ЩHРФ‘QS—РST“N€™[™\—Ш[\›J
NИњ™XZОВ€Ш\ЩHРФ‘QS—РУУ‘’T“N€™[™\—ШЫЫ™љ\›J
NИњ™XZОВ€BџB‚њЭ]XИ›ЪYќZ[ЭZJ›ЪY
HВ€ЧЬШЬ™Y[€H—ЫШљ—ШЬ™X]J•S
NВ€WЫШљ—ЬЩ]ЬЭ[WШ™ЧШЫЫЬЉЧЬШЬ™Y[‹WШЫЫЬ—Ъ^
УУФ—Р‘КK
NВ€—ЫШљ—ЬЩ]ЬЭ[WШ™ЧЫЬJЧЬШЬ™Y[‹УФWРУХ‘T‹
NВ€—ЫШљ—ЬЩ]ЬЭ[WШ›Ь™\—ЭЪY
ЧЬШЬ™Y[‹
NВ€—ЫШљ—ЬЩ]ЬЭ[WЬYШ[
ЧЬШЬ™Y[‹
NВ€—ЫШљ—ШЫX\—Щ›YКЧЬШЬ™Y[‹УР’—С“QЧФРФ“УP“JNВ‚€ЧЭ]HHX™[ШЬ™X]JЧЬШЬ™Y[‹	›—Щ›ЫќЬЫЭ\ЩWЪ[—ЬШ[њЧЬШЧМM—ШЪљЛ€УУФ—РPРСS•ЊЊХVРSQУ—РСS•TЉNВ€WЫШљ—ЬЩ]ЬЬКЧЭ]KLЊЉNВ‚€ЧЭ[YHHX™[ШЬ™X]JЧЬШЬ™Y[‹	›—Щ›ЫќЫ[ЫќЩ\њ]МЊ€УУФ—ХVЊЊ—ХVРSQУ—РСS•TЉNВ€—ЫШљ—ЬЩ]ЬЬКЧЭ[YKLЊКNВ‚€ЧШШ\™HШ\™ШЬ™X]JЧЬШЬ™Y[‹MLL‹ЊL‹LЊУУФ—РРT‘
NВ€ЧЬљ[X\ћHHX™[ШЬ™X]JЧШШ\™	›—Щ›ЫќЬЫЭ\ЩWЪ[—ЬШ[њЧЬШЧМM—ШЪљЛ€УУФ—ХVN—ХVРSQУ—РСS•TЉNВ€—ЫШљ—Ш[YЫЉЧЬљ[X\ћK—РSQУ—ХФУRQ
NВ€ЧЬЩXЫЫ™\ћHHX™[ШЬ™X]JЧШШ\™	›—Щ›ЫќЬЫЭ\ЩWЪ[—ЬШ[њЧЬШЧМM—ШЪљЛ€УУФ—УUUQN—ХVРSQУ—РСS•TЉNВ€—ЫШљ—Ш[YЫЉЧЬЩXЫЫ™\ћKРSQУ—ХФУRQ
NВ‚€ЧЪ[ќHX™[ШЬ™X]JЧЬШЬ™Y[‹	›—Щ›ЫќЬЫЭ\ЩWЪ[—ЬШ[њЧЬШЧМM—ШЪљЛ€УУФ—УUUQЊЊХVРSQУ—РСS•TЉNВ€WЫШљ—ЬЩ]ЬЬКЧЪ[ќLЌL
NВ‚€™[™\—ЭZJ
NВ€—ЬШЬ™Y[—ЫШY
ЧЬШЬ™Y[ЉNВџB‚њЭ]XИ›ЪY]Y[ЧЭЬљ]WЫ›ЭJ[ќњ™\]Y[ЮK[ќ\][Ы—Ы\КHВ€[ќM—ЭШ[\\ЦРUQSЧРТS’ЧNВ€[ќЭ[HUQSЧФРSTWФђUH
€\][Ы—Ы\ИИLВ€[ќ\љ[ЩHњ™\]Y[ЮH€ИUQSЧФРSTWФђUHИњ™\]Y[ЮH€NВ€[ќ\ЩHHВ€Ъ[H
Э[€
HВ€[ќЫЭ[ќHЭ[UQSЧРТS’ИИЭ[€UQSЧРТS’ОВ€›Ь€
[ќHHИHЫЭ[ќИ
КЪJHВ€Ш[\\ЦЪWHHњ™\]Y[ЮHOHИ€
\ЩH\љ[ЩИ€ИЊ€MЊ
NВ€Y€

КЬ\ЩHЏH\љ[Щ
H\ЩHHВ€B€Y€
њЬШ]Y[ЧЭЬљ]JШ[\\Л
Ъ^™WЭ
XЫЭ[ќ
€Ъ^™[ЩЉШ[\\ЦМJJHOHTФУТКHњ™XZОВ€Э[OHЫЭ[ќВ€BџB‚њЭ]XИ›ЪY]Y[ЧЭ\ЪК›ЪY
\™КHВ€
›ЪY
X\™ОВ€Z[ќЭ]™[ќВ€Y€
њЬШ]Y[ЧЬЩ]Щ›Ь›X]
UQSЧФРSTWФђUKM‹JHOHTФУТКHВ€ЧШ]Y[ЧЫЪИH[ЩNВ€•\ЪС[]J•S
NВ€™]\›ЋВ€B€њЬШ]Y[ЧЬЩ]Э›Ы[YJЊ
NВ€›Ь€
ОКHВ€Y€
]Y]YT™XЩZ]™JЧШ]Y[ЧЬ]Y]YK	™]™[ќЬќPVСSVJHOH•QJHЫЫќ[ќYNВ€
›ЪY
Y]™[ќВ€]Y[ЧЭЬљ]WЫ›ЭJM
NВ€]Y[ЧЭЬљ]WЫ›ЭJ
NВ€]Y[ЧЭЬљ]WЫ›ЭJLMНKMЊ
NВ€]Y[ЧЭЬљ]WЫ›ЭJ
NВ€]Y[ЧЭЬљ]WЫ›ЭJЊЊ
NВ€BџB‚њЭ]XИ›ЪY^WШ[\›WЭЫ™J›ЪY
HВ€Y€
\ЧШ]Y[ЧЫЪИ\ЧШ]Y[ЧЬ]Y]YJH™]\›ЋВ€Z[ќЭ]™[ќHNВ€
›ЪY
^]Y]YSЭ™\ќЬљ]JЧШ]Y[ЧЬ]Y]YK	™]™[ќ
NВџB‚њЭ]XИZ[ќЭЬ\ЪЭ\Љ[ќ[YJHВ€Ъ[H
[YH
H[YH
ПHЌВ€Ъ[H
[YHЏHЌ
H[YHOHЌВ€™]\›€
Z[ќЭ
][YNВџB‚њЭ]XИZ[ќЭЬ\ЫZ[ќ]J[ќ[YJHВ€Ъ[H
[YH
H[YH
ПHЊВ€Ъ[H
[YHЏHЊ
H[YHOHЊВ€™]\›€
Z[ќЭ
][YNВџB‚њЭ]XИ›ЪYЭ\ќЫ™]ЫЬљК›ЫЫ›ЬЩWЬ›Эљ\Ъ[ЫЉHВ€ЧЫ[ЩHHРФ‘QS—У‘UУФ’ОВ€ЧЫ\ЭЬЮ[ЧЬЭ]HH
[YWЬЮ[ЧЬЭ]WЭ
KLNВ€\ЬЩ\њ—Э\њ€H[YWЬЮ[ЧЬЭ\ќ
›ЬЩWЬ›Эљ\Ъ[ЫЉNВ€Y€
\њ€OHTФУТИ	‰€\њ€OHTФСT”—ТS•ђSQФХU
HВ€TФУССJQЛђШ[››ЭЭ\ќ[YHЮ[О€	\И‹\ЬЩ\њ—ЭЧЫ[YJ\њЉJNВ€B€™[™\—ЭZJ
NВџB‚њЭ]XИ›ЫЫ[™WЪ[њ]ЫШЪЩY
ЫЫњЭ[њ]Щ]™[ќЭ
љ[њ]
HВ€›ЫЫШ]™HH[ЩNВ‚€Y€
ЧЫ[ЩHOHРФ‘QS—У‘UУФ’КHВ€Y€
[YWЬЮ[ЧЩЩ]ЬЭ]J
HOHSQWФЦSђЧСђRSQ	‰€[њ]O™]™[ќOH”ФР•—РУPТКHВ€Y€
[њ]Oќ€OH”ФР•—УТКHЭ\ќЫ™]ЫЬљК[ЩJNВ€[ЩHY€
[њ]Oќ€OH”ФР•—СХУЉHЭ\ќЫ™]ЫЬљКќYJNВ€B€™]\›€[ЩNВ€B‚€Y€
ЧЫ[ЩHOHРФ‘QS—РST“H	‰€[њ]O™]™[ќOH”ФР•—РУPТКHВ€[ќЌЭ\ШЪЫZ[ќ]HH\ШЪЫZ[ќ]WЫ›ЭК
NВ€Y€
[њ]Oќ€OH”ФР•—УТИ	‰€YYXЪ[™WЫ[Щ[ЫX\љЧЭZЩ[Љ	њЧЫ[Щ[
JHВ€ЧШЫЫ™љ\›WЭ^H№mмє+¬9oey§#z#kИЋВ€Ш]™HHќYNВ€H[ЩHY€
[њ]Oќ€OH”ФР•—ХT	‰€YYXЪ[™WЫ[Щ[ЬЫ›ЫЮ™J	њЧЫ[Щ[\ШЪЫZ[ќ]JJHВ€ЧШЫЫ™љ\›WЭ^H№mм№nн№d#€L9b!єd§ИЋВ€H[ЩHY€
[њ]Oќ€OH”ФР•—СХУ€	‰€YYXЪ[™WЫ[Щ[ЬЪЪ\ЭЩ^J	њЧЫ[Щ[
JHВ€ЧШЫЫ™љ\›WЭ^H№.в№Ґйymмє-мъ/бИЋВ€Ш]™HHќYNВ€H[ЩHВ€™]\›€[ЩNВ€B€ЧЫ[ЩHHРФ‘QS—РУУ‘’T“NВ€ЧШЫЫ™љ\›WЩXY[™WЫ\ИH›ЭЧЫ\К
H
ИУУ‘’T“WФХS‘–WУTОВ€™[™\—ЭZJ
NВ€™]\›€Ш]™NВ€B‚€Y€
ЧЫ[ЩHOHРФ‘QS—ТУQJHВ€™\Щ]ЪЫYWЪYJ
NВ€Y€
[њ]O™]™[ќOH”ФР•—УУ‘И	‰€[њ]Oќ€OH”ФР•—УТКHВ€ЧЩY]ЪЭ\€HЧЫ[Щ[њ™[Z[™\—ЪЭ\ЋВ€ЧЩY]ЫZ[ќ]HHЧЫ[Щ[њ™[Z[™\—ЫZ[ќ]NВ€ЧЫ[ЩHHРФ‘QS—ФСUФ‘SRS‘T—ТХTЋВ€™[™\—ЭZJ
NВ€H[ЩHY€
[њ]O™]™[ќOH”ФР•—УУ‘И	‰€[њ]Oќ€OH”ФР•—ХT
HВ€Э\ќЫ™]ЫЬљК[ЩJNВ€H[ЩHY€
[њ]O™]™[ќOH”ФР•—УУ‘И	‰€[њ]Oќ€OH”ФР•—СХУЉHВ€Э\ќЫ™]ЫЬљКќYJNВ€B€™]\›€[ЩNВ€B‚€Y€

ЧЫ[ЩHOHРФ‘QS—ФСUФ‘SRS‘T—ТХT€ЧЫ[ЩHOHРФ‘QS—ФСUФ‘SRS‘T—УRS•UJH	‰‚€[њ]O™]™[ќOH”ФР•—УУ‘И	‰€[њ]Oќ€OH”ФР•—УТКHВ€ЧЫ[ЩHHРФ‘QQS—ТУQNВ€™\Щ]ЪЫYWЪYJ
NВ€™[™\—ЭZJ
NВ€™]\›€[ЩNВ€B‚€Y€
[њ]O™]™[ќOH”ФР•—РУPТИ	‰€[њ]O™]™[ќOH”ФР•—СХP“JH™]\›€[ЩNВ€[ќЭ\H[њ]O™]™[ќOH”ФР•—СХP“HИH€NВ€Y€
ЧЫ[ЩHOHРФ‘QS—ФСUФ‘SRS‘T—ТХTЉHВ€Y€
[њ]Oќ€OH”ФР•—ХT
HЧЩY]ЪЭ\€HЬ\ЪЭ\Љ
[ќ
\ЧЩY]ЪЭ\€
ИЭ\
NВ€[ЩHY€
[њ]Oќ€OH”ФР•—СХУЉHЧЩY]ЪЭ\€HЬ\ЪЭ\Љ
[ќ
\ЧЩY]ЪЭ\€HЭ\
NВ€[ЩHY€
[њ]Oќ€OH”ФР•—УТКHЧЫ[ЩHHРФ‘QS—ФСUФ‘SRS‘T—УRS•UNВ€H[ЩHY€
ЧЫ[ЩHOHРФ‘QQS—ФСUФ‘SRS‘T—УRS•UJHВ€[ќZ[ќ]WЬЭ\H[њ]O™]™[ќOH”ФР•—СХP“HИL€NВ€Y€
[њ]Oќ€OH”ФР•—ХT
HЧЩY]ЫZ[ќ]HHЬ\ЫZ[ќ]J
[ќ
\ЧЩY]ЫZ[ќ]H
ИZ[ќ]WЬЭ\
NВ€[ЩHY€
[њ]Oќ€OH”ФР•—СХУЉHЧЩY]ЫZ[ќ]HHЬ\ЫZ[ќ]J
[ќ
\ЧЩY]ЫZ[ќ]HHZ[ќ]WЬЭ\
NВ€[ЩHY€
[њ]Oќ€OH”ФР•—УТКHВ€YYXЪ[™WЫ[Щ[ЬЩ]Ь™[Z[™\Љ	њЧЫ[Щ[ЧЩY]ЪЭ\‹ЧЩY]ЫZ[ќ]KќYJNВ€Ш]™HHќYNВ€ЧЫ[ЩHHРФ‘QQS—ТУQNВ€™\Щ]ЪЫYWЪYJ
NВ€B€B€™[™\—ЭZJ
NВ€™]\›€Ш]™NВџB‚њЭ]XИ›ЪY[њ]Э\ЪК›ЪY
\™КHВ€
›ЪY
X\™ОВ€[њ]Щ]™[ќЭ[њ]В€›Ь€
ОКHВ€Y€
]Y]YT™XЩZ]™JЧЪ[њ]Ь]Y]YK	љ[њ]ЬќPVСSVJHOH•QJHЫЫќ[ќYNВ€›ЫЫШ]™HH[ЩNВ€Y€
њЬЫ™ЫЫШЪКL
JHВ€Ш]™HH[™WЪ[њ]ЫШЪЩY
	љ[њ]
NВ€њЬЫ™ЫЭ[›ШЪК
NВ€B€Y€
Ш]™JHВ€\ЬЩ\њ—Э\њ€HYYXЪ[™WЬЭЬ™WЬШ]™J	њЧЫ[Щ[
NВ€Y€
\њ€OHTФУТКHTФУСХКQЛ”Ш]™HZ[Y€	\И‹\ЬЩ\њ—ЭЧЫ[YJ\њЉJNВ€B€BџB‚њЭ]XИ›ЪYЫ—ЪЩ^JњЬШќ—Эќ‹њЬШќ—Щ]—Э]™[ќ›ЪY
ќ\Щ\ЉHВ€
›ЪY
]\Щ\ЋВ€Y€
\ЧЪ[њ]Ь™XYH\ЧЪ[њ]Ь]Y]YJH™]\›ЋВ€ЫЫњЭ[њ]Щ]™[ќЭ[њ]HЛќ€Hќ‹™]™[ќH]™[ќNВ€
›ЪY
^]Y]YTЩ[™
ЧЪ[њ]Ь]Y]YK	љ[њ]
NВџB‚њЭ]XИ›ЪYЫ—ЬЭЩ\—ЭШZЩJ›ЪY
ќ\Щ\ЉHВ€
›ЪY
]\Щ\ЋВ€ЧЭШZЩWЬ[™[™ИHќYNВџB‚њЭ]XИ›ЪY[ќ\—ЬЭ[™ћJ›ЪY
HВ€Y€
][YWЬЮ[ЧШЫШЪЧЭ[Y

H[YWЬЮ[ЧШќ\ЮJ
HЭЩ\—ЫX[YЩ\—Шќ\ЮJ
JH™]\›ЋВ€[YWЭШZЩHH™^ЭШZЩWЩ\ШЪ

NВ€TФУСТJQЛ”Э[™ћH[ќ[\ШЪI[‹
Ы™ИЫ™К]ШZЩJNВ€\ЬЩ\њ—Э\њ€HЭЩ\—ЫX[YЩ\—ЬЫY\Э[ќ[
ШZЩJNВ€Y€
\њ€OHTФУТКHВ€TФУССJQЛ”Э[™ћH™\]Y\ЭZ[Y€	\И‹\ЬЩ\њ—ЭЧЫ[YJ\њЉJNВ€ЧЫ[ЩHHРФ‘QS—ТУQNВ€™\Щ]ЪЫYWЪYJ
NВ€™[™\—ЭZJ
NВ€BџB‚њЭ]XИ›ЪYXЪК—Э[Y\—Э
ќ[Y\ЉHВ€
›ЪY
][Y\ЋВ€Z[ќЌЭ\ИH›ЭЧЫ\К
NВ‚€Y€
ЧЭШZЩWЬ[™[™КHВ€ЧЭШZЩWЬ[™[™ИH[ЩNВ€ЧЫ[ЩHHРФ‘QS—ТУQNВ€™\Щ]ЪЫYWЪYJ
NВ€ЧЫ\ЭЩ\Ь^WЫZ[ќ]HHLNВ€B‚€Y€
ЧЫ[ЩHOHРФ‘QS—У‘UУФ’КHВ€[YWЬЮ[ЧЬЭ]WЭЭ]HH[YWЬЮ[ЧЩЩ]ЬЭ]J
NВ€Y€
Э]HOHЧЫ\ЭЬЮ[ЧЬЭ]JHВ€ЧЫ\ЭЬЮ[ЧЬЭ]HHЭ]NВ€™[™\—ЭZJ
NВ€B€Y€
Э]HOHSQWФЦSђЧСУ‘H	‰€[YWЬЮ[ЧШЫШЪЧЭ[Y

JHВ€ЧЫ[ЩHHРФ‘QS—ТУQNВ€™\Щ]ЪЫYWЪYJ
NВ€™[™\—ЭZJ
NВ€B€™]\›ЋВ€B‚€[YWЭ\ШЪВ€[ќМ—Э^NВ€Z[ќM—ЭZ[ќ]WЫЩ—Щ^NВ€Z[ќЭZ[ќ]NВ€Y€
[ШШ[ЬЫ\ЪЭ
	™\ШЪ	™^K	›Z[ќ]WЫЩ—Щ^K•S	›Z[ќ]JJH™]\›ЋВ‚€YYXЪ[™WЩ]™[ќЭ]™[ќHYYXЪ[™WЫ[Щ[ЭXЪК	њЧЫ[Щ[
[ќЌЭ
Y\ШЪИЊ€^KZ[ќ]WЫЩ—Щ^JNВ€Y€
]™[ќOHQQPТS‘WСU‘S•РST“JHВ€ЧЫ[ЩHHРФ‘QS—РST“NВ€њЬЩ\Ь^WШXЪЫYЪ
L
NВ€^WШ[\›WЭЫ™J
NВ€™[™\—ЭZJ
NВ€™]\›ЋВ€B‚€Y€
ЧЫ[ЩHOHРФ‘QS—РУУ‘’T“JHВ€Y€
\ИЏHЧШЫЫ™љ\›WЩXY[™WЫ\КHВ€[ќ\—ЬЭ[™ћJ
NВ€H[ЩHВ€™[™\—ШЫЫ™љ\›J
NВ€B€™]\›ЋВ€B‚€Y€
ЧЫ[ЩHOHРФ‘QS—ТУQJHВ€YYXЪ[™WЩ^WЬЭ]\ЧЭЭ]\ИHYYXЪ[™WЫ[Щ[Щ^WЬЭ]\К	њЧЫ[Щ[^JNВ€Y€
Z[ќ]HOHЧЫ\ЭЩ\Ь^WЫZ[ќ]HЭ]\ИOHЧЫ\ЭЩ^WЬЭ]\КHВ€ЧЫ\ЭЩ\Ь^WЫZ[ќ]HHZ[ќ]NВ€ЧЫ\ЭЩ^WЬЭ]\ИHЭ]\ОВ€™[™\—ЭZJ
NВ€B€Y€
\ИЏHЧЪЫYWЪYWЩXY[™WЫ\КH[ќ\—ЬЭ[™ћJ
NВ€BџB‚њЭ]XИ\ЬЩ\њ—Э[њ]Ъ[љ]
›ЪY
HВ€ЧЪ[њ]Ь]Y]YHH]Y]YPЬ™X]JS”UФUQUQWСTЪ^™[ЩЉ[њ]Щ]™[ќЭ
JNВ€Y€
\ЧЪ[њ]Ь]Y]YJH™]\›€TФСT”—У“ЧУQSNВ€Y€
\ЪРЬ™X]J[њ]Э\ЪЛ›YYXЪ[™WЪ[њ]‹M‹•SK	њЧЪ[њ]Э\ЪКHOHTФКHВ€”]Y]YQ[]JЧЪ[њ]Ь]Y]YJNВ€ЧЪ[њ]Ь]Y]YHH•SВ€™]\›€TФСT”—У“ЧУQSNВ€B€™]\›€TФУТОВџB‚њЭ]XИ›ЪY]Y[ЧЪ[љ]ЫЬ[Ы[
›ЪY
HВ€Y€
њЬШ]Y[ЧЪ[љ]

HOHTФУТКHВ€TФУСХКQЛђ]Y[И[]Z[X›NИљ\ЭX[™[Z[™\€™[XZ[њИ]Z[X›HЉNВ€™]\›ЋВ€B€ЧШ]Y[ЧЬ]Y]YHH]Y]YPЬ™X]JUQSЧФUQUQWСTЪ^™[ЩЉZ[ќЭ
JNВ€ЧШ]Y[ЧЫЪИHЧШ]Y[ЧЬ]Y]YHOH•SВ€Y€
\ЧШ]Y[ЧЬ]Y]YH\ЪРЬ™X]J]Y[ЧЭ\ЪЛ›YYXЪ[™WШ]Y[И‹ММ‹•S•S
HOHTФКHВ€ЧШ]Y[ЧЫЪИH[ЩNВ€Y€
ЧШ]Y[ЧЬ]Y]YJHВ€”]Y]YQ[]JЧШ]Y[ЧЬ]Y]YJNВ€ЧШ]Y[ЧЬ]Y]YHH•SВ€B€BџB‚ќ›ЪY\ЫXZ[Љ›ЪY
HВ€TФУСТJQЛ“YYXЪ[™H™[Z[™\€Њ€Э\ќ[™ИЉNВ€Щ][ќЉ•€‹ђФХN‹JNВ€њЩ]

NВ‚€YYXЪ[™WЫ[Щ[ЩY][К	њЧЫ[Щ[
NВ€\ЬЩ\њ—ЭЭЬ™WЩ\њ€HYYXЪ[™WЬЭЬ™WЪ[љ]
	њЧЫ[Щ[
NВ€Y€
ЭЬ™WЩ\њ€OHTФУТКHВ€TФУСХКQЛ”\њЪ\Э[ќЩ][™ЬИ[]Z[X›N€	\И‹\ЬЩ\њ—ЭЧЫ[YJЭЬ™WЩ\њЉJNВ€B‚€Y€
њЬЩ\Ь^WЪ[љ]

HOHTФУТИXњЬЫ™ЫЪ[љ]

JHВ€TФУССJQЛ‘\Ь^KУ‘У[љ]Z[YЉNВ€™]\›ЋВ€B€њЬЩ\Ь^WШXЪЫYЪ
МЉNВ‚€\ЬЩ\њ—Э[њ]Щ\њ€H[њ]Ъ[љ]

NВ€Y€
[њ]Щ\њ€OHTФУТКHЧШќ]Ы—ЫЪИHњЬШќ]Ы—Ъ[љ]
Ы—ЪЩ^K•S
HOHTФУТОВ€Y€
\ЧШќ]Ы—ЫЪКHTФУССJQЛђќ]Ы€[њ][]Z[X›HЉNВ‚€]Y[ЧЪ[љ]ЫЬ[Ы[

NВ€
›ЪY
\ЭЩ\—ЫX[YЩ\—Ъ[љ]
Ы—ЬЭЩ\—ЭШZЩK•S
NВ‚€Y€
њЬЫ™ЫЫШЪКL
JHВ€ќZ[ЭZJ
NВ€ЧЭ[Y\€H—Э[Y\—ШЬ™X]JXЪЛЌL•S
NВ€њЬЫ™ЫЭ[›ШЪК
NВ€B€ЧЪ[њ]Ь™XYHHЧШќ]Ы—ЫЪОВ‚€\ЬЩ\њ—ЭЮ[ЧЩ\њ€H[YWЬЮ[ЧЬЭ\ќ
[ЩJNВ€Y€
Ю[ЧЩ\њ€OHTФУТКHВ€TФУССJQЛ’[љ]X[[YHЮ[ИЭ\ќZ[Y€	\И‹\ЬЩ\њ—ЭЧЫ[YJЮ[ЧЩ\њЉJNВ€BџB