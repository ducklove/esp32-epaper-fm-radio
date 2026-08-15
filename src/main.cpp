// Waveshare ESP32-S3-ePaper-1.54 인터넷 스트리밍 FM 라디오
//
// 보드에 FM 튜너 칩이 없어서 전파를 직접 받지는 못한다. 대신 방송사 HLS
// 스트림을 Wi-Fi 로 받아 ES8311 코덱 → 온보드 스피커로 재생하고,
// ePaper 에는 실제 서울 FM 주파수를 그대로 쓴 아날로그 다이얼을 그린다.
//
// 조작 (버튼 2개)
//   BOOT 짧게       : 다음 채널 (주파수 오름차순)
//   BOOT 길게       : 이전 채널
//   PWR  짧게       : 볼륨 +2 (최대에서 다시 0으로)
//   PWR  길게 0.7초 : 일시정지 / 재개
//   PWR  길게 2초   : 전원 끔 (딥슬립). 다시 PWR 을 누르면 켜진다.
//
// 스레드 구조
//   audioTask (core 1, prio 3) : 선국 + audio.loop(). 오디오는 여기서만 만진다.
//   loopTask  (core 1, prio 1) : 버튼 + ePaper. 갱신에 1초 넘게 걸려도
//                                 우선순위가 낮아 소리가 끊기지 않는다.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Audio.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "battery.h"
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

// ── 전역 ──────────────────────────────────────────────────────────
static Audio    audio;
static ES8311   codec;
static Battery  battery;
static TaskHandle_t audioTaskHandle = nullptr;

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

    lockShared();
    shared.paused = true;
    shared.state = ST_PAUSED;
    shared.bitrate = 0;
    unlockShared();
}

static void resumeAudio() {
    RLOGI("재개 — 오디오 전원 복구");
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
class Button {
  public:
    explicit Button(uint8_t pin) : _pin(pin) {}

    void begin() { pinMode(_pin, INPUT_PULLUP); }

    // 눌렀다 뗐을 때 1회만 보고한다.
    // 0 = 없음, 1 = 짧게, 2 = 길게, 3 = 아주 길게
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
            if (held >= kVeryLongPressMs) return 3;
            return held >= kLongPressMs ? 2 : 1;
        }
        return 0;
    }

    bool isDown() const { return _down; }

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
    // 딥슬립에서 깨어난 경우 powerOff() 가 걸어 둔 홀드가 아직 살아 있다.
    // 먼저 풀지 않으면 아래 digitalWrite 가 먹히지 않아 전원이 안 켜진다.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)PIN_PWR_EPD);
    gpio_hold_dis((gpio_num_t)PIN_PWR_AUDIO);
    gpio_hold_dis((gpio_num_t)PIN_PWR_VBAT);
    // 깨우기용으로 RTC 기능을 켜 둔 버튼도 일반 GPIO 로 되돌린다.
    rtc_gpio_deinit((gpio_num_t)PIN_BTN_PWR);

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
}

// ── setup / loop ──────────────────────────────────────────────────
static UiState  lastUi;
static uint32_t lastUiMs = 0;

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
    delay(300);
    RLOGI("ESP32-S3 ePaper FM Radio  (CPU %u MHz, 모뎀슬립 %s)",
          (unsigned)getCpuFrequencyMhz(), WIFI_MODEM_SLEEP ? "on" : "off");

    sharedLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(Cmd));

    powerUpRails();
    btnBoot.begin();
    btnPwr.begin();
    battery.begin(PIN_VBAT_ADC);
    pollSlowStatus();

    uiBegin();
    lastUi = snapshotUi();
    uiRender(lastUi, true);

    // ── Wi-Fi ────────────────────────────────────────────────────
    setState(ST_WIFI);
    lastUi = snapshotUi();
    uiRender(lastUi);

    WiFi.mode(WIFI_STA);
    // 모뎀 슬립은 배터리에서 가장 크게 아끼는 항목이다(30~50mA). 끊김은
    // 640KB 입력 버퍼가 흡수한다. config.h 에서 되돌릴 수 있다.
    WiFi.setSleep(WIFI_MODEM_SLEEP ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
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
    setupOta();

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
    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 3, &audioTaskHandle, 1);

    const Cmd cmd{Cmd::TUNE, kDefaultIndex};
    xQueueSend(cmdQueue, &cmd, 0);
}

// ── 전원 끄기 (딥슬립) ────────────────────────────────────────────
// 완전히 차단하는 스위치가 없어서 딥슬립으로 대신한다. ePaper 는 전원이
// 끊겨도 그림이 남으므로 꺼진 화면이 그대로 유지된다.
// PWR(GPIO18)을 다시 누르면 깨어나고, 그때는 setup() 부터 새로 시작한다.
static void powerOff() {
    RLOGI("전원 끔 — 딥슬립 진입");

    // 소리부터 끊는다. 화면 그리는 동안 계속 울리면 어색하다.
    if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
    audio.stopSong();
    codec.setMute(true);
    codec.setPaEnabled(false);

    uiRenderOff(snapshotUi());
    uiSleep();  // 패널을 하이버네이트한 뒤에 전원을 끊어야 안전하다

    // 누른 채로 잠들면 곧바로 다시 깨어난다. 뗄 때까지 기다린다.
    while (digitalRead(PIN_BTN_PWR) == LOW) delay(20);
    delay(100);

    // 전원 레일 차단. EPD/Audio 는 Active-LOW 라 HIGH 가 OFF,
    // VBAT 분압 게이팅은 Active-HIGH 라 LOW 가 OFF.
    digitalWrite(PIN_PWR_AUDIO, HIGH);
    digitalWrite(PIN_PWR_EPD, HIGH);
    digitalWrite(PIN_PWR_VBAT, LOW);

    // 딥슬립에 들어가면 일반 GPIO 는 전원이 내려가 떠 버린다. 레일이
    // 다시 켜지지 않도록 레벨을 붙잡아 둔다.
    gpio_hold_en((gpio_num_t)PIN_PWR_AUDIO);
    gpio_hold_en((gpio_num_t)PIN_PWR_EPD);
    gpio_hold_en((gpio_num_t)PIN_PWR_VBAT);
    gpio_deep_sleep_hold_en();

    // PWR 버튼이 LOW 로 떨어지면 깨어난다. 슬립 중에도 풀업이 살아 있어야 한다.
    rtc_gpio_pullup_en((gpio_num_t)PIN_BTN_PWR);
    rtc_gpio_pulldown_dis((gpio_num_t)PIN_BTN_PWR);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIN_BTN_PWR, 0);

    Serial.flush();
    esp_deep_sleep_start();
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
        if (ev == 3) {
            powerOff();  // 돌아오지 않는다 (딥슬립)
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
