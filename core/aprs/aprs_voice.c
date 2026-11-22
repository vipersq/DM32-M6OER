// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR voice over the host port (aprs_voice.h)
#include "config.h"
#if CONFIG_KISS_VOICE	// the whole file
#include <string.h>
#include "aprs/aprs_voice.h"
#include "aprs/adpcm.h"
#include "aprs/aprs.h"
#include "hostlink/hostlink_view.h"
#include "radio/radio.h"
#include "usbaudio/audio_mux.h"

// Static, not on the host link task's stack
static struct {
	bool rx_on;		// the host wants receive audio
	bool tx_on;		// the host keyed
	bool tx_seen;		// the transmitter was on at the last refresh
	bool call;		// a call was playing at the last refresh
	uint32_t refresh_ms;
	uint32_t audio_ms;	// the host's last transmit audio
	uint16_t fill;		// receive samples gathered
	adpcm_state_t enc;
	int16_t pcm[KISS_VOICE_SAMPLES];
	uint8_t out[1u + 1u + KISS_VOICE_FRAME];
} v;

static void refresh(uint32_t now_ms)
{
	radio_get_view(&hostlink_view);
	v.refresh_ms = now_ms;
}

static void emit_tx(bool on, uint8_t why)
{
	uint8_t f[5] = { KISS_TAG_VOICE, 'T', on ? 1u : 0u, why, v.tx_on ? 1u : 0u };
	(void)aprs_host_emit_hw(f, sizeof f);
}

static void put32(uint8_t *p, uint32_t x)
{
	p[0] = (uint8_t)(x >> 24);
	p[1] = (uint8_t)(x >> 16);
	p[2] = (uint8_t)(x >> 8);
	p[3] = (uint8_t)x;
}

static void emit_call(const radio_dmr_call_t *c)
{
	uint8_t f[2 + 9 + CODEPLUG_NAME_LEN];
	f[0] = KISS_TAG_VOICE;
	f[1] = 'C';
	put32(f + 2, c->src);
	put32(f + 6, c->dst);
	f[10] = c->group ? 1u : 0u;
	size_t n = strnlen(c->src_name, CODEPLUG_NAME_LEN);
	memcpy(f + 11, c->src_name, n);
	(void)aprs_host_emit_hw(f, 11u + n);
}

static uint8_t key_refusal(void)
{
	if (hostlink_view.hotspot) {
		return KISS_V_HOTSPOT;
	}
	if (hostlink_view.ch.mode != RADIO_MODE_DMR || !hostlink_view.dmr.active) {
		return KISS_V_NOT_DMR;
	}
	if (!hostlink_view.dmr.codec) {
		return KISS_V_NO_CODEC;
	}
	return KISS_V_OK;
}

static void unkey(void)
{
	if (v.tx_on) {
		v.tx_on = false;
		audio_mux_host_tx(false, false);
		radio_req_ptt_host(false);
	}
}

static void key(uint32_t now_ms)
{
	refresh(now_ms);
	uint8_t why = key_refusal();
	if (why != KISS_V_OK) {
		emit_tx(false, why);
		return;
	}
	if (v.tx_on) {
		return;
	}
	v.tx_on = true;
	v.audio_ms = now_ms;
	audio_mux_host_tx(true, true);	// the host's samples replace the microphone's on the way to the encoder
	radio_req_ptt_host(true);
}

void aprs_voice_hw(const uint8_t *p, size_t n, uint32_t now_ms)
{
	if (n == 0) {
		return;
	}
	switch (p[0]) {
	case 'R':
		v.rx_on = n >= 2 && p[1] != 0;
		v.fill = 0;
		audio_mux_host_rx(v.rx_on ? AUDIO_MUX_RX_OUTPUT : AUDIO_MUX_RX_OFF);
		break;
	case 'K':
		key(now_ms);
		break;
	case 'U':
		unkey();
		break;
	case 'A':
		if (v.tx_on) {
			size_t k = adpcm_decode(p + 1, n - 1u, v.pcm, KISS_VOICE_SAMPLES);
			if (k != 0) {
				audio_mux_host_push(v.pcm, (uint32_t)k);
				v.audio_ms = now_ms;
			}
		}
		break;
	default:
		break;
	}
}

static void rx_audio(bool playing)
{
	for (;;) {
		uint32_t k = audio_mux_host_read(v.pcm + v.fill, KISS_VOICE_SAMPLES - v.fill);
		if (k == 0) {
			return;
		}
		v.fill = (uint16_t)(v.fill + k);
		if (v.fill < KISS_VOICE_SAMPLES) {
			continue;
		}
		v.fill = 0;
		if (playing && !v.tx_on) {
			v.out[0] = KISS_TAG_VOICE;
			v.out[1] = 'A';
			size_t len = adpcm_encode(&v.enc, v.pcm, KISS_VOICE_SAMPLES, v.out + 2);
			(void)aprs_host_emit_hw(v.out, 2u + len);	// a full ring drops it: the host hears a gap
		}
	}
}

void aprs_voice_pump(uint32_t now_ms)
{
	if (!v.rx_on && !v.tx_on && !v.tx_seen && !v.call) {
		return;		// no session: not even the view is read
	}
	if ((now_ms - v.refresh_ms) >= KISS_VOICE_REFRESH_MS) {
		refresh(now_ms);
		bool tx = hostlink_view.state == RS_TX;
		if (tx != v.tx_seen) {
			v.tx_seen = tx;
			emit_tx(tx, KISS_V_OK);		// the radio's own PTT too: the host shows it
		}
		bool call = hostlink_view.dmr.state == RDMR_RX;
		if (call != v.call) {
			v.call = call;
			if (call) {
				emit_call(&hostlink_view.dmr.call);
			} else {
				uint8_t f[2] = { KISS_TAG_VOICE, 'E' };
				(void)aprs_host_emit_hw(f, sizeof f);
				v.enc.pred = 0;
				v.enc.index = 0;
			}
		}
	}
	if (v.tx_on && (now_ms - v.audio_ms) >= KISS_VOICE_STALE_MS) {
		unkey();	// the host went quiet (cable pulled, app killed): never a stuck transmitter
	}
	if (v.rx_on) {
		rx_audio(v.call);
	}
}

bool aprs_voice_rx_on(void)
{
	return v.rx_on;
}

bool aprs_voice_tx_on(void)
{
	return v.tx_on;
}

#endif	// CONFIG_KISS_VOICE
