#include "battery.h"

#include "log.h"

namespace {

// 부하가 걸린 상태의 1셀 리튬 방전 곡선. 전압만으로 잔량을 맞추는 건 원래
// 부정확하지만(전류에 따라 처지므로), 눈금 다섯 칸짜리 아이콘에는 충분하다.
struct Point {
    float   volts;
    uint8_t percent;
};
constexpr Point kCurve[] = {
    {4.20f, 100}, {4.00f, 80}, {3.85f, 60}, {3.75f, 40},
    {3.65f, 20},  {3.45f, 5},  {3.30f, 0},
};

constexpr uint8_t kSamples = 8;

}  // namespace

void Battery::begin(uint8_t adcPin) {
    _pin = adcPin;
    // 핀을 먼저 한 번 읽어 ADC 채널로 잡아 둔다. 이걸 건너뛰고 감쇠부터
    // 설정하면 "Pin is not configured as analog channel" 로 무시된다.
    (void)analogRead(_pin);
    // 12dB 감쇠 = 0~3.3V 입력 범위. 분압 후 전압이 최대 2.1V 라 이 범위면 된다.
    analogSetPinAttenuation(_pin, ADC_11db);
    _ready = true;
}

float Battery::readVolts() {
    if (!_ready) return 0.0f;

    uint32_t sum = 0;
    for (uint8_t i = 0; i < kSamples; i++) {
        sum += analogReadMilliVolts(_pin);  // 캘리브레이션 적용된 mV
    }
    const float mv = (float)sum / kSamples;
    return mv * 2.0f / 1000.0f;  // 1:2 분압 복원
}

uint8_t Battery::voltsToPercent(float v) {
    if (v >= kCurve[0].volts) return 100;

    constexpr size_t n = sizeof(kCurve) / sizeof(kCurve[0]);
    if (v <= kCurve[n - 1].volts) return 0;

    for (size_t i = 1; i < n; i++) {
        if (v >= kCurve[i].volts) {
            const Point& hi = kCurve[i - 1];
            const Point& lo = kCurve[i];
            const float t = (v - lo.volts) / (hi.volts - lo.volts);
            return (uint8_t)lroundf(lo.percent + t * (hi.percent - lo.percent));
        }
    }
    return 0;
}
