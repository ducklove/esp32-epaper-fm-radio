#include <unity.h>
#include "../../src/boards/p4/ui.cpp"

static uint8_t brightness;
void hwBacklight(uint8_t value) { brightness = value; }
static UiState state;
void setUp() {
    state = UiState{};
    state.state = ST_PLAYING;
    uiWake();
    uiHandleTouch(nullptr, 0, state);
    // 각 테스트는 라디오 페이지에서 시작한다.
    const TouchPoint back{430, 16};
    uiHandleTouch(&back, 1, state);
    uiHandleTouch(nullptr, 0, state);
}
void tearDown() { TEST_ASSERT_EQUAL(0, uiMutex->depth); }
static UiEvent tap(int16_t x, int16_t y) {
    TouchPoint p{x, y};
    uiHandleTouch(&p, 1, state);
    return uiHandleTouch(nullptr, 0, state);
}

static void sound_presets_and_sleep_are_reachable() {
    TEST_ASSERT_EQUAL(int(UiAction::NONE), int(tap(50, 65).action));
    TEST_ASSERT_TRUE(uiShowsMeters());
    for (int i = 0; i < 4; ++i) {
        const auto ev = tap(60 + 118 * i, 65);
        TEST_ASSERT_EQUAL(int(UiAction::TONE), int(ev.action));
        TEST_ASSERT_EQUAL(i, ev.value);
    }
    TEST_ASSERT_EQUAL(int(UiAction::SLEEP), int(tap(100, 280).action));
    TEST_ASSERT_EQUAL(int(UiAction::TOGGLE_PAUSE), int(tap(350, 280).action));
    uiRender(state);
    tap(430, 16);
    TEST_ASSERT_FALSE(uiShowsMeters());
    TEST_ASSERT_EQUAL(int(UiAction::SLEEP), int(tap(425, 65).action));
}

static void wake_touch_does_not_change_playback() {
    uiScreenOff();
    TEST_ASSERT_EQUAL(0, brightness);
    TEST_ASSERT_EQUAL(int(UiAction::NONE), int(tap(425, 65).action));
    TEST_ASSERT_TRUE(uiScreenIsOn());
    TEST_ASSERT_EQUAL(int(UiAction::SLEEP), int(tap(425, 65).action));
}

static void dragging_outside_button_cancels_it() {
    tap(50, 65);
    TouchPoint p{60, 65};
    uiHandleTouch(&p, 1, state);
    p = {200, 160};
    uiHandleTouch(&p, 1, state);
    TEST_ASSERT_EQUAL(int(UiAction::NONE), int(uiHandleTouch(nullptr, 0, state).action));
}

static void maintenance_swallow_does_not_leak_a_release() {
    state.controlsBlocked = true;
    TouchPoint p{425, 65};
    TEST_ASSERT_EQUAL(int(UiAction::NONE), int(uiHandleTouch(&p, 1, state).action));
    state.controlsBlocked = false;
    TEST_ASSERT_EQUAL(int(UiAction::NONE), int(uiHandleTouch(nullptr, 0, state).action));
    TEST_ASSERT_EQUAL(int(UiAction::SLEEP), int(tap(425, 65).action));
}

static void existing_station_grid_and_volume_still_work() {
    tap(420, 275);
    auto ev = tap(430, 190);
    TEST_ASSERT_EQUAL(int(UiAction::TUNE), int(ev.action));
    TEST_ASSERT_EQUAL(14, ev.value);
    ev = tap(456, 218);
    TEST_ASSERT_EQUAL(int(UiAction::VOLUME), int(ev.action));
    TEST_ASSERT_EQUAL(20, ev.value);
    TEST_ASSERT_EQUAL(int(UiAction::PREV), int(tap(50, 275).action));
}

static void spi_transfer_leaves_touch_unlocked_and_dirty() {
    testOnFlush = []() {
        TEST_ASSERT_EQUAL(0, uiMutex->depth);
        tap(50, 65); // 전송 도중 들어온 페이지 전환을 다음 프레임에 반영해야 한다.
    };
    uiRender(state);
    testOnFlush = nullptr;
    TEST_ASSERT_TRUE(uiShowsMeters());
    TEST_ASSERT_TRUE(uiNeedsRedraw());
}

int main() {
    UNITY_BEGIN();
    uiBegin();
    RUN_TEST(sound_presets_and_sleep_are_reachable);
    RUN_TEST(wake_touch_does_not_change_playback);
    RUN_TEST(dragging_outside_button_cancels_it);
    RUN_TEST(maintenance_swallow_does_not_leak_a_release);
    RUN_TEST(existing_station_grid_and_volume_still_work);
    RUN_TEST(spi_transfer_leaves_touch_unlocked_and_dirty);
    return UNITY_END();
}
