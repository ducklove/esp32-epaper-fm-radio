// 하드웨어에 의존하지 않는 청취 설정. 호스트 회귀 테스트에서도 같은 코드를 쓴다.
#pragma once

#include <stdint.h>

struct TonePreset {
    const char* name;
    int8_t bass, mid, treble;
};

// mad-for-audio/app.js 의 NIGHT, VOCAL, FM NR 의 의도를 3밴드로 옮긴다.
// 원본의 10밴드 곡선과 동일한 필터는 아니다. FLAT 이 기본값이다.
constexpr TonePreset kTonePresets[] = {
    {"FLAT", 0, 0, 0}, {"NIGHT", 2, 0, 1},
    {"VOCAL", -2, 3, -1}, {"FM NR", 0, 0, -4},
};
constexpr uint8_t kTonePresetCount = sizeof(kTonePresets) / sizeof(kTonePresets[0]);
constexpr uint8_t kSleepMinutes[] = {0, 15, 30, 60, 90};
constexpr uint8_t kSleepStepCount = sizeof(kSleepMinutes) / sizeof(kSleepMinutes[0]);

struct ListeningSettings {
    uint8_t index = 2;
    uint8_t volume = 12;
    uint8_t tone = 0;
    bool paused = false;

    void validate(uint8_t stationCount, uint8_t maxVolume) {
        if (index >= stationCount) index = stationCount > 2 ? 2 : 0;
        if (volume > maxVolume) volume = maxVolume < 12 ? maxVolume : 12;
        if (tone >= kTonePresetCount) tone = 0;
    }
};

enum class PlaybackChange : uint8_t { NONE, PAUSE, RESTART, VOLUME };
inline PlaybackChange listeningChange(const ListeningSettings& desired, const ListeningSettings& applied,
                                     bool initialized, uint32_t revision, uint32_t appliedRevision) {
    if (desired.paused) return !initialized || !applied.paused ? PlaybackChange::PAUSE : PlaybackChange::NONE;
    if (!initialized || applied.paused || desired.index != applied.index || revision != appliedRevision)
        return PlaybackChange::RESTART;
    return desired.volume != applied.volume ? PlaybackChange::VOLUME : PlaybackChange::NONE;
}

class SleepTimer {
public:
    void cycle(uint32_t now) {
        step_ = (step_ + 1) % kSleepStepCount;
        started_ = now;
    }
    uint8_t minutes() const { return kSleepMinutes[step_]; }
    uint32_t remainingSeconds(uint32_t now) const {
        const uint32_t duration = uint32_t(minutes()) * 60000;
        const uint32_t elapsed = now - started_;  // millis() 순환에도 안전하다.
        return !duration || elapsed >= duration ? 0 : (duration - elapsed + 999) / 1000;
    }
    bool expire(uint32_t now) {
        if (!step_ || remainingSeconds(now)) return false;
        step_ = 0;  // 만료는 한 번만 전달한다. NTP 와 무관하다.
        return true;
    }
private:
    uint8_t step_ = 0;
    uint32_t started_ = 0;
};

constexpr uint8_t meterByte(uint32_t value) { return value > 255 ? 255 : uint8_t(value); }
constexpr uint8_t kSpectrumBands = 15;
struct AudioMeters {
    uint8_t left = 0, right = 0, peakLeft = 0, peakRight = 0;
    uint8_t bands[kSpectrumBands] = {};
    uint32_t updatedMs = 0;
};
