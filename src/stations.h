// 채널 정의 — D:\Work\mad-for-audio\stations.js 와 같은 소스를 쓴다.
//
// ePaper 에 올릴 수 있는 한글 폰트가 없어서 표시 이름만 로마자로 적었다.
// 주파수는 서울 기준 실제 FM 주파수이고, 다이얼 바늘 위치도 이 값을 쓴다.
// 배열은 주파수 오름차순 — 버튼으로 넘기면 주파수를 따라 올라간다.
#pragma once

#include <Arduino.h>

enum StreamType : uint8_t {
    STREAM_DIRECT,   // url 이 그대로 재생 가능한 m3u8
    STREAM_KBS_API,  // JSON 응답의 channel_item[0].service_url
    STREAM_TEXT_API, // 본문 전체가 URL 한 줄 (SBS / MBC)
};

struct Station {
    const char* name;  // ePaper 표시용
    float       freq;  // MHz (서울)
    StreamType  type;
    const char* url;   // 스트림 URL 또는 해석용 API URL
};

// EBS 는 stations.js 의 familypc 스트림이 640x360 영상을 함께 실어 보낸다.
// ESP32 쪽 TS 디먹서가 감당하지 못하므로 오디오 전용(AAC 128k) 스트림으로 바꿨다.
constexpr Station kStations[] = {
    {"KBS Cool FM",     89.1f, STREAM_KBS_API,  "https://cfpwwwapi.kbs.co.kr/api/v1/landing/live/channel_code/25"},
    {"MBC FM4U",        91.9f, STREAM_TEXT_API, "https://cantabile.tplinkdns.com:3689/?channel=mfm"},
    {"KBS Classic FM",  93.1f, STREAM_KBS_API,  "https://cfpwwwapi.kbs.co.kr/api/v1/landing/live/channel_code/24"},
    {"CBS Music FM",    93.9f, STREAM_DIRECT,   "https://m-aac.cbs.co.kr/mweb_cbs939/_definst_/cbs939.stream/playlist.m3u8"},
    {"YTN Radio",       94.5f, STREAM_DIRECT,   "https://radiolive.ytn.co.kr/radio/_definst_/20211118_fmlive/playlist.m3u8"},
    {"MBC Standard FM", 95.9f, STREAM_TEXT_API, "https://cantabile.tplinkdns.com:3689/?channel=sfm"},
    {"KBS Radio 1",     97.3f, STREAM_KBS_API,  "https://cfpwwwapi.kbs.co.kr/api/v1/landing/live/channel_code/21"},
    {"CBS Standard FM", 98.1f, STREAM_DIRECT,   "https://m-aac.cbs.co.kr/mweb_cbs981/_definst_/cbs981.stream/playlist.m3u8"},
    {"Gugak FM",        99.1f, STREAM_DIRECT,   "https://mgugaklive.nowcdn.co.kr/gugakradio/gugakradio.stream/playlist.m3u8"},
    {"SBS Love FM",    103.5f, STREAM_TEXT_API, "https://apis.sbs.co.kr/play-api/1.0/livestream/lovepc/lovefm?protocol=hls&ssl=Y"},
    {"EBS FM",         104.5f, STREAM_DIRECT,   "http://ebsonairiosaod.ebs.co.kr/fmradiobandiaod/bandiappaac/playlist.m3u8"},
    {"KBS Radio 3",    104.9f, STREAM_KBS_API,  "https://cfpwwwapi.kbs.co.kr/api/v1/landing/live/channel_code/23"},
    {"KBS Happy FM",   106.1f, STREAM_KBS_API,  "https://cfpwwwapi.kbs.co.kr/api/v1/landing/live/channel_code/22"},
    {"FEBC Seoul",     106.9f, STREAM_DIRECT,   "https://mlive3.febc.net/live5/mplive/playlist.m3u8"},
    {"SBS Power FM",   107.7f, STREAM_TEXT_API, "https://apis.sbs.co.kr/play-api/1.0/livestream/powerpc/powerfm?protocol=hls&ssl=Y"},
};

constexpr size_t kStationCount = sizeof(kStations) / sizeof(kStations[0]);

// 방송사 API 를 때려서 실제 스트림 URL 을 얻는다.
// KBS/MBC/SBS URL 은 서명이 붙어 있어 만료되므로 선국할 때마다 새로 받아야 한다.
// 실패하면 빈 문자열.
String resolveStreamUrl(const Station& s);
