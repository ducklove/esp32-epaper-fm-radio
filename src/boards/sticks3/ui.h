// M5StickS3 화면 — 135x240 컬러 LCD.
//
// 전자종이 판과 근본적으로 다른 점: 전원을 끊으면 그림이 사라진다. 그래서
// "항상 보이는 시계"가 성립하지 않는다. 평소에는 화면을 끄고, 집어 들거나
// 버튼을 누를 때만 켠다.
#pragma once

#include <Arduino.h>

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
    float     freq = 0.0f;
    String    name;
    PlayState state = ST_BOOT;
    String    detail;
    uint8_t   volume = 0;
    uint8_t   volumeMax = 20;
    uint32_t  bitrate = 0;

    bool      wifi = false;
    uint8_t   wifiBars = 0;
    int16_t   wifiRssi = 0;

    uint8_t   battPercent = 0;
    float     battVolts = 0.0f;
    bool      charging = false;

    bool      hasTime = false;
    uint8_t   hour = 0;
    uint8_t   minute = 0;

    String    apSsid;   // Wi-Fi 설정 모드에서만
    String    apPass;
    String    apUrl;
};

void uiBegin();
void uiRender(const UiState& s);
void uiRenderWifiSetup(const UiState& s);

// 백라이트. 조작이 없으면 어두워지고 그 뒤 꺼진다.
void uiWake();                 // 조작·움직임이 있을 때 부른다
void uiTickBacklight();        // loop 에서 주기적으로
bool uiScreenIsOn();
void uiScreenOff();
