#include "rtcclock.h"

#include <Wire.h>

#include "log.h"

// PCF85063 레지스터
#define REG_CONTROL1 0x00
#define REG_SECONDS  0x04  // bit7 = OS (발진기 정지 플래그)
#define REG_MINUTES  0x05
#define REG_HOURS    0x06
#define REG_DAYS     0x07
#define REG_WEEKDAYS 0x08
#define REG_MONTHS   0x09
#define REG_YEARS    0x0A

// Control_1
#define CTRL1_STOP   0x20  // 시각을 쓰는 동안 카운터를 멈춘다
#define CTRL1_12_24  0x02  // 0 = 24시간제

bool RtcClock::readRegs(uint8_t reg, uint8_t* buf, size_t len) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)_addr, (int)len) != (int)len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
    return true;
}

bool RtcClock::writeRegs(uint8_t reg, const uint8_t* buf, size_t len) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    for (size_t i = 0; i < len; i++) Wire.write(buf[i]);
    return Wire.endTransmission() == 0;
}

bool RtcClock::begin() {
    _ok = false;
    Wire.beginTransmission(_addr);
    if (Wire.endTransmission() != 0) {
        RLOGE("PCF85063 을 I2C 에서 찾지 못함");
        return false;
    }
    // 24시간제로 돌린다.
    uint8_t ctrl1 = 0;
    if (readRegs(REG_CONTROL1, &ctrl1, 1)) {
        const uint8_t want = (uint8_t)(ctrl1 & ~(CTRL1_STOP | CTRL1_12_24));
        if (want != ctrl1) writeRegs(REG_CONTROL1, &want, 1);
    }
    _ok = true;
    return true;
}

bool RtcClock::timeValid() {
    uint8_t sec = 0;
    if (!readRegs(REG_SECONDS, &sec, 1)) return false;
    return (sec & 0x80) == 0;  // OS 플래그가 서 있으면 시각을 믿을 수 없다
}

bool RtcClock::getTime(struct tm* out) {
    uint8_t b[7] = {0};
    if (!readRegs(REG_SECONDS, b, sizeof(b))) return false;
    if (b[0] & 0x80) return false;  // 발진기가 멈춘 적 있음

    memset(out, 0, sizeof(*out));
    out->tm_sec  = bcdToBin(b[0] & 0x7F);
    out->tm_min  = bcdToBin(b[1] & 0x7F);
    out->tm_hour = bcdToBin(b[2] & 0x3F);
    out->tm_mday = bcdToBin(b[3] & 0x3F);
    out->tm_wday = b[4] & 0x07;
    out->tm_mon  = bcdToBin(b[5] & 0x1F) - 1;      // tm_mon 은 0 기준
    out->tm_year = bcdToBin(b[6]) + 100;           // tm_year 은 1900 기준, RTC 는 00~99
    out->tm_isdst = 0;
    return true;
}

bool RtcClock::setTime(const struct tm& t) {
    // 카운터를 멈추고 쓴 뒤 다시 돌린다. 안 그러면 쓰는 도중 자리올림이 끼어든다.
    uint8_t ctrl1 = 0;
    readRegs(REG_CONTROL1, &ctrl1, 1);
    uint8_t stopped = (uint8_t)(ctrl1 | CTRL1_STOP);
    if (!writeRegs(REG_CONTROL1, &stopped, 1)) return false;

    uint8_t b[7];
    b[0] = binToBcd((uint8_t)t.tm_sec) & 0x7F;   // 쓰면서 OS 플래그를 지운다
    b[1] = binToBcd((uint8_t)t.tm_min);
    b[2] = binToBcd((uint8_t)t.tm_hour);
    b[3] = binToBcd((uint8_t)t.tm_mday);
    b[4] = (uint8_t)(t.tm_wday & 0x07);
    b[5] = binToBcd((uint8_t)(t.tm_mon + 1));
    b[6] = binToBcd((uint8_t)(t.tm_year % 100));
    const bool okWrite = writeRegs(REG_SECONDS, b, sizeof(b));

    uint8_t running = (uint8_t)(ctrl1 & ~CTRL1_STOP);
    writeRegs(REG_CONTROL1, &running, 1);
    return okWrite;
}
