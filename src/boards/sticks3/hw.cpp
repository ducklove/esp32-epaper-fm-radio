#include "hw.h"

#include <M5Unified.h>

#include "es8311.h"
#include "log.h"

// M5StickS3 의 내부 I2C(SCL 48 / SDA 47)에는 PMIC(M5PM1 0x6E), 코덱(ES8311 0x18),
// IMU(BMI270)가 함께 달려 있다. 이 버스의 주인은 M5Unified 다 — M5.begin() 이
// PMIC 와 IMU 를 위해 driver/i2c.h(레거시 드라이버)로 포트를 잡는다.
//
// 처음에는 Arduino Wire 로 PMIC 를 읽으려 했는데 실패했다. 원인은 한 하드웨어
// 포트에 I2C 마스터 드라이버가 둘이었기 때문이다. Arduino 3.x 의 Wire 는
// ESP-IDF 5.5 의 새 i2c_master 드라이버(esp32-hal-i2c-ng)를 쓰는데, 레거시
// 드라이버가 이미 잡은 포트에 올라가면 모든 전송이 ESP_ERR_INVALID_STATE 로
// 떨어진다. 실제 증상이 정확히 그랬다:
//
//   [E][esp32-hal-i2c-ng.c] i2c_master_transmit_receive failed: [259]
//
// 그래서 이 보드에서는 Wire 를 쓰지 않는다. PMIC 도 코덱도 M5.In_I2C 로
// 통일한다. 코덱 드라이버(공용 코드)는 ePaper 판에서 Wire 를 그대로 쓰므로,
// 전송 계층만 갈아 끼우도록 ES8311Bus 를 넘긴다.
namespace {

constexpr uint8_t  kPmicAddr = 0x6E;
constexpr uint32_t kI2cFreq  = 100000;

// PMIC GPIO 출력 레지스터. bit3 이 GPIO3 = 스피커 앰프(AW8737) enable 이다.
// M5.begin() 이 이 핀을 출력·푸시풀로 잡아 두고 LOW 로 내려 둔다.
// (M5Unified 자신도 Speaker 를 켤 때 이 비트를 올린다.)
constexpr uint8_t kRegGpioOut = 0x11;
constexpr uint8_t kBitAmp     = 1 << 3;

// 배터리 전압. 리틀엔디언 2바이트, 단위 mV. (M5PM1_REG_VBAT_L)
constexpr uint8_t kRegVbatL = 0x22;

// 부하가 걸린 상태의 1셀 리튬 방전 곡선.
struct Point { uint16_t mv; uint8_t pct; };
constexpr Point kCurve[] = {
    {4200, 100}, {4000, 80}, {3850, 60}, {3750, 40},
    {3650, 20},  {3450, 5},  {3300, 0},
};

// ── ES8311 드라이버가 쓸 전송 ──────────────────────────────────────
bool codecWrite(uint8_t addr, uint8_t reg, uint8_t val) {
    return M5.In_I2C.writeRegister8(addr, reg, val, kI2cFreq);
}

bool codecRead(uint8_t addr, uint8_t reg, uint8_t* val) {
    return M5.In_I2C.readRegister(addr, reg, val, 1, kI2cFreq);
}

bool codecProbe(uint8_t addr) {
    // START + 주소만 보내고 바로 STOP. NACK 이면 stop() 이 false 를 돌려준다.
    if (!M5.In_I2C.start(addr, false, kI2cFreq)) {
        M5.In_I2C.stop();
        return false;
    }
    return M5.In_I2C.stop();
}

bool readRegs(uint8_t reg, uint8_t* buf, size_t len) {
    return M5.In_I2C.readRegister(kPmicAddr, reg, buf, len, kI2cFreq);
}

}  // namespace

void hwInstallCodecBus() {
    const ES8311Bus bus = {codecWrite, codecRead, codecProbe};
    es8311SetBus(bus);
}

bool hwPmicPresent() {
    return codecProbe(kPmicAddr);
}

void hwSpeakerAmp(bool on) {
    const bool ok = on ? M5.In_I2C.bitOn(kPmicAddr, kRegGpioOut, kBitAmp, kI2cFreq)
                       : M5.In_I2C.bitOff(kPmicAddr, kRegGpioOut, kBitAmp, kI2cFreq);
    if (!ok) {
        RLOGE("앰프 제어 실패 — PMIC(0x%02X) 접근 실패", kPmicAddr);
        return;
    }
    RLOGI("스피커 앰프 %s", on ? "on" : "off");
}

uint16_t hwBatteryMillivolts() {
    uint8_t b[2] = {0, 0};
    if (!readRegs(kRegVbatL, b, sizeof(b))) return 0;
    return (uint16_t)((b[1] << 8) | b[0]);
}

uint8_t hwBatteryPercent(uint16_t mv) {
    if (mv == 0) return 0;
    if (mv >= kCurve[0].mv) return 100;

    constexpr size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (mv <= kCurve[n - 1].mv) return 0;

    for (size_t i = 1; i < n; i++) {
        if (mv >= kCurve[i].mv) {
            const Point& hi = kCurve[i - 1];
            const Point& lo = kCurve[i];
            const float t = (float)(mv - lo.mv) / (float)(hi.mv - lo.mv);
            return (uint8_t)lroundf(lo.pct + t * (hi.pct - lo.pct));
        }
    }
    return 0;
}
