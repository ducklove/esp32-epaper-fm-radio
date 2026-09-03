// FT6336 정전식 터치 — I2C 0x38, 폴링.
//
// 컨트롤러는 패널 고유 좌표(세로 320x480)로 보고한다. 화면을 눕혀 쓰므로
// 여기서 회전에 맞춰 바꿔서 돌려준다. 호출하는 쪽은 화면 좌표만 본다.
#pragma once

#include <Arduino.h>

struct TouchPoint {
    int16_t x = 0;   // 화면 좌표 (0..LCD_W-1)
    int16_t y = 0;   // (0..LCD_H-1)
};

// Wire.begin() 뒤에 부를 것. 컨트롤러가 응답하지 않으면 false.
bool touchBegin();

// 지금 눌린 점들. 없으면 0.
uint8_t touchRead(TouchPoint* out, uint8_t max);
