/* This software is licensed under the MIT License: https://github.com/spacehuhntech/esp8266_deauther */

#pragma once

#include "A_config.h"

#if USE_DISPLAY

class DisplayUI;

extern DisplayUI displayUI;

#else /* USE_DISPLAY */

// Headless: no DisplayUI object — CLI screen commands are compile-time no-ops.

#endif /* USE_DISPLAY */
