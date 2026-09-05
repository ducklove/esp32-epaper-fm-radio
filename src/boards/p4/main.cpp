// Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 인터넷 스트리밍 FM 라디오
//
// 세 번째 보드. 앞의 둘(ESP32-S3)과 코덱(ES8311)은 같아서 오디오 경로는 핀
// 번호만 다르고, 근본적으로 다른 것은 둘이다.
//
// 1) 무선이 P4 안에 없다. 옆의 ESP32-C6 가 SDIO 로 Wi-Fi 를 대신한다
//    (ESP-Hosted). Arduino 3.3 이 감춰 주어 WiFi.begin() 은 그대로지만,
//    부팅에 C6 리셋 1.5초가 더 들고, 호스트와 C6 펌웨어의 버전이 맞아야 한다.
//    부팅 로그에 두 버전이 찍힌다.
//
// 2) 화면이 크고 터치가 된다. 버튼 대신 화면을 눌러 조작한다. 조작 방법은
//    ui.h 에. 전원은 AXP2101 이 진짜로 끊고, PWR 버튼으로 다시 켠다.
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Audio.h>
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <atomic>
#include <esp_system.h>

#include "c6update.h"
#include "config.h"
#include "es8311.h"
#include "hw.h"
#include "log.h"
#include "listening.h"
#include "secrets.h"
#include "stations.h"
#include "touch.h"
#include "ui.h"
#include "wifisetup.h"

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp32-hal-hosted.h"
#endif

// ── 상수 ──────────────────────────────────────────────────────────
constexpr uint8_t  kVolumeSteps     = 20;
constexpr uint8_t  kDefaultVolume   = 12;
constexpr uint8_t  kDefaultIndex    = 2;      // KBS Classic FM 93.1
constexpr uint32_t kWifiFirstTryMs  = 25000;
constexpr uint32_t kWifiRetryMs     = 15000;
constexpr uint8_t  kWifiAttempts    = 3;
constexpr uint32_t kWifiRecoveryMs  = 30000;
constexpr uint32_t kWifiLostRestartMs = 45000;  // 끊긴 채 이만큼 지나면 재시작
constexpr uint32_t kStatusPeriodMs  = 60000;
constexpr uint32_t kUiPeriodMs      = 1000;
constexpr uint32_t kTouchPeriodMs   = 20;     // 50Hz 폴링
constexpr uint32_t kDragFrameMs     = 50;     // 끄는 동안 다시 그리는 간격 (20fps)
constexpr uint32_t kRetryDelayMs    = 8000;
constexpr uint32_t kWifiPortalMs    = 5UL * 60 * 1000;

// ── 전역 ──────────────────────────────────────────────────────────
static Audio  audio;
static ES8311 codec;
static TaskHandle_t audioTaskHandle = nullptr;
static std::atomic<bool> wantMeters{false};

struct Shared : ListeningSettings {
    PlayState state = ST_BOOT;
    String    detail;
    uint32_t  bitrate = 0;
    HwPower   power;
    int16_t   wifiRssi = 0;
    uint8_t   wifiBars = 0;
    SleepTimer sleep;
    AudioMeters meters;
    uint32_t tuneRevision = 0;
    uint32_t prefsRevision = 0;
    bool maintenance = false;
    bool audioIdle = false;
    uint32_t maintenanceRevision = 0, idleRevision = 0;
};
static Shared            shared;
static SemaphoreHandle_t sharedLock;

static QueueHandle_t uiEventQueue;   // 터치 태스크 -> loop

static inline void lockShared()   { xSemaphoreTake(sharedLock, portMAX_DELAY); }
static inline void unlockShared() { xSemaphoreGive(sharedLock); }

// 라이브러리의 PCM 처리 경계에서 EQ 계수를 갱신한다. loop 태스크에서
// setTone() 을 호출하면 별도 디코더 태스크가 읽는 계수를 동시에 덮어쓸 수 있다.
void p4ProcessAudioFrame() {
    const bool meters = wantMeters.load(std::memory_order_relaxed);
    audio.settings.VU_LEVEL = meters;
    audio.settings.SPECTRUM = meters;
    static uint8_t appliedTone = UINT8_MAX;
    lockShared();
    const uint8_t toneIndex = shared.tone;
    unlockShared();
    if (toneIndex != appliedTone) {
        const auto& tone = kTonePresets[toneIndex];
        audio.setTone(tone.bass, tone.mid, tone.treble);
        appliedTone = toneIndex;
    }
}

static void setState(PlayState st, const String& detail = String()) {
    lockShared();
    shared.state = st;
    shared.detail = detail;
    unlockShared();
}

// ── 설정 저장 ─────────────────────────────────────────────────────
// PMIC 가 전원을 진짜로 끊으면 RTC 메모리도 같이 날아간다. NVS 에 둔다.
static void loadPrefs() {
    Preferences p;
    if (p.begin("radio", true)) {
        const uint8_t idx = p.getUChar("station", kDefaultIndex);
        const uint8_t vol = p.getUChar("volume", kDefaultVolume);
        const uint8_t tone = p.getUChar("tone", 0);
        // 타이머 도중 재부팅되면 남은 시간을 추정해 계속 틀지 않고 정지한다.
        // 외장 RTC 가 없으므로 NTP 접속 실패 때도 같은 보수적인 동작을 한다.
        const bool paused = p.getBool("paused", false) || p.getBool("sleepArmed", false);
        p.end();
        lockShared();
        shared.index = (idx < kStationCount) ? idx : kDefaultIndex;
        shared.volume = (vol <= kVolumeSteps) ? vol : kDefaultVolume;
        shared.tone = tone;
        shared.paused = paused;
        shared.validate(kStationCount, kVolumeSteps);
        unlockShared();
    }
}

static uint32_t savedPrefsRevision = 0;
static bool savePrefs() {
    lockShared();
    const uint8_t idx = shared.index, vol = shared.volume;
    const uint8_t tone = shared.tone;
    const bool paused = shared.paused;
    const bool sleepArmed = shared.sleep.minutes() != 0;
    const uint32_t revision = shared.prefsRevision;
    unlockShared();
    Preferences p;
    if (p.begin("radio", false)) {
        const bool ok = p.putUChar("station", idx) && p.putUChar("volume", vol) &&
                        p.putUChar("tone", tone) && p.putBool("paused", paused) &&
                        p.putBool("sleepArmed", sleepArmed);
        p.end();
        if (ok) savedPrefsRevision = revision;
        else RLOGE("청취 설정 저장 실패");
        return ok;
    }
    RLOGE("청취 설정 저장소 열기 실패");
    return false;
}

// 슬라이더를 움직이는 동안 플래시를 계속 쓰지 않고 마지막 변경 1.5초 뒤 저장.
static void flushPrefsWhenIdle() {
    static uint32_t observed = 0, changedAt = 0;
    lockShared();
    const uint32_t revision = shared.prefsRevision;
    unlockShared();
    if (revision != observed) { observed = revision; changedAt = millis(); }
    if (revision != savedPrefsRevision && millis() - changedAt >= 1500) {
        savePrefs();
        changedAt = millis();
    }
}

static void tickSleepTimer() {
    lockShared();
    const bool expired = shared.sleep.expire(millis());
    if (expired) {
        shared.paused = true;
        ++shared.tuneRevision;
        ++shared.prefsRevision;
    }
    unlockShared();
    if (expired) RLOGI("취침 타이머 만료 — 재생 중지");
}

// ── 시각 ──────────────────────────────────────────────────────────
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
    lockShared();
    shared.meters = {};
    shared.bitrate = 0;
    unlockShared();
    setState(ST_TUNING);
    RLOGI("선국: %s (%.1f MHz)", st.name, st.freq);

    const String url = resolveStreamUrl(st);
    if (url.isEmpty()) {
        setState(ST_ERROR, "NO STREAM");
        return;
    }
    // URL 해석을 기다리는 동안 바뀐 선국/정지 요청은 재생을 시작하지 않는다.
    lockShared();
    const bool cancelled = shared.paused || shared.maintenance || shared.index != index;
    unlockShared();
    if (cancelled) return;
    if (!audio.connecttohost(url.c_str())) {
        setState(ST_ERROR, "CONNECT FAIL");
        return;
    }
    applyVolume();
    setState(ST_BUFFERING);
}

static void pauseAudio() {
    RLOGI("일시정지 — 스트림 종료");
    audio.stopSong();
    codec.setMute(true);
    hwSpeakerAmp(false);
    applyWifiPowerSave(false);

    lockShared();
    shared.state = ST_PAUSED;
    shared.bitrate = 0;
    shared.meters = {};
    unlockShared();
}

static void resumeAudio(uint8_t idx) {
    RLOGI("재개");
    applyWifiPowerSave(true);
    hwSpeakerAmp(true);

    applyVolume();
    tune(idx);
}

static void audioTask(void*) {
    uint32_t codecRate = AUDIO_SAMPLE_RATE;
    uint32_t lastRetryMs = millis();
    uint32_t bufferingSince = 0;
    ListeningSettings applied;
    uint32_t appliedTune = 0;
    bool initialized = false;

    for (;;) {
        tickSleepTimer();
        lockShared();
        const ListeningSettings desired = shared;
        const uint32_t revision = shared.tuneRevision;
        const bool maintenance = shared.maintenance;
        const uint32_t maintenanceRevision = shared.maintenanceRevision;
        const bool idle = shared.audioIdle && shared.idleRevision == maintenanceRevision;
        unlockShared();
        if (maintenance) {
            bufferingSince = 0;
            if (!idle) {
                pauseAudio();
                lockShared();
                shared.audioIdle = true;
                shared.idleRevision = maintenanceRevision;
                unlockShared();
            }
            initialized = false;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        lockShared();
        shared.audioIdle = false;
        unlockShared();

        // 전송/코덱 변경은 이 태스크만 한다(EQ 는 위 PCM 콜백). 큐가 차서 마지막 탭이
        // 사라지지 않도록, 선국 중에도 요청은 최신 상태로 합쳐 둔다.
        if (desired.paused) bufferingSince = 0;
        switch (listeningChange(desired, applied, initialized, revision, appliedTune)) {
            case PlaybackChange::PAUSE: pauseAudio(); break;
            case PlaybackChange::RESTART:
                bufferingSince = 0;
                resumeAudio(desired.index);
                break;
            case PlaybackChange::VOLUME: applyVolume(); break;
            case PlaybackChange::NONE: break;
        }
        applied = desired;
        appliedTune = revision;
        initialized = true;
        if (desired.paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        audio.loop();

        const uint32_t rate = audio.getSampleRate();
        if (rate != 0 && rate != codecRate) {
            if (codec.setSampleRate(rate, AUDIO_MCLK_DIV)) {
                RLOGI("샘플레이트 변경: %u -> %u Hz", codecRate, rate);
                codecRate = rate;
            }
        }

        // 라이브러리 4.0 은 connecttohost() 가 바로 true 를 돌려주고 실제 접속은
        // loop() 안에서 한다. 거기서 실패하면(DNS, TLS, 소켓) 로그만 남기고 멈춘다
        // — 상태는 BUFFERING 인 채로. ERROR 에서만 재시도하던 상태 머신은 그
        // 자리에서 영원히 기다렸다(P4 에서 국악FM DNS 실패로 6분간 무반응).
        // 버퍼링이 이만큼 지나도 재생이 시작되지 않으면 오류로 쳐서 다시 붙는다.
        // 재생 중 끊겨 BUFFERING 으로 돌아간 경우도 같은 길을 탄다.
        constexpr uint32_t kStallMs = 45000;
        bool stalled = false;

        lockShared();
        if (shared.state == ST_BUFFERING && audio.isRunning()) shared.state = ST_PLAYING;
        else if (shared.state == ST_PLAYING && !audio.isRunning()) shared.state = ST_BUFFERING;
        if (shared.state == ST_BUFFERING) {
            if (!bufferingSince) bufferingSince = millis();
            else if (millis() - bufferingSince > kStallMs) {
                shared.state = ST_ERROR;
                shared.detail = "STREAM STALL";
                bufferingSince = 0;
                stalled = true;
            }
        } else {
            bufferingSince = 0;
        }
        const PlayState st = shared.state;
        const uint8_t   idx = shared.index;
        unlockShared();
        if (stalled) {
            RLOGE("스트림이 %lu초 넘게 시작되지 않음 — 다시 붙는다",
                  (unsigned long)(kStallMs / 1000));
        }

        if (st == ST_ERROR && WiFi.status() == WL_CONNECTED) {
            if (millis() - lastRetryMs > kRetryDelayMs) {
                lastRetryMs = millis();
                bufferingSince = 0;
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
    u.index = shared.index;
    u.state = shared.state;
    u.detail = shared.detail;
    u.paused = shared.paused;
    u.volume = shared.volume;
    u.bitrate = shared.bitrate;
    u.battery = shared.power.battery;
    u.vbus = shared.power.vbus;
    u.charging = shared.power.charging;
    u.battPercent = shared.power.percent;
    u.battVolts = shared.power.battMv / 1000.0f;
    u.wifiRssi = shared.wifiRssi;
    u.wifiBars = shared.wifiBars;
    u.tone = shared.tone;
    u.sleepMinutes = shared.sleep.minutes();
    u.sleepSeconds = shared.sleep.remainingSeconds(millis());
    u.meters = shared.meters;
    u.controlsBlocked = shared.maintenance || shared.state == ST_BOOT || shared.state == ST_WIFI ||
                        shared.state == ST_WIFISETUP || shared.state == ST_UPDATING;
    u.controlsEpoch = shared.maintenanceRevision;
    unlockShared();
    if (u.paused || u.state != ST_PLAYING || millis() - u.meters.updatedMs > 500) u.meters = {};

    u.freq = kStations[u.index].freq;
    u.name = kStations[u.index].name;
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
    // 구형 C6 펌웨어는 RSSI 질의(Req_WifiStaGetApInfo)에 답하지 않아 0 이 온다.
    // 붙어는 있으니 끊긴 것처럼 보이지 않게 중간으로 그린다.
    if (rssi == 0) return 2;
    if (rssi >= -55) return 4;
    if (rssi >= -65) return 3;
    if (rssi >= -75) return 2;
    if (rssi >= -85) return 1;
    return 0;
}

static void pollSlowStatus() {
    const bool up = WiFi.status() == WL_CONNECTED;
    const int16_t rssi = up ? (int16_t)WiFi.RSSI() : 0;
    const HwPower pw = hwReadPower();

    lockShared();
    shared.wifiRssi = rssi;
    shared.wifiBars = up ? rssiToBars(rssi) : 0;
    shared.power = pw;
    unlockShared();

    const uint32_t bufSize = audio.getInBufferSize();
    const uint32_t bufPct = bufSize ? (audio.inBufferFilled() * 100 / bufSize) : 0;
    const UiTiming ut = uiTakeTiming();
    // 버퍼는 퍼센트와 함께 크기도 적는다. PSRAM 이 크면 라이브러리가 버퍼를 크게
    // 잡아서, 같은 몇 초치가 S3 보드보다 작은 퍼센트로 보인다.
    RLOGI("배터리 %u%% (%.2fV%s%s%s)  VBUS %.2fV  Wi-Fi %ddBm  버퍼 %u%%/%uKB  힙 %u  가동 %lu분  "
          "화면 %u장 그리기 %u/%ums 전송 %u/%ums",
          (unsigned)pw.percent, pw.battMv / 1000.0f, pw.battery ? "" : ", 셀 없음",
          pw.charging ? ", 충전중" : "", pw.vbus ? ", USB" : "", pw.vbusMv / 1000.0f,
          (int)rssi, (unsigned)bufPct, (unsigned)(bufSize / 1024), (unsigned)ESP.getFreeHeap(),
          (unsigned long)(millis() / 60000), (unsigned)ut.frames, (unsigned)ut.drawAvg,
          (unsigned)ut.drawMax, (unsigned)ut.flushAvg, (unsigned)ut.flushMax);
}

// ── 터치 태스크 ───────────────────────────────────────────────────
// 화면 한 장을 SPI 로 밀어 넣는 동안 loop 는 멈춘다. 그 사이에 들어온 탭을
// 놓치지 않으려면 터치는 따로 돌아야 한다. 결과(UiEvent)는 큐로 loop 에 넘긴다.
static void touchTask(void*) {
    TouchPoint pts[2];
    for (;;) {
        const uint8_t n = touchRead(pts, 2);
        const UiState s = snapshotUi();
        UiEvent ev = uiHandleTouch(pts, n, s);
        ev.epoch = s.controlsEpoch;
        // loop 가 잠깐 느려져도 버튼의 마지막 뗌을 버리지 않는다.
        if (ev.action != UiAction::NONE) xQueueSend(uiEventQueue, &ev, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(kTouchPeriodMs));
    }
}

// ── 전원 끄기 ─────────────────────────────────────────────────────
static void endMaintenance() {
    xQueueReset(uiEventQueue);
    lockShared();
    shared.maintenance = false;
    ++shared.tuneRevision;
    unlockShared();
}

static bool beginMaintenance() {
    lockShared();
    shared.maintenance = true;
    const uint32_t revision = ++shared.maintenanceRevision;
    unlockShared();
    if (!audioTaskHandle) return true;
    const uint32_t started = millis();
    while (millis() - started < 45000) {
        lockShared();
        const bool idle = shared.audioIdle && shared.idleRevision == revision;
        unlockShared();
        if (idle) return true;
        delay(10);
    }
    // 잠금이나 네트워크 호출 도중 태스크를 강제 정지하면 교착될 수 있다.
    RLOGE("오디오 정지 대기 시간 초과 — 작업 취소");
    endMaintenance();
    return false;
}

static void powerOff(const char* why) {
    RLOGI("전원 끔 (%s)", why);
    if (!beginMaintenance()) return;
    WiFi.disconnect(true);
    savePrefs();
    uiScreenOff();
    Serial.flush();
    delay(50);
    hwPowerOff();   // 돌아오지 않는다
}

// ── Wi-Fi 접속 ────────────────────────────────────────────────────
static volatile uint8_t lastWifiReason = 0;

static void onWifiEvent(arduino_event_id_t id, arduino_event_info_t info) {
    if (id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        lastWifiReason = info.wifi_sta_disconnected.reason;
        RLOGI("Wi-Fi 끊김 (reason=%u)", (unsigned)lastWifiReason);
    }
}

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

static bool wifiRecovery = false;

// 슬레이브가 버전을 말해 주지 않으면(구형 펌웨어) true.
static bool logHostedVersions() {
#if CONFIG_ESP_HOSTED_ENABLED
    uint32_t hM, hm, hp, sM, sm, sp;
    hostedGetHostVersion(&hM, &hm, &hp);
    hostedGetSlaveVersion(&sM, &sm, &sp);
    RLOGI("ESP-Hosted 호스트 %lu.%lu.%lu / C6 슬레이브 %lu.%lu.%lu%s",
          (unsigned long)hM, (unsigned long)hm, (unsigned long)hp,
          (unsigned long)sM, (unsigned long)sm, (unsigned long)sp,
          (hM == sM && hm == sm) ? "" : "  <- 버전 불일치");
    return (sM == 0 && sm == 0 && sp == 0);
#else
    return false;
#endif
}

// 공장 출하 C6 펌웨어는 리셋 뒤 한동안 Wi-Fi 명령을 받지 않는다. 초기화 직후에
// WiFi.begin() 을 부르면 25초를 기다려도 status 0 에서 멈췄고, 10초쯤 뒤에
// 부르면 바로 붙었다(C6 갱신을 시도하느라 우연히 늦어진 부팅에서 발견).
// 버전을 말해 주는 새 펌웨어에서는 필요 없어 보여서 구형일 때만 기다린다.
constexpr uint32_t kOldSlaveSettleMs = 10000;

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
            WiFi.disconnect(true);
            delay(500);
        }

        // 첫 호출이 C6 를 리셋하고 SDIO 로 붙는다. 여기서 1.5초쯤 걸린다.
        const uint32_t hostedT0 = millis();
        WiFi.mode(WIFI_STA);
        if (attempt == 1) {
            const bool oldSlave = logHostedVersions();
            // 호스트와 C6 의 ESP-Hosted 버전이 다르면 접속이 안 된다. P4 에 박아
            // 둔 C6 펌웨어를 밀어 넣고 재시작한다. (c6update.h)
            if (c6UpdateIfNeeded([](uint8_t pct) {
                    setState(ST_UPDATING, "C6 " + String((unsigned)pct) + "%");
                    uiWake();
                    uiRender(snapshotUi());
                })) {
                setState(ST_UPDATING, "C6 DONE - RESTART");
                uiRender(snapshotUi());
                Serial.flush();
                delay(1500);
                ESP.restart();
            }
            if (oldSlave) {
                while (millis() - hostedT0 < kOldSlaveSettleMs) delay(100);
                RLOGI("구형 C6 펌웨어 — %lu ms 기다린 뒤 접속", (unsigned long)(millis() - hostedT0));
            }
        }
        WiFi.setSleep(WIFI_PS_NONE);
        WiFi.setAutoReconnect(true);
        WiFi.begin(c.ssid.c_str(), c.pass.c_str());

        const uint32_t waitMs = (attempts > 1 && attempt == 1) ? kWifiFirstTryMs : kWifiRetryMs;
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < waitMs) {
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
        tickSleepTimer();
        savePrefs();
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
        // OTA 는 onStart 뒤 본문을 수신한다. 안전하게 정지할 수 없으면
        // 본문 쓰기 전에 재시작해 기존 펌웨어를 유지한다.
        if (!beginMaintenance()) { ESP.restart(); return; }
        savePrefs();
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
        lockShared();
        const bool wasMaintenance = shared.maintenance;
        unlockShared();
        if (wasMaintenance) {
            setState(ST_ERROR, "OTA FAIL");
            endMaintenance();
        }
    });
    ArduinoOTA.begin();
    RLOGI("OTA 대기: %s (%s)", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// ── setup / loop ──────────────────────────────────────────────────
static uint32_t lastUiMs = 0;

void setup() {
    // 이 보드의 Serial 은 USB CDC 가 아니라 UART0 -> CH343 브리지다. 링버퍼가
    // 차면 write 가 막히므로 오디오 태스크의 로그가 소리를 끊지 않게 넉넉히.
    Serial.setTxBufferSize(4096);
    Serial.begin(115200);
    delay(200);
    RLOGI("ESP32-P4 FM Radio  (CPU %u MHz, PSRAM %u KB)", (unsigned)getCpuFrequencyMhz(),
          (unsigned)(ESP.getPsramSize() / 1024));

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
            default:               why = "UNKNOWN";  break;
        }
        RLOGI("리셋 원인: %s", why);
    }

    // 내부 I2C 하나에 코덱 · 터치 · PMIC 가 다 있다. 여기서 한 번 연다.
    // 이 보드는 Arduino Wire 말고 다른 I2C 드라이버가 없으므로 그대로 쓴다.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000UL);

    sharedLock = xSemaphoreCreateMutex();
    uiEventQueue = xQueueCreate(8, sizeof(UiEvent));
    loadPrefs();

    hwPmicBegin();
    touchBegin();
    uiBegin();
    pollSlowStatus();
    uiRender(snapshotUi());

    // 터치는 지금부터 받는다. Wi-Fi 를 기다리는 동안에도 화면은 깨어난다.
    xTaskCreatePinnedToCore(touchTask, "touch", 4096, nullptr, 2, nullptr, 0);

    setState(ST_WIFI);
    uiRender(snapshotUi());

    if (!connectWifi(kWifiAttempts)) {
        runWifiPortal();
        wifiRecovery = true;
        return;
    }
    setupOta();
    syncNtp();
    applyWifiPowerSave(true);

    RLOGI("I2S 설정 중... (BCLK=%d WS=%d DOUT=%d MCLK=%d)  힙=%u PSRAM=%u",
          (int)PIN_I2S_BCLK, (int)PIN_I2S_WS, (int)PIN_I2S_DOUT, (int)PIN_I2S_MCLK,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());
    if (!audio.setPinout(PIN_I2S_BCLK, PIN_I2S_WS, PIN_I2S_DOUT, PIN_I2S_MCLK)) {
        RLOGE("I2S setPinout 실패");
        setState(ST_ERROR, "NO I2S");
        uiRender(snapshotUi());
        return;
    }
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
    codec.setMute(true);
    hwSpeakerAmp(false);
    RLOGI("코덱 준비 완료");

    Audio::audio_info_callback = [](Audio::msg_t m) {
        const bool meter = m.e == Audio::evt_vu || m.e == Audio::evt_spectrum;
        const bool noisy = (m.e == Audio::evt_info);
        if (m.msg && !meter && (AUDIO_VERBOSE_LOG || !noisy)) {
            Serial.printf("[A/%s] %s\n", m.s ? m.s : "?", m.msg);
        }
        switch (m.e) {
            case Audio::evt_vu:
                if (m.vec1.size() >= 4) {
                    lockShared();
                    shared.meters.left = meterByte(m.vec1[0]);
                    shared.meters.right = meterByte(m.vec1[1]);
                    shared.meters.peakLeft = meterByte(m.vec1[2]);
                    shared.meters.peakRight = meterByte(m.vec1[3]);
                    shared.meters.updatedMs = millis();
                    unlockShared();
                }
                break;
            case Audio::evt_spectrum:
                if (m.vec1.size() >= kSpectrumBands) {
                    lockShared();
                    for (uint8_t i = 0; i < kSpectrumBands; ++i)
                        shared.meters.bands[i] = meterByte(m.vec1[i]);
                    unlockShared();
                }
                break;
            case Audio::evt_bitrate:
                lockShared();
                shared.bitrate = (uint32_t)(m.arg1 > 0 ? m.arg1 : 0);
                unlockShared();
                break;
            case Audio::evt_eof:
                lockShared();
                if (!shared.paused && !shared.maintenance) {
                    shared.state = ST_ERROR;
                    shared.detail = "RECONNECT";
                }
                unlockShared();
                break;
            default:
                break;
        }
    };

    xTaskCreatePinnedToCore(audioTask, "audio", 16384, nullptr, 3, &audioTaskHandle, 1);

}

// 터치 이벤트를 실제 동작으로.
static void handleUiEvent(const UiEvent& ev) {
    lockShared();
    const bool maintenance = shared.maintenance;
    const uint32_t epoch = shared.maintenanceRevision;
    unlockShared();
    if (maintenance || ev.epoch != epoch) return;
    switch (ev.action) {
        case UiAction::NONE:
            return;
        case UiAction::TUNE:
        case UiAction::PREV:
        case UiAction::NEXT: {
            if (ev.action == UiAction::TUNE && ev.value >= kStationCount) return;
            lockShared();
            if (ev.action == UiAction::TUNE)      shared.index = ev.value;
            else if (ev.action == UiAction::NEXT) shared.index = (uint8_t)((shared.index + 1) % kStationCount);
            else                                  shared.index = (uint8_t)((shared.index + kStationCount - 1) % kStationCount);
            shared.bitrate = 0;
            shared.paused = false;
            ++shared.tuneRevision;
            ++shared.prefsRevision;
            unlockShared();
            break;
        }
        case UiAction::TOGGLE_PAUSE: {
            lockShared();
            shared.paused = !shared.paused;
            ++shared.tuneRevision;
            ++shared.prefsRevision;
            unlockShared();
            break;
        }
        case UiAction::VOLUME: {
            lockShared();
            shared.volume = (ev.value > kVolumeSteps) ? kVolumeSteps : ev.value;
            ++shared.prefsRevision;
            unlockShared();
            break;
        }
        case UiAction::TONE:
            if (ev.value >= kTonePresetCount) return;
            lockShared();
            shared.tone = ev.value;
            ++shared.prefsRevision;
            unlockShared();
            break;
        case UiAction::SLEEP:
            lockShared();
            shared.sleep.cycle(millis());
            ++shared.prefsRevision;
            unlockShared();
            break;
        case UiAction::WIFI_SETUP:
            if (!beginMaintenance()) break;
            savePrefs();
            runWifiPortal();
            endMaintenance();
            break;
        case UiAction::SCREEN_OFF:
            uiScreenOff();
            break;
        case UiAction::POWER_OFF:
            powerOff("버튼");   // 돌아오지 않는다
            break;
    }
}

void loop() {
    tickSleepTimer();
    flushPrefsWhenIdle();
    static uint32_t lastRetry = 0;
    static uint32_t wifiDownSince = 0;   // 끊김을 처음 본 시각. 붙어 있으면 0
    if (wifiRecovery) {
        if (millis() - lastRetry > kWifiRecoveryMs) {
            lastRetry = millis();
            if (connectWifi(1)) {
                RLOGI("Wi-Fi 복구됨 — 재시작");
                delay(200);
                ESP.restart();
            }
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        // 붙었다가 끊긴 경우. 처음에는 여기서 10초마다 WiFi.reconnect() 를 불렀는데,
        // 구형 C6 펌웨어는 그 안의 Req_WifiDisconnect RPC 에 답하지 않아 호출이
        // 10초씩 통째로 막혔다. 다시 붙지도 않으면서 loop 만 세워 두니 화면이
        // 10초에 한 번 갱신되고 터치도 그때만 처리됐다 — 14시간 뒤 "잠에서 안
        // 깨어난다"로 보였던 것이 이것이다.
        //
        // 이 슬레이브에서 믿을 수 있는 복구는 부팅 경로(C6 리셋 + 10초 대기)뿐이다.
        // 끊긴 채 kWifiLostRestartMs 가 지나면 재시작한다. 설정은 NVS 에 있다.
        if (!wifiDownSince) {
            wifiDownSince = millis();
            RLOGE("Wi-Fi 끊김 감지 — %lu초 안에 안 돌아오면 재시작",
                  (unsigned long)(kWifiLostRestartMs / 1000));
            if (audioTaskHandle) {
                lockShared();
                const bool paused = shared.paused;
                unlockShared();
                if (!paused) setState(ST_ERROR, "WIFI LOST");
            }
            uiWake();
        } else if (millis() - wifiDownSince > kWifiLostRestartMs) {
            RLOGE("Wi-Fi 가 돌아오지 않는다 — 재시작");
            setState(ST_ERROR, "WIFI LOST - RESTART");
            uiRender(snapshotUi());
            savePrefs();
            Serial.flush();
            delay(300);
            ESP.restart();
        }
    } else {
        wifiDownSince = 0;
    }

    // ── 터치 이벤트 (터치 태스크가 보낸 것) ──────────────────────
    UiEvent ev;
    while (xQueueReceive(uiEventQueue, &ev, 0) == pdTRUE) handleUiEvent(ev);

    // 끌고 있으면 바늘·슬라이더가 손을 따라가야 한다. 다만 폴링마다가 아니라
    // 20fps 로 — 한 장 전송이 끝나기 전에 다음 장을 그릴 이유가 없다.
    const uint32_t frameMs = uiNeedsRedraw() ? kDragFrameMs : 100;
    if ((uiNeedsRedraw() || uiShowsMeters()) && uiScreenIsOn() && millis() - lastUiMs >= frameMs) {
        uiRender(snapshotUi());
        lastUiMs = millis();
    }
    uiTickBacklight();
    wantMeters.store(uiShowsMeters() && uiScreenIsOn(), std::memory_order_relaxed);

    if (WiFi.status() == WL_CONNECTED) ArduinoOTA.handle();

    static uint32_t lastStatusMs = 0;
    if (millis() - lastStatusMs > kStatusPeriodMs) {
        lastStatusMs = millis();
        pollSlowStatus();

        // 배터리 컷오프. 외부 전원이 들어오면 지킬 일이 없다.
        static uint8_t strikes = 0;
        lockShared();
        const HwPower pw = shared.power;
        unlockShared();
        if (pw.battery && !pw.vbus && !pw.charging && pw.battMv >= 2500 &&
            pw.percent <= BATT_CUTOFF_PERCENT) {
            if (++strikes >= BATT_CUTOFF_STRIKES) {
                RLOGE("배터리 %u%% — 자동 종료", (unsigned)pw.percent);
                setState(ST_LOWBATT);
                uiWake();
                uiRender(snapshotUi());
                delay(3000);
                powerOff("배터리");
            }
        } else {
            strikes = 0;
        }
    }

    if (uiScreenIsOn() && millis() - lastUiMs > kUiPeriodMs) {
        lastUiMs = millis();
        uiRender(snapshotUi());
    }

    delay(5);
}
