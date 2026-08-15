# ESP32-S3 ePaper FM Radio

Waveshare **ESP32-S3-ePaper-1.54** 보드로 만든 FM 라디오.

## 먼저 알아둘 것 — 왜 "인터넷 스트리밍"인가

이 보드에는 **FM 튜너 칩이 없다.** `esptool` 로 직접 읽은 온보드 구성은 다음과 같다.

```
Chip type : ESP32-S3-PICO-1 (LGA56) rev v0.2
Features  : Wi-Fi, BT 5 (LE), Dual Core + LP Core, 240MHz
Flash     : 8MB (embedded, GD, quad)
PSRAM     : 8MB (AP_3v3)
```

부품은 ePaper 200×200(SSD1681), ES8311 오디오 코덱 + 스피커/마이크, PCF85063 RTC,
SHTC3 온습도 센서, TF 카드 슬롯이 전부다. 88\~108MHz 를 수신하는 회로(RDA5807M,
Si4703 등)가 없고, ESP32-S3 의 무선은 2.4GHz Wi-Fi/BLE 전용이므로 **보드 단독으로는
전파를 직접 받을 수 없다.**

그래서 이 펌웨어는 방송사 HLS 스트림을 Wi-Fi 로 받아 재생한다. 나오는 소리는 실제
FM 방송 그대로이고, ePaper 에는 각 채널의 **실제 서울 FM 주파수**를 쓴 아날로그
다이얼을 그린다.

진짜로 전파를 받고 싶다면 RDA5807M 같은 I2C FM 튜너 모듈을 확장 헤더(2×6, 2.54mm)에
물려야 한다. 모듈의 아날로그 출력을 ES8311 입력으로 넣는 배선이 추가로 필요하다.

## 하드웨어 핀맵

Waveshare 공식 예제([waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54))에서
확인한 값이며 `src/config.h` 에 그대로 들어 있다.

| 블록 | 핀 |
|---|---|
| ePaper (SPI2) | BUSY=8, RST=9, DC=10, CS=11, SCK=12, MOSI=13 |
| ES8311 I2S | MCLK=14, BCLK=15, WS=38, DOUT=45, DIN=16 |
| ES8311 I2C | SDA=47, SCL=48 (주소 0x18) |
| 스피커 앰프 | PA=46 (Active-High) |
| 전원 레일 | EPD=6, Audio=42 — **둘 다 Active-LOW** / VBAT=17 (Active-High) |
| 버튼 | BOOT=0, PWR=18 (둘 다 풀업, Active-LOW) |

전원 레일 극성에 주의. `GPIO42` 를 LOW 로 내리지 않으면 ES8311 이 I2C 에 아예
응답하지 않는다.

## 채널

`src/stations.h` — `mad-for-audio` 프로젝트의 `stations.js` 와 같은 소스를 쓴다.
주파수 오름차순으로 15개.

| MHz | 채널 | 방식 |
|---|---|---|
| 89.1 | KBS Cool FM | KBS API |
| 91.9 | MBC FM4U | 텍스트 API |
| 93.1 | KBS Classic FM | KBS API |
| 93.9 | CBS Music FM | 직접 |
| 94.5 | YTN Radio | 직접 |
| 95.9 | MBC Standard FM | 텍스트 API |
| 97.3 | KBS Radio 1 | KBS API |
| 98.1 | CBS Standard FM | 직접 |
| 99.1 | Gugak FM | 직접 |
| 103.5 | SBS Love FM | 텍스트 API |
| 104.5 | EBS FM | 직접 |
| 104.9 | KBS Radio 3 | KBS API |
| 106.1 | KBS Happy FM | KBS API |
| 106.9 | FEBC Seoul | 직접 |
| 107.7 | SBS Power FM | 텍스트 API |

KBS/MBC/SBS 스트림 URL 에는 만료되는 서명이 붙어 있어서, 선국할 때마다 API 를
다시 호출해 새 URL 을 받는다.

EBS 는 `stations.js` 쪽 `familypc` 스트림이 640×360 영상을 함께 실어 보낸다.
ESP32 의 TS 디먹서가 감당하지 못해 오디오 전용 AAC 스트림으로 바꿔 두었다.

## 조작

버튼이 두 개뿐이라 이렇게 나눴다.

| 버튼 | 동작 |
|---|---|
| BOOT 짧게 | 다음 채널 (주파수 올라감) |
| BOOT 길게 (0.7초+) | 이전 채널 |
| PWR 짧게 | 볼륨 +2 (최대에서 다시 0) |
| PWR 길게 (0.7초+) | 음소거 토글 |

## 빌드 & 플래시

PlatformIO Core 가 필요하다.

```bash
pip install platformio
```

Wi-Fi 정보를 넣는다.

```bash
cp src/secrets.h.example src/secrets.h
```

`src/secrets.h` 는 `.gitignore` 에 걸려 있어 저장소에 올라가지 않는다.

빌드 후 업로드:

```bash
pio run -t upload
```

시리얼 로그:

```bash
pio device monitor
```

`platformio.ini` 의 `upload_port` / `monitor_port` 는 `COM5` 로 잡혀 있다. 포트가
다르면 그 줄을 고치거나 두 줄을 지워서 자동 탐색에 맡기면 된다.

### Windows: 260자 경로 제한

ESP-IDF 5.5 의 `esp_matter/connectedhomeip` 헤더 경로가 260자를 넘어서, 기본
PlatformIO 홈(`C:\Users\<user>\.platformio`)으로는 패키지 압축 해제가
`FileNotFoundError` 로 깨진다. 홈 경로를 짧게 만들면 통과한다.

```bash
cmd //c mklink /J C:\pio C:\Users\%USERNAME%\.platformio
```

이후 빌드할 때마다 `PLATFORMIO_CORE_DIR=C:\pio` 를 지정한다.

```bash
PLATFORMIO_CORE_DIR=C:\pio pio run -t upload
```

(레지스트리의 `LongPathsEnabled` 를 켜는 방법도 있지만 시스템 전역 설정이라
여기서는 건드리지 않았다.)

## 구조

```
src/
  main.cpp      선국·버튼·태스크 배치
  config.h      핀맵 (Waveshare 예제 기준)
  es8311.h/cpp  ES8311 코덱 드라이버 (재생 전용)
  stations.h/cpp 채널 목록 + 스트림 URL 해석
  ui.h/cpp      ePaper FM 다이얼 화면
```

오디오는 `audioTask`(core 1, 우선순위 3)에서만 만진다. ePaper 갱신은 한 번에 1초
가까이 걸리는데, 이쪽은 우선순위 1인 `loopTask` 에서 돌기 때문에 화면을 그리는 동안
오디오 태스크가 선점해서 소리가 끊기지 않는다.

`ESP32-audioI2S` 는 I2S 를 32bit 슬롯 / Philips / MCLK=256×fs 로 잡는다. 리샘플링에
CPU 를 쓰지 않으려고 출력 샘플레이트는 스트림 원본을 그대로 따라가고, 값이 바뀔
때마다 ES8311 클럭 계수를 다시 계산한다.

## 동작 확인

`pio run -t upload` 후 실제 보드에서 잡은 로그.

```
[I] 스트림 URL: https://1fm.gscdn.kbs.co.kr/1fm_192_2.m3u8?Policy=...
[A/info] SSL has been established in 2127 ms
[A/info] next URL: "https://1fm.gscdn.kbs.co.kr/1fm_192_2_44502027.aac?m=..."
[A/info] AACDecoder has been initialized
[A/info] stream ready
[A/info] AAC HeaderFormat: ADTS
[A/info] Channels: 2
[A/info] SampleRate (Hz): 48000
[A/info] BitsPerSample: 32
[A/info] next URL: ".../1fm_192_2_44502028.aac"   ← 세그먼트 연속 수신
[A/bitrate (b/s)] 202105
_Update_Part : 364263                             ← ePaper 부분 갱신 364ms
```

빌드 크기는 RAM 20.3%, Flash 63.6%(3.34MB 파티션 중 2.12MB).

### 시리얼 로그가 안 보인다면

이 보드에는 USB-UART 브리지가 없고 ESP32-S3 네이티브 USB 하나만 있다. Arduino 의
`log_i`/`log_e` 는 결국 `ets_printf` → UART0(GPIO43/44) 로 나가기 때문에 화면에
아무것도 찍히지 않는다. 그래서 이 프로젝트의 진단 출력은 `src/log.h` 의
`RLOGI`/`RLOGE` 로 `Serial`(USB CDC)에 직접 쓴다. 오디오 라이브러리 내부 로그도
`audio_info_callback` 을 통해 같은 경로로 흘려보낸다.

조용히 쓰고 싶으면 `platformio.ini` 의 `CORE_DEBUG_LEVEL` 을 1로 낮추면 된다.

## 알려진 한계

- **한글 표시 불가.** Adafruit GFX 내장 폰트에 한글이 없어 채널명을 로마자로 적었다.
  ICY 메타데이터(곡 제목)도 같은 이유로 화면에 띄우지 않는다. 한글 비트맵 폰트를
  넣으면 둘 다 해결된다.
- ePaper 특성상 화면 갱신이 느리다. 부분 갱신을 쓰되 12회마다 잔상 제거용 전체
  갱신을 한 번 넣는다.
- 방송사 API/스트림 주소는 언제든 바뀔 수 있다. 바뀌면 `src/stations.h` 를 고친다.
