// 배터리 전압 측정
//
// Waveshare 01_ADC_Test 기준: ADC1_CH3(GPIO4), 12dB 감쇠, 12bit, 1:2 분압.
// 분압 회로는 VBAT_PWR(GPIO17)로 게이팅되므로 그 핀이 HIGH 여야 읽힌다.
#pragma once

#include <Arduino.h>

class Battery {
  public:
    void begin(uint8_t adcPin);

    // 여러 번 읽어 평균낸 셀 전압(V). 게이팅이 꺼져 있으면 0 에 가깝게 나온다.
    float readVolts();

    // 전압을 0~100% 로. 부하가 걸린 상태의 리튬 방전 곡선 기준이라 어림값이다.
    static uint8_t voltsToPercent(float v);

  private:
    uint8_t _pin = 0;
    bool    _ready = false;
};
