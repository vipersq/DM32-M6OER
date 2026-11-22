// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// IMA ADPCM (adpcm.h)
#include "config.h"
#if CONFIG_KISS_VOICE || defined(ADPCM_TEST)	// the whole file
#include "aprs/adpcm.h"

static const int16_t step_tab[89] = {
	7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
	107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
	876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
	5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
	27086, 29794, 32767,
};

static const int8_t index_tab[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

// One code into the state: the sample it stands for
static int16_t step(adpcm_state_t *st, uint8_t code)
{
	int32_t s = step_tab[st->index];
	int32_t diff = s >> 3;
	if (code & 4u) {
		diff += s;
	}
	if (code & 2u) {
		diff += s >> 1;
	}
	if (code & 1u) {
		diff += s >> 2;
	}
	int32_t p = st->pred + ((code & 8u) ? -diff : diff);
	if (p > 32767) {
		p = 32767;
	} else if (p < -32768) {
		p = -32768;
	}
	st->pred = (int16_t)p;
	int32_t i = (int32_t)st->index + index_tab[code & 7u];
	st->index = (uint8_t)(i < 0 ? 0 : i > 88 ? 88 : i);
	return st->pred;
}

static uint8_t encode1(adpcm_state_t *st, int16_t x)
{
	int32_t s = step_tab[st->index];
	int32_t d = (int32_t)x - st->pred;
	uint8_t code = 0;
	if (d < 0) {
		code = 8u;
		d = -d;
	}
	if (d >= s) {
		code |= 4u;
		d -= s;
	}
	s >>= 1;
	if (d >= s) {
		code |= 2u;
		d -= s;
	}
	s >>= 1;
	if (d >= s) {
		code |= 1u;
	}
	(void)step(st, code);	// the decoder's state, so both stay in step
	return code;
}

size_t adpcm_encode(adpcm_state_t *st, const int16_t *pcm, size_t n, uint8_t *out)
{
	out[0] = (uint8_t)((uint16_t)st->pred & 0xFFu);
	out[1] = (uint8_t)((uint16_t)st->pred >> 8);
	out[2] = st->index;
	size_t k = ADPCM_HEADER_LEN;
	for (size_t i = 0; i < n; i += 2u) {
		uint8_t lo = encode1(st, pcm[i]);
		uint8_t hi = (i + 1u < n) ? encode1(st, pcm[i + 1u]) : 0u;
		out[k++] = (uint8_t)(lo | (uint8_t)(hi << 4));
	}
	return k;
}

size_t adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, size_t max)
{
	if (len < ADPCM_HEADER_LEN || in[2] > 88u) {
		return 0;
	}
	adpcm_state_t st = { .pred = (int16_t)(uint16_t)(in[0] | ((uint16_t)in[1] << 8)), .index = in[2] };
	size_t n = 0;
	for (size_t k = ADPCM_HEADER_LEN; k < len && n + 2u <= max; k++) {
		pcm[n++] = step(&st, in[k] & 0x0Fu);
		pcm[n++] = step(&st, in[k] >> 4);
	}
	return n;
}

#endif	// CONFIG_KISS_VOICE
