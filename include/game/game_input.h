#pragma once

#include <cstdint>

enum PAD_ID : uint8_t
{
    PAD_ID_BEGIN = 0x0,
    PAD_ID_1CON = 0x0,
    PAD_ID_2CON = 0x1,
    PAD_ID_3CON = 0x2,
    PAD_ID_4CON = 0x3,
    PAD_ID_MAX = 0x4,
    PAD_ID_CPU = 0x4,
    PAD_ID_MAX_WITH_CPU = 0x5,
    PAD_ID_ANY = 0x6,
    PAD_ID_INVALID = 0x7,
};
