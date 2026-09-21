#include "medicine_model.h"

#include <stddef.h>

#define MINUTES_PER_DAY 1440ULL
#define MS_PER_MINUTE 60000ULL

static bool valid_hm(uint8_t hour, uint8_t minute) {
    return hour < 24 && minute < 60;
}

static uint64_t abs_minute_now(const medicine_model_t *model, uint64_t now_ms) {
    if (!model->clock_valid) return 0;
    uint64_t elapsed_ms = now_ms >= model->base_ms ? now_ms - model->base_ms : 0;
    return (uint64_t)model->base_minute_of_day + elapsed_ms / MS_PER_MINUTE;
}

void medicine_model_defaults(medicine_model_t *model) {
    if (!model) return;
    *model = (medicine_model_t){
        .version = MEDICINE_MODEL_VERSION,
        .reminder_hour = 8,
        .reminder_minute = 0,
        .reminder_enabled = true,
        .last_trigger_day = -1,
        .last_taken_day = -1,
        .last_skipped_day = -1,
        .alarm_day = -1,
    };
}

bool medicine_model_set_reminder(medicine_model_t *model, uint8_t hour, uint8_t minute, bool enabled) {
    if (!model || !valid_hm(hour, minute)) return false;
    model->reminder_hour = hour;
    model->reminder_minute = minute;
    model->reminder_enabled = enabled;
    model->snooze_active = false;
    model->alarm_active = false;
    return true;
}

bool medicine_model_set_clock(medicine_model_t *model, uint8_t hour, uint8_t minute, uint64_t now_ms) {
    if (!model || !valid_hm(hour, minute)) return false;
    model->clock_valid = true;
    model->base_minute_of_day = (uint16_t)hour * 60U + minute;
    model->base_ms = now_ms;
    model->last_checked_abs_minute = model->base_minute_of_day;
    model->last_trigger_day = -1;
    model->last_taken_day = -1;
    model->last_skipped_day = -1;
    model->alarm_active = false;
    model->alarm_day = -1;
    model->snooze_active = false;
    return true;
}

bool medicine_model_clock(const medicine_model_t *model, uint64_t now_ms,
                          uint8_t *hour, uint8_t *minute, int32_t *day_index) {
    if (!model || !model->clock_valid) return false;
    uint64_t absolute = abs_minute_now(model, now_ms);
    uint16_t minute_of_day = (uint16_t)(absolute % MINUTES_PER_DAY);
    if (hour) *hour = (uint8_t)(minute_of_day / 60U);
    if (minute) *minute = (uint8_t)(minute_of_day % 60U);
    if (day_index) *day_index = (int32_t)(absolute / MINUTES_PER_DAY);
    return true;
}

medicine_event_t medicine_model_tick(medicine_model_t *model, uint64_t now_ms) {
    if (!model || !model->clock_valid || model->alarm_active) return MEDICINE_EVENT_NONE;

    uint64_t current = abs_minute_now(model, now_ms);
    uint64_t previous = model->last_checked_abs_minute;
    if (current < previous) previous = current;

    if (model->snooze_active && model->snooze_abs_minute > previous &&
        model->snooze_abs_minute <= current) {
        model->snooze_active = false;
        model->alarm_active = true;
        model->alarm_day = (int32_t)(model->snooze_abs_minute / MINUTES_PER_DAY);
        model->last_checked_abs_minute = current;
        return MEDICINE_EVENT_ALARM;
    }

    if (model->reminder_enabled) {
        int32_t current_day = (int32_t)(current / MINUTES_PER_DAY);
        uint64_t target = (uint64_t)current_day * MINUTES_PER_DAY +
                          (uint64_t)model->reminder_hour * 60ULL + model->reminder_minute;
        if (target > previous && target <= current && model->last_trigger_day != current_day) {
            model->last_trigger_day = current_day;
            model->alarm_active = true;
            model->alarm_day = current_day;
            model->last_checked_abs_minute = current;
            return MEDICINE_EVENT_ALARM;
        }
    }

    model->last_checked_abs_minute = current;
    return MEDICINE_EVENT_NONE;
}

bool medicine_model_mark_taken(medicine_model_t *model) {
    if (!model || !model->alarm_active) return false;
    model->last_taken_day = model->alarm_day;
    model->alarm_active = false;
    model->snooze_active = false;
    return true;
}

bool medicine_model_skip_today(medicine_model_t *model) {
    if (!model || !model->alarm_active) return false;
    model->last_skipped_day = model->alarm_day;
    model->alarm_active = false;
    model->snooze_active = false;
    return true;
}

bool medicine_model_snooze(medicine_model_t *model, uint64_t now_ms) {
    if (!model || !model->alarm_active || !model->clock_valid) return false;
    uint64_t current = abs_minute_now(model, now_ms);
    model->alarm_active = false;
    model->snooze_active = true;
    model->snooze_abs_minute = current + MEDICINE_SNOOZE_MINUTES;
    return true;
}

medicine_day_status_t medicine_model_day_status(const medicine_model_t *model, uint64_t now_ms) {
    int32_t day = -1;
    if (!medicine_model_clock(model, now_ms, NULL, NULL, &day)) return MEDICINE_DAY_WAITING;
    if (model->last_taken_day == day) return MEDICINE_DAY_TAKEN;
    if (model->last_skipped_day == day) return MEDICINE_DAY_SKIPPED;
    return MEDICINE_DAY_WAITING;
}
