// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM VW (voice full rate, DT 3): the payload carries the full-rate vocoder's 144-bit code words, the same words with
// the same bit order as a P25 Phase 1 LDU's voice frames (TIA-102.BAAA 5.3; RadioFoundry core/p25/p25_unit.c, checked
// bit for bit against the YSF layout). Five words a frame at payload bytes 0, 18, 36, 54, 72; the first frame after the
// header (FN 0 of FT 1) carries data in its first 45 bytes and voice in the last two words only
#ifndef C4FM_YSF_VW_H
#define C4FM_YSF_VW_H

#include <stdint.h>
#include <stdbool.h>
#include "../mbe/imbe.h"

#define YSF_VW_WORDS		5

// The voice words a frame carries: a bit mask of the five (bit n = word n)
uint8_t ysfVwWords(uint8_t fn, uint8_t ft);
// Word n (0..4) of a frame (120 bytes, sync first) to the 88 vocoder bits; returns what the codes corrected, packed as
// IMBE_ERRS (never refuses: IMBE_ERRS_REPEAT says when not to trust it)
uint8_t ysfVwDecode(const uint8_t *frame, unsigned n, uint8_t imbe[IMBE_BYTES]);

#endif
