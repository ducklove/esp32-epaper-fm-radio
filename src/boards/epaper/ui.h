// ePaper 200x200 FM 다이얼 UI
#pragma once

#include <Arduino.h>

enum PlayState : uint8_t {
    ST_BOOT,      // 부팅 중
    ST_WIFI,      // Wi-Fi 접속 중
    ST_TUNING,    // 방송사 API 로 스트림 URL 해석 중
    ST_BUFFERING, // 연결은 됐고 아직 소리가 안 나옴
    ST_PLAYING,
    ST_PAUSED,    // 스트림을 끊고 오디오 전원까지 내린 상태
    ST_ERROR,
    ST_UPDATING,  // OTA 펌웨어 수신 중
    ST_LOWBATT,   // 배터리 부족으로 스스로 멈춘 상태
    ST_WIFISETUP, // AP 를 띄우고 Wi-Fi 정보를 입력받는 중
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
    uint8_t   wifiBars = 0;       // 0~3, RSSI 로 계산한 실제 신호 세기
    int16_t   wifiRssi = 0;       // dBm (진단용)
    float     battVolts = 0.0f;   // 0 이면 배터리 아이콘 숨김
    uint8_t   battPercent = 0;

    bool      hasTime = false;    // RTC 시각이 유효한지
    uint8_t   hour = 0;
    uint8_t   minute = 0;
    uint8_t   month = 0;          // 1~12, 0 이면 날짜 표시 안 함
    uint8_t   day = 0;
    uint8_t   weekday = 0;        // 0 = 일요일

    bool      hasEnv = false;     // SHTC3 값이 유효한지
    float     tempC = 0.0f;
    float     humidity = 0.0f;

    String    apSsid;             // Wi-Fi 설정 모드에서 띄운 AP 정보
    String    apPass;
    String    apUrl;
};

// RSSI(dBm) → 막대 0~3칸
uint8_t rssiToBars(int32_t rssi);

// initial=false 로 부르면 패널이 이전 이미지를 유지하고 있다고 보고 부분 갱신을
// 이어서 쓴다. 딥슬립에서 깨어나 시계만 고칠 때 쓴다 — true 로 부르면 GxEPD2 가
// 다음 갱신을 전체 갱신으로 승격시켜 1분마다 화면이 번쩍인다.
void uiBegin(bool initial = true);
// 화면을 다시 그린다. force 가 true 면 잔상 제거용 전체 갱신을 수행한다.
void uiRender(const UiState& s, bool forceFull = false);
// 전원 끔 화면 — 시계 / 온습도 / 배터리. ePaper 는 전원이 끊겨도 그림이 남는다.
// 1분마다 갱신하므로 기본은 부분 갱신이고, 30회마다 잔상을 털어낸다.
// frozen=true 면 시계 갱신이 멈춘다는 표시를 남긴다. 완전한 딥슬립에서는
// 화면이 그 시점에 멈추는데, 표시가 없으면 시계가 고장난 것처럼 보인다.
void uiRenderOff(const UiState& s, bool forceFull = false, bool frozen = false);

// 딥슬립 기본 화면 — 사진으로 200x200 을 꽉 채운다.
void uiRenderPhoto();

// Wi-Fi 설정 모드 안내. 보드 앞에서 폰만 보고 따라할 수 있게 적는다.
void uiRenderWifiSetup(const UiState& s);

// 화면에 내보내지 않고 컨트롤러의 '이전 이미지' 버퍼(0x26)에만 그린다.
//
// 부분 갱신은 이전 이미지와 새 이미지의 차분으로 동작한다. 딥슬립에서 깨어나면
// 하드웨어 리셋을 거치기 때문에 컨트롤러가 이전 이미지를 그대로 갖고 있다고
// 믿을 수 없다. 그래서 직전 화면을 여기로 다시 그려 넣은 뒤 부분 갱신을 한다.
// 그러면 무엇이 살아남았든 차분이 항상 맞는다.
void uiRenderOffToPrevious(const UiState& s, bool frozen = false);
void uiSleep();
