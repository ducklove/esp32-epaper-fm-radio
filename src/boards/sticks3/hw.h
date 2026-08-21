// M5StickS3 하드웨어 중 M5Unified 가 대신 해 주지 않는 부분.
//
// 두 가지가 있다.
//
// 1) 스피커 앰프(AW8737) enable 이 ESP32 핀이 아니라 M5PM1 PMIC 의 GPIO3 이다.
//    문서에 G3 으로 적혀 있어 ESP32 핀으로 오해하기 쉽다. M5Unified 는
//    Speaker 를 켤 때 같이 처리하는데, 우리는 I2S 를 오디오 라이브러리가
//    소유해야 해서 M5.Speaker 를 쓰지 않는다. 그래서 직접 켜야 한다.
//
// 2) 배터리 전압. M5Unified 0.2.27 의 Power_Class::getBatteryLevel() 은
//    M5PM1 분기가 ESP32-C61 빌드에만 들어 있어서, ESP32-S3 에서는 -1 을
//    돌려준다. PMIC 레지스터를 직접 읽는다.
//
// 이 보드의 내부 I2C 는 전부 M5.In_I2C 로 다룬다. 이유는 hw.cpp 참고.
#pragma once

#include <Arduino.h>

// 공용 ES8311 드라이버가 M5.In_I2C 로 말하도록 전송을 끼워 넣는다.
// codec.begin() 보다 먼저 부를 것.
void hwInstallCodecBus();

// PMIC 가 I2C 에 응답하는지. M5.begin() 뒤에 부를 것.
bool hwPmicPresent();

// 스피커 앰프 전원. M5PM1 레지스터 0x11 의 bit3 이 PMIC GPIO3 출력이다.
void hwSpeakerAmp(bool on);

// 배터리 전압(mV). 읽지 못하면 0.
uint16_t hwBatteryMillivolts();

// 전압을 0~100% 로. 부하가 걸린 1셀 리튬 방전 곡선 기준이라 어림값이다.
uint8_t hwBatteryPercent(uint16_t mv);

// PMIC 가 보는 전원 소스. 0=VIN, 1=VINOUT, 2=배터리, 3=모름.
// isCharging() 만으로는 부족하다 — USB 를 꽂아 두어도 만충이거나 입력 전류가
// 모자라면 '충전 중'이 아니게 되는데, 그렇다고 배터리로만 도는 것은 아니다.
uint8_t hwPowerSource();

// 외부 전원(USB)이 물려 있는가. 위 값이 VIN/VINOUT 일 때만 참이다.
bool hwExternalPower();

// USB 입력 전압(mV). 꽂혀 있지 않으면 0 부근이다.
// 공급이 모자라 전압이 주저앉는지 보려고 주기 로그에 같이 남긴다.
uint16_t hwVbusMillivolts();
