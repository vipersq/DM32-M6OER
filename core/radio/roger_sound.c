// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Roger sounds (roger_sound.h)
#include "radio/roger_sound.h"

#define FS		8000u
#define AMP		1300	// a level step: 5 of them, the default, about -14 dBFS through the vocoder
#define RAMP		16	// 2 ms fade in and out of each tone: no clicks

typedef struct {
	uint8_t n;
	roger_step_t s[ROGER_STEPS_MAX];
} sound_t;

// Morse at a 50 ms dot (about 24 words a minute, on the 25 ms grid)
static const sound_t sounds[ROGER_COUNT] = {
	[ROGER_CLASSIC] = { 2, { { 880, 100 }, { 660, 100 } } },
	[ROGER_BEEP] = { 1, { { 800, 150 } } },
	[ROGER_DOUBLE] = { 3, { { 800, 75 }, { 0, 50 }, { 800, 75 } } },
	[ROGER_UP] = { 2, { { 660, 75 }, { 880, 100 } } },
	[ROGER_DOWN] = { 2, { { 880, 75 }, { 660, 100 } } },
	[ROGER_CHIRP] = { 5, { { 1050, 50 }, { 0, 25 }, { 1050, 50 }, { 0, 25 }, { 1050, 50 } } },
	[ROGER_SWEEP_UP] = { 6, { { 500, 50 }, { 600, 50 }, { 700, 50 }, { 800, 50 }, { 900, 50 }, { 1000, 50 } } },
	[ROGER_SWEEP_DOWN] = { 6, { { 1000, 50 }, { 900, 50 }, { 800, 50 }, { 700, 50 }, { 600, 50 }, { 500, 50 } } },
	[ROGER_BEE_BOO] = { 3, { { 1000, 125 }, { 0, 25 }, { 700, 150 } } },
	[ROGER_MORSE_K] = { 5, { { 700, 150 }, { 0, 50 }, { 700, 50 }, { 0, 50 }, { 700, 150 } } },
	[ROGER_MORSE_R] = { 5, { { 700, 50 }, { 0, 50 }, { 700, 150 }, { 0, 50 }, { 700, 50 } } },
	[ROGER_QUINDAR] = { 1, { { 2475, 250 } } },
	[ROGER_CB_TWO_TONE] = { 2, { { 1000, 75 }, { 750, 150 } } },
	[ROGER_CB_TOOT] = { 1, { { 600, 200 } } },
	[ROGER_DING_DONG] = { 2, { { 700, 100 }, { 525, 225 } } },
	[ROGER_TRIPLE_LOW] = { 5, { { 600, 50 }, { 0, 25 }, { 600, 50 }, { 0, 25 }, { 600, 50 } } },
	[ROGER_ARPEGGIO] = { 3, { { 525, 75 }, { 650, 75 }, { 775, 125 } } },
};

uint8_t roger_steps(uint8_t sound, const roger_step_t **steps)
{
	const sound_t *s = &sounds[sound < ROGER_COUNT ? sound : ROGER_CLASSIC];
	*steps = s->s;
	return s->n;
}

uint32_t roger_length_ms(uint8_t sound)
{
	const roger_step_t *s;
	uint8_t n = roger_steps(sound, &s);
	uint32_t t = 0;
	for (uint8_t i = 0; i < n; i++) {
		t += s[i].ms;
	}
	return t;
}

int roger_step_at(uint8_t sound, uint32_t t_ms)
{
	const roger_step_t *s;
	uint8_t n = roger_steps(sound, &s);
	for (uint8_t i = 0; i < n; i++) {
		if (t_ms < s[i].ms) {
			return i;
		}
		t_ms -= s[i].ms;
	}
	return -1;
}

// sin(2 pi x / 256) in Q15, a quarter wave of 65 points
static const int16_t quarter[65] = {
	0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512, 10278, 11039, 11793,
	12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
	23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
	30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
	32767,
};

static int32_t sine(uint32_t phase)
{
	uint32_t x = phase >> 24;	// 0..255
	uint32_t q = x & 63u;
	int32_t v;
	switch (x >> 6) {
	case 0: v = quarter[q]; break;
	case 1: v = quarter[64 - q]; break;
	case 2: v = -quarter[q]; break;
	default: v = -quarter[64 - q]; break;
	}
	return v;
}

void roger_gen_start(roger_gen_t *g, uint8_t sound, uint8_t level)
{
	g->amp = AMP * (int32_t)(level >= 1u && level <= 10u ? level : ROGER_LEVEL_DEFAULT);
	g->n = roger_steps(sound, &g->s);
	g->i = 0;
	g->left = (uint16_t)(g->s[0].ms * (FS / 1000u));
	g->phase = 0;
	g->on = g->n != 0;
}

bool roger_gen_block(roger_gen_t *g, int16_t *pcm, unsigned count)
{
	for (unsigned k = 0; k < count; k++) {
		while (g->on && g->left == 0) {
			if (++g->i >= g->n) {
				g->on = false;
				break;
			}
			g->left = (uint16_t)(g->s[g->i].ms * (FS / 1000u));
			g->phase = 0;
		}
		if (!g->on) {
			pcm[k] = 0;
			continue;
		}
		const roger_step_t *st = &g->s[g->i];
		int32_t v = 0;
		if (st->hz != 0) {
			uint32_t len = (uint32_t)st->ms * (FS / 1000u), pos = len - g->left;
			int32_t a = g->amp;
			if (pos < RAMP) {
				a = a * (int32_t)pos / RAMP;
			} else if (g->left <= RAMP) {
				a = a * (int32_t)(g->left - 1u) / RAMP;
			}
			v = sine(g->phase) * a / 32767;
			g->phase += (uint32_t)(((uint64_t)st->hz << 32) / FS);
		}
		pcm[k] = (int16_t)v;
		g->left--;
	}
	return g->on;
}
