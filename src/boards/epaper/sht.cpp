#include "sht.h"

#include <Wire.h>

#include "config.h"
#include "log.h"

namespace {
constexpr uint16_t CMD_READ_ID    = 0xEFC8;
constexpr uint16_t CMD_SOFT_RESET = 0x805D;
constexpr uint16_t CMD_SLEEP      = 0xB098;
constexpr uint16_t CMD_WAKEUP     = 0x3517;
constexpr uint16_t CMD_MEAS_T_RH  = 0x7866;  // 온도 먼저, 클럭 스트레칭 없음
constexpr uint16_t CRC_POLYNOMIAL = 0x131;
}  // namespace

bool Shtc3::sendCmd(uint16_t cmd) {
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)(cmd >> 8));
    Wire.write((uint8_t)(cmd & 0xFF));
    return Wire.endTransmission() == 0;
}

bool Shtc3::crcOk(const uint8_t* data, uint8_t len, uint8_t checksum) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; --bit) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ CRC_POLYNOMIAL) : (uint8_t)(crc << 1);
        }
    }
    return crc == checksum;
}

bool Shtc3::begin() {
    _ok = false;
    if (!sendCmd(CMD_WAKEUP)) {
        RLOGE("SHTC3 을 I2C 에서 찾지 못함");
        return false;
    }
    delay(2);
    sendCmd(CMD_SOFT_RESET);
    delay(20);

    // ID 를 읽어 실제로 응답하는지 확인한다.
    if (!sendCmd(CMD_READ_ID)) return false;
    uint8_t b[3] = {0};
    if (Wire.requestFrom((int)_addr, 3) != 3) return false;
    for (uint8_t i = 0; i < 3; i++) b[i] = Wire.read();
    if (!crcOk(b, 2, b[2])) {
        RLOGE("SHTC3 ID CRC 불일치");
        return false;
    }

    sendCmd(CMD_SLEEP);
    _ok = true;
    return true;
}

bool Shtc3::read(float* tempC, float* humidity) {
    if (!_ok) return false;

    if (!sendCmd(CMD_WAKEUP)) return false;
    delay(2);
    if (!sendCmd(CMD_MEAS_T_RH)) return false;
    delay(20);

    uint8_t b[6] = {0};
    if (Wire.requestFrom((int)_addr, 6) != 6) {
        sendCmd(CMD_SLEEP);
        return false;
    }
    for (uint8_t i = 0; i < 6; i++) b[i] = Wire.read();
    sendCmd(CMD_SLEEP);

    if (!crcOk(b, 2, b[2]) || !crcOk(&b[3], 2, b[5])) {
        RLOGE("SHTC3 측정값 CRC 불일치");
        return false;
    }

    const uint16_t rawT = (uint16_t)((b[0] << 8) | b[1]);
    const uint16_t rawH = (uint16_t)((b[3] << 8) | b[4]);

    // 데이터시트 그대로. 자체 발열 보정은 하지 않는다.
    *tempC = 175.0f * (float)rawT / 65536.0f - 45.0f;
    *humidity = 100.0f * (float)rawH / 65536.0f;
    return true;
}
