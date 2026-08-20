// ES8311 코덱 최소 드라이버 (재생 전용)
//
// Espressif esp_codec_dev 의 es8311.c 레지스터 시퀀스를 그대로 옮긴 것.
// esp_codec_dev 전체를 끌어오지 않는 이유는 ESP32-audioI2S 가 I2S 채널을
// 직접 소유하기 때문이다. 여기서는 I2C 로 코덱 레지스터만 설정한다.
#pragma once

#include <Arduino.h>

// 코덱 레지스터를 실어 나르는 I2C 전송. 기본 구현은 Arduino Wire 다.
//
// 갈아 끼울 수 있게 해 둔 이유는 M5StickS3 때문이다. 그 보드는 M5Unified 가
// PMIC·IMU 를 위해 내부 버스를 자기 I2C 드라이버로 이미 잡는데, 같은 하드웨어
// 포트에 Arduino Wire(ESP-IDF 5.5 의 새 i2c_master 드라이버)를 또 올리면
// 두 번째 마스터가 ESP_ERR_INVALID_STATE 로 죽는다. 그래서 그 보드에서는
// M5.In_I2C 를 쓰는 구현을 끼워 넣어 버스 주인을 하나로 만든다.
struct ES8311Bus {
    bool (*write)(uint8_t addr, uint8_t reg, uint8_t val);
    bool (*read)(uint8_t addr, uint8_t reg, uint8_t* val);
    bool (*probe)(uint8_t addr);
};

// codec.begin() 보다 먼저 부를 것. 부르지 않으면 Wire 를 쓴다.
void es8311SetBus(const ES8311Bus& bus);

class ES8311 {
  public:
    // 코덱을 재생 가능한 상태로 만든다. 버스는 이미 열려 있어야 한다 —
    // sda/scl 은 실패했을 때 로그에 찍기 위해서만 쓴다.
    // 호출 전에 오디오 전원 레일(PIN_PWR_AUDIO)이 켜져 있어야 하고,
    // MCLK 가 이미 나오고 있어야 안정적이다.
    bool begin(uint8_t sda, uint8_t scl, uint32_t sampleRate, uint8_t bitsPerSample,
               uint16_t mclkDiv, int8_t paPin);

    // 샘플레이트가 바뀌었을 때 클럭 계수를 다시 잡는다.
    bool setSampleRate(uint32_t sampleRate, uint16_t mclkDiv);

    // -95.5 dB ~ +32 dB. 0.5 dB 단위.
    void setVolumeDb(float db);
    void setMute(bool mute);
    void setPaEnabled(bool on);

    uint8_t address() const { return _addr; }
    bool    ok() const { return _ok; }

  private:
    bool writeReg(uint8_t reg, uint8_t val);
    bool readReg(uint8_t reg, uint8_t* val);
    bool updateReg(uint8_t reg, uint8_t mask, uint8_t val);  // (cur & ~mask) | val
    bool probe(uint8_t addr);

    uint8_t _addr = 0x18;
    int8_t  _paPin = -1;
    bool    _ok = false;
};
