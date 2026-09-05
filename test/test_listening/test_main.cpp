#include <unity.h>
#include "listening.h"

void setUp() {}
void tearDown() {}

static void sleep_expires_once_at_deadline() {
    SleepTimer t;
    TEST_ASSERT_EQUAL(0, t.remainingSeconds(123));
    TEST_ASSERT_FALSE(t.expire(123));
    t.cycle(123);
    TEST_ASSERT_EQUAL(15, t.minutes());
    TEST_ASSERT_EQUAL(900, t.remainingSeconds(123));
    TEST_ASSERT_EQUAL(1, t.remainingSeconds(900122));
    TEST_ASSERT_FALSE(t.expire(900122));
    TEST_ASSERT_TRUE(t.expire(900123));
    TEST_ASSERT_EQUAL(0, t.minutes());
    TEST_ASSERT_FALSE(t.expire(900124));
}

static void sleep_cycles_restarts_and_cancels() {
    SleepTimer t;
    t.cycle(0);
    t.cycle(800000); // 15분의 잔여 시간에 더하는 것이 아니라 지금부터 30분.
    TEST_ASSERT_EQUAL(30, t.minutes());
    TEST_ASSERT_EQUAL(1800, t.remainingSeconds(800000));
    t.cycle(800001);
    TEST_ASSERT_EQUAL(60, t.minutes());
    t.cycle(800002);
    TEST_ASSERT_EQUAL(90, t.minutes());
    t.cycle(800003);
    TEST_ASSERT_EQUAL(0, t.minutes());
    TEST_ASSERT_FALSE(t.expire(9000000));
}

static void sleep_survives_millis_rollover() {
    SleepTimer t;
    const uint32_t start = UINT32_MAX - 999;
    t.cycle(start);
    TEST_ASSERT_EQUAL(899, t.remainingSeconds(0));
    TEST_ASSERT_FALSE(t.expire(uint32_t(start + 899999)));
    TEST_ASSERT_TRUE(t.expire(uint32_t(start + 900000)));
}

static void invalid_saved_settings_recover() {
    ListeningSettings s;
    s.index = s.volume = s.tone = 255;
    s.paused = true;
    s.validate(15, 20);
    TEST_ASSERT_EQUAL(2, s.index);
    TEST_ASSERT_EQUAL(12, s.volume);
    TEST_ASSERT_EQUAL(0, s.tone);
    TEST_ASSERT_TRUE(s.paused);
    s.index = 14; s.volume = 0; s.tone = 3;
    s.validate(15, 20);
    TEST_ASSERT_EQUAL(14, s.index);
    TEST_ASSERT_EQUAL(0, s.volume);
    TEST_ASSERT_EQUAL(3, s.tone);
}

static void malformed_meter_values_are_clamped() {
    TEST_ASSERT_EQUAL(0, meterByte(0));
    TEST_ASSERT_EQUAL(128, meterByte(128));
    TEST_ASSERT_EQUAL(255, meterByte(255));
    TEST_ASSERT_EQUAL(255, meterByte(256));
    TEST_ASSERT_EQUAL(255, meterByte(UINT32_MAX));
}

static void restored_pause_never_autoplays() {
    ListeningSettings applied, desired;
    desired.paused = true;
    TEST_ASSERT_EQUAL(int(PlaybackChange::PAUSE), int(listeningChange(desired, applied, false, 0, 0)));
    applied = desired;
    desired.volume = 5;
    TEST_ASSERT_EQUAL(int(PlaybackChange::NONE), int(listeningChange(desired, applied, true, 1, 0)));
    desired.paused = false;
    TEST_ASSERT_EQUAL(int(PlaybackChange::RESTART), int(listeningChange(desired, applied, true, 2, 0)));
}

static void coalesced_pause_resume_restarts_cancelled_tune() {
    ListeningSettings applied, desired;
    // URL 해석 중 정지 때문에 취소된 뒤 다시 재생: 최종 bool 값이 같아도
    // 변경 세대가 다르므로 TUNING 상태에 멈추지 않고 새로 연결해야 한다.
    TEST_ASSERT_EQUAL(int(PlaybackChange::RESTART), int(listeningChange(desired, applied, true, 2, 0)));
    desired.index = 14;
    TEST_ASSERT_EQUAL(int(PlaybackChange::RESTART), int(listeningChange(desired, applied, true, 3, 2)));
}

static void tone_and_volume_do_not_reconnect_stream() {
    ListeningSettings applied, desired;
    desired.tone = 3;
    TEST_ASSERT_EQUAL(int(PlaybackChange::NONE), int(listeningChange(desired, applied, true, 0, 0)));
    desired.volume = 0;
    TEST_ASSERT_EQUAL(int(PlaybackChange::VOLUME), int(listeningChange(desired, applied, true, 0, 0)));
}

#include "networkhealth.h"

static void stale_connected_c6_recovers_after_two_failed_checks() {
    NetworkHealth health;
    TEST_ASSERT_FALSE(health.observe(true, -63, false));
    TEST_ASSERT_FALSE(health.observe(true, 0, true));
    TEST_ASSERT_TRUE(health.observe(true, 0, true));
}

static void station_outage_and_unsupported_rssi_do_not_restart() {
    NetworkHealth health;
    for (int i = 0; i < 10; ++i) TEST_ASSERT_FALSE(health.observe(true, 0, true));
    for (int i = 0; i < 10; ++i) TEST_ASSERT_FALSE(health.observe(true, -63, true));
    TEST_ASSERT_FALSE(health.observe(true, 0, true));
    TEST_ASSERT_FALSE(health.observe(true, -63, true));
    TEST_ASSERT_FALSE(health.observe(true, 0, true));
    TEST_ASSERT_FALSE(health.observe(true, 0, false)); // 정지 또는 재생 복구 시 누적 취소.
    TEST_ASSERT_FALSE(health.observe(true, 0, true));
    TEST_ASSERT_FALSE(health.observe(false, 0, true));
    TEST_ASSERT_FALSE(health.observe(true, 0, true));
}

static void blocked_audio_worker_recovers_without_false_restarts() {
    TEST_ASSERT_FALSE(audioWorkerStalled(200000, 0, 0));
    TEST_ASSERT_FALSE(audioWorkerStalled(91000, 1001, 0));
    TEST_ASSERT_TRUE(audioWorkerStalled(91000, 1000, 0));
    TEST_ASSERT_FALSE(audioWorkerStalled(91000, 1000, 90000));
    TEST_ASSERT_FALSE(audioWorkerStalled(200000, 199999, 0));
    TEST_ASSERT_TRUE(audioWorkerStalled(89000, UINT32_MAX - 1000, 0));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(sleep_expires_once_at_deadline);
    RUN_TEST(sleep_cycles_restarts_and_cancels);
    RUN_TEST(sleep_survives_millis_rollover);
    RUN_TEST(invalid_saved_settings_recover);
    RUN_TEST(malformed_meter_values_are_clamped);
    RUN_TEST(restored_pause_never_autoplays);
    RUN_TEST(coalesced_pause_resume_restarts_cancelled_tune);
    RUN_TEST(tone_and_volume_do_not_reconnect_stream);
    RUN_TEST(stale_connected_c6_recovers_after_two_failed_checks);
    RUN_TEST(station_outage_and_unsupported_rssi_do_not_restart);
    RUN_TEST(blocked_audio_worker_recovers_without_false_restarts);
    return UNITY_END();
}
