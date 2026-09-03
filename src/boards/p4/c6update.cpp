#include "c6update.h"

#include <Preferences.h>

#include "log.h"

#if CONFIG_ESP_HOSTED_ENABLED
#include "esp32-hal-hosted.h"

// c6fw.S 가 .incbin 으로 넣어 둔 바이너리. 경로는 tools/p4_c6fw.py 참고.
extern "C" const uint8_t c6fw_start[];
extern "C" const uint8_t c6fw_end[];
static const uint8_t* const c6fwStart = c6fw_start;
static const uint8_t* const c6fwEnd   = c6fw_end;

namespace {

constexpr uint8_t kMaxAttempts = 2;
constexpr size_t  kChunk = 4096;

uint8_t loadAttempts() {
    Preferences p;
    uint8_t v = 0;
    if (p.begin("radio", true)) {
        v = p.getUChar("c6tries", 0);
        p.end();
    }
    return v;
}

void saveAttempts(uint8_t v) {
    Preferences p;
    if (p.begin("radio", false)) {
        p.putUChar("c6tries", v);
        p.end();
    }
}

}  // namespace

uint8_t c6UpdateAttempts() { return loadAttempts(); }

bool c6UpdateIfNeeded(void (*progress)(uint8_t)) {
    if (!hostedIsInitialized()) {
        RLOGE("C6 갱신: ESP-Hosted 가 초기화되지 않았다");
        return false;
    }

    // 버전이 같으면 할 일이 없다. 시도 횟수도 여기서 되돌린다.
    if (!hostedHasUpdate()) {
        if (loadAttempts()) saveAttempts(0);
        return false;
    }

    // 밀어 넣어도 버전이 계속 안 맞으면 (쓰기는 됐는데 활성화가 안 되는 등)
    // 부팅마다 1.2MB 를 다시 쓰는 일은 하지 않는다.
    const uint8_t tries = loadAttempts();
    if (tries >= kMaxAttempts) {
        RLOGE("C6 갱신: 이미 %u번 시도했다 — 건너뜀", (unsigned)tries);
        return false;
    }
    saveAttempts((uint8_t)(tries + 1));

    const size_t total = (size_t)(c6fwEnd - c6fwStart);
    RLOGI("C6 펌웨어 갱신 시작 — %u bytes, 시도 %u/%u", (unsigned)total, (unsigned)(tries + 1),
          (unsigned)kMaxAttempts);
    if (progress) progress(0);

    if (!hostedBeginUpdate()) {
        RLOGE("C6 갱신: begin 실패");
        return false;
    }

    size_t  off = 0;
    uint8_t lastPct = 0;
    while (off < total) {
        const size_t n = (total - off < kChunk) ? (total - off) : kChunk;
        // API 가 non-const 를 받지만 읽기만 한다.
        if (!hostedWriteUpdate((uint8_t*)(c6fwStart + off), (uint32_t)n)) {
            RLOGE("C6 갱신: write 실패 (offset %u)", (unsigned)off);
            return false;
        }
        off += n;
        const uint8_t pct = (uint8_t)(off * 100 / total);
        if (pct / 5 != lastPct / 5) {
            lastPct = pct;
            RLOGI("C6 갱신 %u%%", (unsigned)pct);
            if (progress) progress(pct);
        }
        delay(1);
    }

    if (!hostedEndUpdate()) {
        RLOGE("C6 갱신: end 실패");
        return false;
    }
    // 구형 슬레이브는 activate 에 답하지 않는다. 코어도 그 경우를 치명적으로
    // 보지 않으므로 여기서도 실패로 치지 않는다 — 재시작하면 새 펌웨어로 뜬다.
    if (!hostedActivateUpdate()) {
        RLOGE("C6 갱신: activate 응답 없음 (구형 펌웨어면 정상) — 재시작으로 적용");
    }
    RLOGI("C6 펌웨어 갱신 완료 — 재시작 필요");
    if (progress) progress(100);
    return true;
}

#else

uint8_t c6UpdateAttempts() { return 0; }
bool c6UpdateIfNeeded(void (*)(uint8_t)) { return false; }

#endif
