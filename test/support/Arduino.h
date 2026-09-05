// 터치 경로를 실제 ui.cpp 로 검사하기 위한 최소 호스트 대역.
#pragma once
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

class String : public std::string {
public:
    using std::string::string;
    String(const std::string& s) : std::string(s) {}
    template<typename T, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
    String(T value) : std::string(std::to_string(value)) {}
    String(float value, int places) {
        char b[32]; std::snprintf(b, sizeof(b), "%.*f", places, value); assign(b);
    }
    bool isEmpty() const { return empty(); }
};

inline uint32_t testMillis = 0;
inline uint32_t millis() { return testMillis; }
inline void delay(uint32_t ms) { testMillis += ms; }
struct TestMutex { int depth = 0; };
using SemaphoreHandle_t = TestMutex*;
constexpr uint32_t portMAX_DELAY = UINT32_MAX;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return new TestMutex; }
inline void xSemaphoreTakeRecursive(SemaphoreHandle_t m, uint32_t) { ++m->depth; }
inline void xSemaphoreGiveRecursive(SemaphoreHandle_t m) { assert(m->depth > 0); --m->depth; }
struct TestSerial { template<typename... T> void printf(T...) {} };
inline TestSerial Serial;
