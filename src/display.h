#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

#ifdef BOARD_CLUE

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_PURPLE  0xF81F
#define COLOR_GRAY    0x8410

class DisplayModule {
public:
    DisplayModule();
    void init();
    void fill(uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawText(int x, int y, const char *str, uint16_t fg, uint16_t bg);
    void drawText(int x, int y, const char *str, uint16_t fg);
    void setBacklight(bool on);

private:
    void writeCommand(uint8_t cmd);
    void writeCommand(uint8_t cmd, const uint8_t *data, int len);
    void writePixel(int x, int y, uint16_t color);
    void setAddrWindow(int x, int y, int w, int h);
    void select();
    void deselect();
    void dcCommand();
    void dcData();
};

#else

class DisplayModule {
public:
    DisplayModule() {}
    void init() {}
    void fill(uint16_t) {}
    void fillRect(int, int, int, int, uint16_t) {}
    void drawText(int, int, const char *, uint16_t, uint16_t) {}
    void drawText(int, int, const char *, uint16_t) {}
    void setBacklight(bool) {}
};

#endif

#endif
