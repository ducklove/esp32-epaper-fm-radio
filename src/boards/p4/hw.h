// ESP32-P4-WIFI6-Touch-LCD-3.5 의 전원 · 앰프 · 백라이트.
//
// 전원관리는 AXP2101 이다. 벤더 예제는 이 칩을 아예 건드리지 않는다 — 기본
// 설정으로 LCD·코덱 레일이 다 켜진다는 뜻이라, 우리도 읽기와 끄기만 한다.
// 배터리 잔량은 AXP2101 의 게이지(쿨롱 카운터)에서 퍼센트로 바로 나온다.
// 앞 보드들처럼 전압을 곡선에 맞춰 어림할 필요가 없다.
#pragma once

#include <Arduino.h>

struct HwPower {
    bool     pmic = false;      // AXP2101 이 응답했는가
    bool     battery = false;   // 셀이 물려 있는가
    bool     vbus = false;      // USB 전원이 들어오는가
    bool     charging = false;
    uint8_t  percent = 0;       // 셀이 없으면 0
    uint16_t battMv = 0;
    uint16_t vbusMv = 0;
};

// Wire.begin() 뒤에 부를 것.
bool    hwPmicBegin();
HwPower hwReadPower();

// 진짜 전원 차단. PWR 버튼을 누르면 다시 켜진다. 돌아오지 않는다.
void hwPowerOff();

// 스피커 앰프 (GPIO53, HIGH = 켜짐)
void hwSpeakerAmp(bool on);

// 백라이트 0~100%
void hwBacklight(uint8_t percent);
