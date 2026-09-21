#include <assert.h>
#include <stdio.h>

#include "medicine_model.h"

#define MINUTE_MS 60000ULL

static void test_daily_trigger_and_taken(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    assert(medicine_model_set_reminder(&m, 8, 0, true));
    assert(medicine_model_set_clock(&m, 7, 50, 1000));
    assert(medicine_model_tick(&m, 1000 + 9 * MINUTE_MS) == MEDICINE_EVENT_NONE);
    assert(medicine_model_tick(&m, 1000 + 10 * MINUTE_MS) == MEDICINE_EVENT_ALARM);
    assert(m.alarm_active);
    assert(medicine_model_mark_taken(&m));
    assert(medicine_model_day_status(&m, 1000 + 10 * MINUTE_MS) == MEDICINE_DAY_TAKEN);
}

static void test_snooze(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, true);
    medicine_model_set_clock(&m, 7, 59, 0);
    assert(medicine_model_tick(&m, 1 * MINUTE_MS) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_snooze(&m, 1 * MINUTE_MS));
    assert(medicine_model_tick(&m, 10 * MINUTE_MS) == MEDICINE_EVENT_NONE);
    assert(medicine_model_tick(&m, 11 * MINUTE_MS) == MEDICINE_EVENT_ALARM);
}

static void test_next_day_triggers_again(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, true);
    medicine_model_set_clock(&m, 7, 59, 0);
    assert(medicine_model_tick(&m, 1 * MINUTE_MS) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_mark_taken(&m));
    assert(medicine_model_tick(&m, (24 * 60 + 1) * MINUTE_MS) == MEDICINE_EVENT_ALARM);
}

static void test_midnight_crossing(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 0, 2, true);
    medicine_model_set_clock(&m, 23, 58, 0);
    assert(medicine_model_tick(&m, 3 * MINUTE_MS) == MEDICINE_EVENT_NONE);
    assert(medicine_model_tick(&m, 4 * MINUTE_MS) == MEDICINE_EVENT_ALARM);
}

static void test_disabled(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, false);
    medicine_model_set_clock(&m, 7, 59, 0);
    assert(medicine_model_tick(&m, 2 * MINUTE_MS) == MEDICINE_EVENT_NONE);
    assert(!m.alarm_active);
}

int main(void) {
    test_daily_trigger_and_taken();
    test_snooze();
    test_next_day_triggers_again();
    test_midnight_crossing();
    test_disabled();
    puts("medicine_model tests: PASS");
    return 0;
}
