// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * C4FM voice calls in DN mode (V/D mode 2) on one frequency: what the radio core runs on a C4FM channel.
 *
 * Identity is a callsign (the radio name's callsign), not a numeric ID: there are no contacts and no talk groups. The
 * one selector is the DG-ID (0..99): 0 hears every transmission and sends with DG-ID 0, as Yaesu radios do; another
 * value sends with it and hears only transmissions that carry it.
 *
 * Transmit: preamble, a header frame (CSD: ALL / our callsign), communications frames of five AMBE+2 frames each with
 * the DCH cycling through destination, source, downlink, uplink and data (FT 6: seven frames), then a terminator.
 * The modem is kept between one and two frames ahead of the air; a frame whose voice isn't all there yet when the
 * modem runs low is padded with silence, so the stream never breaks.
 *
 * Receive: discriminator audio -> c4fm_dsp -> frames. A header or the first decodable communications frame (late
 * entry) starts a call; its callsign comes from the header's CSD1 or from the DCH of FN 1. The terminator, a lost
 * signal or C4FM_RX_HANG_MS without a frame ends it. Voice other than DN (VW full rate, data) is shown but not played:
 * the radio's AMBE+2 codec only speaks the half rate frame DN carries.
 *
 * The voice sink and source are the DMR stack's (ambeVoice: 72-bit AMBE+2 frames); the engine converts the 49
 * parameter bits with ambeFec, which is lossless both ways. No RTOS calls: poll() does everything.
 */
#ifndef RFOUNDRY_C4FM_CALL_H
#define RFOUNDRY_C4FM_CALL_H

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "hal_c4fm.h"
#include "dmr/call/dmr_call.h"	// dmr_voice_sink_t / dmr_voice_source_t: the voice path is shared with DMR
#include "c4fm/c4fm_dsp.h"
#include "c4fm/ysf.h"

#define C4FM_CALLSIGN_LEN	10
#define C4FM_RADIO_ID_LEN	5	// Yaesu's radio ID: five characters 0-9 A-Z
#define C4FM_DGID_MAX		99
#define C4FM_RX_HANG_MS		400	// no frame for this long ends a call that had no terminator
#define C4FM_PREAMBLE_BYTES	120	// 100 ms of 0x77 before the header (receivers lock on it): the default
#define C4FM_PREAMBLE_MAX_MS	500	// longest preamble (600 bytes: with the header it fits the modem's queue)
#define C4FM_FT			6	// DCH cycle of seven frames
#define C4FM_RX_PRIME_MS	60	// the voice path's prime wait in C4FM (ambeVoiceSetRxPrimeWait): over twice a poll period
#define C4FM_TX_LOW_BYTES	120	// queue below this (100 ms of air): send the next frame, voice or not

typedef enum {
	C4FM_PHASE_IDLE,
	C4FM_PHASE_RX_VOICE,	// a DN call we play
	C4FM_PHASE_RX_BUSY,	// a transmission we don't play (another DG-ID, VW / data mode)
	C4FM_PHASE_TX_VOICE,
	C4FM_PHASE_TX_ENDING,	// terminator queued, playing out
	C4FM_PHASE_TX_DENIED,	// the modem refused to key: release PTT to clear
	C4FM_PHASE_TX_DATA,	// sending a Wires-X command (data FR frames), unkeys when done
} c4fm_phase_t;

// Wires-X (docs/spec/c4fm.md 5.1): the commands a radio sends its node, as data FR transmissions
enum {
	C4FM_WX_DX,		// the node's status: its name, the room it is in
	C4FM_WX_CONNECT,	// connect the node to a room (5-digit ID)
	C4FM_WX_DISCONNECT,	// leave the room
	C4FM_WX_ALL,		// the list of active rooms (a page of up to 20 from a 1-based entry)
	C4FM_WX_SEARCH,		// the rooms whose names match a text (c4fm_call_wx_text), the same list
};
#define C4FM_WX_TEXT_LEN	16	// a search text
#define C4FM_WX_DATA_MAX	320	// a command, and the raw bytes kept for the bench (the first 320 of a reply)
#define C4FM_WX_RX_MAX		1300	// a reply being received: several blocks (the room list is about 1030 bytes)
#define C4FM_WX_ROOMS		20	// rooms of one page of the list

typedef struct {
	char id[6];			// 5 digits
	char name[17];
	uint8_t users;			// as the node counts them
} c4fm_wx_room_t;

// The node's last reply (DX / connect / disconnect)
typedef struct {
	uint32_t serial;		// bumped at every reply
	uint32_t reply_ms;		// when it came, 0 = none yet
	uint8_t kind;			// C4FM_WX_*: what it answered
	bool connected;			// the node is in a room
	char node_id[6];		// the node's 5-digit ID
	char node_call[11];		// its callsign
	char node_name[15];		// its name
	char room_id[6];		// the room's ID, "" when not connected
	char room_name[17];
	bool search;			// the list is a search's result (else the room list)
	uint8_t rooms;			// ALL: rooms on this page
	uint16_t rooms_total;		// ALL: rooms the node knows
	uint16_t rooms_start;		// ALL: the page's first entry (1-based)
	uint32_t rooms_serial;		// bumped at every list received
	c4fm_wx_room_t room[C4FM_WX_ROOMS];
	// The last command sent and the raw bytes of the last data exchange heard (bench: compare a Yaesu radio's)
	uint8_t sent;			// C4FM_WX_* last sent, 0xFF none
	uint32_t sent_ms;
	uint16_t data_len;
	uint8_t data[C4FM_WX_DATA_MAX];
	uint8_t csd3[20];
} c4fm_wx_t;

typedef struct {
	char callsign[C4FM_CALLSIGN_LEN + 1];	// ours, upper case; "" = can't transmit
	uint8_t dgid;				// 0 = all
	char radio_id[C4FM_RADIO_ID_LEN + 1];	// ours (c4fm_radio_id); "" = "00000"
	uint16_t preamble_ms;			// before the header, 100-C4FM_PREAMBLE_MAX_MS; 0 = 100
	bool plain;				// destination ALL and CM 0, not an FT-70D's fields
} c4fm_config_t;

typedef struct {
	uint8_t phase;		// c4fm_phase_t
	uint32_t serial;	// bumped on every change the UI should redraw
	// The call heard (RX) or the last one
	char src[C4FM_CALLSIGN_LEN + 1];	// trailing spaces trimmed, "" until known
	char dst[C4FM_CALLSIGN_LEN + 1];
	char src_id[C4FM_RADIO_ID_LEN + 1];	// the caller's radio ID (destination "*****ID" or DCH FN 5), "" unknown
	uint8_t dgid;
	uint8_t dt;		// YSF_DT_* of the last frame
	bool late_entry;	// no header heard for this call
	// Counters (diagnostics)
	uint32_t rx_frames, rx_fich_bad, rx_calls, rx_vch_errors, rx_ambe;
	uint32_t tx_frames, tx_padded, tx_calls;
	uint32_t dsp_syncs, dsp_losts, dsp_flywheels;
	int16_t rx_gain, rx_center;	// the receiver's last level fit
	bool rx_inverted;
	bool rx_eq;			// the receiver's equalizer is on
	bool rx_tone_on;		// the receiver's tone filter is on
	uint16_t rx_tone;		// the tone it notches, tenths of Hz (0 none found)
	// FICH bits the Viterbi decoder corrected, summed over the good FICHs (200 coded bits each): the raw bit error
	// rate is about rx_fich_fec / (200 * rx_fich_ok). rx_fich_bad counts every failed FICH, also outside a call
	uint32_t rx_fich_fec, rx_fich_ok;
} c4fm_call_view_t;

// The fields of the last call heard, as sent (diagnostics: compare another radio's bytes with ours). Cleared when a
// call starts; the bits say which parts arrived with a good CRC
typedef struct {
	uint32_t serial;		// bumped on every capture
	ysf_fich_t fich;		// the last good FICH
	uint8_t hdr_ok, term_ok;	// bit 0 CSD1, bit 1 CSD2
	uint8_t hdr_csd1[20], hdr_csd2[20];
	uint8_t term_csd1[20], term_csd2[20];
	uint8_t dch_ok;			// bit n: DCH of FN n
	uint8_t dch[8][YSF_CALLSIGN_LEN];
} c4fm_raw_t;

void c4fm_call_init(const hal_c4fm_t *modem);
// Where VW (voice full rate) frames go, one 88-bit full-rate frame and what its FEC corrected (core/mbe/imbe.h IMBE_ERRS)
// at a time, in the call the voice sink has open; NULL: VW is shown, not played
void c4fm_call_set_imbe(void (*out)(const uint8_t imbe[11], uint8_t errs));
void c4fm_call_set_voice(const dmr_voice_sink_t *sink, const dmr_voice_source_t *source);
void c4fm_call_configure(const c4fm_config_t *cfg);
// On a C4FM channel, with the board's RF in C4FM receive: start receiving (false: no modem)
bool c4fm_call_start(void);
// Leaving C4FM: abort any call, stop the modem
void c4fm_call_stop(void);
bool c4fm_call_running(void);
void c4fm_call_ptt(bool down);
void c4fm_call_poll(uint32_t now_ms);
const c4fm_call_view_t *c4fm_call_view(void);
const c4fm_raw_t *c4fm_call_raw(void);

// Bench capture (diagnostics, docs/spec/c4fm.md 8): the receiver's input audio in a ring, and the last frames' figures.
// Armed, the ring freezes C4FM_CAP_POST samples after the next call starts (a header or a late entry), so it holds
// the lead-in and the call's first frames; read it from RAM over the debug link (READ_MEM at samples)
#if CONFIG_C4FM_CAPTURE
#define C4FM_CAP_SAMPLES	16000	// 2 s at 8 kHz
#else
#define C4FM_CAP_SAMPLES	1	// the ring is off (CONFIG_C4FM_CAPTURE): 32 KB of RAM back; frames are still recorded
#endif
#define C4FM_CAP_POST		12000	// 1.5 s after the call starts
#define C4FM_CAP_FRAMES		32
enum { C4FM_CAP_RUN, C4FM_CAP_ARMED, C4FM_CAP_TRIGGERED, C4FM_CAP_FROZEN };
typedef struct {
	uint32_t ms;			// when the frame ended
	uint16_t sample;		// ring index at the frame's end
	uint8_t sync_errors, flags;	// flags: bit 0 FICH good, 1 flywheel, 2 inverted
	int16_t gain, center;
	uint8_t fi, fn, fec, vch;	// FICH FI / FN, FICH bits corrected, VCH triplets corrected (the frame's five)
} c4fm_cap_frame_t;
typedef struct {
	uint8_t state;			// C4FM_CAP_*
	uint16_t head;			// next sample written (frozen: the oldest)
	uint16_t post;			// samples left before freezing (triggered)
	uint32_t frozen_ms, trigger_ms;
	uint32_t seq;			// bumped at every freeze
	uint8_t frame_head;		// next frame record
	int16_t *samples;		// C4FM_CAP_SAMPLES
	c4fm_cap_frame_t frames[C4FM_CAP_FRAMES];
} c4fm_capture_t;
const c4fm_capture_t *c4fm_call_capture(void);
// 0 run (the ring records continuously), 1 arm (freeze after the next call starts), 2 freeze now
void c4fm_call_capture_ctl(uint8_t cmd);
// The receiver's equalizer and tone filter on or off (bench A/B over the debug link; both on from the start, there is
// no setting). The tone filter off forgets the tone found
void c4fm_call_set_equalizer(bool on);
void c4fm_call_set_tone_filter(bool on);
// The configuration in force (our callsign and DG-ID)
const c4fm_config_t *c4fm_call_config(void);
// Send a Wires-X command (C4FM_WX_*; room: the 5-digit room ID for CONNECT, the first entry (1-based) for ALL). The radio core keys the transmitter first;
// the engine sends when idle or receiving and returns to receive when done (phase C4FM_PHASE_TX_DATA meanwhile).
// False: not running, no callsign, or transmitting already
bool c4fm_call_wx(uint8_t cmd, uint32_t room);
// The text the next SEARCH sends (upper case, up to C4FM_WX_TEXT_LEN characters)
void c4fm_call_wx_text(const char *text);
const c4fm_wx_t *c4fm_call_wx_state(void);
// Exposed for the tests: a command's data block (00, the command, its parameters, 03, the checksum); returns its
// length. And the parse of a node's reply data
uint16_t c4fm_wx_command(uint8_t cmd, uint32_t room, const char *text, uint8_t out[C4FM_WX_DATA_MAX]);
bool c4fm_wx_parse(const uint8_t *data, uint16_t len, c4fm_wx_t *wx);
// The transmitter is in use by the engine (TX_VOICE, TX_ENDING or TX_DATA)
bool c4fm_call_transmitting(void);

// Exposed for the tests: the callsign as YSF sends it (10 characters, upper case, space padded)
void c4fm_callsign_field(const char *call, uint8_t out[C4FM_CALLSIGN_LEN]);
// A stable radio ID from any seed (the chip's unique ID): five characters 0-9 A-Z, never all zeros
void c4fm_radio_id(const uint32_t *seed, unsigned words, char out[C4FM_RADIO_ID_LEN + 1]);

#endif
