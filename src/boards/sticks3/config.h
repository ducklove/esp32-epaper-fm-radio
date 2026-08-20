// M5Stack M5StickS3 핀맵과 조정 가능한 상수.
//
// 출처: M5Stack 공식 문서 https://docs.m5stack.com/en/core/StickS3
//
// MCU 는 ePaper 보드와 같은 ESP32-S3-PICO-1-N8R8 이고 오디오 코덱도 ES8311 로
// 같다. I2C 핀(47/48)까지 우연히 같아서 코덱 드라이버는 그대로 쓴다.
// 다른 것은 I2S 핀 번호, 화면, 전원관리, 버튼이다.
#pragma once

#include <stdint.h>

// ── ES8311 오디오 코덱 ────────────────────────────────────────────
// ePaper 보드와 핀 번호가 다르다. I2C 만 우연히 같다.
constexpr int8_t PIN_I2S_MCLK = 18;
constexpr int8_t PIN_I2S_BCLK = 17;
constexpr int8_t PIN_I2S_WS   = 15;  // LRCK
constexpr int8_t PIN_I2S_DOUT = 14;  // ESP32 -> 코덱 (재생)
constexpr int8_t PIN_I2S_DIN  = 16;  // 코덱 -> ESP32 (마이크, 미사용)
constexpr int8_t PIN_I2C_SDA  = 47;
constexpr int8_t PIN_I2C_SCL  = 48;

// 스피커 앰프(AW8737) enable 은 ESP32 핀이 아니다. 문서의 "G3" 는 PMIC(M5PM1)
// 의 GPIO3 이고, 레지스터 0x11 의 bit3 로 켠다. M5Unified 도 Speaker 를 켤 때
// 같은 비트를 올린다. 우리는 I2S 를 오디오 라이브러리가 소유해야 해서
// M5.Speaker 를 쓰지 않으므로 hw.cpp 에서 직접 올린다.
// 여기에 핀 번호로 적어 두면 ESP32 GPIO3 으로 오해하게 되어 아예 두지 않는다.

// ── 버튼 ─────────────────────────────────────────────────────────
// ePaper 보드의 BOOT/PWR 과 달리 둘 다 일반 버튼이다. 풀업, Active-LOW.
constexpr int8_t PIN_BTN_A = 11;  // KEY1 — 큰 버튼(앞면)
constexpr int8_t PIN_BTN_B = 12;  // KEY2 — 작은 버튼(옆면)

// ── 그 외 ────────────────────────────────────────────────────────
constexpr int8_t PIN_IMU_INT = 4;   // BMI270 인터럽트. 움직임으로 깨울 때 쓴다
constexpr int8_t PIN_IR_TX   = 46;
constexpr int8_t PIN_IR_RX   = 42;
constexpr int8_t PIN_CHG_STAT = 0;  // 충전 상태 입력

// ── 오디오 파이프라인 ─────────────────────────────────────────────
// ESP32-audioI2S 는 I2S 를 32bit 슬롯 / Philips / MCLK = 256 x fs 로 설정한다.
// 샘플레이트는 스트림 원본을 그대로 따라가고, 바뀔 때마다 코덱 클럭을 다시 잡는다.
constexpr uint32_t AUDIO_SAMPLE_RATE = 48000;
constexpr uint8_t  AUDIO_BITS        = 32;
constexpr uint16_t AUDIO_MCLK_DIV    = 256;

// ── 전력 절감 ─────────────────────────────────────────────────────
// 근거는 ePaper 보드와 같다. 재생 중 모뎀 슬립을 켜면 입력 버퍼가 마르고
// 소리가 튄다. 유휴 상태에서만 켠다.
constexpr uint32_t CPU_FREQ_MHZ = 160;
constexpr bool WIFI_SLEEP_WHILE_PLAYING = false;
constexpr bool WIFI_SLEEP_WHILE_IDLE    = true;

// ── 화면 ─────────────────────────────────────────────────────────
// 135x240 세로형. 눕히면 240x135 가 되어 고전적인 다이얼 비율에 가깝다.
// 전자종이와 달리 전원을 끊으면 그림이 사라지므로, 평소에는 꺼 두고 필요할
// 때만 켠다. 이게 이 보드에서 시계 모드를 다시 설계해야 하는 이유다.
constexpr uint32_t SCREEN_DIM_MS = 20000;   // 조작이 없으면 어둡게
constexpr uint32_t SCREEN_OFF_MS = 45000;   // 그 뒤 아예 끈다
constexpr uint8_t  SCREEN_BRIGHT = 110;     // 0~255
constexpr uint8_t  SCREEN_DIM    = 20;

// ── 오디오 라이브러리 로그 ────────────────────────────────────────
// 켜면 HLS 세그먼트마다 긴 URL 이 쏟아진다. 이 콜백은 오디오 태스크에서
// 실행되므로 출력 자체가 재생을 방해할 수 있다. 문제를 쫓을 때만 켠다.
constexpr bool AUDIO_VERBOSE_LOG = false;

// ── 배터리 보호 ───────────────────────────────────────────────────
// 잔량은 M5PM1 PMIC 에서 읽는다. ADC 분압이 아니다.
constexpr uint8_t BATT_CUTOFF_PERCENT = 10;
constexpr uint8_t BATT_CUTOFF_STRIKES = 2;

// ── NTP ──────────────────────────────────────────────────────────
// 이 보드에는 외장 RTC 가 없다. ESP32 내부 RTC 로 딥슬립은 넘길 수 있지만
// 전원이 끊기면 시각을 잃으므로, 부팅할 때마다 받는다.
#define NTP_TZ      "KST-9"
#define NTP_SERVER1 "kr.pool.ntp.org"
#define NTP_SERVER2 "time.google.com"
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
