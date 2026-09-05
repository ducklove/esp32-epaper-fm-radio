// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 핀맵과 조정 가능한 상수.
//
// 출처: 벤더 Arduino 보드 계층 (waveshareteam/ESP32-P4-WIFI6-Touch-LCD-3.5,
// examples/arduino/libraries/Waveshare_LCD35/src/lcd35_board.h).
//
// 앞의 두 보드(ESP32-S3)와 근본적으로 다른 점 둘.
//
// 1) P4 에는 무선이 없다. 옆에 붙은 ESP32-C6 가 SDIO 로 Wi-Fi 를 대신한다
//    (ESP-Hosted). Arduino 3.3 이 그 사실을 감춰 주어 WiFi.begin() 은 그대로
//    쓰지만, 부팅에 C6 리셋 1.5초가 더 든다. SDIO 핀(18/19/14~17, RST 54)은
//    Arduino P4 기본값과 같아서 따로 지정하지 않는다.
//
// 2) 화면이 크고 터치가 된다. 버튼이 아니라 화면을 눌러 조작한다.
#pragma once

#include <stdint.h>

// ── LCD: ST7796, SPI 80MHz, 320x480 ────────────────────────────────
constexpr int8_t PIN_LCD_MOSI = 20;
constexpr int8_t PIN_LCD_SCK  = 21;
constexpr int8_t PIN_LCD_CS   = 23;
constexpr int8_t PIN_LCD_DC   = 26;
constexpr int8_t PIN_LCD_RST  = 27;
constexpr int8_t PIN_LCD_BL   = 28;   // PWM

constexpr uint32_t LCD_SPI_HZ = 80000000;

// 화면을 미는 SPI 버스 구현. 셋 다 Arduino_GFX 안에 있고 핀은 같다.
//   1  Arduino_ESP32SPIDMA  IDF spi_master + DMA. 전송 37ms 로 찍히지만 이 보드
//                           에서는 패널에 아무것도 그려지지 않았다.
//   2  Arduino_ESP32SPI     레지스터 직접 제어. 한 장 47ms, 실제로 그려진다. ★
//   3  Arduino_HWSPI        Arduino SPI 객체 경유. 벤더 예제. 그려지지만 2.5초.
// 부팅 직후 빨강·초록·파랑 자가 진단이 보이지 않으면 버스가 패널을 못 잡는
// 것이니 번호를 바꿔 본다.
#define LCD_BUS 2

// 벤더는 세로(320x480)로 회전 4 를 쓴다. Arduino_GFX 에서 4~7 은 0~3 에 화면
// 가로 반전을 더한 것이라, 처음엔 "이 패널은 반전이 필요하다"고 보고 가로도
// 5(=1+반전)로 잡았다. 실물에서는 거울상으로 보였다 — 세로에서 필요한 반전이
// 패널의 열(column) 축에 대한 것이라, 눕히면 화면의 세로 축이 되기 때문이다.
// 그래서 가로는 반전 없는 1 이다. 위아래가 뒤집혀 보이면 3 으로 바꾼다.
// 터치 좌표 변환(touch.cpp)도 이 값을 따라간다.
constexpr uint8_t  LCD_ROTATION = 1;
constexpr int16_t  LCD_W = 480;   // 회전 뒤
constexpr int16_t  LCD_H = 320;

// ── 터치: FT6336, I2C 0x38 ─────────────────────────────────────────
// INT/RST 는 벤더도 안 쓴다. 폴링한다.
constexpr uint8_t TOUCH_ADDR = 0x38;
// 회전 변환 뒤에도 한 축이 반대로 나오면 여기서 뒤집는다. 누를 때마다 원시
// 좌표와 화면 좌표가 로그에 찍히니 그걸 보고 정한다.
constexpr bool TOUCH_FLIP_X = false;
constexpr bool TOUCH_FLIP_Y = false;

// ── 내부 I2C ─────────────────────────────────────────────────────
// 코덱(0x18) · 터치(0x38) · PMIC AXP2101(0x34) · 카메라 SCCB 가 한 버스다.
constexpr int8_t PIN_I2C_SDA = 7;
constexpr int8_t PIN_I2C_SCL = 8;

// ── ES8311 오디오 코덱 ────────────────────────────────────────────
constexpr int8_t PIN_I2S_MCLK = 13;
constexpr int8_t PIN_I2S_BCLK = 12;
constexpr int8_t PIN_I2S_WS   = 10;  // LRCK
constexpr int8_t PIN_I2S_DOUT = 9;   // ESP32 -> 코덱 (재생)
constexpr int8_t PIN_I2S_DIN  = 11;  // 코덱 -> ESP32 (마이크, 미사용)
constexpr int8_t PIN_PA       = 53;  // 스피커 앰프 enable, HIGH = 켜짐

// ── 그 외 (미사용) ───────────────────────────────────────────────
constexpr int8_t PIN_SD_CLK = 43, PIN_SD_CMD = 44;
constexpr int8_t PIN_SD_D0 = 39, PIN_SD_D1 = 40, PIN_SD_D2 = 41, PIN_SD_D3 = 42;

// ── 오디오 파이프라인 ─────────────────────────────────────────────
constexpr uint32_t AUDIO_SAMPLE_RATE = 48000;
constexpr uint8_t  AUDIO_BITS        = 32;
constexpr uint16_t AUDIO_MCLK_DIV    = 256;

// ── 전력 절감 ─────────────────────────────────────────────────────
// 근거는 앞 보드들과 같다. 재생 중 모뎀 슬립을 켜면 버퍼가 마른다.
// (hosted 에서 setSleep 이 실제로 C6 까지 전달되는지는 확인 안 됨.)
constexpr bool WIFI_SLEEP_WHILE_PLAYING = false;
constexpr bool WIFI_SLEEP_WHILE_IDLE    = true;

// ── 화면 ─────────────────────────────────────────────────────────
// 3.5인치 백라이트는 StickS3 보다 훨씬 먹는다. 조작이 없으면 어둡게, 더 지나면
// 끈다. 화면 어디든 건드리면 켜진다(그 터치는 동작으로 치지 않는다).
constexpr uint32_t SCREEN_DIM_MS = 30000;
constexpr uint32_t SCREEN_OFF_MS = 180000;
constexpr uint8_t  SCREEN_BRIGHT = 80;    // 퍼센트
constexpr uint8_t  SCREEN_DIM    = 12;

// ── 오디오 라이브러리 로그 ────────────────────────────────────────
constexpr bool AUDIO_VERBOSE_LOG = false;

// ── 배터리 보호 ───────────────────────────────────────────────────
// 잔량은 AXP2101 의 쿨롱 카운터 기반 퍼센트를 그대로 쓴다.
constexpr uint8_t BATT_CUTOFF_PERCENT = 10;
constexpr uint8_t BATT_CUTOFF_STRIKES = 2;

// ── NTP ──────────────────────────────────────────────────────────
// 외장 RTC 가 없다. 부팅할 때마다 받는다.
#define NTP_TZ      "KST-9"
#define NTP_SERVER1 "kr.pool.ntp.org"
#define NTP_SERVER2 "time.google.com"
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
