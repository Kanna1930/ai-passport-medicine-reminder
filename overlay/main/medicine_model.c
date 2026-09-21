#include "medicine_model.h"

static bool valid_hm(uint8_t hour, uint8_t minute) {
    return hour < 24 && minute < 60;
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
        .snooze_epoch_minute = -1,
    };
}

bool medicine_model_set_reminder(medicine_model_t *model, uint8_t hour, uint8_t minute, bool enabled) {
    if (!model || !valid_hm(hour, minute)) return false;
    model->reminder_hour = hour;
    model->reminder_minute = minute;
    model->reminder_enabled = enabled;
    model->snooze_active = false;
    model->snooze_epoch_minute = -1;
    model->alarm_active = false;
    model->alarm_day = -1;
    return true;
}

medicine_event_t medicine_model_tick(medicine_model_t *model,
                                      int64_t epoch_minute,
                                      int32_t local_day,
                                      uint16_t local_minute_of_day) {
    if (!model || epoch_minute < 0 || local_minute_of_day >= 1440 || model->alarm_active) {
        return MEDICINE_EVENT_NONE;
    }

    if (model->snooze_active && epoch_minute >= model->snooze_epoch_minute) {
        model->snooze_active = false;
        model->snooze_epoch_minute = -1;
        model->alarm_active = true;
        model->alarm_day = local_day;
        return MEDICINE_EVENT_ALARM;
    }

    if (!model->reminder_enabled || model->last_trigger_day == local_day ||
        model->last_taken_day == local_day || model->last_skipped_day == local_day) {
        return MEDICINE_EVENT_NONE;
    }

    uint16_t target = (uint16_t)model->reminder_hour * 60U + model->reminder_minute;
    if (local_minute_of_day >= target) {
        model->last_trigger_day = local_day;
        model->alarm_active = true;
        model->alarm_day = local_day;
        return MEDICINE_EVENT_ALARM;
    }
    return MEDICINE_EVENT_NONE;
}

bool medicine_model_mark_taken(medicine_model_t *model) {
    if (!model || !model->alarm_active) return false;
    model->last_taken_day = model->alarm_day;
    model->alarm_active = false;
    model->snooze_active = false;
    model->snooze_epoch_minute = -1;
    return true;
}

bool medicine_model_skip_today(medicine_model_t *model) {
    if (!model || !model->alarm_active) return false;
    model->last_skipped_day = model->alarm_day;
    model->alarm_active = false;
    model->snooze_active = false;
    model->snooze_epoch_minute = -1;
    return true;
}

bool medicine_model_snooze(medicine_model_t *model, int64_t epoch_minute) {
    if (!model || !model->alarm_active || epoch_minute < 0) return false;
    model->alarm_active = false;
    model->snooze_active = true;
    model->snooze_epoch_minute = epoch_minute + MEDICINE_SNOOZE_MINUTES;
    return true;
}

medicine_day_status_t medicine_model_day_status(const medicine_model_t *model, int32_t local_day) {
    if (!model) return MEDICINE_DAY_WAITING;
    if (model->last_taken_day == local_day) return MEDICINE_DAY_TAKEN;
    if (model->last_skipped_day == local_day) return MEDICINE_DAY_SKIPPED;
    return MEDICINE_DAY_WAITING;
}
