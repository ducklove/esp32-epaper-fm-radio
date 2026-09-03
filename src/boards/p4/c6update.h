// ESP32-C6 (Wi-Fi 코프로세서) 펌웨어 갱신.
//
// P4 의 Wi-Fi 는 옆의 C6 가 ESP-Hosted 로 대신한다. 호스트(P4 쪽 라이브러리)와
// 슬레이브(C6 펌웨어)의 버전이 맞아야 하는데, 공장 출하 C6 펌웨어는 오래돼서
// 버전 질의에조차 답하지 않았고(0.0.0), 그 상태로는 접속이 안 됐다.
//
// Arduino 코어에 호스트와 같은 버전의 C6 펌웨어(esp32c6-v2.12.11.bin)가 들어
// 있고, P4 가 SDIO 로 그걸 밀어 넣는 API 도 있다. 그 바이너리를 P4 펌웨어에
// 함께 구워 두고(platformio.ini 의 board_build.embed_files), 부팅 때 버전이
// 다르면 갱신한다. 인터넷이 필요 없다 — Wi-Fi 가 안 되는 상황을 고치는
// 것이니 당연히 그래야 한다.
#pragma once

#include <Arduino.h>

// WiFi.mode() 로 ESP-Hosted 가 초기화된 뒤에 부를 것.
// 갱신했으면 true — 호출한 쪽에서 재시작해야 새 펌웨어로 C6 가 다시 뜬다.
// 버전이 같거나, 갱신에 실패했거나, 이미 두 번 시도했으면 false.
bool c6UpdateIfNeeded(void (*progress)(uint8_t percent));

// 갱신을 몇 번 시도했는지 (NVS). 성공해서 버전이 맞으면 0 으로 돌아간다.
uint8_t c6UpdateAttempts();
