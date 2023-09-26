#ifndef MY_UTILS_HPP
#define MY_UTILS_HPP

#include "color.h"

rgb_t mWheel(int color, uint8_t brig) {
    uint8_t r{}, g{}, b{};
    if (color <= 255) {           			  // красный макс, зелёный растёт
        r = 255;
        g = color;
        b = 0;
    }
    else if (color > 255 && color <= 510) {   // зелёный макс, падает красный
        r = 510 - color;
        g = 255;
        b = 0;
    }
    else if (color > 510 && color <= 765) {   // зелёный макс, растёт синий
        r = 0;
        g = 255;
        b = color - 510;
    }
    else if (color > 765 && color <= 1020) {  // синий макс, падает зелёный
        r = 0;
        g = 1020 - color;
        b = 255;
    }
    else if (color > 1020 && color <= 1275) { // синий макс, растёт красный
        r = color - 1020;
        g = 0;
        b = 255;
    }
    else if (color > 1275 && color <= 1530) { // красный макс, падает синий
        r = 255;
        g = 0;
        b = 1530 - color;
    }
    return  rgb_t{
        uint8_t(r & brig), uint8_t(g & brig), uint8_t(b & brig)
    };
}

size_t noteToPix(uint8_t note)
{
    static constexpr uint16_t noteToOffset[12] = {
        0,
        23 / 2,
        23 * 1,
        23 * 1 + 23 / 2,
        23 * 2,
        23 * 3,
        23 * 3 + 23 / 2,
        23 * 4,
        23 * 4 + 23 / 2,
        23 * 5,
        23 * 5 + 23 / 2,
        23 * 6
    };

    uint8_t step = note % 12;
    uint8_t octave = note / 12;
    uint16_t length = octave * 160;

    length += noteToOffset[step];
    return length / 7;
}

#endif