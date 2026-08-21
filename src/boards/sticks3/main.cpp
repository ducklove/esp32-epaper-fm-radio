// M5Stack M5StickS3 인터넷 스트리밍 FM 라디오
//
// ePaper 판과 MCU(ESP32-S3-PICO-1-N8R8)도 코덱(ES8311)도 같아서 오디오 경로는
// 핀 번호만 다르다. 근본적으로 다른 것은 화면이다.
//
// 전자종이는 전원을 끊어도 그림이 남아서 "1분마다 깨어나 시계를 고치고 다시
// 자는" 모드가 성립했다. LCD 는 전원을 끊는 순간 화면이 사라지므로 같은 것을
// 하려면 백라이트를 계속 켜 둬야 하고, 250mAh 로 다섯 시간이면 끝난다.
//
// 그래서 상시 표시를 포기하고 손목시계처럼 만든다 — 평소에는 화면을 끄고,
// 집어 들면(BMI270 움직임 감지) 켠다. 라디오는 화면과 무관하게 계속 재생된다.
//
// 조작 (버튼 3개 — 앞면 A, 옆면 B, 그리고 PWR)
//
//   A(G11) 짧게   : 다음 채널 (주파수 오름차순)
//   A(G11) 0.7초  : 이전 채널
//   A(G11) 2초    : Wi-Fi 설정 포털
//
//   B(G12) 짧게   : 볼륨 +2 (최대에서 다시 0으로)
//   B(G12) 0.7초  : 일시정지 / 재개
//   B(G12) 2초    : 전원 끔 (딥슬립)
//
//   PWR           : 펌웨어가 읽지 않는다. PMIC(M5PM1)가 직접 맡는다.
//                   M5Stack 공식 문서 기준으로 한 번 누름 = 켜기/리셋,
//                   두 번 누름 = 끄기, 길게 누름 = 다운로드 모드다.
//                   읽고 싶으면 M5.BtnPWR 로 읽을 수는 있다.
//
//                   PWR 로 끄면 powerOff() 를 타지 않는다. 듣던 채널도 못
//                   남기고 Wi-Fi 도 정리하지 못한 채 전원이 끊기므로, 끌
//                   때는 B 2초 쪽을 쓰는 편이 낫다.
//
//   흔들기        : 화면 켜기 (움직임 감지)
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Audio.h>
#include <M5Unified.h>
#include <WiFi.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "es8311.h"
#include "hw.h"
#include "log.h"
#include "secrets.h"
#include "stations.h"
#include "ui.h"
#include "wifisetup.h"

// ── 상수 ──────────────────────────────────────────────────────────
constexpr uint8_t  kVolumeSteps     = 20;
constexpr uint8_t  kDefaultVolume   = 14;
constexpr uint8_t  kDefaultIndex    = 2;      // KBS Classic FM 93.1
// 첫 시도는 넉넉하게. 전원을 껐다 켠 직후에는 공유기가 예전 세션을 아직
// 붙들고 있어 인증이 느려지는 일이 있다. 다시 걸 때는 조금 짧게.
constexpr uint32_t kWifiFirstTryMs  = 25000;
constexpr uint32_t kWifiRetryMs     = 15000;
constexpr uint8_t  kWifiAttempts    = 3;      // 이만큼 실패해야 설정 포털로 간다
constexpr uint32_t kWifiRecoveryMs  = 30000;  // 붙지 못한 채 부팅했을 때 재시도 간격
constexpr uint32_t kLongPressMs     = 700;
constexpr uint32_t kVeryLongPressMs = 2000;
constexpr uint32_t kStatusPeriodMs  = 60000;
constexpr uint32_t kUiPeriodMs      = 1000;   // LCD 는 싸다. 초 단위로 갱신
constexpr uint32_t kRetryDelayMs    = 8000;
constexpr uint32_t kWifiPortalMs    = 5UL * 60 * 1000;

// 움직임으로 화면을 깨우는 문턱. 1G 를 넘는 가속이면 집어 든 것으로 본다.
constexpr float kWakeAccelG = 0.25f;

// ── 전역 ──────────────────────────────────────────────────────────
static Audio  audio;
static ES8311 codec;
static TaskHandle_t audioTaskHandle = nullptr;

RTC_DATA_ATTR static uint8_t rtcStationIndex = kDefaultIndex;
RTC_DATA_ATTR static uint8_t rtcVolume = kDefaultVolume;

struct Shared {
    uint8_t   index = kDefaultIndex;
    uint8_t   volume = kDefaultVolume;
    bool      paused = false;
    PlayState state = ST_BOOT;
    String    detail;
    uint32_t  bitrate = 0;
    uint8_t   battPercent = 0;
    float     battVolts = 0.0f;
    bool      charging = false;
    bool      extPower = false;   // USB 등 외부 전원이 물려 있는가
    int16_t   wifiRssi = 0;
    uint8_t   wifiBars = 0;
};
static Shared            shared;
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

// ── 시각 ──────────────────────────────────────────────────────────
// 이 보드에는 외장 RTC 가 없다. ESP32 내부 RTC 로 딥슬립은 넘기지만 전원이
// 끊기면 잃으므로 부팅할 때마다 NTP 를 받는다.
static bool haveTime = false;

static void syncNtp() {
    if (WiFi.status() != WL_CONNECTED) return;
    RLOGI("NTP 동기화 시도...");
    configTzTime(NTP_TZ, NTP_SERVER1, NTP_SERVER2);
    struct tm t = {};
    if (getLocalTime(&t, NTP_TIMEOUT_MS)) {
        haveTime = true;
        RLOGI("NTP 완료: %04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1,
              t.tm_mday, t.tm_hour, t.tm_min);
    } else {
        RLOGE("NTP 응답 없음");
    }
}

// ── Wi-Fi ─────────────────────────────────────────────────────────
static void applyWifiPowerSave(bool playing) {
    const bool sleepOn = playing ? WIFI_SLEEP_WHILE_PLAYING : WIFI_SLEEP_WHILE_IDLE;
    WiFi.setSleep(sleepOn ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    RLOGI("Wi-Fi 모뎀 슬립: %s (%s)", sleepOn ? "on" : "off", playing ? "재생" : "유휴");
}

// ── 볼륨 ──────────────────────────────────────────────────────────
static float volumeToDb(uint8_t vol) { return -40.0f + 2.0f * (float)vol; }

static void applyVolume() {
    lockShared();
    const uint8_t vol = shared.volume;
    const bool    paused = shared.paused;
    unlockShared();
    if (paused) return;

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
    if (!audio.connecttohost(url.c_str())) {
        setState(ST_ERROR, "CONNECT FAIL");
        return;
    }
    applyVolume();
    setState(ST_BUFFERING);
}

// 일시정지는 뮤트가 아니다. 스트림을 끊어야 Wi-Fi 수신과 디코딩이 멎는다.
// 코덱 전원은 M5PM1 이 관리하므로 여기서는 앰프만 끈다.
static void pauseAudio() {
    RLOGI("일시정지 — 스트림 종료");
    audio.stopSong();
    codec.setMute(true);
    hwSpeakerAmp(false);
    applyWifiPowerSave(false);

    lockShared();
    shared.paused = true;
    shared.state = ST_PAUSED;
    shared.bitrate = 0;
    unlockShared();
}

static void resumeAudio() {
    RLOGI("재개");
    applyWifiPowerSave(true);
    hwSpeakerAmp(true);

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

        // 스트림마다 샘플레이트가 다르다. 리샘플링 대신 원본을 그대로 내보내고
        // 바뀔 때만 코덱 클럭을 다시 잡는다.
        const uint32_t rate = audio.getSampleRate();
        if (rate != 0 && rate != codecRate) {
            if (codec.setSampleRate(rate, AUDIO_MCLK_DIV)) {
                RLOGI("샘플레이트 변경: %u -> %u Hz", codecRate, rate);
                codecRate = rate;
            }
        }

        lockShared();
        if (shared.state == ST_BUFFERING && audio.isRunning()) shared.state = ST_PLAYING;
        else if (shared.state == ST_PLAYING && !audio.isRunning()) shared.state = ST_BUFFERING;
        const PlayState st = shared.state;
        const uint8_t   idx = shared.index;
        unlockShared();

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

// ── 화면 상태 ─────────────────────────────────────────────────────
static UiState snapshotUi() {
    UiState u;
    lockShared();
    const uint8_t idx = shared.index;
    u.state = shared.state;
    u.detail = shared.detail;
    u.volume = shared.paused ? 0 : shared.volume;
    u.bitrate = shared.bitrate;
    u.battPercent = shared.battPercent;
    u.battVolts = shared.battVolts;
    u.charging = shared.charging;
    u.wifiRssi = shared.wifiRssi;
    u.wifiBars = shared.wifiBars;
    unlockShared();

    u.freq = kStations[idx].freq;
    u.name = kStations[idx].name;
    u.volumeMax = kVolumeSteps;
    u.wifi = WiFi.status() == WL_CONNECTED;

    struct tm now = {};
    if (haveTime && getLocalTime(&now, 5)) {
        u.hasTime = true;
        u.hour = (uint8_t)now.tm_hour;
        u.minute = (uint8_t)now.tm_min;
    }
    return u;
}

static uint8_t rssiToBars(int32_t rssi) {
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

// 배터리는 ADC 분압이 아니라 M5PM1 PMIC 에서 읽는다. M5Unified 가 감싸 준다.
static void pollSlowStatus() {
    const bool up = WiFi.status() == WL_CONNECTED;
    const int16_t rssi = up ? (int16_t)WiFi.RSSI() : 0;

    // M5Unified 0.2.27 의 getBatteryLevel() 은 M5PM1 분기가 ESP32-C61 빌드에만
    // 있어서 S3 에서는 -1 을 돌려준다. PMIC 를 직접 읽고 퍼센트는 우리가 환산한다.
    const uint16_t mv = hwBatteryMillivolts();
    const int32_t pct = (mv > 0) ? hwBatteryPercent(mv) : -1;
    const bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    const uint8_t src = hwPowerSource();
    const bool extPower = hwExternalPower();

    lockShared();
    shared.wifiRssi = rssi;
    shared.wifiBars = up ? rssiToBars(rssi) : 0;
    shared.battPercent = (pct < 0) ? 0 : (uint8_t)pct;
    shared.battVolts = (mv > 0) ? mv / 1000.0f : 0.0f;
    shared.charging = charging;
    shared.extPower = extPower;
    unlockShared();

    const uint32_t bufSize = audio.getInBufferSize();
    const uint32_t bufPct = bufSize ? (audio.inBufferFilled() * 100 / bufSize) : 0;
    RLOGI("배터리 %ld%% (%.2fV%s, src=%u%s)  Wi-Fi %ddBm  버퍼 %u%%  가동 %lu분",
          (long)pct, mv / 1000.0f, charging ? ", 충전중" : "", (unsigned)src,
          extPower ? ", 외부전원" : "", (int)rssi, (unsigned)bufPct,
          (unsigned long)(millis() / 60000));
}

// ── 전원 끄기 ─────────────────────────────────────────────────────
// M5PM1 은 진짜 전원 차단을 지원한다. GPIO 를 붙잡아 두는 편법이 필요 없고,
// 소비가 딥슬립보다도 낮다. 버튼을 누르면 다시 켜진다.
static void powerOff() {
    RLOGI("전원 끔");
    if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
    audio.stopSong();
    codec.setMute(true);
    hwSpeakerAmp(false);

    // 공유기에 인사를 하고 내려간다. 그냥 사라지면 공유기가 한동안 예전 세션을
    // 붙들고 있어서, 다시 켰을 때 4-way 핸드셰이크가 막히는 일이 있다.
    WiFi.disconnect(true);

    lockShared();
    rtcStationIndex = shared.index;
    rtcVolume = shared.volume;
    unlockShared();

    uiScreenOff();
    Serial.flush();
    delay(50);
    M5.Power.powerOff();   // 돌아오지 않는다
}

// ── setup / loop ──────────────────────────────────────────────────
static UiState  lastUi;
static uint32_t lastUiMs = 0;

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
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setAutoReconnect(true);
        WiFi.begin(c.ssid.c_str(), c.pass.c_str());

        const uint32_t waitMs = (attempts > 1 && attempt == 1) ? kWifiFirstTryMs : kWifiRetryMs;
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
            M5.update();   // setup 을 통째로 막지 않는다
            delay(100);
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

static void runWifiPortal() {
    setState(ST_WIFISETUP);
    const bool saved = wifiRunPortal(
        kWifiPortalMs, [](const String& ap, const String& url, const String& pass) {
            UiState u;
            u.state = ST_WIFISETUP;
            u.apSsid = ap;
            u.apPass = pass;
            u.apUrl = url;
            u.detail = wifiReasonText();
            uiRenderWifiSetup(u);
        });

    if (saved) {
        RLOGI("새 Wi-Fi 정보 저장됨 — 재시작");
        delay(300);
        ESP.restart();
    }
    RLOGE("설정 포털 시간 초과");
    setState(ST_ERROR, lastWifiReason ? ("NO WIFI r" + String((unsigned)lastWifiReason))
                                      : String("NO WIFI"));
}

static void setupOta() {
    ArduinoOTA.setHostname(OTA_HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
        RLOGI("OTA 시작");
        if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
        audio.stopSong();
        codec.setMute(true);
        setState(ST_UPDATING);
        uiWake();
        uiRender(snapshotUi());
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        static uint8_t last = 255;
        const uint8_t pct = total ? (uint8_t)(done * 100 / total) : 0;
        if (pct / 10 != last / 10) {
            last = pct;
            RLOGI("OTA %u%%", pct);
        }
    });
    ArduinoOTA.onError([](ota_error_t e) {
        RLOGE("OTA 실패: %u", (unsigned)e);
        setState(ST_ERROR, "OTA FAIL");
        if (audioTaskHandle) vTaskResume(audioTaskHandle);
    });
    ArduinoOTA.begin();
    RLOGI("OTA 대기: %s (%s)", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

void setup() {
    setCpuFrequencyMhz(CPU_FREQ_MHZ);

    auto cfg = M5.config();
    cfg.internal_imu = true;   // BMI270 — 집어 들면 화면을 켠다
    cfg.internal_mic = false;  // 마이크는 안 쓴다. I2S 는 오디오 라이브러리가 소유
    cfg.internal_spk = false;  // 스피커도 마찬가지. 코덱은 우리가 직접 설정한다
    M5.begin(cfg);

    Serial.begin(115200);
    // USB 가 호스트에 연결됐는데 아무도 포트를 읽지 않으면 CDC write 가 최대
    // 2초까지 막힌다. 그게 오디오 태스크에서 일어나면 소리가 끊긴다.
    //
    // 그래도 부팅 동안에는 짧게 블로킹으로 둔다. 여기서 패닉이 나면 그 메시지가
    // 유일한 단서인데, 논블로킹이면 그대로 버려져 "조용한 재부팅"만 남는다.
    // 재생이 시작되기 직전에 0 으로 내린다(아래 xTaskCreate 앞).
    Serial.setTxTimeoutMs(100);
    delay(200);
    RLOGI("M5StickS3 FM Radio  (CPU %u MHz)", (unsigned)getCpuFrequencyMhz());

    // 조용히 재부팅하는 원인을 좁히려고 남긴다. 패닉이면 PANIC, 전원이 처지면
    // BROWNOUT, 워치독이면 *WDT, 외부/PMIC 리셋이면 POWERON 이나 EXT 로 나온다.
    {
        const char* why = "?";
        switch (esp_reset_reason()) {
            case ESP_RST_POWERON:  why = "POWERON";  break;
            case ESP_RST_EXT:      why = "EXT";      break;
            case ESP_RST_SW:       why = "SW";       break;
            case ESP_RST_PANIC:    why = "PANIC";    break;
            case ESP_RST_INT_WDT:  why = "INT_WDT";  break;
            case ESP_RST_TASK_WDT: why = "TASK_WDT"; break;
            case ESP_RST_WDT:      why = "WDT";      break;
            case ESP_RST_DEEPSLEEP:why = "DEEPSLEEP";break;
            case ESP_RST_BROWNOUT: why = "BROWNOUT"; break;
            case ESP_RST_SDIO:     why = "SDIO";     break;
            default:               why = "UNKNOWN";  break;
        }
        RLOGI("리셋 원인: %s", why);
    }

    // 내부 I2C(SCL 48 / SDA 47)는 M5.begin() 이 이미 열어 두었다. 여기에
    // Arduino Wire 를 또 올리면 안 된다 — 한 포트에 마스터 드라이버가 둘이 되어
    // 모든 전송이 ESP_ERR_INVALID_STATE 로 떨어진다. 코덱도 그 버스를 쓰므로
    // 드라이버를 M5.In_I2C 쪽으로 통일한다.
    hwInstallCodecBus();

    sharedLock = xSemaphoreCreateMutex();
    cmdQueue = xQueueCreate(4, sizeof(Cmd));

    lockShared();
    shared.index = (rtcStationIndex < kStationCount) ? rtcStationIndex : kDefaultIndex;
    shared.volume = (rtcVolume <= kVolumeSteps) ? rtcVolume : kDefaultVolume;
    unlockShared();

    if (!hwPmicPresent()) {
        RLOGE("PMIC(0x6E)가 I2C 에 없다 — 앰프와 배터리를 제어할 수 없다");
    }

    uiBegin();
    pollSlowStatus();
    lastUi = snapshotUi();
    uiRender(lastUi);

    setState(ST_WIFI);
    uiRender(snapshotUi());

    if (!connectWifi(kWifiAttempts)) {
        runWifiPortal();
        wifiRecovery = true;
        return;  // 포털도 소용없었으면 loop() 에서 계속 재시도
    }
    setupOta();
    syncNtp();
    applyWifiPowerSave(true);

    RLOGI("I2S 설정 중... (BCLK=%d WS=%d DOUT=%d MCLK=%d)  힙=%u PSRAM=%u",
          (int)PIN_I2S_BCLK, (int)PIN_I2S_WS, (int)PIN_I2S_DOUT, (int)PIN_I2S_MCLK,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    Serial.flush();
    if (!audio.setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_MCLK)) {
        RLOGE("I2S setPinout 실패");
        setState(ST_ERROR, "NO I2S");
        uiRender(snapshotUi());
        return;
    }
    RLOGI("I2S 설정 완료");
    audio.setVolumeSteps(21);
    audio.setVolume(21);  // 라이브러리는 풀스케일, 실제 음량은 코덱이 담당
    audio.setConnectionTimeout(8000, 12000);

    RLOGI("ES8311 초기화 중...");
    if (!codec.begin(PIN_I2C_SDA, PIN_I2C_SCL, AUDIO_SAMPLE_RATE, AUDIO_BITS,
                     AUDIO_MCLK_DIV, -1)) {
        setState(ST_ERROR, "NO CODEC");
        uiRender(snapshotUi());
        return;
    }
    applyVolume();
    // 스피커 앰프는 ESP32 핀이 아니라 M5PM1 의 GPIO3 이 켠다. M5.Speaker 를
    // 쓰지 않으므로 아무도 켜 주지 않는다 — 여기서 직접 올린다.
    hwSpeakerAmp(true);
    RLOGI("코덱 준비 완료");

    Audio::audio_info_callback = [](Audio::msg_t m) {
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
                setState(ST_ERROR, "RECONNECT");
                break;
            default:
                break;
        }
    };

    // TLS 핸드셰이크가 스택을 많이 먹어서 넉넉히 잡는다.
    // 여기부터는 오디오 태스크가 로그를 찍는다. CDC 가 막히면 그대로 음이
    // 끊기므로 논블로킹으로 내린다. 부팅은 무사히 끝났으니 더 볼 패닉도 없다.
    Serial.flush();
    Serial.setTxTimeoutMs(0);

    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 3, &audioTaskHandle, 1);

    lockShared();
    const uint8_t startIndex = shared.index;
    unlockShared();
    const Cmd cmd{Cmd::TUNE, startIndex};
    xQueueSend(cmdQueue, &cmd, 0);
}

// 집어 들었는지 본다. BMI270 가속도가 1G 에서 크게 벗어나면 움직인 것이다.
static bool pickedUp() {
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
    const float mag = sqrtf(ax * ax + ay * ay + az * az);
    return fabsf(mag - 1.0f) > kWakeAccelG;
}

void loop() {
    M5.update();

    static uint32_t lastRetry = 0;
    if (wifiRecovery) {
        // 붙지 못한 채로 부팅했고 설정 포털도 소용없었던 경우. 여기서 계속 다시
        // 붙어 본다. 공유기가 늦게 올라오는 일이 있는데, 그냥 포기해 버리면
        // 사용자가 직접 껐다 켜야 한다. 붙으면 재시작해서 setup 을 처음부터
        // 다시 탄다 — 오디오 태스크가 그때 만들어진다.
        if (millis() - lastRetry > kWifiRecoveryMs) {
            lastRetry = millis();
            if (connectWifi(1)) {
                RLOGI("Wi-Fi 복구됨 — 재시작");
                delay(200);
                ESP.restart();
            }
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        // 붙었다가 끊긴 경우. 드라이버에 설정이 남아 있으므로 이쪽이면 된다.
        if (millis() - lastRetry > 10000) {
            lastRetry = millis();
            WiFi.reconnect();
        }
    }

    // ── 버튼 ─────────────────────────────────────────────────────
    // M5Unified 가 눌림 길이를 재 준다. 별도 디바운스가 필요 없다.
    if (M5.BtnA.wasReleased() || M5.BtnA.wasReleaseFor(kLongPressMs)) {
        uiWake();
        if (M5.BtnA.wasReleaseFor(kVeryLongPressMs)) {
            if (audioTaskHandle) vTaskSuspend(audioTaskHandle);
            audio.stopSong();
            runWifiPortal();
            if (audioTaskHandle) vTaskResume(audioTaskHandle);
        } else {
            lockShared();
            if (M5.BtnA.wasReleaseFor(kLongPressMs)) {
                shared.index = (uint8_t)((shared.index + kStationCount - 1) % kStationCount);
            } else {
                shared.index = (uint8_t)((shared.index + 1) % kStationCount);
            }
            shared.bitrate = 0;
            const uint8_t idx = shared.index;
            unlockShared();
            const Cmd cmd{Cmd::TUNE, idx};
            xQueueSend(cmdQueue, &cmd, 0);
        }
    }

    if (M5.BtnB.wasReleased() || M5.BtnB.wasReleaseFor(kLongPressMs)) {
        uiWake();
        if (M5.BtnB.wasReleaseFor(kVeryLongPressMs)) {
            powerOff();  // 돌아오지 않는다
        } else if (M5.BtnB.wasReleaseFor(kLongPressMs)) {
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

    // ── 집어 들면 화면을 켠다 ────────────────────────────────────
    // 전자종이라면 그림이 그냥 남아 있겠지만 LCD 는 백라이트가 상시 전력을
    // 먹는다. 평소에는 꺼 두고 움직임이 있을 때만 켜는 편이 훨씬 오래 간다.
    static uint32_t lastImuMs = 0;
    if (millis() - lastImuMs > 120) {
        lastImuMs = millis();
        if (!uiScreenIsOn() && pickedUp()) uiWake();
    }
    uiTickBacklight();

    if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

    static uint32_t lastStatusMs = 0;
    if (millis() - lastStatusMs > kStatusPeriodMs) {
        lastStatusMs = millis();
        pollSlowStatus();

        // 배터리가 바닥나면 알아서 끈다. 재생 중 갑자기 죽는 것보다 낫다.
        // USB 가 물려 있으면 끄지 않는다. 이 컷오프는 셀을 과방전에서 지키려는
        // 것인데, 외부 전원으로 도는 동안에는 지킬 일이 없다.
        //
        // isCharging() 만 보면 부족하다. 만충이거나 입력 전류가 소비를 못 따라가면
        // 충전이 멈춰 '충전 아님'이 되는데, 그 상태로 오래 재생하면 꽂아 둔 채로
        // 잔량이 10% 까지 내려가 스스로 꺼져 버린다. 실제로 그렇게 꺼졌다.
        static uint8_t strikes = 0;
        lockShared();
        const uint8_t pct = shared.battPercent;
        const bool chg = shared.charging;
        const bool ext = shared.extPower;
        unlockShared();
        if (!chg && !ext && pct > 0 && pct <= BATT_CUTOFF_PERCENT) {
            if (++strikes >= BATT_CUTOFF_STRIKES) {
                RLOGE("배터리 %u%% (src=%u) — 자동 종료", (unsigned)pct,
                      (unsigned)hwPowerSource());
                setState(ST_LOWBATT);
                uiWake();
                uiRender(snapshotUi());
                delay(3000);
                powerOff();
            }
        } else {
            strikes = 0;
        }
    }

    // 화면이 꺼져 있으면 그릴 이유가 없다.
    if (uiScreenIsOn() && millis() - lastUiMs > kUiPeriodMs) {
        lastUiMs = millis();
        uiRender(snapshotUi());
    }

    delay(20);
}
