// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * DMR voice over the host port (CONFIG_KISS_VOICE, docs/spec/aprs.md 9.5): the host's microphone and speaker for the
 * radio's DMR channel, as KISS SetHardware 'V' frames of IMA ADPCM (adpcm.h), half duplex. The radio keeps the
 * vocoder: AMBE never crosses the port. Runs in the host link task; host tested (tests/host/kissVoiceTest.c).
 *
 * Host to radio, SetHardware 'V' then:
 *   'R' 1/0         receive audio on / off (off at power-on: a data-only host gets none)
 *   'K'             key: transmit on the channel's talk target with the host's audio
 *   'U'             unkey
 *   'A' frame       20 ms of transmit audio (ADPCM header and 80 bytes)
 * Radio to host:
 *   'A' frame       20 ms of the DMR call playing, while receive audio is on and we aren't transmitting
 *   'C' src dst g   a call started playing: IDs big endian u32, 1 group / 0 private, then the caller's name
 *   'E'             the call ended
 *   'T' on why own  the radio's transmitter: 1 on / 0 off; why a key was refused (KISS_V_*); 1 the host keyed it
 */
#ifndef RFOUNDRY_APRS_VOICE_H
#define RFOUNDRY_APRS_VOICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#define KISS_TAG_VOICE		'V'
#define KISS_VOICE_SAMPLES	160u	// 20 ms at 8 kHz
#define KISS_VOICE_FRAME	(3u + KISS_VOICE_SAMPLES / 2u)	// the ADPCM frame: 83 bytes
#define KISS_VOICE_REFRESH_MS	40u	// how often the pump reads the radio's state
#define KISS_VOICE_STALE_MS	1000u	// a host transmission whose audio stopped ends after this

// 'T' why: a key the radio refused
#define KISS_V_OK		0u
#define KISS_V_NOT_DMR		1u	// the channel isn't DMR
#define KISS_V_HOTSPOT		2u	// the radio is a hotspot
#define KISS_V_NO_CODEC		3u	// no vocoder: voice can't play or go out

#if CONFIG_KISS_VOICE
// A SetHardware 'V' frame from the host: p is what follows the tag
void aprs_voice_hw(const uint8_t *p, size_t n, uint32_t now_ms);
// Every pass of the host link task: receive audio, call and transmitter changes, the stale watchdog
void aprs_voice_pump(uint32_t now_ms);
bool aprs_voice_rx_on(void);
bool aprs_voice_tx_on(void);
#else
static inline void aprs_voice_pump(uint32_t now_ms) { (void)now_ms; }
#endif

#endif
