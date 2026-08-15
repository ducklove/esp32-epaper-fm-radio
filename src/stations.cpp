#include "stations.h"

#include <HTTPClient.h>
#include <WiFi.h>  // core 3.x 에서 WiFiClient 는 NetworkClient 의 별칭이고, 이 헤더에 있다
#include <WiFiClientSecure.h>

#include "log.h"

namespace {

// 방송사 API 가 응답을 물고 있으면 '선국 중'에서 영원히 멈춘다 — 타임아웃을 건다.
constexpr uint16_t kApiTimeoutMs = 12000;

String httpGet(const char* url) {
    String body;

    WiFiClientSecure secure;
    secure.setInsecure();  // 방송사 CA 를 전부 넣고 다닐 수는 없다
    WiFiClient plain;

    const bool https = strncmp(url, "https://", 8) == 0;
    HTTPClient http;
    http.setTimeout(kApiTimeoutMs);
    http.setConnectTimeout(kApiTimeoutMs);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (!http.begin(https ? (WiFiClient&)secure : plain, url)) {
        RLOGE("HTTP begin 실패: %s", url);
        return body;
    }
    http.setUserAgent("Mozilla/5.0");

    const int code = http.GET();
    if (code == HTTP_CODE_OK) {
        body = http.getString();
    } else {
        RLOGE("API 응답 오류 %d: %s", code, url);
    }
    http.end();
    return body;
}

// {"service_url":"https://..."} 에서 값만 뽑는다.
// 채널 하나만 필요해서 JSON 파서를 끌어오지 않았다.
String extractServiceUrl(const String& json) {
    const char* key = "\"service_url\":\"";
    const int start = json.indexOf(key);
    if (start < 0) return String();
    const int from = start + strlen(key);
    const int end = json.indexOf('"', from);
    if (end < 0) return String();
    return json.substring(from, end);
}

}  // namespace

String resolveStreamUrl(const Station& s) {
    switch (s.type) {
        case STREAM_DIRECT:
            return String(s.url);

        case STREAM_KBS_API: {
            const String body = httpGet(s.url);
            if (body.isEmpty()) return String();
            const String url = extractServiceUrl(body);
            if (url.isEmpty()) RLOGE("KBS 응답에서 service_url 을 찾지 못함");
            return url;
        }

        case STREAM_TEXT_API: {
            String body = httpGet(s.url);
            body.trim();
            if (!body.startsWith("http")) {
                if (!body.isEmpty()) RLOGE("URL 이 아닌 응답: %.60s", body.c_str());
                return String();
            }
            // 혹시 개행 뒤에 뭐가 더 붙어 오면 첫 줄만 쓴다
            const int nl = body.indexOf('\n');
            return nl > 0 ? body.substring(0, nl) : body;
        }
    }
    return String();
}
