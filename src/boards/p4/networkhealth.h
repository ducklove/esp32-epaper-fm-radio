#pragma once
#include <stdint.h>

inline bool audioWorkerStalled(uint32_t now, uint32_t workerAt, uint32_t pcmAt) {
    return workerAt && uint32_t(now - workerAt) >= 90000 &&
           (!pcmAt || uint32_t(now - pcmAt) >= 5000);
}

// C6가 멈추면 WL_CONNECTED가 남아도 RSSI RPC는 실패하여 0을 반환한다.
// 정상 RSSI를 받은 이력이 있고, 재생도 막힌 상태가 두 번 연속 관측되어야 복구한다.
// 부팅부터 RSSI를 지원하지 않는 펌웨어나 방송사 서버 오류만으로 재시작하지 않는다.
class NetworkHealth {
    bool hadRssi = false;
    uint8_t misses = 0;
public:
    bool observe(bool connected, int16_t rssi, bool playbackBlocked) {
        if (!connected) { hadRssi = false; misses = 0; return false; }
        if (rssi < 0 && rssi >= -127) { hadRssi = true; misses = 0; return false; }
        if (!hadRssi || rssi != 0 || !playbackBlocked) { misses = 0; return false; }
        if (misses < 2) ++misses;
        return misses >= 2;
    }
};
