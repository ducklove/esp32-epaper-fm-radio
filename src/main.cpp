// Waveshare ESP32-S3-ePaper-1.54 인터넷 스트리밍 FM 라디오
//
// 보드에 FM 튜너 칩이 없어서 전파를 직접 받지는 못한다. 대신 방송사 HLS
// 스트림을 Wi-Fi 로 받아 ES8311 코덱 → 온보드 스피커로 재생하고,
// ePaper 에는 실제 서울 FM 주파수를 그대로 쓴 아날로그 다이얼을 그린다.
//
// 조작 (버튼 2개)
//   BOOT 짧게  : 다음 채널 (주파수 오름차순)
//   BOOT 길게  : 이전 채널
//   PWR  짧게  : 볼륨 +2 (최대에서 다시 0으로)
//   PWR  길게  : 음소거 토글
//
// 스레드 구조
//   audioTask (core 1, prio 3) : 선국 + audio.loop(). 오디오는 여기서만 만진다.
//   loopTask  (core 1, prio 1) : 버튼 + ePaper. 갱신에 1초 넘게 걸려도
//                                 우선순위가 낮아 소리가 끊기지 않는다.
#include <Arduino.h>
#include <Audio.h>
#include <WiFi.h>

#include "config.h"
#include "es8311.h"
#include "log.h"
#include "secrets.h"
#include "stations.h"
#include "ui.h"

// ── 상수 ──────────────────────────────────────────────────────────
constexpr uint8_t  kVolumeSteps   = 20;
constexpr uint8_t  kDefaultVolume = 14;
constexpr uint8_t  kDefaultIndex  = 2;      // KBS Classic FM 93.1
constexpr uint32_t kWifiTimeoutMs = 30000;
constexpr uint32_t kLongPressMs   = 700;
constexpr uint32_t kUiPeriodMs    = 30000;  // 아무 일 없어도 이 주기로 한 번 갱신
constexpr uint32_t kRetryDelayMs  = 8000;   // 재접속 간격

// ── 전역 ──────────────────────────────────────────────────────────
static Audio  audio;
static ES8311 codec;

struct Shared {
    uint8_t   index = kDefaultIndex;
    uint8_t   volume = kDefaultVolume;
    bool      muted = false;
    PlayState state = ST_BOOT;
    String    detail;
    uint32_t  bitrate = 0;
};
static Shared           shared;
static SemaphoreHandle_t sharedLock;

static QueueHandle_t cmdQueue;
struct Cmd {
    enum Kind : uint8_t { TUNE } kind;
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

// 볼륨 단계를 코덱 dB 로. 20단계가 -38dB ~ 0dB 를 덮는다.
static float volumeToDb(uint8_t vol) { return -40.0f + 2.0f * (float)vol; }

static void applyVolume() {
    lockShared();
    const uint8_t vol = shared.volume;
    const bool    muted = shared.muted;
    unlockShared();

    if (muted || vol == 0) {
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

static void audioTask(void*) {
    uint32_t codecRate = AUDIO_SAMPLE_RATE;
    uint32_t lastRetryMs = millis();

    for (;;) {
        Cmd cmd;
        if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
            if (cmd.kind == Cmd::TUNE) tune(cmd.index);
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
class Button {
  public:
    explicit Button(uint8_t pin) : _pin(pin) {}

    void begin() { pinMode(_pin, INPUT_PULLUP); }

    // 눌렀다 뗐을 때 1회만 보고한다. 0 = 없음, 1 = 짧게, 2 = 길게
    uint8_t poll() {
        const bool down = digitalRead(_pin) == LOW;
        const uint32_t now = millis();

        if (down && !_down) {          // 눌림 시작
            _down = true;
            _since = now;
        } else if (!down && _down) {   // 뗌
            _down = false;
            const uint32_t held = now - _since;
            if (held < 40) return 0;   // 채터링
            return held >= kLongPressMs ? 2 : 1;
        }
        return 0;
    }

  private:
    uint8_t  _pin;
    bool     _down = false;
    uint32_t _since = 0;
};

static Button btnBoot(PIN_BTN_BOOT);
static Button btnPwr(PIN_BTN_PWR);

// ── 전원 레일 ─────────────────────────────────────────────────────
// EPD/Audio 는 Active-LOW, VBAT 만 Active-HIGH (Waveshare board_power_bsp.cpp)
static void powerUpRails() {
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
    const bool    muted = shared.muted;
    u.state = shared.state;
    u.detail = shared.detail;
    u.volume = muted ? 0 : shared.volume;
    u.bitrate = shared.bitrate;
    unlockShared();

    u.freq = kStations[idx].freq;
    u.name = kStations[idx].name;
    u.volumeMax = kVolumeSteps;
    u.wifi = WiFi.status() == WL_CONNECTED;
    if (u.state == ST_PLAYING && muted) u.state = ST_MUTED;
    return u;
}

static bool uiChanged(const UiState& a, const UiState& b) {
    return a.freq != b.freq || a.state != b.state || a.volume != b.volume ||
           a.wifi != b.wifi || a.detail != b.detail ||
           (a.bitrate / 1000) != (b.bitrate / 1000);
}

// ── setup / loop ──────────────────────────────────────────────────
static UiState  lastUi;
static uint32_t lastUiMs = 0;

void setup() {
    Serial.begin(115200);
    delay(300);
    RLOGI("ESP32-S3 ePaper FM Radio");

    sharedLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(Cmd));

    powerUpRails();
    btnBoot.begin();
    btnPwr.begin();

    uiBegin();
    lastUi = snapshotUi();
    uiRender(lastUi, true);

    // ── Wi-Fi ────────────────────────────────────────────────────
    setState(ST_WIFI);
    lastUi = snapshotUi();
    uiRender(lastUi);

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);  // 스트리밍 중 끊김 방지
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < kWifiTimeoutMs) {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) {
        RLOGE("Wi-Fi 접속 실패");
        setState(ST_ERROR, "NO WIFI");
        lastUi = snapshotUi();
        uiRender(lastUi, true);
        return;  // loop() 에서 계속 재시도된다
    }
    RLOGI("Wi-Fi 접속됨: %s", WiFi.localIP().toString().c_str());

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
        // 그냥 버리면 스트리밍이 왜 안 되는지 알 길이 없어서 시리얼로 흘려보낸다.
        if (m.msg) Serial.printf("[A/%s] %s\n", m.s ? m.s : "?", m.msg);

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
    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 3, nullptr, 1);

    const Cmd cmd{Cmd::TUNE, kDefaultIndex};
    xQueueSend(cmdQueue, &cmd, 0);
}

void loop() {
    // Wi-Fi 가 끊긴 채로 부팅했으면 계속 재시도
    if (WiFi.status() != WL_CONNECTED) {
        static uint32_t lastRetry = 0;
        if (millis() - lastRetry > 10000) {
            lastRetry = millis();
            WiFi.reconnect();
        }
    }

    // ── 버튼 ─────────────────────────────────────────────────────
    if (const uint8_t ev = btnBoot.poll()) {
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
        lockShared();
        if (ev == 1) {
            shared.volume = (uint8_t)(shared.volume + 2);
            if (shared.volume > kVolumeSteps) shared.volume = 0;
            shared.muted = false;
        } else {
            shared.muted = !shared.muted;
        }
        unlockShared();
        applyVolume();
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
