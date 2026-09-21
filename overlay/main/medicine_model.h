#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MEDICINE_MODEL_VERSION 2
#define MEDICINE_SNOOZE_MINUTES 10

typedef enum {
    MEDICINE_EVENT_NONE = 0,
    MEDICINE_EVENT_ALARM,
} medicine_event_t;

typedef enum {
    MEDICINE_DAY_WAITING = 0,
    MEDICINE_DAY_TAKEN,
    MEDICINE_DAY_SKIPPED,
} medicine_day_status_t;

typedef struct {
    uint16_t version;
    uint8_t reminder_hour;
    uint8_t reminder_minute;
    bool reminder_enabled;

    int32_t last_trigger_day;
    int32_t last_taken_day;
    int32_t last_skipped_day;

    bool alarm_active;
    int32_t alarm_day;
    bool snooze_active;
    int64_t snooze_epoch_minute;
} medicine_model_t;

void medicine_model_defaults(medicine_model_t *model);
bool medicine_model_set_reminder(medicine_model_t *model, uint8_t hour, uint8_t minute, bool enabled);
medicine_event_t medicine_model_tick(medicine_model_t *model,
                                      int64_t epoch_minute,
                                      int32_t local_day,
                                      uint16_t local_minute_of_day);
bool medicine_model_mark_taken(medicine_model_t *model);
bool medicine_model_skip_today(medicine_model_t *model);
bool medicine_model_snooze(medicine_model_t *model, int64_t epoch_minute);
medicine_day_status_t medicine_model_day_status(const medicine_model_t *model, int32_t local_day);
