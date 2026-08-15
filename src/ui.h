// ePaper 200x200 FM 다이얼 UI
#pragma once

#include <Arduino.h>

enum PlayState : uint8_t {
    ST_BOOT,      // 부팅 중
    ST_WIFI,      // Wi-Fi 접속 중
    ST_TUNING,    // 방송사 API 로 스트림 URL 해석 중
    ST_BUFFERING, // 연결은 됐고 아직 소리가 안 나옴
    ST_PLAYING,
    ST_MUTED,
    ST_ERROR,
};

struct UiState {
    float     freq = 0.0f;
    String    name;
    PlayState state = ST_BOOT;
    String    detail;        // 상태 줄에 덧붙일 문구 (에러 사유 등)
    uint8_t   volume = 0;    // 0..kVolumeSteps
    uint8_t   volumeMax = 20;
    uint32_t  bitrate = 0;   // bit/s, 0 이면 표시 안 함
    bool      wifi = false;
};

void uiBegin();
// 화면을 다시 그린다. force 가 true 면 잔상 제거용 전체 갱신을 수행한다.
void uiRender(const UiState& s, bool forceFull = false);
void uiSleep();
