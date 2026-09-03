#include "touch.h"

#include <Wire.h>

#include "config.h"
#include "log.h"

namespace {

// FT6336 레지스터. 0x02 가 눌린 점 개수(하위 4비트), 그 뒤로 점마다 6바이트:
//   XH (3:0 = x[11:8], 7:6 = 이벤트), XL, YH (3:0 = y[11:8], 7:4 = ID), YL, 무게, 면적
constexpr uint8_t kRegStatus = 0x02;
constexpr uint8_t kMaxPoints = 2;   // FT6336 은 2점

bool present = false;

// 패널 좌표(세로 320x480, 벤더 회전 4 기준) -> 눕힌 화면 좌표.
// LCD_ROTATION 5 와 7 은 서로 180도라 변환도 반대다.
void mapToScreen(uint16_t px, uint16_t py, int16_t& sx, int16_t& sy) {
    if (px > 319) px = 319;
    if (py > 479) py = 479;
    if (LCD_ROTATION == 5) {
        sx = (int16_t)py;
        sy = (int16_t)(319 - px);
    } else {
        sx = (int16_t)(479 - py);
        sy = (int16_t)px;
    }
}

}  // namespace

bool touchBegin() {
    Wire.beginTransmission(TOUCH_ADDR);
    present = (Wire.endTransmission() == 0);
    if (!present) {
        RLOGE("터치 컨트롤러(0x%02X)가 I2C 에 없다", TOUCH_ADDR);
    }
    return present;
}

uint8_t touchRead(TouchPoint* out, uint8_t max) {
    if (!present || max == 0) return 0;

    uint8_t raw[1 + kMaxPoints * 6] = {0};
    Wire.beginTransmission(TOUCH_ADDR);
    Wire.write(kRegStatus);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)TOUCH_ADDR, (int)sizeof(raw)) != (int)sizeof(raw)) return 0;
    for (size_t i = 0; i < sizeof(raw); i++) raw[i] = Wire.read();

    uint8_t n = raw[0] & 0x0F;
    if (n > kMaxPoints) n = 0;   // 리셋 직후 0xFF 같은 쓰레기값
    if (n > max) n = max;

    for (uint8_t i = 0; i < n; i++) {
        const uint8_t* p = raw + 1 + i * 6;
        const uint16_t px = (uint16_t)(((p[0] & 0x0F) << 8) | p[1]);
        const uint16_t py = (uint16_t)(((p[2] & 0x0F) << 8) | p[3]);
        mapToScreen(px, py, out[i].x, out[i].y);
    }
    return n;
}
