// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The mode menus' service side (CONFIG_C4FM builds, docs/spec/c4fm.md 6): FM's rows on the channel or VFO in use, and
// the C4FM stations heard (Menu > C4FM > Last heard). The menus themselves are in ui_menus.c.
#include "config.h"

#if CONFIG_C4FM
#include <string.h>
#include "ui_svc.h"
#include "ui_nav.h"
#include "ui_modes.h"
#include "radio/radio.h"
#include "radio/tones.h"
#include "hal_time.h"

// --- FM rows ---

// The tones_picker index of a tone (0 = none); a tone not on the lists: the nearest standard one above it
static int32_t tone_index(const subtone_t *t)
{
	if (t->type == TONE_NONE) {
		return 0;
	}
	for (uint16_t i = 1; i < TONES_PICKER_COUNT; i++) {
		subtone_t p = tones_picker(i);
		if (p.type == t->type && p.value == t->value) {
			return i;
		}
	}
	subtone_t n = tones_step(t, 1);
	for (uint16_t i = 1; i < TONES_PICKER_COUNT; i++) {
		subtone_t p = tones_picker(i);
		if (p.type == n.type && p.value == n.value) {
			return i;
		}
	}
	return 0;
}

int32_t ui_modes_setting_get(uint8_t id)
{
	const radio_view_t *v = ui_view();
	switch (id) {
	case UI_SET_FM_BCL:
	case UI_SET_FM_ROGER:
	case UI_SET_TAC_KEY:
	case UI_SET_TAC_UNKEY:
	case UI_SET_TAC_RX:
	case UI_SET_TAC_LEDS:
	case UI_SET_TAC_STYLE:
	case UI_SET_TAC_RXEND:
	case UI_SET_TONES_FM: case UI_SET_TONES_DMR: case UI_SET_TONES_C4FM: case UI_SET_TONES_P25: case UI_SET_TONES_NXDN:
	case UI_SET_ROGER_FM: case UI_SET_ROGER_DMR: case UI_SET_ROGER_C4FM:
	case UI_SET_DMR_BOTH_TS: case UI_SET_DMR_ANY_TG: case UI_SET_DMR_ANY_CC:
	case UI_SET_ROGER_TAIL: case UI_SET_TAC_LEVEL: case UI_SET_ROGER_LEVEL:
	case UI_SET_TAC_N_KEY: case UI_SET_TAC_N_UNKEY: case UI_SET_TAC_N_RX: case UI_SET_TAC_N_RXEND:
		return ui_svc_fm_get(id);
	case UI_SET_CH_WIDE:
		return v->ch.bw == RADIO_BW_25K;
	case UI_SET_CH_RX_TONE:
		return tone_index(&v->ch.rx_tone);
	case UI_SET_CH_TX_TONE:
		return tone_index(&v->ch.tx_tone);
	case UI_SET_VFO_OFFSET:
		return v->vfo && v->ch.tx_hz != 0 ? ((int32_t)v->ch.tx_hz - (int32_t)v->ch.rx_hz) / 1000 : 0;
	default:
		return 0;
	}
}

bool ui_modes_setting_set(uint8_t id, int32_t value)
{
	const radio_view_t *v = ui_view();
	switch (id) {
	case UI_SET_FM_BCL:
	case UI_SET_FM_ROGER:
	case UI_SET_TAC_KEY:
	case UI_SET_TAC_UNKEY:
	case UI_SET_TAC_RX:
	case UI_SET_TAC_LEDS:
	case UI_SET_TAC_STYLE:
	case UI_SET_TAC_RXEND:
	case UI_SET_TONES_FM: case UI_SET_TONES_DMR: case UI_SET_TONES_C4FM: case UI_SET_TONES_P25: case UI_SET_TONES_NXDN:
	case UI_SET_ROGER_FM: case UI_SET_ROGER_DMR: case UI_SET_ROGER_C4FM:
	case UI_SET_DMR_BOTH_TS: case UI_SET_DMR_ANY_TG: case UI_SET_DMR_ANY_CC:
	case UI_SET_ROGER_TAIL: case UI_SET_TAC_LEVEL: case UI_SET_ROGER_LEVEL:
	case UI_SET_TAC_N_KEY: case UI_SET_TAC_N_UNKEY: case UI_SET_TAC_N_RX: case UI_SET_TAC_N_RXEND:
		ui_svc_fm_set(id, value);
		return true;
	case UI_SET_CH_WIDE:
		radio_req_bandwidth(value ? RADIO_BW_25K : RADIO_BW_12K5);
		return true;
	case UI_SET_CH_RX_TONE:
	case UI_SET_CH_TX_TONE:
		if (value < 0 || value >= TONES_PICKER_COUNT) {
			return false;
		}
		radio_req_tone(id == UI_SET_CH_RX_TONE ? RADIO_TONE_RX : RADIO_TONE_TX, tones_picker((uint16_t)value));
		return true;
	case UI_SET_VFO_OFFSET:
		if (!v->vfo) {
			return false;	// a channel's offset is the codeplug's
		}
		radio_req_vfo_tx(value == 0 ? 0u : (uint32_t)((int32_t)v->ch.rx_hz + value * 1000));
		return true;
	default:
		return false;
	}
}

void ui_modes_tone_text(int32_t index, sbuf_t *b)
{
	subtone_t t = tones_picker((uint16_t)(index < 0 ? 0 : index));
	if (t.type == TONE_NONE) {
		sb_str(b, TR(VALUE_OFF, "Off"));
	} else if (t.type == TONE_CTCSS) {
		sb_u32(b, t.value / 10u, 1);
		sb_str(b, ".");
		sb_u32(b, t.value % 10u, 1);
		sb_str(b, " Hz");
	} else {
		char d[5] = { 'D', (char)('0' + ((t.value >> 6) & 7u)), (char)('0' + ((t.value >> 3) & 7u)),
			      (char)('0' + (t.value & 7u)), 0 };
		sb_str(b, d);
		sb_str(b, t.type == TONE_DCS_INV ? "I" : "N");
	}
}

// --- C4FM stations heard ---

static ui_c4fm_heard_t heard[UI_C4FM_HEARD_MAX];
static uint8_t heard_n;

void ui_modes_poll(const radio_view_t *v)
{
	const radio_c4fm_view_t *c = &v->c4fm;
	bool rx = c->active && (c->phase == RC4FM_RX_VOICE || c->phase == RC4FM_RX_BUSY);
	if (!rx || c->src[0] == '\0') {
		return;
	}
	// The station on air goes to the top; a new one pushes the oldest out when the list is full
	uint8_t at = heard_n;
	for (uint8_t i = 0; i < heard_n; i++) {
		if (strcmp(heard[i].call, c->src) == 0) {
			at = i;
			break;
		}
	}
	if (at == heard_n) {
		if (heard_n < UI_C4FM_HEARD_MAX) {
			heard_n++;
		}
		at = (uint8_t)(heard_n - 1u);
	}
	memmove(&heard[1], &heard[0], at * sizeof heard[0]);
	ui_c4fm_heard_t *h = &heard[0];
	memcpy(h->call, c->src, sizeof h->call);
	memcpy(h->id, c->src_id, sizeof h->id);
	h->dgid = c->rx_dgid;
	h->busy = c->phase == RC4FM_RX_BUSY;
	h->ms = hal_millis();
}

uint8_t ui_c4fm_heard_count(void)
{
	return heard_n;
}

const ui_c4fm_heard_t *ui_c4fm_heard(uint8_t i)
{
	return i < heard_n ? &heard[i] : NULL;
}
#endif
