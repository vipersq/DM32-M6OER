// SPDX-License-Identifier: MIT
// The boot screen's picture (tools/splash_logo.py): SIZE x SIZE, 4 bits a pixel (high nibble left), each an index into the palette (RGB565)
#ifndef UI_SPLASH_LOGO_H
#define UI_SPLASH_LOGO_H
#include <stdint.h>
#define SPLASH_LOGO_SIZE	216
#define SPLASH_LOGO_COLORS	13
extern const uint16_t splash_logo_palette[SPLASH_LOGO_COLORS];
extern const uint8_t splash_logo_pixels[SPLASH_LOGO_SIZE * SPLASH_LOGO_SIZE / 2];
#endif
