#include "../src/boards/p4/ui.cpp"
void hwBacklight(uint8_t) {}
int main() {
    uiBegin();
    UiState s;
    s.freq = 93.1f; s.name = "KBS Classic FM"; s.index = 2;
    s.state = ST_PLAYING; s.bitrate = 192000; s.volume = 12;
    s.hasTime = true; s.hour = 21; s.minute = 48; s.wifi = true; s.wifiBars = 3; s.vbus = true;
    uiRender(s); gfx->save(".pio/preview/radio.draw");
    page = Page::MENU; uiRender(s); gfx->save(".pio/preview/stations.draw");
    page = Page::SOUND; s.tone = 1; s.sleepMinutes = 30; s.sleepSeconds = 1439;
    s.meters.left = 168; s.meters.right = 142; s.meters.peakLeft = 198; s.meters.peakRight = 176;
    for (int i = 0; i < kSpectrumBands; ++i) s.meters.bands[i] = 40 + (i * 71 + 43) % 190;
    uiRender(s); gfx->save(".pio/preview/sound.draw");
    page = Page::RADIO; s.freq = 107.7f; s.name = "SBS Power FM"; s.index = 14;
    s.paused = true; s.state = ST_PAUSED; s.battery = true; s.battPercent = 100;
    uiRender(s); gfx->save(".pio/preview/paused.draw");
    s.state = ST_ERROR; s.detail = "Connection failed. Please check your Wi-Fi network and retry.";
    uiRender(s); gfx->save(".pio/preview/error.draw");
    s.apSsid = "ESP32-Radio"; s.apPass = "radio1234"; s.apUrl = "http://192.168.4.1"; s.detail = "";
    uiRenderWifiSetup(s); gfx->save(".pio/preview/wifi.draw");
}
