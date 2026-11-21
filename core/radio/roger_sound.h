// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Roger sounds: the end-of-transmission tone, all but Quindar below 1.1 kHz (a mellow sound on FM and through the
// vocoder), one table for every mode. FM sends it as tones on the carrier
// (board.radio->tx_tone, stepped by the radio core's poll); DMR and C4FM send it through the AMBE+2 encoder in place
// of the microphone (ambeVoiceTxTone). Steps are on a 25 ms grid, the radio core's poll period, so FM plays them as
// written. Pure data and a sample generator: no RTOS, no board
#ifndef RADIO_ROGER_SOUND_H
#define RADIO_ROGER_SOUND_H

#include <stdint.h>
#include <stdbool.h>

#define ROGER_STEPS_MAX	8u
#define ROGER_GRID_MS	25u

typedef enum {
	ROGER_CLASSIC,		// 1200 Hz then 900 Hz, 100 ms each: the radio's original roger beep
	ROGER_BEEP,		// one short beep
	ROGER_DOUBLE,		// two short beeps
	ROGER_UP,		// two tones, rising
	ROGER_DOWN,		// two tones, falling
	ROGER_CHIRP,		// three high chirps
	ROGER_SWEEP_UP,		// a rising sweep
	ROGER_SWEEP_DOWN,	// a falling sweep
	ROGER_BEE_BOO,		// high, then low
	ROGER_MORSE_K,		// K (-.-) in Morse, "over"
	ROGER_MORSE_R,		// R (.-.) in Morse, "received"
	ROGER_QUINDAR,		// one 250 ms tone at 2475 Hz, the Apollo-era release tone
	ROGER_CB_TWO_TONE,	// a CB rig's "dee-doo": a short high then a longer low tone, the 1980s-90s mobile kind
	ROGER_CB_TOOT,		// one low round toot, a CB power mic's
	ROGER_DING_DONG,	// a soft doorbell fall
	ROGER_TRIPLE_LOW,	// three low pips
	ROGER_ARPEGGIO,		// three rising notes (C E G)
	ROGER_COUNT
} roger_sound_t;

typedef struct {
	uint16_t hz;		// 0 = a pause
	uint16_t ms;		// a multiple of ROGER_GRID_MS
} roger_step_t;

// The sound's steps (a sound past the table plays as ROGER_CLASSIC); the count
uint8_t roger_steps(uint8_t sound, const roger_step_t **steps);
// The sound's whole length in ms
uint32_t roger_length_ms(uint8_t sound);
// The step playing t ms into the sound, or -1 once it is over
int roger_step_at(uint8_t sound, uint32_t t_ms);

// A generator of the sound as 8 kHz PCM, block by block (the AMBE encoder's input)
typedef struct {
	const roger_step_t *s;
	uint8_t n, i;		// steps, the one playing
	uint16_t left;		// samples left in step i
	uint32_t phase;		// Q32 phase accumulator
	bool on;
} roger_gen_t;

void roger_gen_start(roger_gen_t *g, uint8_t sound);
// Fill count samples (silence once over); false once the sound has ended (the block is silence then)
bool roger_gen_block(roger_gen_t *g, int16_t *pcm, unsigned count);

#endif
