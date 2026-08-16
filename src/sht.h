// SHTC3 온습도 센서 (온보드, I2C 0x70)
#pragma once

#include <Arduino.h>

class Shtc3 {
  public:
    // Wire 는 이미 열려 있어야 한다.
    bool begin();

    // 센서가 읽은 값 그대로. 자체 발열 보정은 호출하는 쪽에서 한다 —
    // 보정량이 그때그때 얼마나 오래 깨어 있었느냐에 달려 있어서, 드라이버가
    // 알 수 있는 정보가 아니다.
    // 측정 후 곧바로 다시 재운다. 측정에 약 20ms 걸린다.
    bool read(float* tempC, float* humidity);

    bool ok() const { return _ok; }

  private:
    bool sendCmd(uint16_t cmd);
    static bool crcOk(const uint8_t* data, uint8_t len, uint8_t checksum);

    uint8_t _addr = 0x70;
    bool    _ok = false;
};
