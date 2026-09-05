// ESP32-P4-WIFI6-Touch-LCD-3.5 화면 — 480x320 컬러 LCD, 터치.
//
// 앞 보드들과 달리 버튼이 없다. 화면이 곧 조작면이다.
//
//   라디오 페이지  다이얼을 누르거나 끌어서 선국, 볼륨 슬라이더, 이전/재생/다음,
//                 메뉴 버튼
//   메뉴 페이지    채널 15개 격자, Wi-Fi 설정, 화면 끄기, 전원 끄기
//
// 터치 처리는 uiHandleTouch() 가 맡고, 결과를 UiEvent 로 돌려준다. 화면은
// 무엇을 할지 모른다 — 선국·볼륨·전원은 main 이 결정한다.
#pragma once

#include <Arduino.h>

#include "touch.h"
#include "listening.h"

enum PlayState : uint8_t {
    ST_BOOT,
    ST_WIFI,
    ST_TUNING,
    ST_BUFFERING,
    ST_PLAYING,
    ST_PAUSED,
    ST_ERROR,
    ST_UPDATING,
    ST_LOWBATT,
    ST_WIFISETUP,
};

struct UiState {
    uint8_t   index = 0;
    float     freq = 0.0f;
    String    name;
    PlayState state = ST_BOOT;
    String    detail;
    bool      paused = false;
    uint8_t   volume = 0;
    uint8_t   volumeMax = 20;
    uint32_t  bitrate = 0;
    uint8_t   tone = 0;
    uint8_t   sleepMinutes = 0;
    uint32_t  sleepSeconds = 0;
    AudioMeters meters;
    bool      controlsBlocked = false;
    uint32_t  controlsEpoch = 0;

    bool      wifi = false;
    uint8_t   wifiBars = 0;
    int16_t   wifiRssi = 0;

    bool      battery = false;
    bool      vbus = false;
    bool      charging = false;
    uint8_t   battPercent = 0;
    float     battVolts = 0.0f;

    bool      hasTime = false;
    uint8_t   hour = 0;
    uint8_t   minute = 0;

    String    apSsid;   // Wi-Fi 설정 모드에서만
    String    apPass;
    String    apUrl;
};

enum class UiAction : uint8_t {
    NONE,
    TUNE,          // value = 채널 인덱스
    PREV,
    NEXT,
    TOGGLE_PAUSE,
    VOLUME,        // value = 0..volumeMax
    TONE,          // value = 프리셋 인덱스
    SLEEP,         // 끔 -> 15 -> 30 -> 60 -> 90분
    WIFI_SETUP,
    SCREEN_OFF,
    POWER_OFF,
};

struct UiEvent {
    UiAction action = UiAction::NONE;
    uint8_t  value = 0;
    uint32_t epoch = 0;  // 정비 모드 진입 전의 밀린 이벤트는 폐기한다.
};

void uiBegin();
void uiRender(const UiState& s);
void uiRenderWifiSetup(const UiState& s);

// 터치 폴링마다 부른다. 눌린 점이 없으면 n = 0 으로 불러야 뗌을 안다.
UiEvent uiHandleTouch(const TouchPoint* pts, uint8_t n, const UiState& s);

// 드래그 중이면 상태가 안 바뀌어도 다시 그려야 한다.
bool uiNeedsRedraw();
bool uiShowsMeters();

// 터치는 별도 태스크에서, 그리기는 loop 에서 돈다. 상태 접근을 잠그되
// 캔버스의 SPI 전송 중에는 풀어서 터치 폴링이 계속되게 한다.
void uiLock();
void uiUnlock();

// 마지막 상태 로그 이후의 렌더 시간(ms). 그리기와 SPI 전송을 따로 잰다.
struct UiTiming {
    uint16_t frames = 0;
    uint16_t drawAvg = 0, drawMax = 0;
    uint16_t flushAvg = 0, flushMax = 0;
};
UiTiming uiTakeTiming();

// 백라이트. 조작이 없으면 어두워지고 그 뒤 꺼진다.
void uiWake();
void uiTickBacklight();
bool uiScreenIsOn();
void uiScreenOff();
