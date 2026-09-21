#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MEDICINE_MODEL_VERSION 1
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

    bool clock_valid;
    uint16_t base_minute_of_day;
    uint64_t base_ms;
    uint64_t last_checked_abs_minute;

    int32_t last_trigger_day;
    int32_t last_taken_day;
    int32_t last_skipped_day;

    bool alarm_active;
    int32_t alarm_day;
    bool snooze_active;
    uint64_t snooze_abs_minute;
} medicine_model_t;

void medicine_model_defaults(medicine_model_t *model);
bool medicine_model_set_reminder(medicine_model_t *model, uint8_t hour, uint8_t minute, bool enabled);
bool medicine_model_set_clock(medicine_model_t *model, uint8_t hour, uint8_t minute, uint64_t now_ms);
bool medicine_model_clock(const medicine_model_t *model, uint64_t now_ms,
                          uint8_t *hour, uint8_t *minute, int32_t *day_index);
medicine_event_t medicine_model_tick(medicine_model_t *model, uint64_t now_ms);
bool medicine_model_mark_taken(medicine_model_t *model);
bool medicine_model_skip_today(medicine_model_t *model);
bool medicine_model_snooze(medicine_model_t *model, uint64_t now_ms);
medicine_day_status_t medicine_model_day_status(const medicine_model_t *model, uint64_t now_ms);
