#include "hw.h"

#define XPOWERS_CHIP_AXP2101
#include <Wire.h>
#include <XPowersLib.h>

#include "config.h"
#include "log.h"

namespace {

XPowersPMU pmu;
bool pmuOk = false;

// 백라이트 PWM. 5kHz / 10비트 — 벤더 값 그대로.
bool blAttached = false;

}  // namespace

bool hwPmicBegin() {
    // XPowersLib 은 안에서 Wire.begin(sda, scl) 을 또 부른다. Arduino 3.x 의
    // TwoWire 는 이미 열린 버스면 경고만 찍고 그대로 두므로 문제없다.
    pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, PIN_I2C_SDA, PIN_I2C_SCL);
    if (!pmuOk) {
        RLOGE("AXP2101(0x%02X)이 I2C 에 없다 — 배터리 표시와 전원 끄기가 안 된다",
              AXP2101_SLAVE_ADDRESS);
        return false;
    }
    RLOGI("AXP2101 chip id 0x%02X", pmu.getChipID());

    // 읽고 싶은 ADC 채널을 켠다. 기본은 꺼져 있어 0 만 나온다.
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.enableSystemVoltageMeasure();

    // PWR 버튼: 길게 누르면 하드웨어가 알아서 끈다. 4초로 둔다.
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_4S);
    return true;
}

HwPower hwReadPower() {
    HwPower p;
    if (!pmuOk) return p;
    p.pmic = true;

    // 셀이 붙어 있는데도 가끔 "없음"으로 읽힌다(실제로 1분 상태 로그에 한 번
    // 찍혔다). 한 번 어긋난 값으로 화면이 NO BAT 로 깜빡이지 않게, 두 번
    // 연속으로 없다고 해야 없는 것으로 친다.
    static uint8_t absentStreak = 0;
    const bool present = pmu.isBatteryConnect();
    if (present) absentStreak = 0;
    else if (absentStreak < 2) absentStreak++;
    p.battery = present || absentStreak < 2;
    p.vbus = pmu.isVbusIn();
    p.charging = pmu.isCharging();
    p.battMv = pmu.getBattVoltage();
    p.vbusMv = pmu.getVbusVoltage();
    if (p.battery) {
        const int pct = pmu.getBatteryPercent();
        p.percent = (pct < 0) ? 0 : (pct > 100 ? 100 : (uint8_t)pct);
    }
    return p;
}

void hwPowerOff() {
    if (!pmuOk) {
        RLOGE("PMIC 없이 전원을 끌 수 없다 — 재시작으로 대신한다");
        delay(100);
        ESP.restart();
    }
    pmu.shutdown();   // 돌아오지 않는다
    for (;;) delay(1000);
}

void hwSpeakerAmp(bool on) {
    pinMode(PIN_PA, OUTPUT);
    digitalWrite(PIN_PA, on ? HIGH : LOW);
    RLOGI("스피커 앰프 %s", on ? "on" : "off");
}

void hwBacklight(uint8_t percent) {
    if (percent > 100) percent = 100;
    if (!blAttached) blAttached = ledcAttach(PIN_LCD_BL, 5000, 10);
    if (blAttached) ledcWrite(PIN_LCD_BL, (uint32_t)1023 * percent / 100);
}
