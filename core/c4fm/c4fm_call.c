// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
// C4FM DN voice calls (c4fm_call.h)
#include "ysf_vw.h"
#include "dmr/ambe/ambeVoice.h"
#include "c4fm_call.h"

#include <stddef.h>
#include <string.h>

#include "c4fm/ysf.h"
#include "dmr/ambe/ambeFec.h"

static const hal_c4fm_t *modem;
static const dmr_voice_sink_t *sink;
static const dmr_voice_source_t *source;
static c4fm_config_t cfg;
static c4fm_call_view_t view;
static c4fm_raw_t raw;
static int16_t cap_samples[C4FM_CAP_SAMPLES];
static c4fm_capture_t cap = { .samples = cap_samples };
static c4fm_demod_t demod;
static bool running;
static bool ptt;
static uint32_t now;

// Receive
static bool in_call;		// the sink has a call open
static uint32_t last_frame_ms;
static uint8_t fn_next;		// FN expected next (a frame whose FICH failed is taken as this one)

// Transmit
static uint8_t tx_fn;
static uint8_t tx_ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS];
static uint8_t tx_have;		// voice frames pulled for the next communications frame
static bool tx_term;		// terminator queued

// Wires-X (docs/spec/c4fm.md 5.1)
static c4fm_wx_t wx = { .sent = 0xFF };
static uint8_t wx_rx[C4FM_WX_RX_MAX];	// a data FR transmission being received: FN 1.. of every block
static uint16_t wx_rx_len;		// bytes written so far
static bool wx_rx_on;
static uint8_t wx_rx_bn, wx_rx_blk;	// the last BN heard and the block count it means (BN has 2 bits: it wraps)
static bool wx_pending;			// a command asked for, sent once idle
static uint8_t wx_cmd;
static uint32_t wx_room;
static char wx_text[C4FM_WX_TEXT_LEN + 1];
static uint8_t wx_tx[C4FM_WX_DATA_MAX];
static uint16_t wx_tx_len;
static uint8_t wx_tx_fn, wx_tx_ft;	// next FN to send, the last

static void changed(void)
{
	view.serial++;
}

void c4fm_callsign_field(const char *call, uint8_t out[C4FM_CALLSIGN_LEN])
{
	unsigned i = 0;
	for (; call != NULL && call[i] != '\0' && i < C4FM_CALLSIGN_LEN; i++) {
		char c = call[i];
		out[i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c);
	}
	for (; i < C4FM_CALLSIGN_LEN; i++) {
		out[i] = ' ';
	}
}

void c4fm_radio_id(const uint32_t *seed, unsigned words, char out[C4FM_RADIO_ID_LEN + 1])
{
	uint32_t h = 2166136261u;	// FNV-1a over the seed's bytes
	for (unsigned w = 0; w < words; w++) {
		for (int b = 0; b < 4; b++) {
			h = (h ^ ((seed[w] >> (8 * b)) & 0xFFu)) * 16777619u;
		}
	}
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
	for (int i = C4FM_RADIO_ID_LEN - 1; i >= 0; i--) {
		out[i] = digits[h % 36u];
		h /= 36u;
	}
	if (memcmp(out, "00000", 5) == 0) {
		out[0] = 'A';	// "00000" is the engine's "none"
	}
	out[C4FM_RADIO_ID_LEN] = '\0';
}

// A received 10-byte field to text: printable characters only, trailing spaces trimmed
static void fieldText(const uint8_t *f, char out[C4FM_CALLSIGN_LEN + 1])
{
	int n = 0;
	for (int i = 0; i < C4FM_CALLSIGN_LEN; i++) {
		char c = (char)f[i];
		out[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
	}
	for (n = C4FM_CALLSIGN_LEN; n > 0 && out[n - 1] == ' '; n--) {
	}
	out[n] = '\0';
}

// --- receive ---

// The caller's radio ID: the destination "*****ID" (FN 0, the header) or the end of DCH FN 5, five of 0-9 A-Z
static void takeRadioId(const uint8_t *field, uint8_t fn)
{
	if (fn == 0 && memcmp(field, "*****", 5) != 0) {
		return;
	}
	char id[C4FM_RADIO_ID_LEN + 1];
	for (int i = 0; i < C4FM_RADIO_ID_LEN; i++) {
		char c = (char)field[5 + i];
		if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) {
			return;
		}
		id[i] = c;
	}
	id[C4FM_RADIO_ID_LEN] = '\0';
	if (strcmp(id, view.src_id) != 0) {
		memcpy(view.src_id, id, sizeof id);
		changed();
	}
}

static void callEnd(void)
{
	if (in_call && sink != NULL && sink->end != NULL) {
		sink->end(sink->ctx);
	}
	in_call = false;
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		view.phase = C4FM_PHASE_IDLE;
		changed();
	}
}

static void callBegin(bool play, bool late)
{
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		return;
	}
	view.src[0] = view.dst[0] = view.src_id[0] = '\0';
	view.late_entry = late;
	if (cap.state == C4FM_CAP_ARMED) {
		cap.state = C4FM_CAP_TRIGGERED;
		cap.post = C4FM_CAP_POST;
		cap.trigger_ms = now;
	}
	raw.hdr_ok = raw.term_ok = raw.dch_ok = 0;
	raw.serial++;
	view.rx_calls++;
	view.phase = play ? C4FM_PHASE_RX_VOICE : C4FM_PHASE_RX_BUSY;
	if (play && sink != NULL) {
		if (sink->begin != NULL) {
			sink->begin(NULL, sink->ctx);
		}
		in_call = true;
	}
	changed();
}

static void playAmbe(const uint8_t ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS])
{
	if (!in_call || sink == NULL) {
		return;
	}
	for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
		uint8_t f72[AMBE_FRAME_BYTES];
		ambeFecEncode(ambe[j], f72);
		sink->frame(f72, sink->ctx);
		view.rx_ambe++;
	}
}

// C4FM VW: the frame's full-rate words into the call (ambeVoiceImbeFrame); the FEC's corrections counted as DN's are
static void playImbe(const uint8_t *frame, const ysf_fich_t *f)
{
	if (!in_call) {
		return;
	}
	uint8_t words = ysfVwWords(f->fn, f->ft);
	for (unsigned j = 0; j < YSF_VW_WORDS; j++) {
		if ((words >> j) & 1u) {
			uint8_t imbe[IMBE_BYTES];
			uint8_t e = ysfVwDecode(frame, j, imbe);
			view.rx_vch_errors += IMBE_ERRS_TOTAL(e);
			ambeVoiceImbeFrame(imbe, e);
			view.rx_ambe++;
		}
	}
}

static void capFrame(const c4fm_frame_info_t *info, bool fich, const ysf_fich_t *f, uint8_t fec, uint8_t vch)
{
	if (cap.state == C4FM_CAP_FROZEN) {
		return;
	}
	c4fm_cap_frame_t *r = &cap.frames[cap.frame_head % C4FM_CAP_FRAMES];
	cap.frame_head++;
	r->ms = now;
	r->sample = cap.head;
	r->sync_errors = info->sync_errors;
	r->flags = (uint8_t)((fich ? 1u : 0u) | (info->flywheel ? 2u : 0u) | (info->inverted ? 4u : 0u));
	r->gain = info->gain;
	r->center = info->center;
	r->fi = fich ? f->fi : 0xFF;
	r->fn = fich ? f->fn : 0xFF;
	r->fec = fec;
	r->vch = vch;
}

// --- Wires-X ---

static const uint8_t wx_codes[4][3] = {
	{ 0x5D, 0x71, 0x5F },	// DX
	{ 0x5D, 0x23, 0x5F },	// CONNECT
	{ 0x5D, 0x2A, 0x5F },	// DISCONNECT
	{ 0x5D, 0x66, 0x5F },	// ALL
};

static uint8_t wxSum(const uint8_t *d, uint16_t n)
{
	uint8_t s = 0;
	for (uint16_t i = 0; i < n; i++) {
		s = (uint8_t)(s + d[i]);
	}
	return s;
}

// A command's data: a sequence byte (0), the three-byte command, 0x26, the parameters as ASCII, the end marker
// 0x03 and the byte sum of everything before it. As a node parses it (MMDVM_CM WiresX.cpp: the command at [1..3],
// the parameters from [5]: CONNECT the room ID in 6 characters, ALL "01" and the first entry "001"). Bytes [0] and
// [4] are not checked by a node; they follow its replies' layout (to compare with a Yaesu radio's capture)
uint16_t c4fm_wx_command(uint8_t cmd, uint32_t room, const char *text, uint8_t out[C4FM_WX_DATA_MAX])
{
	if (cmd > C4FM_WX_SEARCH) {
		return 0;
	}
	uint16_t n = 0;
	out[n++] = 0x00;
	memcpy(out + n, wx_codes[cmd == C4FM_WX_SEARCH ? C4FM_WX_ALL : cmd], 3);	// a search is ALL with "11"
	n += 3;
	out[n++] = 0x26;
	if (cmd == C4FM_WX_CONNECT) {
		room %= 100000u;
		for (int i = 4; i >= 0; i--) {
			out[n + (unsigned)i] = (uint8_t)('0' + room % 10u);
			room /= 10u;
		}
		n += 5;
		out[n++] = ' ';
	} else if (cmd == C4FM_WX_ALL) {
		uint32_t start = room == 0 ? 1u : room % 1000u;	// "01" and the first entry, 3 digits
		out[n++] = '0';
		out[n++] = '1';
		out[n++] = (uint8_t)('0' + start / 100u);
		out[n++] = (uint8_t)('0' + start / 10u % 10u);
		out[n++] = (uint8_t)('0' + start % 10u);
	} else if (cmd == C4FM_WX_SEARCH) {
		// "11", the first entry (3 digits), the text in 16 characters, space padded, upper case (MMDVM_CM
		// processAll: the search text at the parameters' [5..20])
		memcpy(out + n, "11001", 5);
		n += 5;
		for (unsigned i = 0; i < C4FM_WX_TEXT_LEN; i++) {
			char c = text != NULL && text[0] != '\0' ? text[0] : ' ';
			if (text != NULL && *text != '\0') {
				text++;
			}
			out[n++] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
		}
	}
	out[n++] = 0x03;
	out[n] = wxSum(out, n);
	return (uint16_t)(n + 1);
}

static void wxText(const uint8_t *src, unsigned n, char *out)
{
	unsigned k = 0;
	for (unsigned i = 0; i < n; i++) {
		char c = (char)src[i];
		out[i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
	}
	for (k = n; k > 0 && out[k - 1] == ' '; k--) {
	}
	out[k] = '\0';
}

// A node's reply (MMDVM_CM WiresX.cpp sendDXReply / sendConnectReply / sendDisconnectReply): [1..4] the reply code,
// [5..9] the node's ID, [10..19] its callsign, [20..33] its name, [34..35] "15" in a room ("12" not), [36..40] the
// room's ID, [41..56] its name; the end marker 0x03 then the byte sum
bool c4fm_wx_parse(const uint8_t *data, uint16_t len, c4fm_wx_t *w)
{
	// The end marker: the last 0x03 whose byte sum checks (the sum itself may be 0x03)
	int end = -1;
	for (int i = (int)len - 2; i >= 1; i--) {
		if (data[i] == 0x03 && wxSum(data, (uint16_t)(i + 1)) == data[i + 1]) {
			end = i;
			break;
		}
	}
	if (end < 0) {
		return false;
	}
	bool dx = data[1] == 0x5D && data[2] == 0x51 && data[3] == 0x5F;
	bool conn = data[1] == 0x5D && data[2] == 0x41 && data[3] == 0x5F;	// connect and disconnect alike
	bool all = data[1] == 0x5D && data[2] == 0x46 && data[3] == 0x5F;
	if (all && end >= 29) {
		// The room list (MMDVM_CM sendAllReply): [7..11] the node's ID, [12..21] its callsign, [22..24] rooms on this
		// page, [25..27] rooms in all, [28] CR; then 50 bytes a room: '5', [1..5] its ID, [6..21] its name,
		// [22..24] its users, [35..48] a description, [49] CR. A search's result ([5] '0'): [22] '1' and the count in
		// [23..24] ("01" at [5..6] and no rooms: nothing found)
		bool search = data[5] == '0';
		unsigned n = search ? (unsigned)(data[23] - '0') * 10u + (unsigned)(data[24] - '0')
				    : (unsigned)(data[22] - '0') * 100u + (unsigned)(data[23] - '0') * 10u + (unsigned)(data[24] - '0');
		w->search = search;
		unsigned total = (unsigned)(data[25] - '0') * 100u + (unsigned)(data[26] - '0') * 10u + (unsigned)(data[27] - '0');
		if (n > C4FM_WX_ROOMS) {
			n = C4FM_WX_ROOMS;
		}
		unsigned k = 0;
		for (unsigned j = 0; j < n; j++) {
			unsigned o = 29u + j * 50u;
			if (o + 50u > (unsigned)end) {
				break;
			}
			wxText(data + o + 1, 5, w->room[k].id);
			wxText(data + o + 6, 16, w->room[k].name);
			unsigned u = 0;
			for (unsigned i = 22; i < 25; i++) {
				u = data[o + i] >= '0' && data[o + i] <= '9' ? u * 10u + (unsigned)(data[o + i] - '0') : u;
			}
			w->room[k].users = (uint8_t)(u > 255 ? 255 : u);
			k++;
		}
		w->rooms = (uint8_t)k;
		w->rooms_total = (uint16_t)(total > 999 ? 999 : total);
		w->kind = C4FM_WX_ALL;
		w->rooms_serial++;
		wxText(data + 7, 5, w->node_id);
		wxText(data + 12, 10, w->node_call);
		return true;
	}
	if ((!dx && !conn) || end < 57) {
		return false;
	}
	w->kind = dx ? C4FM_WX_DX : (data[34] == '1' && data[35] == '5') ? C4FM_WX_CONNECT : C4FM_WX_DISCONNECT;
	wxText(data + 5, 5, w->node_id);
	wxText(data + 10, 10, w->node_call);
	wxText(data + 20, 14, w->node_name);
	w->connected = data[34] == '1' && data[35] == '5';
	if (w->connected) {
		wxText(data + 36, 5, w->room_id);
		wxText(data + 41, 16, w->room_name);
	} else {
		w->room_id[0] = w->room_name[0] = '\0';
	}
	return true;
}

// A data FR transmission: FN 1 carries CSD3 and data, FN 2-7 40 bytes each; a long reply goes on in further blocks
// (BN), whose FN 1 starts with a 0x00 before its 19 bytes (MMDVM_CM createReply). Block 0 holds 260 bytes, each next
// 259. Complete at the terminator, or at FN = FT of a block that isn't full
static void wxReceive(const uint8_t *frame, const c4fm_frame_info_t *info, const ysf_fich_t *f, bool soft)
{
	if (f->fi == YSF_FI_HEADER) {
		wx_rx_on = true;
		wx_rx_len = 0;
		wx_rx_bn = 0;
		wx_rx_blk = 0;
		memset(wx_rx, 0, sizeof wx_rx);
		return;
	}
	// The block: counted as BN moves on, since a reply of five blocks or more wraps BN back to 0
	if (f->fi == YSF_FI_COMMS && f->bn != wx_rx_bn) {
		wx_rx_blk = (uint8_t)(wx_rx_blk + ((f->bn - wx_rx_bn) & 3u));
		wx_rx_bn = f->bn;
	}
	bool done = f->fi == YSF_FI_TERMINATOR;
	if (f->fi == YSF_FI_COMMS && f->fn != 0) {
		uint8_t d1[20], d2[20];
		unsigned ok = soft && info->soft != NULL ? ysfCsdDecodeSoft(info->soft, d1, d2) : ysfCsdDecode(frame, d1, d2);
		unsigned blk = wx_rx_blk;
		unsigned base = blk == 0 ? 0u : 260u + (blk - 1u) * 259u;
		wx_rx_on = true;
		if (f->fn == 1) {
			if ((ok & 1u) && blk == 0) {
				memcpy(wx.csd3, d1, sizeof wx.csd3);
			}
			unsigned skip = blk == 0 ? 0u : 1u, n = 20u - skip;
			if (base + n <= sizeof wx_rx) {
				memcpy(wx_rx + base, d2 + skip, n);
				if (base + n > wx_rx_len) {
					wx_rx_len = (uint16_t)(base + n);
				}
			}
		} else {
			unsigned off = base + (blk == 0 ? 20u : 19u) + (unsigned)(f->fn - 2) * 40u;
			if (off + 40u <= sizeof wx_rx) {
				memcpy(wx_rx + off, d1, 20);
				memcpy(wx_rx + off + 20, d2, 20);
				if (off + 40u > wx_rx_len) {
					wx_rx_len = (uint16_t)(off + 40u);
				}
			}
		}
		done = f->fn == f->ft && f->ft < 7;
	}
	if (!done || !wx_rx_on || wx_rx_len == 0) {
		return;
	}
	wx_rx_on = false;
	uint16_t len = wx_rx_len;
	wx.data_len = len < C4FM_WX_DATA_MAX ? len : C4FM_WX_DATA_MAX;
	memcpy(wx.data, wx_rx, wx.data_len);
	static c4fm_wx_t parsed;	// static: about 900 bytes, more than the radio task's stack spares
	parsed = wx;
	if (c4fm_wx_parse(wx_rx, len, &parsed)) {
		parsed.serial = wx.serial + 1;
		parsed.reply_ms = now != 0 ? now : 1;
		wx = parsed;
	} else {
		wx.serial++;	// raw data captured (a command heard, or a reply we don't parse)
	}
	changed();
}

static uint8_t frame_vch;	// the frame's VCH triplets corrected (capFrame)

static void onFrameBody(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, ysf_fich_t *f_out,
			bool *fich_out, uint8_t *fec_out)
{
	ysf_fich_t f;
	ysf_fec_info_t fec;
	bool soft = info->soft != NULL;	// soft decisions: the hard ones lost half the FICHs through a repeater
	bool fich = soft ? ysfFichDecodeSoft(info->soft, &f, &fec) : ysfFichDecode(frame, &f, &fec);
	*fich_out = fich;
	*fec_out = fich ? fec.errors : 0;
	*f_out = f;
	if (fich) {
		view.rx_fich_fec += fec.errors;
		view.rx_fich_ok++;
		raw.fich = f;
	}
	view.rx_gain = info->gain;
	view.rx_center = info->center;
	view.rx_inverted = info->inverted;
	view.rx_tone = c4fmDemodTone(&demod);
	if (!fich) {
		view.rx_fich_bad++;
		if (view.phase != C4FM_PHASE_RX_VOICE || info->flywheel) {
			return;		// nothing to go on: no call open, or a frame without a sync (likely the end)
		}
		// In a call: take it as the communications frame due (its voice has FEC of its own)
		memset(&f, 0, sizeof f);
		f.fi = YSF_FI_COMMS;
		f.dt = YSF_DT_VD2;
		f.fn = fn_next;
		f.ft = C4FM_FT;
		f.dgid = view.dgid;
	}
	last_frame_ms = now;
	view.rx_frames++;
	fn_next = (uint8_t)(f.fn >= f.ft ? 0 : f.fn + 1);

	bool ours = cfg.dgid == 0 || f.dgid == cfg.dgid;
	bool dn = f.dt == YSF_DT_VD2;
	bool vw = f.dt == YSF_DT_VOICE_FR;	// full rate: played with core/mbe (ysf_vw.h)
	if (f.dt == YSF_DT_DATA_FR && fich) {
		wxReceive(frame, info, &f, soft);
	}

	switch (f.fi) {
	case YSF_FI_HEADER: {
		callEnd();	// a header always starts a new call
		view.dgid = f.dgid;
		view.dt = f.dt;
		callBegin(ours && (dn || vw), false);
		raw.hdr_ok = (uint8_t)(soft ? ysfCsdDecodeSoft(info->soft, raw.hdr_csd1, raw.hdr_csd2)
					     : ysfCsdDecode(frame, raw.hdr_csd1, raw.hdr_csd2));
		raw.serial++;
		if ((raw.hdr_ok & 1u) != 0) {
			takeRadioId(raw.hdr_csd1, 0);
			fieldText(raw.hdr_csd1, view.dst);
			fieldText(raw.hdr_csd1 + 10, view.src);
			changed();
		}
		break;
	}
	case YSF_FI_COMMS: {
		if (view.phase != C4FM_PHASE_RX_VOICE && view.phase != C4FM_PHASE_RX_BUSY) {
			view.dgid = f.dgid;
			view.dt = f.dt;
			callBegin(ours && (dn || vw), true);
		}
		if (f.dt != view.dt) {
			view.dt = f.dt;
			changed();
		}
		if (vw) {
			playImbe(frame, &f);
			break;
		}
		if (!dn) {
			break;
		}
		uint8_t dch[YSF_CALLSIGN_LEN], ambe[YSF_VCH_PER_FRAME][YSF_AMBE_BITS], errs[YSF_VCH_PER_FRAME];
		bool dch_ok = soft ? ysfVd2DecodeSoft(info->soft, dch, ambe, errs) : ysfVd2Decode(frame, dch, ambe, errs);
		for (int j = 0; j < YSF_VCH_PER_FRAME; j++) {
			view.rx_vch_errors += errs[j];
			frame_vch = (uint8_t)(frame_vch + errs[j]);
		}
		if (dch_ok && (f.fn == 0 || f.fn == 5)) {
			takeRadioId(dch, f.fn);
		}
		if (dch_ok && f.fn < 8) {
			memcpy(raw.dch[f.fn], dch, YSF_CALLSIGN_LEN);
			raw.dch_ok |= (uint8_t)(1u << f.fn);
			raw.serial++;
		}
		if (dch_ok && (f.fn == 0 || f.fn == 1)) {
			char *dst = f.fn == 1 ? view.src : view.dst;
			char text[C4FM_CALLSIGN_LEN + 1];
			fieldText(dch, text);
			if (strcmp(text, dst) != 0) {
				memcpy(dst, text, sizeof text);
				changed();
			}
		}
		playAmbe((const uint8_t (*)[YSF_AMBE_BITS])ambe);
		break;
	}
	case YSF_FI_TERMINATOR:
		raw.term_ok = (uint8_t)(soft ? ysfCsdDecodeSoft(info->soft, raw.term_csd1, raw.term_csd2)
					      : ysfCsdDecode(frame, raw.term_csd1, raw.term_csd2));
		raw.serial++;
		callEnd();
		break;
	default:
		break;
	}
}

static void onFrame(const uint8_t frame[YSF_FRAME_BYTES], const c4fm_frame_info_t *info, void *ctx)
{
	(void)ctx;
	ysf_fich_t f;
	bool fich = false;
	uint8_t fec = 0;
	frame_vch = 0;
	onFrameBody(frame, info, &f, &fich, &fec);
	capFrame(info, fich, &f, fec, frame_vch);
}

static void onLost(void *ctx)
{
	(void)ctx;
	callEnd();
}

// The input ring (bench capture): written before the receiver sees the block, so a frame's record points past it
static void capSamples(const int16_t *buf, uint16_t n)
{
	for (uint16_t i = 0; i < n && cap.state != C4FM_CAP_FROZEN; i++) {
		cap.samples[cap.head] = buf[i];
		cap.head = (uint16_t)(cap.head + 1u >= C4FM_CAP_SAMPLES ? 0 : cap.head + 1u);
		if (cap.state == C4FM_CAP_TRIGGERED && --cap.post == 0) {
			cap.state = C4FM_CAP_FROZEN;
			cap.frozen_ms = now;
			cap.seq++;
		}
	}
}

static void rxService(void)
{
	if (modem == NULL || modem->rx_read == NULL) {
		return;
	}
	int16_t buf[128];
	uint16_t n;
	while ((n = modem->rx_read(buf, sizeof buf / sizeof buf[0])) > 0) {
		capSamples(buf, n);
		c4fmDemodFeed(&demod, buf, n);
	}
	if (view.phase == C4FM_PHASE_RX_VOICE || view.phase == C4FM_PHASE_RX_BUSY) {
		if (now - last_frame_ms > C4FM_RX_HANG_MS) {
			callEnd();
		}
	}
	view.dsp_syncs = demod.syncs;
	view.dsp_losts = demod.losts;
	view.dsp_flywheels = demod.flywheels;
}

// --- transmit ---

static void fichFor(uint8_t fi, uint8_t fn, ysf_fich_t *f)
{
	memset(f, 0, sizeof *f);
	f->fi = fi;
	f->cs = 2;
	f->cm = cfg.plain ? YSF_CM_GROUP1 : YSF_CM_GROUP2;	// an FT-70D: CM 1, destination "*****" + radio ID
	f->fn = fn;
	f->ft = C4FM_FT;
	f->mr = YSF_MR_DIRECT;
	f->dt = YSF_DT_VD2;
	f->dgid = cfg.dgid;
}

// The fields as an FT-70D fills them (measured on air, docs/spec/c4fm.md 3): the destination is five asterisks and
// the radio ID, the radio ID again at the end of DCH FN 5, and FN 6 a fixed data block (meaning unknown: copied)
static const uint8_t ft70d_fn6[YSF_CALLSIGN_LEN] = { 0x01, 0x22, 0x61, 0x5F, 0x2B, 0x03, 0x11, 0x00, 0x00, 0x00 };

static void radioId(uint8_t out[C4FM_RADIO_ID_LEN])
{
	for (int i = 0; i < C4FM_RADIO_ID_LEN; i++) {
		out[i] = cfg.radio_id[0] != '\0' ? (uint8_t)cfg.radio_id[i] : '0';
	}
}

static void destField(uint8_t out[YSF_CALLSIGN_LEN])
{
	if (cfg.plain) {
		c4fm_callsign_field("ALL", out);
		return;
	}
	memset(out, '*', 5);
	radioId(out + 5);
}

static void csdFrame(uint8_t fi, uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t csd1[20], csd2[20];
	destField(csd1);
	c4fm_callsign_field(cfg.callsign, csd1 + 10);
	memset(csd2, ' ', sizeof csd2);
	ysf_fich_t f;
	fichFor(fi, 0, &f);
	memset(frame, 0, YSF_FRAME_BYTES);
	ysfAddSync(frame);
	ysfFichEncode(&f, frame);
	ysfCsdEncode(csd1, csd2, frame);
}

// DCH content by FN: destination, source, downlink, uplink, remarks 1-2 (blank), remarks 3-4 (blank, radio ID), data
static void dchFor(uint8_t fn, uint8_t dch[YSF_CALLSIGN_LEN])
{
	switch (fn) {
	case 0:
		destField(dch);
		break;
	case 1:
		c4fm_callsign_field(cfg.callsign, dch);
		break;
	case 5:
		memset(dch, ' ', YSF_CALLSIGN_LEN);
		if (!cfg.plain) {
			radioId(dch + 5);
		}
		break;
	case 6:
		if (cfg.plain) {
			memset(dch, 0, YSF_CALLSIGN_LEN);
		} else {
			memcpy(dch, ft70d_fn6, YSF_CALLSIGN_LEN);
		}
		break;
	case 7:
		memset(dch, 0, YSF_CALLSIGN_LEN);
		break;
	default:
		memset(dch, ' ', YSF_CALLSIGN_LEN);
		break;
	}
}

static bool txWrite(const uint8_t *b, uint16_t n)
{
	return modem->tx_write(b, n);
}

static void pullVoice(void)
{
	while (tx_have < YSF_VCH_PER_FRAME && source != NULL) {
		uint8_t f72[AMBE_FRAME_BYTES];
		if (!source->next(f72, source->ctx)) {
			break;
		}
		ambeFecDecode(f72, tx_ambe[tx_have], NULL);
		tx_have++;
	}
}

static void commsFrame(void)
{
	for (; tx_have < YSF_VCH_PER_FRAME; tx_have++) {
		memcpy(tx_ambe[tx_have], ysf_ambe_silence, YSF_AMBE_BITS);
		view.tx_padded++;
	}
	uint8_t frame[YSF_FRAME_BYTES], dch[YSF_CALLSIGN_LEN];
	ysf_fich_t f;
	fichFor(YSF_FI_COMMS, tx_fn, &f);
	memset(frame, 0, sizeof frame);
	ysfAddSync(frame);
	ysfFichEncode(&f, frame);
	dchFor(tx_fn, dch);
	ysfVd2Encode(dch, (const uint8_t (*)[YSF_AMBE_BITS])tx_ambe, frame);
	txWrite(frame, sizeof frame);
	view.tx_frames++;
	tx_have = 0;
	tx_fn = (uint8_t)(tx_fn >= C4FM_FT ? 0 : tx_fn + 1);
}

// A data FR frame of the command being sent: FI, FN; header, FN 0 and terminator carry CSD1 / CSD2, FN 1 CSD3 and
// data [0..19], FN 2.. 40 bytes each (space padded)
static void wxFrame(uint8_t fi, uint8_t fn, uint8_t frame[YSF_FRAME_BYTES])
{
	uint8_t c1[20], c2[20];
	if (fi != YSF_FI_COMMS || fn == 0) {
		destField(c1);
		c4fm_callsign_field(cfg.callsign, c1 + 10);
		memset(c2, ' ', sizeof c2);
	} else if (fn == 1) {
		memset(c1, ' ', sizeof c1);	// CSD3: remarks 1-4, our radio ID last (as DCH FN 4-5 in DN)
		if (!cfg.plain) {
			radioId(c1 + 15);
		}
		memset(c2, ' ', sizeof c2);
		memcpy(c2, wx_tx, wx_tx_len < 20 ? wx_tx_len : 20);
	} else {
		unsigned off = (unsigned)(fn - 2) * 40u + 20u;
		memset(c1, ' ', sizeof c1);
		memset(c2, ' ', sizeof c2);
		for (unsigned i = 0; i < 20; i++) {
			if (off + i < wx_tx_len) {
				c1[i] = wx_tx[off + i];
			}
			if (off + 20 + i < wx_tx_len) {
				c2[i] = wx_tx[off + 20 + i];
			}
		}
	}
	ysf_fich_t f;
	fichFor(fi, fn, &f);
	f.dt = YSF_DT_DATA_FR;
	f.ft = wx_tx_ft;
	memset(frame, 0, YSF_FRAME_BYTES);
	ysfAddSync(frame);
	ysfFichEncode(&f, frame);
	ysfCsdEncode(c1, c2, frame);
}

static void wxTxBegin(void)
{
	wx_pending = false;
	callEnd();
	wx_tx_len = c4fm_wx_command(wx_cmd, wx_room, wx_text, wx_tx);
	if (modem == NULL || cfg.callsign[0] == '\0' || wx_tx_len == 0 || !modem->tx_start()) {
		view.phase = C4FM_PHASE_TX_DENIED;
		changed();
		return;
	}
	// FT: FN 1 holds 20 bytes, each next FN 40
	wx_tx_ft = 1;
	while (20u + (unsigned)(wx_tx_ft - 1) * 40u < wx_tx_len && wx_tx_ft < 7) {
		wx_tx_ft++;
	}
	uint8_t pre[C4FM_PREAMBLE_BYTES];
	memset(pre, 0x77, sizeof pre);
	for (uint16_t ms = 0; ms < cfg.preamble_ms; ms += 100) {
		txWrite(pre, sizeof pre);
	}
	uint8_t frame[YSF_FRAME_BYTES];
	wxFrame(YSF_FI_HEADER, 0, frame);
	txWrite(frame, sizeof frame);
	wx_tx_fn = 0;
	tx_term = false;
	wx.sent = wx_cmd;
	wx.sent_ms = now != 0 ? now : 1;
	if (wx_cmd == C4FM_WX_ALL || wx_cmd == C4FM_WX_SEARCH) {
		wx.rooms_start = (uint16_t)(wx_room == 0 || wx_cmd == C4FM_WX_SEARCH ? 1u : wx_room);
	}
	view.phase = C4FM_PHASE_TX_DATA;
	changed();
}

static void wxTxService(void)
{
	uint8_t frame[YSF_FRAME_BYTES];
	if (wx_tx_fn <= wx_tx_ft) {
		if (modem->tx_space() >= YSF_FRAME_BYTES) {
			wxFrame(YSF_FI_COMMS, wx_tx_fn, frame);
			txWrite(frame, sizeof frame);
			wx_tx_fn++;
		}
		return;
	}
	if (!tx_term) {
		if (modem->tx_space() >= YSF_FRAME_BYTES) {
			wxFrame(YSF_FI_TERMINATOR, 0, frame);
			txWrite(frame, sizeof frame);
			tx_term = true;
		}
		return;
	}
	if (modem->tx_queued() == 0) {
		modem->tx_stop();
		c4fmDemodReset(&demod);
		view.phase = C4FM_PHASE_IDLE;
		changed();
	}
}

static void txBegin(void)
{
	callEnd();
	if (modem == NULL || cfg.callsign[0] == '\0' || !modem->tx_start()) {
		view.phase = C4FM_PHASE_TX_DENIED;
		changed();
		return;
	}
	uint8_t pre[C4FM_PREAMBLE_BYTES];
	memset(pre, 0x77, sizeof pre);
	for (uint16_t ms = 0; ms < cfg.preamble_ms; ms += 100) {
		txWrite(pre, sizeof pre);
	}
	uint8_t frame[YSF_FRAME_BYTES];
	csdFrame(YSF_FI_HEADER, frame);
	txWrite(frame, sizeof frame);
	tx_fn = 0;
	tx_have = 0;
	tx_term = false;
	if (source != NULL && source->begin != NULL) {
		source->begin(source->ctx);
	}
	view.tx_calls++;
	view.phase = C4FM_PHASE_TX_VOICE;
	changed();
}

static void txService(void)
{
	if (view.phase == C4FM_PHASE_TX_DATA) {
		wxTxService();
		return;
	}
	if (view.phase == C4FM_PHASE_TX_VOICE) {
		pullVoice();
		// A whole frame of voice, or the air about to run out: send. One frame per poll at most
		if (modem->tx_space() >= YSF_FRAME_BYTES &&
		    (tx_have == YSF_VCH_PER_FRAME || modem->tx_queued() < C4FM_TX_LOW_BYTES)) {
			commsFrame();
		}
		if (!ptt) {
			if (source != NULL && source->end != NULL) {
				source->end(source->ctx);
			}
			view.phase = C4FM_PHASE_TX_ENDING;
			changed();
		}
		return;
	}
	if (view.phase == C4FM_PHASE_TX_ENDING) {
		if (!tx_term) {
			if (tx_have > 0 && modem->tx_space() >= YSF_FRAME_BYTES) {
				commsFrame();	// what voice is left, padded
			}
			if (tx_have == 0 && modem->tx_space() >= YSF_FRAME_BYTES) {
				uint8_t frame[YSF_FRAME_BYTES];
				csdFrame(YSF_FI_TERMINATOR, frame);
				txWrite(frame, sizeof frame);
				tx_term = true;
			}
			return;
		}
		if (modem->tx_queued() == 0) {
			modem->tx_stop();
			c4fmDemodReset(&demod);
			view.phase = C4FM_PHASE_IDLE;
			changed();
		}
	}
}

// --- API ---

void c4fm_call_init(const hal_c4fm_t *m)
{
	modem = m;
	memset(&view, 0, sizeof view);
	memset(&raw, 0, sizeof raw);
	memset(&wx, 0, sizeof wx);
	wx.sent = 0xFF;
	wx_pending = false;
	c4fmDemodInit(&demod, onFrame, onLost, NULL);
	c4fm_call_set_equalizer(true);
	c4fm_call_set_tone_filter(true);
	running = false;
	ptt = false;
	in_call = false;
}

void c4fm_call_set_voice(const dmr_voice_sink_t *s, const dmr_voice_source_t *src)
{
	sink = s;
	source = src;
}

void c4fm_call_configure(const c4fm_config_t *c)
{
	uint8_t field[C4FM_CALLSIGN_LEN];
	cfg.dgid = c->dgid > C4FM_DGID_MAX ? 0 : c->dgid;
	c4fm_callsign_field(c->callsign, field);
	fieldText(field, cfg.callsign);
	// Only a well-formed ID (five of 0-9 A-Z); anything else sends "00000"
	cfg.radio_id[0] = '\0';
	bool ok = strlen(c->radio_id) == C4FM_RADIO_ID_LEN;
	for (int i = 0; ok && i < C4FM_RADIO_ID_LEN; i++) {
		char ch = c->radio_id[i];
		ok = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z');
	}
	if (ok) {
		memcpy(cfg.radio_id, c->radio_id, sizeof cfg.radio_id);
	}
	cfg.preamble_ms = c->preamble_ms == 0 ? 100 : c->preamble_ms > C4FM_PREAMBLE_MAX_MS ? C4FM_PREAMBLE_MAX_MS
		: (uint16_t)((c->preamble_ms + 99) / 100 * 100);
	cfg.plain = c->plain;
}

void c4fm_call_set_tone_filter(bool on)
{
	c4fmDemodSetToneFilter(&demod, on);
	view.rx_tone_on = on;
	view.rx_tone = c4fmDemodTone(&demod);
}

// The equalizer's shape (bench, tests/host/c4fmReplay --eq on SR5WI captures): 7 taps a symbol apart; mu 2^-6 of the
// normalized step settles in a few frames and halves the voice bit errors from a repeater
#define EQ_TAPS		7u
#define EQ_STEP		5u
#define EQ_MU		6u

void c4fm_call_set_equalizer(bool on)
{
	c4fmDemodSetEq(&demod, on ? EQ_TAPS : 0, EQ_STEP, EQ_MU);
	view.rx_eq = on;
}

bool c4fm_call_start(void)
{
	if (running) {
		return true;
	}
	if (modem == NULL || modem->rx_start == NULL || !modem->rx_start()) {
		return false;
	}
	c4fmDemodReset(&demod);
	running = true;
	return true;
}

void c4fm_call_stop(void)
{
	if (!running) {
		return;
	}
	callEnd();
	if (c4fm_call_transmitting()) {
		if (source != NULL && source->end != NULL) {
			source->end(source->ctx);
		}
		modem->tx_stop();
	}
	if (modem->rx_stop != NULL) {
		modem->rx_stop();
	}
	view.phase = C4FM_PHASE_IDLE;
	ptt = false;
	running = false;
	changed();
}

bool c4fm_call_running(void)
{
	return running;
}

void c4fm_call_ptt(bool down)
{
	ptt = down;
}

bool c4fm_call_transmitting(void)
{
	return view.phase == C4FM_PHASE_TX_VOICE || view.phase == C4FM_PHASE_TX_ENDING || view.phase == C4FM_PHASE_TX_DATA;
}

bool c4fm_call_wx(uint8_t cmd, uint32_t room)
{
	if (!running || cfg.callsign[0] == '\0' || cmd > C4FM_WX_SEARCH || c4fm_call_transmitting()) {
		return false;
	}
	wx_cmd = cmd;
	wx_room = room;
	wx_pending = true;
	return true;
}

void c4fm_call_wx_text(const char *text)
{
	unsigned i = 0;
	for (; text != NULL && text[i] != '\0' && i < C4FM_WX_TEXT_LEN; i++) {
		wx_text[i] = text[i];
	}
	wx_text[i] = '\0';
}

const c4fm_wx_t *c4fm_call_wx_state(void)
{
	return &wx;
}

void c4fm_call_poll(uint32_t now_ms)
{
	now = now_ms;
	if (!running) {
		return;
	}
	bool can_start = view.phase == C4FM_PHASE_IDLE || view.phase == C4FM_PHASE_RX_VOICE ||
			 view.phase == C4FM_PHASE_RX_BUSY;
	if (ptt && can_start) {
		txBegin();
	} else if (wx_pending && can_start) {
		wxTxBegin();
	}
	if (!ptt && !wx_pending && view.phase == C4FM_PHASE_TX_DENIED) {
		view.phase = C4FM_PHASE_IDLE;
		changed();
	}
	if (c4fm_call_transmitting()) {
		txService();
	} else {
		rxService();
	}
}

const c4fm_call_view_t *c4fm_call_view(void)
{
	return &view;
}

const c4fm_raw_t *c4fm_call_raw(void)
{
	return &raw;
}

const c4fm_capture_t *c4fm_call_capture(void)
{
	return &cap;
}

void c4fm_call_capture_ctl(uint8_t cmd)
{
	if (cmd == 2) {
		if (cap.state != C4FM_CAP_FROZEN) {
			cap.state = C4FM_CAP_FROZEN;
			cap.frozen_ms = now;
			cap.trigger_ms = now;
			cap.seq++;
		}
	} else {
		cap.state = cmd == 1 ? C4FM_CAP_ARMED : C4FM_CAP_RUN;
	}
}

const c4fm_config_t *c4fm_call_config(void)
{
	return &cfg;
}
