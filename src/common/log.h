// 진단 출력.
//
// Arduino 의 log_i/log_e 는 결국 ets_printf 로 가는데, 그건 UART0(GPIO43/44)로
// 나간다. 이 보드는 USB-Serial/JTAG 하나뿐이고 USB-UART 브리지가 없어서 그쪽
// 출력은 아무데도 보이지 않는다. 그래서 Arduino Serial(USB CDC)로 직접 찍는다.
#pragma once

#include <Arduino.h>

#define RLOGI(fmt, ...) Serial.printf("[I] " fmt "\n", ##__VA_ARGS__)
#define RLOGE(fmt, ...) Serial.printf("[E] " fmt "\n", ##__VA_ARGS__)
