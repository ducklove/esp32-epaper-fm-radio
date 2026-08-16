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
constexpr int8_t PIN_PWR_VBAT  = 17;  // 이쪽만 Active-High (1 = ON). 배터리 분압 게이팅.

// ── 전력 절감 ─────────────────────────────────────────────────────
// 300mAh 급 셀로도 버티게 하려고 넣은 설정. 소리가 튀면 여기부터 되돌린다.
//
// CPU 클럭. AAC-LC 디코딩과 TLS 를 감당해야 해서 무작정 낮출 수는 없지만,
// 240MHz 는 과하다. ESP32-S3 의 동작 전류는 클럭에 거의 비례한다.
constexpr uint32_t CPU_FREQ_MHZ = 160;

// Wi-Fi 모뎀 슬립은 상태에 따라 나눠 건다. 이득과 위험이 정반대이기 때문이다.
//
//  재생 중  : 스트리밍이라 무선이 어차피 자주 깨어 있어 절감폭이 작은 반면,
//             수신이 밀리면 입력 버퍼가 마르고 소리가 끊긴다. 그래서 끈다.
//  일시정지 : 트래픽이 없어 비콘 간격 내내 잘 수 있다. 여기가 실제로 크게
//             아끼는 구간이다(대기 100mA 대 -> 15~20mA). 그래서 켠다.
//
// 재생 중에도 아끼고 싶으면 WIFI_SLEEP_WHILE_PLAYING 을 true 로. 대신 1분
// 상태 로그의 '버퍼 %' 가 떨어지지 않는지 지켜봐야 한다.
constexpr bool WIFI_SLEEP_WHILE_PLAYING = false;
constexpr bool WIFI_SLEEP_WHILE_IDLE    = true;

// ── 시계 / 센서 ───────────────────────────────────────────────────
// 시각은 온보드 PCF85063 RTC 에서 읽고, NTP 는 하루 한 번만 받는다.
// 딥슬립에 들어가도 RTC 는 계속 돌아서 깨어날 때마다 맞출 필요가 없다.
#define NTP_TZ      "KST-9"          // 한국 표준시, 서머타임 없음
#define NTP_SERVER1 "kr.pool.ntp.org"
#define NTP_SERVER2 "time.google.com"
constexpr uint32_t NTP_RESYNC_SEC  = 24UL * 60 * 60;  // 하루
constexpr uint32_t NTP_TIMEOUT_MS  = 10000;

// SHTC3 센서 보정값. 읽은 값에서 이만큼 뺀다.
//
// Waveshare 예제는 4도를 빼는데, 그건 연속 동작으로 보드가 더워진 상태를
// 전제한 값이다. 여기서 온도를 보여주는 곳은 꺼짐(시계) 화면뿐이고 그때는
// 1분 중 1초만 깨어 있어 자체 발열이 사실상 없다. 그대로 가져오면 오히려
// 실제보다 낮게 나오므로 0 에서 시작한다.
//
// 실측 없이 정한 값이다. 정확히 맞추려면 시리얼 로그의 raw 값을 실제
// 온도계와 비교해서 이 상수를 고치면 된다.
constexpr float SHTC3_TEMP_OFFSET_C = 0.0f;

// ── 꺼짐(시계) 모드 ───────────────────────────────────────────────
// 전원을 끄면 완전히 잠들지 않고 1분마다 잠깐 깨어나 시각·온습도·배터리를
// 갱신하고 다시 잠든다. 깨어 있는 시간이 1초 남짓이라 평균 1mA 안팎이다.
constexpr uint64_t CLOCK_TICK_SEC = 60;

// 잔상 제거용 전체 갱신 주기 (틱 단위). 전체 갱신은 1.4초 동안 화면이
// 번쩍이므로 드물수록 좋다. 1분 틱 기준 1440 = 하루 한 번.
constexpr uint16_t CLOCK_FULL_REFRESH_TICKS = 1440;

// ── 배터리 보호 ───────────────────────────────────────────────────
// 이 값 아래로 떨어지면 재생을 멈추고 알아서 꺼진다. 리튬 셀을 과방전에서
// 지키고, 재생 중 갑자기 죽는 것보다 낫다.
// 충전 중에는 전압이 4V 부근이라 여기에 걸리지 않는다.
constexpr uint8_t  BATT_CUTOFF_PERCENT = 10;
constexpr uint8_t  BATT_CUTOFF_STRIKES = 2;  // 연속 몇 번 측정되면 실행할지

// 오디오 라이브러리의 상세 로그(evt_info). 켜면 HLS 세그먼트마다 긴 URL 과
// content-length 가 4~5초 간격으로 쏟아진다. 이 콜백은 오디오 태스크에서
// 실행되므로 출력 자체가 재생을 방해할 수 있다. 문제를 쫓을 때만 켠다.
constexpr bool AUDIO_VERBOSE_LOG = false;

// ── 배터리 전압 ADC ───────────────────────────────────────────────
// Waveshare 01_ADC_Test: ADC1_CH3 = GPIO4, 12dB 감쇠, 12bit, 읽은 값 x2 (1:2 분압)
constexpr int8_t PIN_VBAT_ADC = 4;

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
