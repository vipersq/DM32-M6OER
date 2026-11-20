// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM VW (core/c4fm/ysf_vw.c): a full-rate word put into a frame as a transmitter would (the same map and masks,
// encoded here) decodes to its 88 bits, up to the codes' limits of errors; the words a frame carries; and a decoded
// frame plays through the full-rate decoder (core/mbe)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../core/c4fm/ysf_vw.c"
#include "../../core/mbe/imbe.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void put_word(uint8_t *frame, unsigned n, const uint8_t imbe[IMBE_BYTES])
{
	uint32_t c[8], m[8];
	uint16_t u[8];
	unsigned o = 0, at = (YSF_PAYLOAD_OFFSET + 18u * n) * 8u;
	for (uint8_t k = 0; k < 8; k++) {
		uint8_t len = k < 4 ? 12 : k < 7 ? 11 : 7;
		u[k] = 0;
		for (uint8_t b = 0; b < len; b++, o++)
			u[k] = (uint16_t)((u[k] << 1) | ((imbe[o >> 3] >> (7 - (o & 7))) & 1));
	}
	voice_masks(u[0], m);
	c[0] = fec_golay23_encode(u[0]);
	for (uint8_t k = 1; k < 4; k++)
		c[k] = fec_golay23_encode(u[k]) ^ m[k];
	for (uint8_t k = 4; k < 7; k++)
		c[k] = fec_hamming15_encode(u[k]) ^ m[k];
	c[7] = u[7];
	for (unsigned i = 0; i < 144; i++)
		ysfPutBit(frame, at + i, (int)((c[VOICE_MAP[i] >> 5] >> (VOICE_MAP[i] & 31)) & 1));
}

int main(void)
{
	uint8_t frame[YSF_FRAME_BYTES], in[IMBE_BYTES], out[IMBE_BYTES];
	srand(7);
	CHECK(ysfVwWords(0, 1) == 0x18 && ysfVwWords(1, 1) == 0x1F && ysfVwWords(0, 0) == 0x1F && ysfVwWords(3, 6) == 0x1F);
	for (int round = 0; round < 200; round++) {
		memset(frame, 0, sizeof frame);
		for (int i = 0; i < IMBE_BYTES; i++)
			in[i] = (uint8_t)rand();
		in[IMBE_BYTES - 1] &= 0xFE;	// 88 bits: the last byte's low bit is not one
		unsigned n = (unsigned)round % YSF_VW_WORDS;
		put_word(frame, n, in);
		uint8_t e = ysfVwDecode(frame, n, out);
		CHECK(memcmp(in, out, IMBE_BYTES) == 0 && e == 0);
		// one error in each Golay word and each Hamming word: all corrected
		static const uint8_t pos_of_word[7] = { 0, 1, 2, 3, 4, 5, 6 };
		for (int w = 0; w < 7; w++) {
			for (unsigned i = 0; i < 144; i++) {
				if ((VOICE_MAP[i] >> 5) == pos_of_word[w]) {
					unsigned b = (YSF_PAYLOAD_OFFSET + 18u * n) * 8u + i;
					ysfPutBit(frame, b, !ysfGetBit(frame, b));
					break;
				}
			}
		}
		e = ysfVwDecode(frame, n, out);
		CHECK(memcmp(in, out, IMBE_BYTES) == 0 && IMBE_ERRS_TOTAL(e) == 7 && IMBE_ERRS_C0(e) == 1 &&
		      IMBE_ERRS_C4(e) == 1 && !IMBE_ERRS_REPEAT(e));
	}
	// Through the full-rate decoder: a silence word decodes to quiet audio, a coded voiced frame to sound
	static imbe_dec_state_t st;
	static mbe_params_t p;
	static mbe_state_t ms;
	static mbe_frame_t prev, cur;
	static int32_t buf[MBE_BUF_WORDS];
	const imbe_mem_t mem = { &st, &p, { &ms, &prev, &cur, buf } };
	int16_t pcm[MBE_FRAME_LEN];
	imbe_dec_init(&mem);
	memset(frame, 0, sizeof frame);
	put_word(frame, 2, imbe_silence);
	CHECK(ysfVwDecode(frame, 2, out) == 0 && memcmp(out, imbe_silence, IMBE_BYTES) == 0);
	int peak = 0;
	for (int f = 0; f < 5; f++) {
		CHECK(imbe_dec_frame(&mem, out, 0, pcm) == 0);
		for (int i = 0; i < MBE_FRAME_LEN; i++)
			peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
	}
	CHECK(peak < 200);
	printf("c4fmVwTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
