// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * IMA ADPCM (the IMA Digital Audio Focus and Technical Working Groups' recommended practice, 1992): 16-bit PCM in
 * 4 bits a sample, for voice over the 115200 baud host port (docs/spec/aprs.md 9.5). Hardware independent, host
 * tested (tests/host/adpcmTest.c).
 *
 * A frame is self-contained: a 3-byte header with the state at its start (predictor, little endian, then the step
 * index), then two samples a byte, the first in the low nibble. A lost frame costs only itself.
 */
#ifndef RFOUNDRY_ADPCM_H
#define RFOUNDRY_ADPCM_H

#include <stddef.h>
#include <stdint.h>

#define ADPCM_HEADER_LEN	3u

typedef struct {
	int16_t pred;		// the last sample decoded
	uint8_t index;		// into the step table, 0..88
} adpcm_state_t;

// n samples (even) into out, ADPCM_HEADER_LEN + n / 2 bytes; the state carries on to the next frame. Returns the length
size_t adpcm_encode(adpcm_state_t *st, const int16_t *pcm, size_t n, uint8_t *out);
// A frame of len bytes into at most max samples; returns how many (0: too short or a bad header)
size_t adpcm_decode(const uint8_t *in, size_t len, int16_t *pcm, size_t max);

#endif
