#include <assert.h>
#include <stdio.h>

#include "medicine_model.h"

static void test_daily_trigger_and_taken(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    assert(medicine_model_set_reminder(&m, 8, 0, true));
    assert(medicine_model_tick(&m, 1000, 20260922, 7 * 60 + 59) == MEDICINE_EVENT_NONE);
    assert(medicine_model_tick(&m, 1001, 20260922, 8 * 60) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_mark_taken(&m));
    assert(medicine_model_day_status(&m, 20260922) == MEDICINE_DAY_TAKEN);
    assert(medicine_model_tick(&m, 1100, 20260922, 12 * 60) == MEDICINE_EVENT_NONE);
}

static void test_snooze(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, true);
    assert(medicine_model_tick(&m, 2000, 20260922, 8 * 60) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_snooze(&m, 2000));
    assert(medicine_model_tick(&m, 2009, 20260922, 8 * 60 + 9) == MEDICINE_EVENT_NONE);
    assert(medicine_model_tick(&m, 2010, 20260922, 8 * 60 + 10) == MEDICINE_EVENT_ALARM);
}

static void test_next_day_triggers_again(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, true);
    assert(medicine_model_tick(&m, 3000, 20260922, 8 * 60) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_mark_taken(&m));
    assert(medicine_model_tick(&m, 4440, 20260923, 8 * 60) == MEDICINE_EVENT_ALARM);
}

static void test_skip_persists_for_day(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, true);
    assert(medicine_model_tick(&m, 5000, 20260922, 9 * 60) == MEDICINE_EVENT_ALARM);
    assert(medicine_model_skip_today(&m));
    assert(medicine_model_day_status(&m, 20260922) == MEDICINE_DAY_SKIPPED);
    assert(medicine_model_tick(&m, 5200, 20260922, 15 * 60) == MEDICINE_EVENT_NONE);
}

static void test_disabled(void) {
    medicine_model_t m;
    medicine_model_defaults(&m);
    medicine_model_set_reminder(&m, 8, 0, false);
    assert(medicine_model_tick(&m, 6000, 20260922, 10 * 60) == MEDICINE_EVENT_NONE);
}

int main(void) {
    test_daily_trigger_and_taken();
    test_snooze();
    test_next_day_triggers_again();
    test_skip_persists_for_day();
    test_disabled();
    puts("medicine_model tests: PASS");
    return 0;
}
