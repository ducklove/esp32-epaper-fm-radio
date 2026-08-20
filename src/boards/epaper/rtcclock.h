// PCF85063 RTC (온보드, I2C 0x51)
//
// NTP 는 하루 한 번만 받고 평소 시각은 여기서 읽는다. 딥슬립에 들어가도
// RTC 는 계속 돌기 때문에 깨어날 때마다 시간을 다시 맞출 필요가 없다.
//
// Waveshare 예제는 SensorLib 을 쓰지만 BCD 레지스터 몇 개가 전부라 직접 다룬다.
#pragma once

#include <Arduino.h>
#include <time.h>

class RtcClock {
  public:
    // Wire 는 이미 열려 있어야 한다.
    bool begin();

    // 발진기가 멈춘 적이 있으면(= 시각을 믿을 수 없으면) false.
    bool timeValid();

    bool getTime(struct tm* out);
    bool setTime(const struct tm& t);

    bool ok() const { return _ok; }

  private:
    bool readRegs(uint8_t reg, uint8_t* buf, size_t len);
    bool writeRegs(uint8_t reg, const uint8_t* buf, size_t len);

    static uint8_t bcdToBin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
    static uint8_t binToBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

    uint8_t _addr = 0x51;
    bool    _ok = false;
};
