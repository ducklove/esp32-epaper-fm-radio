#include "es8311.h"

#include "log.h"

// ── 레지스터 (es8311_reg.h) ───────────────────────────────────────
#define REG00_RESET       0x00
#define REG01_CLK_MGR     0x01
#define REG02_CLK_DIV     0x02
#define REG03_ADC_OSR     0x03
#define REG04_DAC_OSR     0x04
#define REG05_CLK_DIV2    0x05
#define REG06_BCLK_DIV    0x06
#define REG07_LRCK_H      0x07
#define REG08_LRCK_L      0x08
#define REG09_SDPIN       0x09  // DAC serial port
#define REG0A_SDPOUT      0x0A  // ADC serial port
#define REG0B_SYSTEM      0x0B
#define REG0C_SYSTEM      0x0C
#define REG0D_PWR         0x0D
#define REG0E_PWR         0x0E
#define REG10_SYSTEM      0x10
#define REG11_SYSTEM      0x11
#define REG12_DAC_EN      0x12
#define REG13_SYSTEM      0x13
#define REG14_DMIC_PGA    0x14
#define REG15_ADC_RAMP    0x15
#define REG16_ADC_GAIN    0x16
#define REG17_ADC_VOL     0x17
#define REG1B_ADC_HPF1    0x1B
#define REG1C_ADC_HPF2    0x1C
#define REG31_DAC_MUTE    0x31
#define REG32_DAC_VOL     0x32
#define REG37_DAC_RAMP    0x37
#define REG44_GPIO        0x44
#define REG45_GP          0x45

// MCLK = mclkDiv x fs 인 경우의 클럭 계수.
// es8311.c 의 coeff_div 표에서 mclk == 256*rate 인 행만 추린 것이며,
// 해당 행들은 pre_div/pre_multi/adc_div/dac_div/fs_mode/lrck 가 모두 동일하고
// OSR 만 저역 샘플레이트에서 0x20 으로 달라진다.
namespace {
struct Coeff {
    uint32_t rate;
    uint8_t  osr;  // adc_osr == dac_osr
};
constexpr Coeff kCoeff256[] = {
    {8000, 0x20},  {11025, 0x10}, {12000, 0x20}, {16000, 0x20}, {22050, 0x10},
    {24000, 0x10}, {32000, 0x10}, {44100, 0x10}, {48000, 0x10}, {64000, 0x10},
    {88200, 0x10}, {96000, 0x10},
};
}  // namespace

bool ES8311::writeReg(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission() == 0;
}

bool ES8311::readReg(uint8_t reg, uint8_t* val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)_addr, 1) != 1) return false;
    *val = Wire.read();
    return true;
}

bool ES8311::updateReg(uint8_t reg, uint8_t mask, uint8_t val) {
    uint8_t cur = 0;
    if (!readReg(reg, &cur)) return false;
    return writeReg(reg, (uint8_t)((cur & ~mask) | (val & mask)));
}

bool ES8311::probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

bool ES8311::begin(uint8_t sda, uint8_t scl, uint32_t sampleRate, uint8_t bitsPerSample,
                   uint16_t mclkDiv, int8_t paPin) {
    _ok = false;
    _paPin = paPin;

    Wire.end();
    if (!Wire.begin(sda, scl, 100000UL)) {
        RLOGE("I2C begin 실패 (sda=%d scl=%d)", sda, scl);
        return false;
    }

    // CE 핀 상태에 따라 0x18 또는 0x19
    if (probe(0x18)) {
        _addr = 0x18;
    } else if (probe(0x19)) {
        _addr = 0x19;
    } else {
        RLOGE("ES8311 를 I2C 에서 찾지 못함 — 오디오 전원(GPIO42)이 켜져 있는지 확인");
        return false;
    }
    RLOGI("ES8311 발견: 0x%02X", _addr);

    // ── es8311_open() ────────────────────────────────────────────
    // 첫 I2C 쓰기가 간헐적으로 실패하는 칩 특성 때문에 두 번 쓴다.
    writeReg(REG44_GPIO, 0x08);
    if (!writeReg(REG44_GPIO, 0x08)) return false;

    writeReg(REG01_CLK_MGR, 0x30);
    writeReg(REG02_CLK_DIV, 0x00);
    writeReg(REG03_ADC_OSR, 0x10);
    writeReg(REG16_ADC_GAIN, 0x24);
    writeReg(REG04_DAC_OSR, 0x10);
    writeReg(REG05_CLK_DIV2, 0x00);
    writeReg(REG0B_SYSTEM, 0x00);
    writeReg(REG0C_SYSTEM, 0x00);
    writeReg(REG10_SYSTEM, 0x1F);
    writeReg(REG11_SYSTEM, 0x7F);
    writeReg(REG00_RESET, 0x80);

    // 슬레이브 모드 (ESP32 가 I2S 마스터) — reg00 의 bit6 을 0 으로
    updateReg(REG00_RESET, 0x40, 0x00);

    // MCLK 를 외부에서 받는다(use_mclk=1, invert 없음) → 0x3F & 0x7F, bit6 클리어
    writeReg(REG01_CLK_MGR, 0x3F);
    // SCLK 비반전
    updateReg(REG06_BCLK_DIV, 0x20, 0x00);

    writeReg(REG13_SYSTEM, 0x10);
    writeReg(REG1B_ADC_HPF1, 0x0A);
    writeReg(REG1C_ADC_HPF2, 0x6A);
    writeReg(REG44_GPIO, 0x08);  // ADC 입력에 DAC 를 물리지 않음 (재생 전용)

    // ── 포맷 / 워드 길이 ─────────────────────────────────────────
    // reg09/0A bit[4:2]: 000=24bit, 011=16bit, 100=32bit
    uint8_t wl;
    switch (bitsPerSample) {
        case 16: wl = 0x03 << 2; break;
        case 24: wl = 0x00 << 2; break;
        case 32:
        default: wl = 0x04 << 2; break;
    }
    updateReg(REG09_SDPIN, 0x1C, wl);
    updateReg(REG0A_SDPOUT, 0x1C, wl);
    // bit[1:0] = 00 → 표준 I2S (Philips)
    updateReg(REG09_SDPIN, 0x03, 0x00);
    updateReg(REG0A_SDPOUT, 0x03, 0x00);

    if (!setSampleRate(sampleRate, mclkDiv)) return false;

    // ── es8311_start(), 재생(DAC)만 사용 ─────────────────────────
    updateReg(REG09_SDPIN, 0x40, 0x00);   // DAC 언뮤트
    updateReg(REG0A_SDPOUT, 0x40, 0x40);  // ADC 출력은 뮤트

    writeReg(REG17_ADC_VOL, 0xBF);
    writeReg(REG0E_PWR, 0x02);
    writeReg(REG12_DAC_EN, 0x00);
    writeReg(REG14_DMIC_PGA, 0x1A);
    updateReg(REG14_DMIC_PGA, 0x40, 0x00);  // 디지털 마이크 아님
    writeReg(REG0D_PWR, 0x01);
    writeReg(REG15_ADC_RAMP, 0x40);
    writeReg(REG37_DAC_RAMP, 0x08);
    writeReg(REG45_GP, 0x00);

    setMute(false);
    setVolumeDb(-12.0f);

    if (_paPin >= 0) {
        pinMode(_paPin, OUTPUT);
        setPaEnabled(true);
    }

    _ok = true;
    return true;
}

bool ES8311::setSampleRate(uint32_t sampleRate, uint16_t mclkDiv) {
    if (mclkDiv != 256) {
        RLOGE("MCLK 배수 %u 는 이 표에 없음 (256 만 지원)", mclkDiv);
        return false;
    }
    const Coeff* c = nullptr;
    for (const auto& e : kCoeff256) {
        if (e.rate == sampleRate) { c = &e; break; }
    }
    if (!c) {
        RLOGE("지원하지 않는 샘플레이트: %u Hz", sampleRate);
        return false;
    }

    // MCLK = 256*fs 행은 전부 pre_div=1, pre_multi=1, adc_div=1, dac_div=1,
    // fs_mode=0, lrck=0x00ff, bclk_div=4 이다.
    updateReg(REG02_CLK_DIV, 0xF8, 0x00);          // pre_div-1=0 (bit7:5), pre_multi=x1 (bit4:3)
    writeReg(REG05_CLK_DIV2, 0x00);                // adc_div-1=0, dac_div-1=0
    updateReg(REG03_ADC_OSR, 0x7F, c->osr);        // fs_mode=0 (bit6) + adc_osr
    updateReg(REG04_DAC_OSR, 0x7F, c->osr);        // dac_osr
    updateReg(REG07_LRCK_H, 0x3F, 0x00);           // lrck_h = 0x00
    writeReg(REG08_LRCK_L, 0xFF);                  // lrck_l = 0xff
    updateReg(REG06_BCLK_DIV, 0x1F, 4 - 1);        // bclk_div = 4 → BCLK = MCLK/4 = 64*fs
    return true;
}

void ES8311::setVolumeDb(float db) {
    // reg32: 0x00 = -95.5 dB, 0xFF = +32 dB, 0.5 dB/step
    if (db < -95.5f) db = -95.5f;
    if (db > 32.0f) db = 32.0f;
    int reg = (int)lroundf(db * 2.0f + 191.0f);
    if (reg < 0) reg = 0;
    if (reg > 255) reg = 255;
    writeReg(REG32_DAC_VOL, (uint8_t)reg);
}

void ES8311::setMute(bool mute) {
    updateReg(REG31_DAC_MUTE, 0x60, mute ? 0x60 : 0x00);
}

void ES8311::setPaEnabled(bool on) {
    if (_paPin >= 0) digitalWrite(_paPin, on ? HIGH : LOW);
}
