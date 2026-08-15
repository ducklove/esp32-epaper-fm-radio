// 보드 핀맵 — Waveshare ESP32-S3-ePaper-1.54 (V2)
//
// 출처: Waveshare 공식 예제 저장소 waveshareteam/ESP32-S3-ePaper-1.54
//   - 02_Example/Arduino/08_Audio_Test/user_config.h            (EPD / 전원 / 버튼 / I2C)
//   - 02_Example/Arduino/08_Audio_Test/src/codec_board/board_cfg.h
//       "Board: S3_ePaper_1_54"
//       i2c: {sda: 47, scl: 48}
//       i2s: {bclk: 15, ws: 38, dout: 45, din: 16, mclk: 14}
//       in_out: {codec: ES8311, pa: 46, use_mclk: 1, pa_gain: 6}
#pragma once

#include <stdint.h>

// ── ePaper (SSD1681, 200x200, SPI2) ──────────────────────────────
constexpr int8_t PIN_EPD_BUSY = 8;
constexpr int8_t PIN_EPD_RST  = 9;
constexpr int8_t PIN_EPD_DC   = 10;
constexpr int8_t PIN_EPD_CS   = 11;
constexpr int8_t PIN_EPD_SCK  = 12;
constexpr int8_t PIN_EPD_MOSI = 13;

// ── ES8311 오디오 코덱 ────────────────────────────────────────────
constexpr int8_t PIN_I2S_MCLK = 14;
constexpr int8_t PIN_I2S_BCLK = 15;
constexpr int8_t PIN_I2S_WS   = 38;  // LRCK
constexpr int8_t PIN_I2S_DOUT = 45;  // ESP32 -> 코덱 (재생)
constexpr int8_t PIN_I2S_DIN  = 16;  // 코덱 -> ESP32 (녹음, 미사용)
constexpr int8_t PIN_I2C_SDA  = 47;
constexpr int8_t PIN_I2C_SCL  = 48;
constexpr int8_t PIN_AUDIO_PA = 46;  // 스피커 앰프 enable (Active-High)

// ── 전원 레일 ─────────────────────────────────────────────────────
// 주의: EPD / Audio 전원은 Active-LOW (0 = ON). Waveshare board_power_bsp.cpp 기준.
constexpr int8_t PIN_PWR_EPD   = 6;
constexpr int8_t PIN_PWR_AUDIO = 42;
constexpr int8_t PIN_PWR_VBAT  = 17;  // 이쪽만 Active-High (1 = ON)

// ── 버튼 (둘 다 풀업, Active-LOW) ─────────────────────────────────
constexpr int8_t PIN_BTN_BOOT = 0;
constexpr int8_t PIN_BTN_PWR  = 18;

// ── 오디오 파이프라인 ─────────────────────────────────────────────
// ESP32-audioI2S 는 I2S 를 32bit 슬롯 / Philips / MCLK = 256 x fs 로 설정한다.
// (Audio.cpp: I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, STEREO),
//              clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256)
// 샘플레이트는 스트림 원본을 그대로 따라가고(리샘플링 안 함), 바뀔 때마다
// ES8311 클럭 계수를 다시 잡는다. 아래 값은 초기 설정용.
constexpr uint32_t AUDIO_SAMPLE_RATE = 48000;
constexpr uint8_t  AUDIO_BITS        = 32;
constexpr uint16_t AUDIO_MCLK_DIV    = 256;
