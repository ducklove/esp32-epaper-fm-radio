// 제품 UI의 좌표와 실제 비트맵 글꼴을 호스트에서 렌더링한다.
#pragma once
#include "Arduino.h"
#include "Adafruit_GFX.h"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <glcdfont.c>
constexpr int GFX_NOT_DEFINED = -1, FSPI = 0;
class Arduino_DataBus {};
class Arduino_ESP32SPI : public Arduino_DataBus {
public: template<typename... T> Arduino_ESP32SPI(T...) {}
};
class Arduino_GFX {
    const GFXfont* face = nullptr;
    int size = 1, cx = 0, cy = 0;
    uint16_t ink = 0;
    std::ostringstream commands;
    template<typename... T> void emit(const char* op, T... args) {
        commands << op; ((commands << ' ' << args), ...); commands << '\n';
    }
public:
    virtual ~Arduino_GFX() = default;
    bool begin(uint32_t) { return true; }
    void setFont(const GFXfont* f) { face = f; }
    void setTextSize(uint8_t s) { size = s; }
    void setTextColor(uint16_t c) { ink = c; }
    void setCursor(int16_t x, int16_t y) { cx = x; cy = y; }
    void getTextBounds(const char* s, int x, int y, int16_t* bx, int16_t* by, uint16_t* w, uint16_t* h) {
        int loX = 32767, loY = 32767, hiX = -32768, hiY = -32768;
        for (; *s; ++s) {
            const unsigned char c = *s;
            if (!face) { loX = std::min(loX, x); loY = y; hiX = x + 6 * size - 1; hiY = y + 8 * size - 1; x += 6 * size; }
            else if (c >= face->first && c <= face->last) {
                const auto& g = face->glyph[c - face->first];
                if (g.width && g.height) {
                    loX = std::min(loX, x + g.xOffset * size); loY = std::min(loY, y + g.yOffset * size);
                    hiX = std::max(hiX, x + (g.xOffset + g.width) * size - 1);
                    hiY = std::max(hiY, y + (g.yOffset + g.height) * size - 1);
                }
                x += g.xAdvance * size;
            }
        }
        *bx = hiX >= loX ? loX : 0; *by = hiY >= loY ? loY : 0;
        *w = hiX >= loX ? hiX - loX + 1 : 0; *h = hiY >= loY ? hiY - loY + 1 : 0;
    }
    void print(const String& s) {
        for (unsigned char c : s) {
            if (!face) {
                for (int x = 0; x < 5; ++x) for (int y = 0; y < 8; ++y)
                    if (font[c * 5 + x] & (1 << y)) fillRect(cx + x * size, cy + y * size, size, size, ink);
                cx += 6 * size;
            } else if (c >= face->first && c <= face->last) {
                const auto& g = face->glyph[c - face->first];
                for (int y = 0; y < g.height; ++y) for (int x = 0; x < g.width; ++x) {
                    const int bit = y * g.width + x;
                    if (face->bitmap[g.bitmapOffset + bit / 8] & (0x80 >> (bit % 8)))
                        fillRect(cx + (x + g.xOffset) * size, cy + (y + g.yOffset) * size, size, size, ink);
                }
                cx += g.xAdvance * size;
            }
        }
    }
    void fillRoundRect(int x,int y,int w,int h,int r,uint16_t c) { if(w>0 && h>0) emit("round",x,y,w,h,r,c); }
    void drawRoundRect(int x,int y,int w,int h,int r,uint16_t c) { emit("outline",x,y,w,h,r,c); }
    void fillRect(int x,int y,int w,int h,uint16_t c) { if(w>0 && h>0) emit("rect",x,y,w,h,c); }
    void drawRect(int x,int y,int w,int h,uint16_t c) { drawRoundRect(x,y,w,h,0,c); }
    void drawLine(int x,int y,int x2,int y2,uint16_t c) { emit("line",x,y,x2,y2,c); }
    void drawFastHLine(int x,int y,int w,uint16_t c) { fillRect(x,y,w,1,c); }
    void drawFastVLine(int x,int y,int h,uint16_t c) { fillRect(x,y,1,h,c); }
    void fillCircle(int x,int y,int r,uint16_t c) { emit("circle",x,y,r,c); }
    void drawCircle(int x,int y,int r,uint16_t c) { emit("ring",x,y,r,c); }
    void fillTriangle(int x,int y,int x2,int y2,int x3,int y3,uint16_t c) { emit("triangle",x,y,x2,y2,x3,y3,c); }
    void fillScreen(uint16_t c) { commands.str(""); commands.clear(); fillRect(0,0,480,320,c); }
    void flush() {}
    void save(const char* path) { std::ofstream(path) << commands.str(); }
};
class Arduino_ST7796 : public Arduino_GFX {
public: template<typename... T> Arduino_ST7796(T...) {}
};
class Arduino_Canvas : public Arduino_GFX {
public: template<typename... T> Arduino_Canvas(T...) {}
};
