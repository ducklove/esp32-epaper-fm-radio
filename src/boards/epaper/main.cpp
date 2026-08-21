// Waveshare ESP32-S3-ePaper-1.54 인터넷 스트리밍 FM 라디오
//
// 보드에 FM 튜너 칩이 없어서 전파를 직접 받지는 못한다. 대신 방송사 HLS
// 스트림을 Wi-Fi 로 받아 ES8311 코덱 → 온보드 스피커로 재생하고,
// ePaper 에는 실제 서울 FM 주파수를 그대로 쓴 아날로그 다이얼을 그린다.
//
// 조작 (버튼 2개)
//
//   [라디오 모드]
//     BOOT 짧게       : 다음 채널 (주파수 오름차순)
//     BOOT 길게       : 이전 채널
//     PWR  짧게       : 볼륨 +2 (최대에서 다시 0으로)
//     PWR  길게 0.7초 : 일시정지 / 재개
//     PWR  길게 2초   : 시계 모드로
//
//   [시계 모드] — 1분마다 깨어나 시각·온습도를 갱신한다
//     PWR  누름       : 라디오 모드로
//     BOOT 누름       : 완전한 딥슬립으로 (시계도 멈춘다)
//
//   [딥슬립] 아무 버튼이나 누르면 라디오 모드로
//
// 스레드 구조
//   audioTask (core 1, prio 3) : 선국 + audio.loop(). 오디오는 여기서만 만진다.
//   loopTask  (core 1, prio 1) : 버튼 + ePaper. 갱신에 1초 넘게 걸려도
//                                 우선순위가 낮아 소리가 끊기지 않는다.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Audio.h>
#include <WiFi.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_timer.h>

#include "battery.h"
#include "config.h"
#include "es8311.h"
#include "log.h"
#include "rtcclock.h"
#include "secrets.h"
#include "sht.h"
#include "stations.h"
#include "ui.h"
#include "wifisetup.h"

// ── 상수 ──────────────────────────────────────────────────────────
constexpr uint8_t  kVolumeSteps   = 20;
constexpr uint8_t  kDefaultVolume = 14;
constexpr uint8_t  kDefaultIndex  = 2;      // KBS Classic FM 93.1
// 첫 시도는 넉넉하게 잡는다. 딥슬립에서 깨어난 직후에는 공유기가 예전 세션을
// 아직 붙들고 있어 인증이 느려지는 일이 있다. 다시 걸 때는 조금 짧게.
constexpr uint32_t kWifiFirstTryMs = 25000;
constexpr uint32_t kWifiRetryMs    = 15000;
constexpr uint8_t  kWifiAttempts   = 3;  // 이만큼 실패해야 설정 포털로 간다
constexpr uint32_t kWifiRecoveryMs = 30000;  // 붙지 못한 채 부팅했을 때 재시도 간격
constexpr uint32_t kLongPressMs     = 700;
constexpr uint32_t kVeryLongPressMs = 2000;  // PWR 을 이만큼 누르면 전원 끔
// 표시할 내용이 바뀌지 않으면 다시 그리지 않는다.
//
// 잔상은 부분 갱신이 쌓여서 생기는 것이지 시간이 지나서 생기는 게 아니고,
// 그건 이미 부분 갱신 12회마다 전체 갱신을 넣어 처리하고 있다. 시간 기준
// 갱신은 얻는 것 없이 364ms 짜리 SPI 전송과 패널 charge pump 전류 스파이크만
// 만든다. 충전 중처럼 전원 레일이 빠듯할 때 이게 겹치면 Wi-Fi 수신이 순간
// 끊겨 소리가 튄다.
//
// 정적 이미지를 아주 오래 방치하지 말라는 패널 쪽 권고만 하루 단위로 남긴다.
constexpr uint32_t kUiPeriodMs    = 24UL * 60 * 60 * 1000;  // 24시간
constexpr uint32_t kRetryDelayMs  = 8000;   // 재접속 간격
constexpr uint32_t kStatusPeriodMs = 60000; // 배터리·신호세기 측정 주기 (1분)
constexpr uint32_t kWifiPortalMs   = 5UL * 60 * 1000;  // 설정 포털 대기 시간

// ── 전역 ──────────────────────────────────────────────────────────
static Audio    audio;
static ES8311   codec;
static Battery  battery;
static RtcClock rtcClock;
static Shtc3    sensor;
static TaskHandle_t audioTaskHandle = nullptr;

// 딥슬립을 건너 살아남아야 하는 것들. 시계 모드로 깨어날 때마다 잃으면
// 마지막 채널도, 마지막 NTP 시각도 알 수 없다.
RTC_DATA_ATTR static bool     rtcInOffMode = false;
RTC_DATA_ATTR static uint8_t  rtcStationIndex = kDefaultIndex;
RTC_DATA_ATTR static uint8_t  rtcVolume = kDefaultVolume;
RTC_DATA_ATTR static uint32_t rtcLastNtpEpoch = 0;
RTC_DATA_ATTR static uint16_t rtcClockTicks = 0;

// 부분 갱신의 기준이 될 '직전 화면'. 딥슬립을 건너 살아남아야 한다.
// 화면 전체를 담을 필요는 없다 — 이 값들로 같은 그림을 다시 그릴 수 있다.
struct ClockSnapshot {
    bool    valid;
    bool    hasTime;
    bool    hasEnv;
    bool    frozen;
    uint8_t hour, minute, month, day, weekday;
    uint8_t battPercent;
    uint8_t stationIndex;
    float   tempC, humidity, battVolts;
};
RTC_DATA_ATTR static ClockSnapshot rtcPrevScreen;

// 딥슬립(타이머 없는 상태)인지, 그리고 그 화면에 사진 대신 측정값을 띄우고
// 있는지. BOOT 를 누를 때마다 둘을 오간다.
RTC_DATA_ATTR static bool rtcDeepSleepMode = false;
RTC_DATA_ATTR static bool rtcSleepShowValues = false;

struct Shared {
    uint8_t   index = kDefaultIndex;
    uint8_t   volume = kDefaultVolume;
    bool      paused = false;
    PlayState state = ST_BOOT;
    String    detail;
    uint32_t  bitrate = 0;
    float     battVolts = 0.0f;
    uint8_t   battPercent = 0;
    int16_t   wifiRssi = 0;
    uint8_t   wifiBars = 0;
};
static Shared           shared;
static SemaphoreHandle_t sharedLock;

static QueueHandle_t cmdQueue;
struct Cmd {
    enum Kind : uint8_t { TUNE, PAUSE, RESUME } kind;
    uint8_t index;
};

static inline void lockShared()   { xSemaphoreTake(sharedLock, portMAX_DELAY); }
static inline void unlockShared() { xSemaphoreGive(sharedLock); }

static void setState(PlayState st, const String& detail = String()) {
    lockShared();
    shared.state = st;
    shared.detail = detail;
    unlockShared();
}

// ── 시계 / 센서 ───────────────────────────────────────────────────
// 코덱·RTC·온습도 센서가 모두 같은 I2C 버스에 있다. 한 번만 올린다.
static bool i2cReady = false;
static void i2cBegin() {
    if (i2cReady) return;
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000UL);
    i2cReady = true;
}

// 현재 시각. RTC 가 없거나 발진기가 멈춘 적이 있으면 false.
static bool readClock(struct tm* out) {
    return rtcClock.ok() && rtcClock.getTime(out);
}

// NTP 로 받아 RTC 에 써 넣는다. 하루 한 번이면 충분하다.
static bool syncNtp() {
    if (WiFi.status() != WL_CONNECTED) return false;

    RLOGI("NTP 동기화 시도...");
    configTzTime(NTP_TZ, NTP_SERVER1, NTP_SERVER2);

    struct tm t = {};
    if (!getLocalTime(&t, NTP_TIMEOUT_MS)) {
        RLOGE("NTP 응답 없음");
        return false;
    }
    if (!rtcClock.setTime(t)) {
        RLOGE("RTC 쓰기 실패");
        return false;
    }
    rtcLastNtpEpoch = (uint32_t)mktime(&t);
    RLOGI("NTP 동기화 완료: %04d-%02d-%02d %02d:%02d:%02d", t.tm_year + 1900,
          t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

// 마지막 동기화로부터 하루가 지났거나 RTC 시각을 믿을 수 없으면 다시 받는다.
static void syncNtpIfDue() {
    if (WiFi.status() != WL_CONNECTED) return;

    if (!rtcClock.ok() || !rtcClock.timeValid() || rtcLastNtpEpoch == 0) {
        syncNtp();
        return;
    }
    struct tm now = {};
    if (!readClock(&now)) {
        syncNtp();
        return;
    }
    const uint32_t epoch = (uint32_t)mktime(&now);
    if (epoch < rtcLastNtpEpoch || epoch - rtcLastNtpEpoch >= NTP_RESYNC_SEC) {
        syncNtp();
    }
}

// 직전 화면을 기억해 두었다가, 다음 갱신 때 부분 갱신의 기준으로 되살린다.
static void saveClockSnapshot(const UiState& u, bool frozen) {
    rtcPrevScreen.valid = true;
    rtcPrevScreen.hasTime = u.hasTime;
    rtcPrevScreen.hasEnv = u.hasEnv;
    rtcPrevScreen.frozen = frozen;
    rtcPrevScreen.hour = u.hour;
    rtcPrevScreen.minute = u.minute;
    rtcPrevScreen.month = u.month;
    rtcPrevScreen.day = u.day;
    rtcPrevScreen.weekday = u.weekday;
    rtcPrevScreen.battPercent = u.battPercent;
    rtcPrevScreen.stationIndex = rtcStationIndex;
    rtcPrevScreen.tempC = u.tempC;
    rtcPrevScreen.humidity = u.humidity;
    rtcPrevScreen.battVolts = u.battVolts;
}

static UiState restoreClockSnapshot() {
    UiState u;
    const uint8_t idx =
        (rtcPrevScreen.stationIndex < kStationCount) ? rtcPrevScreen.stationIndex : 0;
    u.freq = kStations[idx].freq;
    u.name = kStations[idx].name;
    u.volumeMax = kVolumeSteps;
    u.state = ST_PAUSED;
    u.hasTime = rtcPrevScreen.hasTime;
    u.hasEnv = rtcPrevScreen.hasEnv;
    u.hour = rtcPrevScreen.hour;
    u.minute = rtcPrevScreen.minute;
    u.month = rtcPrevScreen.month;
    u.day = rtcPrevScreen.day;
    u.weekday = rtcPrevScreen.weekday;
    u.battPercent = rtcPrevScreen.battPercent;
    u.tempC = rtcPrevScreen.tempC;
    u.humidity = rtcPrevScreen.humidity;
    u.battVolts = rtcPrevScreen.battVolts;
    return u;
}

// 모뎀 슬립을 재생/유휴 상태에 따라 갈아 끼운다. 근거는 config.h 참고.
static void applyWifiPowerSave(bool playing) {
    const bool sleepOn = playing ? WIFI_SLEEP_WHILE_PLAYING : WIFI_SLEEP_WHILE_IDLE;
    WiFi.setSleep(sleepOn ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    RLOGI("Wi-Fi 모뎀 슬립: %s (%s)", sleepOn ? "on" : "off",
          playing ? "재생" : "유휴");
}

// 볼륨 단계를 코덱 dB 로. 20단계가 -38dB ~ 0dB 를 덮는다.
static float volumeToDb(uint8_t vol) { return -40.0f + 2.0f * (float)vol; }

static void applyVolume() {
    lockShared();
    const uint8_t vol = shared.volume;
    const bool    paused = shared.paused;
    unlockShared();

    if (paused) return;  // 일시정지 중에는 코덱 전원이 아예 내려가 있다

    if (vol == 0) {
        codec.setMute(true);
    } else {
        codec.setMute(false);
        codec.setVolumeDb(volumeToDb(vol));
    }
}

// ── 오디오 태스크 ─────────────────────────────────────────────────
static void tune(uint8_t index) {
    if (index >= kStationCount) return;
    const Station& st = kStations[index];

    audio.stopSong();
    setState(ST_TUNING);
    RLOGI("선국: %s (%.1f MHz)", st.name, st.freq);

    const String url = resolveStreamUrl(st);
    if (url.isEmpty()) {
        setState(ST_ERROR, "NO STREAM");
        return;
    }
    RLOGI("스트림 URL: %.80s", url.c_str());

    if (!audio.connecttohost(url.c_str())) {
        setState(ST_ERROR, "CONNECT FAIL");
        return;
    }

    applyVolume();
    setState(ST_BUFFERING);
}

// 일시정지는 단순한 뮤트가 아니다. 뮤트는 DAC 만 막을 뿐 Wi-Fi 수신과 디코딩이
// 그대로 돌아서 전력의 대부분(약 120mA)을 계속 먹는다. 여기서는 스트림을 끊고
// 오디오 전원 레일까지 내려 15~20mA 수준으로 떨어뜨린다.
// Wi-Fi 는 유휴 상태로 남긴다 — 모뎀 슬립 중이라 싸고, OTA 가 살아 있고,
// 재개가 즉시 된다.
static void pauseAudio() {
    RLOGI("일시정지 — 스트림 종료, 오디오 전원 차단");
    audio.stopSong();
    codec.setMute(true);
    codec.setPaEnabled(false);
    digitalWrite(PIN_PWR_AUDIO, HIGH);  // Active-LOW 라 HIGH 가 OFF

    // 트래픽이 없어졌으니 이제 무선을 재워도 안전하다. 여기가 크게 아끼는 구간.
    applyWifiPowerSave(false);

    lockShared();
    shared.paused = true;
    shared.state = ST_PAUSED;
    shared.bitrate = 0;
    unlockShared();
}

static void resumeAudio() {
    RLOGI("재개 — 오디오 전원 복구");
    applyWifiPowerSave(true);  // 다시 받아야 하므로 슬립 해제
    digitalWrite(PIN_PWR_AUDIO, LOW);
    delay(100);  // 코덱 전원이 올라올 시간

    // 전원이 끊겼으니 ES8311 은 초기 상태다. 부팅 때와 똑같이 다시 세운다.
    if (!codec.begin(PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE, AUDIO_BITS,
                     AUDIO_MCLK_DIV, PIN_AUDIO_PA)) {
        RLOGE("재개 실패 — 코덱을 다시 세우지 못함");
        setState(ST_ERROR, "NO CODEC");
        return;
    }

    lockShared();
    shared.paused = false;
    const uint8_t idx = shared.index;
    unlockShared();

    applyVolume();
    tune(idx);
}

static void audioTask(void*) {
    uint32_t codecRate = AUDIO_SAMPLE_RATE;
    uint32_t lastRetryMs = millis();

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
            switch (cmd.kind) {
                case Cmd::TUNE:   tune(cmd.index); break;
                case Cmd::PAUSE:  pauseAudio();    break;
                case Cmd::RESUME: resumeAudio();   break;
            }
        }

        // 일시정지 중에는 라이브러리를 돌릴 것도, 상태를 볼 것도 없다.
        {
            lockShared();
            const bool paused = shared.paused;
            unlockShared();
            if (paused) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
        }

        audio.loop();

        // 방송마다 샘플레이트가 다르다(보통 44.1k 또는 48k). 리샘플링으로 CPU 를
        // 쓰는 대신 스트림 원본 그대로 내보내고, 바뀔 때만 코덱 클럭을 다시 잡는다.
        // ESP32-audioI2S 는 MCLK 를 항상 256 x fs 로 내보내므로 계수표가 그대로 맞는다.
        const uint32_t rate = audio.getSampleRate();
        if (rate != 0 && rate != codecRate) {
            if (codec.setSampleRate(rate, AUDIO_MCLK_DIV)) {
                RLOGI("샘플레이트 변경: %u -> %u Hz", codecRate, rate);
                codecRate = rate;
            }
        }

        // 버퍼가 차서 실제로 소리가 나기 시작하면 BUFFERING → ON AIR
        lockShared();
        if (shared.state == ST_BUFFERING && audio.isRunning()) shared.state = ST_PLAYING;
        else if (shared.state == ST_PLAYING && !audio.isRunning()) shared.state = ST_BUFFERING;
        const PlayState st = shared.state;
        const uint8_t   idx = shared.index;
        unlockShared();

        // 스트림이 죽었거나 연결에 실패했으면 알아서 다시 붙는다.
        // 방송사 서버를 두들기지 않게 간격을 둔다.
        if (st == ST_ERROR && WiFi.status() == WL_CONNECTED) {
            if (millis() - lastRetryMs > kRetryDelayMs) {
                lastRetryMs = millis();
                tune(idx);
            }
        } else if (st != ST_ERROR) {
            lastRetryMs = millis();
        }

        vTaskDelay(1);
    }
}

// ── 버튼 ──────────────────────────────────────────────────────────
// 눌린 시간은 인터럽트에서 잰다. 폴링으로 재면 '눌림을 본 poll' 과 '뗌을 본
// poll' 사이의 간격이 그대로 누른 시간에 더해진다. ePaper 전체 갱신은 2초를
// 통째로 잡아먹으므로, 채널을 바꾸려고 톡 누른 것이 하필 그 앞에 걸리면
// '아주 길게 누름'으로 둔갑해 Wi-Fi 설정 포털이 떴다.
class Button {
  public:
    explicit Button(uint8_t pin) : _pin(pin) {}

    void begin() {
        pinMode(_pin, INPUT_PULLUP);
        _down = digitalRead(_pin) == LOW;
        // 켤 때 이미 눌려 있다면 깨우려고 누른 그 버튼이다. 언제부터 눌려
        // 있었는지 알 수 없으므로 뗄 때까지를 통째로 버린다.
        _armed = !_down;
        _event = 0;
        _downUs = esp_timer_get_time();
        attachInterruptArg(digitalPinToInterrupt(_pin), &Button::onEdge, this, CHANGE);
    }

    // 눌렀다 뗐을 때 1회만 보고한다.
    // 0 = 없음, 1 = 짧게, 2 = 길게, 3 = 아주 길게
    uint8_t poll() {
        const uint8_t ev = _event;
        if (ev) _event = 0;
        return ev;
    }

    bool isDown() const { return _down; }

    // 밀린 이벤트를 버린다. 오래 자리를 비웠다가 돌아올 때 쓴다.
    void clear() { _event = 0; }

  private:
    // IRAM_ATTR 을 붙이지 않는다. Arduino 코어가 GPIO ISR 서비스를
    // ESP_INTR_FLAG_IRAM 없이 설치하고 디스패처(__onPinInterrupt)부터가 플래시에
    // 있어서, 여기만 IRAM 에 올려 봐야 얻는 게 없다. IDF 는 플래시를 쓰는 동안
    // IRAM 이 아닌 인터럽트를 잠시 꺼 두므로 캐시가 꺼진 채로 불릴 일도 없다.
    // (붙이면 리터럴이 코드 뒤에 놓여 링크가 깨진다 — dangerous relocation.)
    //
    // 그래도 64bit 나눗셈은 피한다. 비교는 마이크로초 그대로 한다.
    static void onEdge(void* arg) {
        Button* b = static_cast<Button*>(arg);
        const bool down = gpio_get_level((gpio_num_t)b->_pin) == 0;
        if (down == b->_down) return;  // 상태가 그대로면 잡음이다
        b->_down = down;

        const int64_t now = esp_timer_get_time();
        if (down) {
            b->_downUs = now;
            return;
        }
        if (!b->_armed) {              // 켜질 때부터 눌려 있던 것
            b->_armed = true;
            return;
        }
        const int64_t heldUs = now - b->_downUs;
        if (heldUs < 40 * 1000) return;  // 채터링
        b->_event = heldUs >= (int64_t)kVeryLongPressMs * 1000
                        ? 3
                        : (heldUs >= (int64_t)kLongPressMs * 1000 ? 2 : 1);
    }

    uint8_t          _pin;
    volatile bool    _down = false;
    volatile bool    _armed = false;
    volatile uint8_t _event = 0;
    volatile int64_t _downUs = 0;
};

static Button btnBoot(PIN_BTN_BOOT);
static Button btnPwr(PIN_BTN_PWR);

// ── 전원 레일 ─────────────────────────────────────────────────────
// EPD/Audio 는 Active-LOW, VBAT 만 Active-HIGH (Waveshare board_power_bsp.cpp)
static void powerUpRails() {
    // 딥슬립에서 깨어난 경우 powerOff() 가 걸어 둔 홀드가 아직 살아 있다.
    // 먼저 풀지 않으면 아래 digitalWrite 가 먹히지 않아 전원이 안 켜진다.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PIN_PWR_EPD);
    gpio_hold_dis((gpio_num_t)PIN_PWR_AUDIO);
    gpio_hold_dis((gpio_num_t)PIN_PWR_VBAT);
    // 깨우기용으로 RTC 기능을 켜 둔 버튼들도 일반 GPIO 로 되돌린다.
    rtc_gpio_deinit((gpio_num_t)PIN_BTN_PWR);
    rtc_gpio_deinit((gpio_num_t)PIN_BTN_BOOT);

    pinMode(PIN_PWR_EPD, OUTPUT);
    pinMode(PIN_PWR_AUDIO, OUTPUT);
    pinMode(PIN_PWR_VBAT, OUTPUT);
    digitalWrite(PIN_PWR_EPD, LOW);
    digitalWrite(PIN_PWR_AUDIO, LOW);
    digitalWrite(PIN_PWR_VBAT, HIGH);
    delay(100);  // 코덱 전원이 올라올 시간
}

// ── UI 헬퍼 ───────────────────────────────────────────────────────
static UiState snapshotUi() {
    UiState u;
    lockShared();
    const uint8_t idx = shared.index;
    const bool    paused = shared.paused;
    u.state = shared.state;
    u.detail = shared.detail;
    u.volume = paused ? 0 : shared.volume;
    u.bitrate = shared.bitrate;
    u.battVolts = shared.battVolts;
    u.battPercent = shared.battPercent;
    u.wifiRssi = shared.wifiRssi;
    u.wifiBars = shared.wifiBars;
    unlockShared();

    u.freq = kStations[idx].freq;
    u.name = kStations[idx].name;
    u.volumeMax = kVolumeSteps;
    u.wifi = WiFi.status() == WL_CONNECTED;
    if (paused) u.state = ST_PAUSED;

    struct tm now = {};
    if (readClock(&now)) {
        u.hasTime = true;
        u.hour = (uint8_t)now.tm_hour;
        u.minute = (uint8_t)now.tm_min;
        u.month = (uint8_t)(now.tm_mon + 1);
        u.day = (uint8_t)now.tm_mday;
        u.weekday = (uint8_t)now.tm_wday;
    }
    return u;
}

static bool uiChanged(const UiState& a, const UiState& b) {
    return a.freq != b.freq || a.state != b.state || a.volume != b.volume ||
           a.wifi != b.wifi || a.wifiBars != b.wifiBars || a.detail != b.detail ||
           (a.bitrate / 1000) != (b.bitrate / 1000) ||
           // 전압은 화면에 적는 자릿수(0.1V)로만 비교한다. 더 잘게 보면
           // ADC 잡음과 충전 전류 변동 때문에 화면이 쉴 새 없이 다시 그려진다.
           (int)(a.battVolts * 10) != (int)(b.battVolts * 10);
}

// 배터리 전압과 Wi-Fi 신호 세기. 둘 다 천천히 변하고, 무엇보다 매 루프마다
// 읽으면 임계값 근처에서 값이 흔들려 ePaper 가 쉴 새 없이 다시 그려진다.
static void pollSlowStatus() {
    const float   v = battery.readVolts();
    const uint8_t pct = Battery::voltsToPercent(v);

    const bool    up = WiFi.status() == WL_CONNECTED;
    const int16_t rssi = up ? (int16_t)WiFi.RSSI() : 0;
    const uint8_t bars = up ? rssiToBars(rssi) : 0;

    lockShared();
    shared.battVolts = v;
    shared.battPercent = pct;
    shared.wifiRssi = rssi;
    shared.wifiBars = bars;
    unlockShared();

    // 방전 추이를 보려면 이 줄을 모아 두면 된다. 1분에 한 줄이라 부담 없다.
    // 버퍼 잔량도 같이 찍는다 — 모뎀 슬립이 수신을 방해하면 여기가 먼저 마른다.
    const uint32_t bufSize = audio.getInBufferSize();
    const uint32_t bufPct = bufSize ? (audio.inBufferFilled() * 100 / bufSize) : 0;
    RLOGI("배터리 %.2fV (%u%%)  Wi-Fi %ddBm (%u칸)  버퍼 %u%%  가동 %lu분", v,
          (unsigned)pct, (int)rssi, (unsigned)bars, (unsigned)bufPct,
          (unsigned long)(millis() / 60000));

    // 하루 한 번 시각을 다시 맞춘다. 이미 최근에 받았으면 그냥 지나간다.
    syncNtpIfDue();
}

// 배터리가 바닥나면 알아서 끈다. 재생 중 갑자기 죽는 것보다 낫고, 리튬 셀을
// 과방전에서 지킨다. 부하가 걸릴 때 순간적으로 처지는 것과 구분하려고
// 연속으로 몇 번 잡힐 때만 실행한다.
// 충전 중에는 전압이 4V 부근이라 여기에 걸리지 않는다.
static bool batteryCutoffDue() {
    static uint8_t strikes = 0;

    lockShared();
    const float   v = shared.battVolts;
    const uint8_t pct = shared.battPercent;
    unlockShared();

    if (v < 2.5f) {  // 배터리 미장착 — USB 로만 돌고 있다
        strikes = 0;
        return false;
    }
    if (pct > BATT_CUTOFF_PERCENT) {
        strikes = 0;
        return false;
    }
    if (strikes < 255) strikes++;
    if (strikes < BATT_CUTOFF_STRIKES) {
        RLOGI("배터리 낮음 %u%% (%u/%u)", (unsigned)pct, (unsigned)strikes,
              (unsigned)BATT_CUTOFF_STRIKES);
        return false;
    }
    return true;
}

// ── setup / loop ──────────────────────────────────────────────────
static UiState  lastUi;
static uint32_t lastUiMs = 0;

// NVS 에 저장된 정보로 접속한다. 저장된 게 없으면 secrets.h 의 초기값을 쓴다.
//
// 한 번 실패했다고 바로 설정 포털로 가지 않는다. 정전 후 복구처럼 공유기가
// 아직 안 올라왔거나 첫 인증이 어긋나는 일이 있는데, 그 상태는 기다린다고
// 풀리지 않는다. 접속을 처음부터 다시 걸어야 한다.
// 접속이 왜 안 되는지는 이 이벤트의 reason 코드에 다 들어 있다. 2=AUTH_EXPIRE,
// 15=4WAY_HANDSHAKE_TIMEOUT(암호가 틀렸거나 공유기가 예전 세션을 붙들고 있다),
// 201=NO_AP_FOUND, 205=CONNECTION_FAIL.
static volatile uint8_t lastWifiReason = 0;

static void onWifiEvent(arduino_event_id_t id, arduino_event_info_t info) {
    if (id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        lastWifiReason = info.wifi_sta_disconnected.reason;
        RLOGI("Wi-Fi 끊김 (reason=%u)", (unsigned)lastWifiReason);
    }
}

// 시리얼을 물리지 않아도 원인을 알 수 있게 화면에 같이 띄운다.
static String wifiReasonText() {
    const uint8_t r = lastWifiReason;
    if (!r) return String();
    const char* what = "";
    switch (r) {
        case 2:   what = " auth expire"; break;
        case 15:  what = " handshake"; break;
        case 201: what = " no AP"; break;
        case 202: what = " auth fail"; break;
        case 205: what = " conn fail"; break;
        default:  break;
    }
    return "fail r" + String((unsigned)r) + what;
}

// 접속하지 못한 채로 부팅했는지. loop() 에서 계속 다시 붙어 본다.
static bool wifiRecovery = false;

static bool connectWifi(uint8_t attempts) {
    const WifiCreds c = wifiLoadCreds();

    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        WiFi.onEvent(onWifiEvent);
    }

    for (uint8_t attempt = 1; attempt <= attempts; attempt++) {
        RLOGI("Wi-Fi 접속 시도 %u/%u: %s", (unsigned)attempt, (unsigned)attempts,
              c.ssid.c_str());
        if (attempt > 1) {
            WiFi.disconnect(true);  // 이전 시도의 찌꺼기를 지운다
            delay(500);
        }

        WiFi.mode(WIFI_STA);
        WiFi.setSleep(WIFI_PS_NONE);  // 접속 과정은 확실하게
        WiFi.setAutoReconnect(true);
        WiFi.begin(c.ssid.c_str(), c.pass.c_str());

        const uint32_t waitMs = (attempts > 1 && attempt == 1) ? kWifiFirstTryMs : kWifiRetryMs;
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
            delay(250);
        }
        if (WiFi.status() == WL_CONNECTED) {
            RLOGI("Wi-Fi 접속됨: %s", WiFi.localIP().toString().c_str());
            return true;
        }
        RLOGE("Wi-Fi 접속 실패 (status=%d)", (int)WiFi.status());
    }
    RLOGE("Wi-Fi 접속 포기: %s — 설정 포털로", c.ssid.c_str());
    return false;
}

// 설정 포털. AP 를 띄우고 화면에 접속 방법을 적는다. 저장되면 재시작한다.
static void runWifiPortal() {
    setState(ST_WIFISETUP);
    lastUi = snapshotUi();
    uiRender(lastUi, true);

    const bool saved = wifiRunPortal(kWifiPortalMs, [](const String& ap, const String& url,
                                                      const String& pass) {
        UiState u;
        u.state = ST_WIFISETUP;
        u.apSsid = ap;
        u.apPass = pass;
        u.apUrl = url;
        u.detail = wifiReasonText();
        uiRenderWifiSetup(u);
    });

    // 포털이 도는 동안 눌린 것은 잊는다. 닫자마자 채널이 바뀌면 곤란하다.
    btnBoot.clear();
    btnPwr.clear();

    if (saved) {
        RLOGI("새 Wi-Fi 정보 저장됨 — 재시작");
        delay(300);
        ESP.restart();
    }
    RLOGE("설정 포털 시간 초과 — Wi-Fi 없이 계속");
    setState(ST_ERROR, lastWifiReason ? ("NO WIFI r" + String((unsigned)lastWifiReason))
                                      : String("NO WIFI"));
    lastUi = snapshotUi();
    uiRender(lastUi, true);
}

static void clockTick();               // 시계 모드 갱신, 돌아오지 않는다
static void enterDeepSleepFromClock(); // 시계 모드 -> 완전한 딥슬립, 돌아오지 않는다
static void sleepAgain(bool clockTimer);  // 딥슬립 진입, 돌아오지 않는다

// ── OTA ───────────────────────────────────────────────────────────
// 이 보드를 매번 뜯어서 USB 를 꽂기 번거로워서 무선 업데이트를 넣었다.
// 파티션 테이블(default_8MB.csv)에 app0/app1 두 슬롯이 이미 있어서 그대로 쓴다.
static void setupOta() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);

    ArduinoOTA.onStart([]() {
        RLOGI("OTA 시작");
        // 업데이트 중에는 대역폭과 CPU 를 전부 내준다. 오디오 태스크가 계속
        // 돌면 전송이 느려지고 힙도 물고 있다.
        if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
        audio.stopSong();
        codec.setMute(true);
        codec.setPaEnabled(false);

        setState(ST_UPDATING);
        lastUi = snapshotUi();
        uiRender(lastUi, true);
        // 전송 중에는 화면을 안 건드린다. 한 번 갱신에 0.36초씩 잡아먹는다.
    });

    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t lastPct = 255;
        const uint8_t pct = total ? (uint8_t)(done * 100 / total) : 0;
        if (pct / 10 != lastPct / 10) {
            lastPct = pct;
            RLOGI("OTA %u%%", pct);
        }
    });

    ArduinoOTA.onEnd([]() {
        RLOGI("OTA 완료 — 재시작");
        setState(ST_UPDATING, "REBOOT");
        lastUi = snapshotUi();
        uiRender(lastUi, true);
    });

    ArduinoOTA.onError([](ota_error_t e) {
        RLOGE("OTA 실패: %u", (unsigned)e);
        setState(ST_ERROR, "OTA FAIL");
        if (audioTaskHandle) vTaskResume(audioTaskHandle);
        codec.setPaEnabled(true);
    });

    ArduinoOTA.begin();
    RLOGI("OTA 대기: %s.local (%s)", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void setup() {
    setCpuFrequencyMhz(CPU_FREQ_MHZ);

    Serial.begin(115200);
    // USB 가 호스트에 연결됐는데 아무도 포트를 읽지 않으면 CDC 링버퍼가 차고,
    // write 한 번이 최대 2초(20회 x 100ms)까지 블로킹된다. 그게 오디오 태스크에서
    // 일어나면 소리가 끊긴다 — 충전 중에만 끊기던 원인이 이것이다.
    // 0 으로 두면 막히는 대신 그냥 버린다. 로그보다 소리가 우선이다.
    Serial.setTxTimeoutMs(0);
    delay(300);

    RLOGI("ESP32-S3 ePaper FM Radio  (CPU %u MHz, 모뎀슬립 재생=%s 유휴=%s)",
          (unsigned)getCpuFrequencyMhz(), WIFI_SLEEP_WHILE_PLAYING ? "on" : "off",
          WIFI_SLEEP_WHILE_IDLE ? "on" : "off");

    // 딥슬립에서 깨어난 것인지, 전원을 새로 넣은 것인지 남긴다.
    // 깨우기가 안 될 때 '못 깨어난 것'과 '깨어나서 죽은 것'을 구분해 준다.
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT1:
            RLOGI("딥슬립에서 기상 (버튼)");
            break;
        case ESP_SLEEP_WAKEUP_EXT0:
            RLOGI("딥슬립에서 기상 (ext0)");
            break;
        case ESP_SLEEP_WAKEUP_UNDEFINED:
            RLOGI("전원 인가로 부팅");
            break;
        default:
            RLOGI("기상 원인 코드 %d", (int)esp_sleep_get_wakeup_cause());
            break;
    }

    sharedLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(Cmd));

    const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
    const bool wasOff = rtcInOffMode;

    // 시계 모드에서 타이머로 깨어난 것이면 Wi-Fi 도, 오디오도 올리지 않는다.
    // 값만 갱신하고 곧바로 다시 잠든다 — 돌아오지 않는다.
    if (wasOff && wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
        clockTick();
    }

    // 시계 모드에서 버튼으로 깨웠다면, 어느 버튼이 깨웠는지로 갈린다.
    //   PWR  : 라디오 모드로 켠다
    //   BOOT : 완전한 딥슬립으로
    //
    // 처음에는 '누른 길이'로 구분했는데 그게 동작하지 않았다. 딥슬립에서
    // 부팅하는 데만 1초 가까이 걸리는데 그 뒤부터 2초를 재고 있어서, 실제로는
    // 3초 넘게 눌러야 했다. ext1 은 어느 핀이 깨웠는지 알려주므로 그걸 쓰면
    // 타이밍에 전혀 기대지 않아도 된다.
    if (wasOff && wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
        const uint64_t who = esp_sleep_get_ext1_wakeup_status();
        if (who & (1ULL << PIN_BTN_BOOT)) {
            RLOGI("BOOT 로 깨움 — 완전한 딥슬립으로");
            enterDeepSleepFromClock();  // 돌아오지 않는다
        }
        RLOGI("PWR 로 깨움 — 라디오 모드로");
    }

    // 전원을 새로 넣었거나 짧게 눌러 깨운 것이면 평소대로 켜진다.
    rtcInOffMode = false;
    rtcDeepSleepMode = false;

    powerUpRails();
    btnBoot.begin();
    btnPwr.begin();
    battery.begin(PIN_VBAT_ADC);
    i2cBegin();
    rtcClock.begin();
    sensor.begin();

    // 딥슬립 전에 듣던 채널과 음량을 이어받는다.
    lockShared();
    shared.index = (rtcStationIndex < kStationCount) ? rtcStationIndex : kDefaultIndex;
    shared.volume = (rtcVolume <= kVolumeSteps) ? rtcVolume : kDefaultVolume;
    unlockShared();

    pollSlowStatus();

    uiBegin();

    // 배터리가 바닥인 채로 라디오를 켜면 Wi-Fi 송신 피크(300mA+)에서 전압이
    // 주저앉아 그대로 죽는다. 사용자에게는 '버튼을 눌러도 아무 반응이 없다'로
    // 보인다. 아예 시작하지 않고 이유를 화면에 남긴 뒤 다시 잠든다.
    // USB 가 꽂혀 있으면 전압이 4V 부근이라 여기에 걸리지 않는다.
    {
        lockShared();
        const float   v = shared.battVolts;
        const uint8_t pct = shared.battPercent;
        unlockShared();

        if (v >= 2.5f && pct <= BATT_CUTOFF_PERCENT) {
            RLOGE("배터리 %u%% (%.2fV) — 라디오를 켜지 않는다", (unsigned)pct, v);
            UiState low;
            low.state = ST_LOWBATT;
            low.battVolts = v;
            low.battPercent = pct;
            low.freq = kStations[rtcStationIndex].freq;
            low.name = kStations[rtcStationIndex].name;
            struct tm now = {};
            if (readClock(&now)) {
                low.hasTime = true;
                low.hour = (uint8_t)now.tm_hour;
                low.minute = (uint8_t)now.tm_min;
            }
            uiRenderOff(low, true);
            rtcInOffMode = true;
            sleepAgain(false);  // 돌아오지 않는다
        }
    }

    lastUi = snapshotUi();
    uiRender(lastUi, true);

    // ── Wi-Fi ────────────────────────────────────────────────────
    setState(ST_WIFI);
    lastUi = snapshotUi();
    uiRender(lastUi);

    if (!connectWifi(kWifiAttempts)) {
        // 접속에 실패하면 설정 포털을 띄운다. 저장되면 재시작해서 다시 붙는다.
        runWifiPortal();
        wifiRecovery = true;
        return;  // 포털이 끝났는데도 못 붙었으면 loop() 에서 계속 재시도
    }
    RLOGI("Wi-Fi 접속됨: %s", WiFi.localIP().toString().c_str());
    setupOta();
    syncNtpIfDue();            // 하루 한 번. RTC 가 멀쩡하면 그냥 지나간다
    applyWifiPowerSave(true);  // 곧 재생을 시작하므로 재생용 정책으로

    // ── I2S + 코덱 ───────────────────────────────────────────────
    // 코덱을 먼저 설정하려면 MCLK 가 나오고 있어야 하므로 setPinout 이 먼저다.
    RLOGI("I2S 설정 중...");
    if (!audio.setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_MCLK)) {
        RLOGE("I2S setPinout 실패");
        setState(ST_ERROR, "NO I2S");
        lastUi = snapshotUi();
        uiRender(lastUi, true);
        return;
    }
    audio.setVolumeSteps(21);
    audio.setVolume(21);  // 라이브러리는 풀스케일, 실제 음량은 코덱이 담당
    audio.setConnectionTimeout(8000, 12000);

    RLOGI("ES8311 초기화 중...");
    if (!codec.begin(PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE, AUDIO_BITS,
                     AUDIO_MCLK_DIV, PIN_AUDIO_PA)) {
        setState(ST_ERROR, "NO CODEC");
        lastUi = snapshotUi();
        uiRender(lastUi, true);
        return;
    }
    applyVolume();
    RLOGI("코덱 준비 완료");

    // 방송국 이름 / 비트레이트 콜백. 오디오 태스크에서 불리므로 여기선 값만 담는다.
    // m.s 는 이벤트 이름, m.msg 가 내용, m.arg1 이 그 안에서 뽑아낸 마지막 숫자다.
    Audio::audio_info_callback = [](Audio::msg_t m) {
        // 콜백을 걸어두면 라이브러리 내부 로그도 전부 이쪽으로 넘어온다.
        // 다만 evt_info 는 세그먼트마다 쏟아지는데다 이 콜백이 오디오 태스크에서
        // 돌기 때문에, 기본적으로는 버리고 의미 있는 이벤트만 남긴다.
        const bool noisy = (m.e == Audio::evt_info);
        if (m.msg && (AUDIO_VERBOSE_LOG || !noisy)) {
            Serial.printf("[A/%s] %s\n", m.s ? m.s : "?", m.msg);
        }

        switch (m.e) {
            case Audio::evt_bitrate:
                lockShared();
                shared.bitrate = (uint32_t)(m.arg1 > 0 ? m.arg1 : 0);
                unlockShared();
                break;
            case Audio::evt_eof:
                // 라이브 스트림이 끊긴 것. audioTask 의 재시도 루프가 알아서 다시 붙는다.
                setState(ST_ERROR, "RECONNECT");
                break;
            default:
                break;
        }
    };

    // TLS 핸드셰이크(WiFiClientSecure)가 스택을 많이 먹어서 넉넉히 잡는다.
    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 3, &audioTaskHandle, 1);

    lockShared();
    const uint8_t startIndex = shared.index;
    unlockShared();
    const Cmd cmd{Cmd::TUNE, startIndex};
    xQueueSend(cmdQueue, &cmd, 0);
}

// ── 전원 끄기 (딥슬립) ────────────────────────────────────────────
// 완전히 차단하는 스위치가 없어서 딥슬립으로 대신한다. ePaper 는 전원이
// 끊겨도 그림이 남으므로 꺼진 화면이 그대로 유지된다.
// PWR(GPIO18)을 다시 누르면 깨어나고, 그때는 setup() 부터 새로 시작한다.
static void sleepAgain(bool clockTimer);

// 라디오 모드에서 빠져나가 잠든다.
//
//   clockMode=true  : 시계 모드. 1분마다 깨어나 시각·온습도를 고친다(~1mA).
//   clockMode=false : 완전한 딥슬립. 화면은 그 시점에서 멈춘다(수십 µA).
static void powerOff(bool clockMode, const char* why) {
    RLOGI("%s — %s", why, clockMode ? "시계 모드" : "딥슬립");

    // 소리부터 끊는다. 화면 그리는 동안 계속 울리면 어색하다.
    if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
    audio.stopSong();
    codec.setMute(true);
    codec.setPaEnabled(false);

    // 공유기에 인사를 하고 내려간다. 그냥 사라지면 공유기가 한동안 예전 세션을
    // 붙들고 있어서, 다시 켰을 때 4-way 핸드셰이크가 막히는 일이 있다.
    WiFi.disconnect(true);

    // 깨어났을 때 이어서 쓸 수 있도록 남긴다.
    lockShared();
    rtcStationIndex = shared.index;
    rtcVolume = shared.volume;
    unlockShared();
    rtcInOffMode = true;
    rtcClockTicks = 0;

    rtcDeepSleepMode = !clockMode;

    if (clockMode) {
        UiState off = snapshotUi();
        if (sensor.ok() && sensor.read(&off.tempC, &off.humidity)) {
            off.hasEnv = true;
            off.tempC -= SHTC3_TEMP_OFFSET_C;
            // 방금까지 재생 중이라 보드가 더워져 있어서 이 한 프레임은 실제보다
            // 높게 나온다. 1분 뒤 시계 갱신이 식은 값으로 덮어쓴다.
        }
        uiRenderOff(off, true, false);  // 들어갈 때 한 번은 깨끗하게
        // 전체 갱신이라 이 그림이 컨트롤러의 이전/현재 버퍼 양쪽에 들어간다.
        // 다음 시계 갱신이 이걸 기준으로 부분 갱신을 할 수 있다.
        saveClockSnapshot(off, false);
    } else {
        // 딥슬립은 사진으로 화면을 채운다. BOOT 를 누르면 측정값 화면으로 바뀐다.
        rtcSleepShowValues = false;
        uiRenderPhoto();
        rtcPrevScreen.valid = false;  // 사진이라 시계 부분 갱신의 기준이 못 된다
    }

    sleepAgain(clockMode);
}

// 딥슬립 진입부. 전원을 끌 때와 시계 갱신 뒤 다시 잠들 때가 같은 코드를 쓴다.
// clockTimer=false 면 타이머를 걸지 않아 버튼을 누를 때까지 완전히 잔다.
// 돌아오지 않는다.
static void sleepAgain(bool clockTimer) {
    // 누른 채로 잠들면 곧바로 다시 깨어난다. 뗄 때까지 기다린다.
    while (digitalRead(PIN_BTN_PWR) == LOW || digitalRead(PIN_BTN_BOOT) == LOW) delay(20);
    delay(100);

    // 전원 레일 차단. EPD/Audio 는 Active-LOW 라 HIGH 가 OFF,
    // VBAT 분압 게이팅은 Active-HIGH 라 LOW 가 OFF.
    //
    // EPD 레일은 끄지 않는다. 1분마다 시계를 갱신해야 하는데, 전원을 끊으면
    // 패널의 이전 이미지가 사라져 매번 전체 갱신(2초 + 번쩍임)을 해야 한다.
    // 하이버네이트된 패널은 1µA 수준이라 켜 두는 편이 싸다.
    digitalWrite(PIN_PWR_AUDIO, HIGH);

    // VBAT(GPIO17)은 절대 내리지 않는다. 이 핀은 배터리 전압 분압 게이팅이
    // 아니라 배터리 전원 자체를 끊는 스위치다 (Waveshare 07_BATT_PWR_Test 가
    // 화면에 OFF 를 찍고 VBAT_POWER_OFF() 를 부르는 것이 소프트 전원 차단이다).
    // 여기서 LOW 로 내리면 딥슬립에 들어가며 스스로 전원을 끊는다. USB 가
    // 꽂혀 있으면 USB 가 먹여 살려서 멀쩡하지만, 배터리만 있으면 그대로 죽는다.
    digitalWrite(PIN_PWR_VBAT, HIGH);

    // 딥슬립에 들어가면 일반 GPIO 는 전원이 내려가 떠 버린다. 레일이
    // 다시 켜지지 않도록 레벨을 붙잡아 둔다.
    gpio_hold_en((gpio_num_t)PIN_PWR_AUDIO);
    gpio_hold_en((gpio_num_t)PIN_PWR_EPD);
    gpio_hold_en((gpio_num_t)PIN_PWR_VBAT);
    gpio_deep_sleep_hold_en();

    // 기상 조건. ext0 대신 ext1 을 쓴다.
    //
    // ext0 는 핀 하나만 되고 RTC 주변장치 도메인이 켜져 있어야 하는데,
    // 그 설정을 빠뜨리면 슬립 중 내부 풀업이 죽어 핀이 떠 버리고 버튼을
    // 눌러도 아무 일이 없다. ext1 은 여러 핀을 한꺼번에 볼 수 있어서
    // PWR 과 BOOT 어느 쪽을 눌러도 깨어나게 할 수 있다.
    //
    // 두 핀 모두 내부 풀업으로 HIGH 를 유지하다가 눌리면 LOW 로 떨어지므로
    // ANY_LOW 로 잡는다. 풀업이 슬립 중에도 살아 있으려면 RTC 주변장치
    // 도메인을 켜 둬야 한다 — 이게 앞서 빠졌던 부분이다.
    const uint64_t wakeMask = (1ULL << PIN_BTN_PWR) | (1ULL << PIN_BTN_BOOT);
    for (int pin : {(int)PIN_BTN_PWR, (int)PIN_BTN_BOOT}) {
        rtc_gpio_init((gpio_num_t)pin);
        rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_dis((gpio_num_t)pin);
        rtc_gpio_pullup_en((gpio_num_t)pin);
    }
    esp_sleep_enable_ext1_wakeup(wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

    // 시계 모드면 1분마다 잠깐 깨어난다. 딥슬립 모드면 타이머를 걸지 않아
    // 버튼을 누를 때까지 완전히 잔다.
    if (clockTimer) {
        esp_sleep_enable_timer_wakeup(CLOCK_TICK_SEC * 1000000ULL);
    } else {
        RLOGI("타이머 없이 완전히 잠든다");
    }

    Serial.flush();
    delay(50);
    esp_deep_sleep_start();
}

// ── 시계 모드 ─────────────────────────────────────────────────────
// 꺼진 상태에서 1분마다 깨어나는 경로. Wi-Fi 는 절대 올리지 않는다 —
// 그게 전력의 대부분이라 올리는 순간 시계 모드의 의미가 없어진다.
// 값만 읽어 화면을 고치고 곧바로 다시 잠든다. 돌아오지 않는다.
static void clockTick() {
    powerUpRails();          // EPD 는 이미 켜져 있고, 여기서 홀드를 푼다
    digitalWrite(PIN_PWR_AUDIO, HIGH);  // 오디오는 계속 꺼 둔다

    battery.begin(PIN_VBAT_ADC);
    i2cBegin();
    rtcClock.begin();
    sensor.begin();

    UiState u;
    u.freq = kStations[rtcStationIndex].freq;
    u.name = kStations[rtcStationIndex].name;
    u.state = ST_PAUSED;
    u.volumeMax = kVolumeSteps;

    struct tm now = {};
    if (readClock(&now)) {
        u.hasTime = true;
        u.hour = (uint8_t)now.tm_hour;
        u.minute = (uint8_t)now.tm_min;
        u.month = (uint8_t)(now.tm_mon + 1);
        u.day = (uint8_t)now.tm_mday;
        u.weekday = (uint8_t)now.tm_wday;
    }
    float rawTemp = 0.0f;
    u.hasEnv = sensor.read(&rawTemp, &u.humidity);
    u.tempC = rawTemp - SHTC3_TEMP_OFFSET_C;
    u.battVolts = battery.readVolts();
    u.battPercent = Battery::voltsToPercent(u.battVolts);

    // raw 를 같이 찍어 둔다. 실제 온도계와 비교해 보정값을 정할 때 쓴다.
    RLOGI("시계 갱신: %02u:%02u  %.1fC (raw %.1f) %.0f%%  배터리 %.2fV (%u%%)",
          (unsigned)u.hour, (unsigned)u.minute, u.tempC, rawTemp, u.humidity,
          u.battVolts, (unsigned)u.battPercent);

    // initial=false — 패널은 하이버네이트만 됐을 뿐 이전 이미지를 갖고 있다.
    // true 로 부르면 GxEPD2 가 다음 갱신을 전체 갱신으로 승격시켜서, 1분마다
    // 1.4초씩 화면이 번쩍인다.
    // 부분 갱신으로 조용히 숫자만 바꾼다. 매분 전체 갱신을 하면 1.4초씩
    // 번쩍인다.
    //
    // initial=false 로 열어야 GxEPD2 가 부분 갱신을 허용한다. 다만 그것만으로는
    // 부족했다 — 부분 갱신은 컨트롤러 안의 '이전 이미지'와의 차분으로 동작하는데,
    // 딥슬립에서 깨어나며 하드웨어 리셋을 거치면 그게 남아 있다고 믿을 수 없다.
    // 그래서 직전 화면을 0x26 에 다시 그려 넣고 나서 갱신한다.
    uiBegin(false);

    // 잔상은 30분에 한 번 전체 갱신으로 턴다. 기억해 둔 직전 화면이 없으면
    // 기준이 없으니 어쩔 수 없이 전체 갱신.
    // 틱 0 은 제외한다 — 시계 모드에 들어올 때 이미 전체 갱신을 했다.
    const bool full =
        !rtcPrevScreen.valid ||
        (rtcClockTicks > 0 && (rtcClockTicks % CLOCK_FULL_REFRESH_TICKS) == 0);
    RLOGI("갱신 방식: %s (tick=%u, prev=%s)", full ? "전체" : "부분",
          (unsigned)rtcClockTicks, rtcPrevScreen.valid ? "있음" : "없음");
    if (!full) {
        uiRenderOffToPrevious(restoreClockSnapshot(), rtcPrevScreen.frozen);
    }
    uiRenderOff(u, full);
    saveClockSnapshot(u, false);
    rtcClockTicks++;

    // 배터리가 바닥이면 더 이상 깨어나지 않는다. 셀을 과방전에서 지킨다.
    const bool low = u.battVolts >= 2.5f && u.battPercent <= BATT_CUTOFF_PERCENT;
    if (low) RLOGI("배터리 %u%% — 시계 갱신을 멈춘다", (unsigned)u.battPercent);
    sleepAgain(!low);
}

// 시계 모드에서 PWR 을 길게 눌러 완전히 재우는 경로. 화면이 그 시점에서
// 멈추므로, 시계가 멈춘 게 고장이 아니라는 걸 알 수 있게 표시를 남긴다.
static void enterDeepSleepFromClock() {
    powerUpRails();
    digitalWrite(PIN_PWR_AUDIO, HIGH);

    battery.begin(PIN_VBAT_ADC);
    i2cBegin();
    rtcClock.begin();
    sensor.begin();

    UiState u;
    u.freq = kStations[rtcStationIndex].freq;
    u.name = kStations[rtcStationIndex].name;
    u.volumeMax = kVolumeSteps;

    struct tm now = {};
    if (readClock(&now)) {
        u.hasTime = true;
        u.hour = (uint8_t)now.tm_hour;
        u.minute = (uint8_t)now.tm_min;
        u.month = (uint8_t)(now.tm_mon + 1);
        u.day = (uint8_t)now.tm_mday;
        u.weekday = (uint8_t)now.tm_wday;
    }
    float rawTemp = 0.0f;
    u.hasEnv = sensor.read(&rawTemp, &u.humidity);
    u.tempC = rawTemp - SHTC3_TEMP_OFFSET_C;
    u.battVolts = battery.readVolts();
    u.battPercent = Battery::voltsToPercent(u.battVolts);

    uiBegin();

    // 시계 모드에서 처음 넘어오면 사진. 이미 딥슬립인데 BOOT 를 또 눌렀으면
    // 사진 ↔ 측정값을 오간다.
    if (!rtcDeepSleepMode) {
        rtcDeepSleepMode = true;
        rtcSleepShowValues = false;
    } else {
        rtcSleepShowValues = !rtcSleepShowValues;
    }

    if (rtcSleepShowValues) {
        uiRenderOff(u, true, true);  // 그 순간 잰 값 + AT HH:MM
    } else {
        uiRenderPhoto();
    }
    rtcPrevScreen.valid = false;  // 시계 부분 갱신의 기준이 못 된다
    sleepAgain(false);
}

void loop() {
    // 붙지 못한 채로 부팅했고 설정 포털도 소용없었던 경우. 여기서 계속 다시
    // 붙어 본다. 공유기가 늦게 올라오는 일이 있는데, 그냥 포기해 버리면
    // 사용자가 직접 껐다 켜야 한다. 붙으면 재시작해서 setup 을 처음부터 다시
    // 탄다 — 오디오 태스크가 그때 만들어지므로 이어서 하기보다 이 편이 낫다.
    if (wifiRecovery) {
        static uint32_t lastTry = 0;
        if (millis() - lastTry > kWifiRecoveryMs) {
            lastTry = millis();
            if (connectWifi(1)) {
                RLOGI("Wi-Fi 복구됨 — 재시작");
                delay(200);
                ESP.restart();
            }
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        // 붙었다가 끊긴 경우. 드라이버에 설정이 남아 있으므로 이쪽이면 된다.
        static uint32_t lastRetry = 0;
        if (millis() - lastRetry > 10000) {
            lastRetry = millis();
            WiFi.reconnect();
        }
    }

    // ── 버튼 ─────────────────────────────────────────────────────
    if (const uint8_t ev = btnBoot.poll()) {
        if (ev == 3) {
            // 아주 길게 누르면 Wi-Fi 설정 포털. 다른 곳에 들고 갔을 때 쓴다.
            if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
            audio.stopSong();
            codec.setMute(true);
            codec.setPaEnabled(false);
            runWifiPortal();
            if (audioTaskHandle) vTaskResume(audioTaskHandle);
            return;
        }
        lockShared();
        if (ev == 1) shared.index = (uint8_t)((shared.index + 1) % kStationCount);
        else         shared.index = (uint8_t)((shared.index + kStationCount - 1) % kStationCount);
        shared.bitrate = 0;
        const uint8_t idx = shared.index;
        unlockShared();

        const Cmd cmd{Cmd::TUNE, idx};
        xQueueSend(cmdQueue, &cmd, 0);
    }

    if (const uint8_t ev = btnPwr.poll()) {
        if (ev == 3) {
            // 라디오 -> 시계 모드. 여기서 다시 길게 누르면 완전한 딥슬립.
            powerOff(true, "전원 끔");  // 돌아오지 않는다
        } else if (ev == 2) {
            // 일시정지 토글. 뮤트와 달리 스트림과 오디오 전원까지 끊는다.
            lockShared();
            const bool paused = shared.paused;
            unlockShared();

            const Cmd cmd{paused ? Cmd::RESUME : Cmd::PAUSE, 0};
            xQueueSend(cmdQueue, &cmd, 0);
        } else {
            lockShared();
            shared.volume = (uint8_t)(shared.volume + 2);
            if (shared.volume > kVolumeSteps) shared.volume = 0;
            unlockShared();
            applyVolume();
        }
    }

    // ── 무선 업데이트 ────────────────────────────────────────────
    if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

    // ── 배터리 ───────────────────────────────────────────────────
    static uint32_t lastStatusMs = 0;
    if (millis() - lastStatusMs > kStatusPeriodMs) {
        lastStatusMs = millis();
        pollSlowStatus();
        if (batteryCutoffDue()) {
            // 배터리 보호는 시계 갱신조차 하지 않고 완전히 재운다.
            powerOff(false, "배터리 부족");  // 돌아오지 않는다
        }
    }

    // ── ePaper ───────────────────────────────────────────────────
    const UiState now = snapshotUi();
    if (uiChanged(lastUi, now) || millis() - lastUiMs > kUiPeriodMs) {
        lastUi = now;
        lastUiMs = millis();
        uiRender(now);
    }

    delay(20);
}
