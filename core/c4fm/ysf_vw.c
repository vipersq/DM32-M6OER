// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM VW voice words (ysf_vw.h). The code word map and the PN masks are RadioFoundry's P25 voice frame code
// (core/p25/p25_unit.c), on a word of 144 contiguous bits
#include <string.h>
#include "ysf.h"
#include "ysf_vw.h"
#include "../fec/golay.h"
#include "../fec/hamming.h"

// Where each bit of the 72 dibits comes from: code word in bits 7-5, its bit number in bits 4-0
#define V(c, i)		(uint8_t)(((c) << 5) | (i))
static const uint8_t VOICE_MAP[144] = {
	V(0, 22), V(1, 21), V(2, 20), V(3, 19), V(4, 10), V(5, 1), V(1, 20), V(0, 21), V(3, 18), V(2, 19), V(5, 0), V(4, 9),
	V(0, 20), V(1, 19), V(2, 18), V(3, 17), V(4, 8), V(6, 14), V(1, 18), V(0, 19), V(3, 16), V(2, 17), V(6, 13), V(4, 7),
	V(0, 18), V(1, 17), V(2, 16), V(3, 15), V(4, 6), V(6, 12), V(1, 16), V(0, 17), V(3, 14), V(2, 15), V(6, 11), V(4, 5),
	V(0, 16), V(1, 15), V(2, 14), V(3, 13), V(4, 4), V(6, 10), V(1, 14), V(0, 15), V(3, 12), V(2, 13), V(6, 9), V(4, 3),
	V(0, 14), V(1, 13), V(2, 12), V(3, 11), V(4, 2), V(6, 8), V(1, 12), V(0, 13), V(3, 10), V(2, 11), V(6, 7), V(4, 1),
	V(0, 12), V(1, 11), V(2, 10), V(3, 9), V(4, 0), V(6, 6), V(1, 10), V(0, 11), V(3, 8), V(2, 9), V(6, 5), V(5, 14),
	V(0, 10), V(1, 9), V(2, 8), V(3, 7), V(5, 13), V(6, 4), V(1, 8), V(0, 9), V(3, 6), V(2, 7), V(6, 3), V(5, 12),
	V(0, 8), V(1, 7), V(2, 6), V(3, 5), V(5, 11), V(6, 2), V(1, 6), V(0, 7), V(3, 4), V(2, 5), V(6, 1), V(5, 10),
	V(0, 6), V(1, 5), V(2, 4), V(3, 3), V(5, 9), V(6, 0), V(1, 4), V(0, 5), V(3, 2), V(2, 3), V(7, 6), V(5, 8),
	V(0, 4), V(1, 3), V(2, 2), V(3, 1), V(5, 7), V(7, 5), V(1, 2), V(0, 3), V(3, 0), V(2, 1), V(7, 4), V(5, 6),
	V(0, 2), V(1, 1), V(2, 0), V(4, 14), V(5, 5), V(7, 3), V(1, 0), V(0, 1), V(4, 13), V(3, 22), V(7, 2), V(5, 4),
	V(0, 0), V(2, 22), V(3, 21), V(4, 12), V(5, 3), V(7, 1), V(2, 21), V(1, 22), V(4, 11), V(3, 20), V(7, 0), V(5, 2),
};

// The PN masks m_1..m_6 from u_0: 114 values of p = 173 p + 13849 mod 65536 from p0 = 16 u_0, bit 15 of each
static void voice_masks(uint16_t u0, uint32_t m[8])
{
	static const uint8_t LEN[8] = { 0, 23, 23, 23, 15, 15, 15, 0 };
	uint16_t p = (uint16_t)(16 * u0);

	for (uint8_t c = 0; c < 8; c++) {
		m[c] = 0;
		for (uint8_t i = 0; i < LEN[c]; i++) {
			p = (uint16_t)(173u * p + 13849u);
			m[c] = (m[c] << 1) | (p >> 15);
		}
	}
}

uint8_t ysfVwWords(uint8_t fn, uint8_t ft)
{
	return fn == 0 && ft == 1 ? 0x18u : 0x1Fu;
}

uint8_t ysfVwDecode(const uint8_t *frame, unsigned n, uint8_t imbe[IMBE_BYTES])
{
	uint32_t c[8] = { 0 }, m[8];
	uint16_t u[8];
	unsigned at = (YSF_PAYLOAD_OFFSET + 18u * n) * 8u, o = 0;
	int errs, e0, e4 = 0;

	for (unsigned i = 0; i < 144; i++)
		c[VOICE_MAP[i] >> 5] |= (uint32_t)ysfGetBit(frame, at + i) << (VOICE_MAP[i] & 31);
	errs = e0 = fec_golay23_decode(c[0], &u[0]);
	voice_masks(u[0], m);
	for (uint8_t k = 1; k < 4; k++)
		errs += fec_golay23_decode(c[k] ^ m[k], &u[k]);
	for (uint8_t k = 4; k < 7; k++) {
		uint16_t cw = (uint16_t)(c[k] ^ m[k]), was = cw;

		fec_hamming15_correct(&cw);
		errs += cw != was;
		if (k == 4)
			e4 = cw != was;
		u[k] = cw >> 4;
	}
	u[7] = (uint16_t)c[7];
	memset(imbe, 0, IMBE_BYTES);
	for (uint8_t k = 0; k < 8; k++) {
		uint8_t len = k < 4 ? 12 : k < 7 ? 11 : 7;

		for (uint8_t b = 0; b < len; b++, o++)
			if ((u[k] >> (len - 1 - b)) & 1)
				imbe[o >> 3] |= (uint8_t)(0x80u >> (o & 7));
	}
	if (errs > 15)
		errs = 15;
	if (e0 > 3)
		e0 = 3;
	return IMBE_ERRS(errs, e0, e4);
}
