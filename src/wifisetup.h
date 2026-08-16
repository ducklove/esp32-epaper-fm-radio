// Wi-Fi 설정을 기기에서 바꾸는 방법.
//
// 접속 정보를 펌웨어에 박아 두면 다른 곳에 들고 갔을 때 재빌드 없이는 못 쓴다.
// 여기서는 NVS 에 저장해 두고, 없거나 접속에 실패하면 보드가 자기 AP 를 띄워
// 브라우저로 입력받는다. secrets.h 의 값은 NVS 가 비어 있을 때의 초기값으로만
// 쓰인다.
#pragma once

#include <Arduino.h>

struct WifiCreds {
    String ssid;
    String pass;
};

// NVS 에 저장된 값. 없으면 secrets.h 의 값을 돌려준다.
WifiCreds wifiLoadCreds();
void      wifiSaveCreds(const String& ssid, const String& pass);
void      wifiClearCreds();

// 설정 포털을 띄운다. AP 를 열고 웹 폼을 제공하며, 저장되면 true 를 돌려주고
// 호출한 쪽에서 재시작하면 된다. timeoutMs 동안 아무도 저장하지 않으면 false.
//
// 진행 상황(AP 이름, 접속 주소)은 콜백으로 넘겨 ePaper 에 표시할 수 있게 한다.
using WifiPortalStatus = void (*)(const String& apSsid, const String& url, const String& note);
bool wifiRunPortal(uint32_t timeoutMs, WifiPortalStatus onStatus);
