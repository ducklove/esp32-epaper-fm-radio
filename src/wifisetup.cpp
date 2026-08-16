#include "wifisetup.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "log.h"
#include "secrets.h"

namespace {

constexpr const char* kNvsNamespace = "radio";
constexpr const char* kKeySsid = "ssid";
constexpr const char* kKeyPass = "pass";

constexpr const char* kApSsid = "esp32-radio-setup";
constexpr const char* kApPass = "fmradio1234";  // 8자 이상이어야 WPA2 가 걸린다

bool      g_saved = false;
WebServer g_server(80);
DNSServer g_dns;

// 스마트폰이 AP 에 붙으면 어디를 열든 설정 화면이 뜨도록 모든 요청을 돌린다.
constexpr byte kDnsPort = 53;

String htmlEscape(const String& s) {
    String out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

String pageForm(const String& current, const String& msg) {
    String h;
    h += "<!doctype html><meta charset=utf-8>";
    h += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    h += "<title>FM Radio Wi-Fi</title>";
    h += "<style>body{font-family:system-ui,sans-serif;margin:0;padding:24px;"
         "background:#111;color:#eee}h1{font-size:20px;margin:0 0 4px}"
         "p{color:#aaa;font-size:14px;margin:0 0 20px}"
         "label{display:block;margin:14px 0 6px;font-size:14px}"
         "input{width:100%;box-sizing:border-box;padding:12px;font-size:16px;"
         "border:1px solid #444;border-radius:8px;background:#1c1c1c;color:#eee}"
         "button{margin-top:22px;width:100%;padding:14px;font-size:16px;border:0;"
         "border-radius:8px;background:#2d7;color:#062;font-weight:600}"
         ".msg{background:#243;padding:10px 12px;border-radius:8px;margin-bottom:16px;"
         "font-size:14px;color:#bfd}</style>";
    h += "<h1>FM Radio Wi-Fi 설정</h1>";
    h += "<p>접속할 Wi-Fi 를 입력하세요. 2.4GHz 만 됩니다.</p>";
    if (msg.length()) h += "<div class=msg>" + htmlEscape(msg) + "</div>";
    h += "<form method=POST action=/save>";
    h += "<label>SSID</label>";
    h += "<input name=ssid value='" + htmlEscape(current) + "' autocapitalize=off autocorrect=off>";
    h += "<label>비밀번호</label>";
    h += "<input name=pass type=password placeholder='없으면 비워 두세요'>";
    h += "<button type=submit>저장하고 재시작</button></form>";
    return h;
}

void handleRoot() { g_server.send(200, "text/html; charset=utf-8", pageForm(wifiLoadCreds().ssid, "")); }

void handleSave() {
    const String ssid = g_server.arg("ssid");
    const String pass = g_server.arg("pass");
    if (ssid.isEmpty()) {
        g_server.send(200, "text/html; charset=utf-8", pageForm("", "SSID 를 입력하세요."));
        return;
    }
    wifiSaveCreds(ssid, pass);
    g_saved = true;

    String h = "<!doctype html><meta charset=utf-8>";
    h += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    h += "<style>body{font-family:system-ui,sans-serif;margin:0;padding:24px;"
         "background:#111;color:#eee}</style>";
    h += "<h1>저장했습니다</h1><p>재시작해서 <b>" + htmlEscape(ssid) +
         "</b> 에 접속합니다.</p>";
    g_server.send(200, "text/html; charset=utf-8", h);
}

}  // namespace

WifiCreds wifiLoadCreds() {
    Preferences p;
    WifiCreds c;
    if (p.begin(kNvsNamespace, true)) {
        c.ssid = p.getString(kKeySsid, "");
        c.pass = p.getString(kKeyPass, "");
        p.end();
    }
    // 저장된 게 없으면 펌웨어에 박아 둔 초기값을 쓴다.
    if (c.ssid.isEmpty()) {
        c.ssid = WIFI_SSID;
        c.pass = WIFI_PASSWORD;
    }
    return c;
}

void wifiSaveCreds(const String& ssid, const String& pass) {
    Preferences p;
    if (!p.begin(kNvsNamespace, false)) {
        RLOGE("NVS 열기 실패 — Wi-Fi 정보를 저장하지 못했다");
        return;
    }
    p.putString(kKeySsid, ssid);
    p.putString(kKeyPass, pass);
    p.end();
    RLOGI("Wi-Fi 정보 저장: %s", ssid.c_str());
}

void wifiClearCreds() {
    Preferences p;
    if (p.begin(kNvsNamespace, false)) {
        p.remove(kKeySsid);
        p.remove(kKeyPass);
        p.end();
        RLOGI("저장된 Wi-Fi 정보를 지웠다");
    }
}

bool wifiRunPortal(uint32_t timeoutMs, WifiPortalStatus onStatus) {
    g_saved = false;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(kApSsid, kApPass);
    delay(300);

    const String url = "http://" + WiFi.softAPIP().toString();
    RLOGI("설정 포털: SSID=%s  암호=%s  주소=%s", kApSsid, kApPass, url.c_str());
    if (onStatus) onStatus(kApSsid, url, kApPass);

    g_dns.start(kDnsPort, "*", WiFi.softAPIP());
    g_server.on("/", handleRoot);
    g_server.on("/save", HTTP_POST, handleSave);
    g_server.onNotFound(handleRoot);  // 캡티브 포털처럼 아무 주소나 설정 화면으로
    g_server.begin();

    const uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        g_dns.processNextRequest();
        g_server.handleClient();
        if (g_saved) {
            delay(500);  // 브라우저가 응답을 받을 시간
            break;
        }
        delay(5);
    }

    g_server.stop();
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    return g_saved;
}
