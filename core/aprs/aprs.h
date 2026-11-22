// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The APRS engine (docs/spec/aprs.md): who we are, where we are, when to beacon, what to send, and what was heard.
// It runs in the radio task (aprs_tick from radio_sm_poll); the radio core does the RF part of a transmission
// (aprs_rf_*: CSMA on the APRS frequency, keying, back to the user's channel). The UI, the debug link and the host
// link reach it through the calls marked "any task", which take the lock set with aprs_set_lock.
//
// CONFIG_APRS is the engine: the settings, the beacon triggers, the view. It sends through two backends, each built
// by its own switch and working without the other: AX.25 / AFSK on FM with everything packet (CONFIG_APRS_AFSK:
// messages, KISS, APRS-IS, receive) and position reports over DMR (CONFIG_APRS_DMR).
//
// Nothing is transmitted until the user turns APRS transmit on (or presses "send now"), the callsign is set and the
// APRS frequency is in an amateur band the radio may transmit in (docs/spec/aprs.md 3).
#ifndef RFOUNDRY_APRS_H
#define RFOUNDRY_APRS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "aprs/ax25.h"
#include "position/position.h"

// Destination: our registered tocall APRDF?, the sixth character the board's (board.toml aprs_tocall, passed in by the
// build; docs/spec/aprs.md 4.2). "0" is no radio: the host tests
#ifndef APRS_TOCALL_ID
#define APRS_TOCALL_ID		"0"
#endif
#define APRS_TOCALL		"APRDF" APRS_TOCALL_ID
#define APRS_SSID_DEFAULT	7		// handheld
#define APRS_DMR_DST_DEFAULT	310999u		// BrandMeister's APRS gateway, <master MCC>999 (310999 for US masters); 5057 retired 2019-03-01
#define APRS_LEVEL_DEFAULT	50
#define APRS_TXDELAY_DEFAULT	30		// 300 ms of flags
#define APRS_INTERVAL_DEFAULT	600		// s, when automatic beacons are turned on without SmartBeaconing
#define APRS_STATUS_MAX		40
#define APRS_STATIONS		12
#define APRS_MESSAGES		4
#define APRS_MSG_TRIES		3

typedef enum {
	APRS_REGION_NA, APRS_REGION_EU, APRS_REGION_AU, APRS_REGION_NZ, APRS_REGION_JP, APRS_REGION_CN,
	APRS_REGION_CUSTOM, APRS_REGION_COUNT,
} aprs_region_t;

typedef enum {
	APRS_PATH_NONE, APRS_PATH_WIDE1, APRS_PATH_WIDE1_WIDE2, APRS_PATH_WIDE2, APRS_PATH_WIDE2_2, APRS_PATH_COUNT,
} aprs_path_t;

typedef enum { APRS_VIA_RF, APRS_VIA_DMR, APRS_VIA_BOTH, APRS_VIA_COUNT } aprs_via_t;

typedef struct {
	bool tx_on;		// automatic beacons (manual "send now" works without)
	uint8_t region;		// aprs_region_t
	uint32_t freq_hz;	// APRS_REGION_CUSTOM's frequency
	bool narrow;		// 12.5 kHz on the APRS frequency
	uint8_t ssid;
	char sym_table, sym_code;
	uint8_t path;		// aprs_path_t
	bool compressed;
	bool messaging;		// we take messages
	uint16_t interval_s;	// 0 = none
	bool smart;
	uint16_t distance_m;	// 0 = off
	bool after_ptt;
	bool boot;
	uint8_t level;		// AFSK level 0-100
	uint8_t power;		// RF power: 0 = the radio's current level (as for PTT), n = the board's level n - 1
	uint8_t txdelay;	// 10 ms units
	uint8_t via;		// aprs_via_t
	bool dmr_udt;		// ETSI UDT location, else Motorola LRRP
	uint32_t dmr_dst;	// destination ID (private call)
	bool kiss;		// KISS TNC on the host port
	bool kiss_data;		// data mode: host frames limited only by the queue and CSMA, not the APRS burst rule
	uint32_t data_hz;	// data mode's frequency (Tools > Data link); 0: the APRS frequency
	bool data_wide;		// data mode at 25 kHz, else 12.5 kHz
	bool data_dmr;		// data mode's frames as DMR data on the DMR channel, not FM (CONFIG_DATALINK_DMR)
	bool to_is;		// our beacons to APRS-IS through the host helper
	bool rx_on;		// receive on the APRS frequency (boards with a receive path)
	bool igate;		// RF -> APRS-IS
	bool igate_tx;		// APRS-IS -> RF
	uint8_t digi;		// aprs_digi_mode_t
} aprs_cfg_t;	// where our position comes from and its ambiguity: the position service's settings (position.h)

void aprs_cfg_default(aprs_cfg_t *c);
// The APRS frequency c selects (0 = none)
uint32_t aprs_cfg_freq(const aprs_cfg_t *c);
// KISS data mode's frequency: its own, else the APRS frequency
uint32_t aprs_data_freq(const aprs_cfg_t *c);
// Frequency of a region (0 for APRS_REGION_CUSTOM)
uint32_t aprs_region_hz(uint8_t region);
const char *aprs_region_name(uint8_t region);
// In a 2 m, 1.25 m or 70 cm amateur band
bool aprs_amateur_hz(uint32_t hz);
// A plausible amateur callsign (letters and digits, 3-6 characters, a digit after the first character, not N0CALL)
bool aprs_callsign_ok(const char *call);

// Why nothing can be sent now (aprs_view_t.block)
typedef enum {
	APRS_OK, APRS_NO_CALL, APRS_NO_FREQ, APRS_NOT_AMATEUR, APRS_NO_POSITION, APRS_NO_AFSK, APRS_BUSY,
	APRS_NOT_DMR,		// the data link over DMR: not on a DMR channel with a talk target we may send to
	APRS_NO_DMR_ID,		// ... the radio has no DMR ID
} aprs_block_t;

// How an RF transmission ended (aprs_rf_done). The failures name the step that stopped it
typedef enum {
	APRS_RF_SENT, APRS_RF_BUSY, APRS_RF_ABORTED, APRS_RF_NO_KEY, APRS_RF_NO_AFSK, APRS_RF_TIMEOUT,
} aprs_rf_result_t;

typedef struct {
	char call[AX25_CALL_LEN + 4];	// "Q0TST-7", "" = none
	uint8_t block;		// aprs_block_t for RF
	uint32_t freq_hz;
	uint32_t data_hz;	// KISS data mode's frequency (its own or the APRS one)
	uint8_t data_block;	// aprs_block_t on it (over DMR: on the DMR channel)
	uint32_t data_tg;	// over DMR: the talk target the frames go to, 0 none
	bool data_private;	// ... a private call
	uint32_t data_sent, data_failed;	// over DMR: frames on air, frames given up
	bool sos;		// the data link's SOS is on (told to the host)
	uint8_t pos_src;	// position_src_t in use
	bool pos_valid;
	int32_t lat, lon;
	uint16_t speed_kmh;	// 0xFFFF unknown
	uint16_t course;	// 0xFFFF unknown
	uint32_t pos_age_s;
	uint16_t next_s;	// to the next time based beacon, 0xFFFF none
	uint8_t last_why;	// aprs_why_t of the last beacon
	uint32_t last_ms;	// when (0 = none yet)
	uint32_t rf_sent, rf_failed, rf_busy;	// RF transmissions done, failed, given up on a busy channel
	uint8_t rf_fail;	// aprs_rf_result_t of the last failed one (APRS_RF_SENT: none yet)
	uint32_t dmr_sent, dmr_failed;
	uint32_t rx_frames, rx_bad;	// frames decoded, rejected (RX builds)
	uint32_t host_in, host_out, host_dropped;	// KISS frames from / to the host, host frames refused
	uint32_t igated, is_to_rf, digi_sent;
	uint8_t stations, messages, unread;
	bool rf_active;		// the radio is on the APRS frequency for us now
	bool msg_pending;	// our message waiting for its ack / its next try
	uint8_t msg_tries;
	bool msg_acked;
	char last_info[48];	// the information field of our last beacon (truncated)
	uint32_t serial;	// bumped on every change worth a redraw
} aprs_view_t;

typedef struct {
	char call[AX25_CALL_LEN + 4];	// with SSID
	uint32_t heard_ms;
	bool direct;		// heard without a digipeater
	bool has_pos;
	int32_t lat, lon;
	uint32_t pos_ms;	// when the position was heard (heard_ms moves on with every packet; the position only with one)
	char sym_table, sym_code;
	uint16_t course, speed_kn;
	uint8_t type;		// aprs_type_t of the last packet
	uint16_t count;
	char text[24];		// comment / status (truncated)
} aprs_station_t;

typedef struct {
	char from[AX25_CALL_LEN + 4];
	char text[68];
	char msgno[6];
	uint32_t ms;
	bool read;
} aprs_msg_t;

// --- The radio task ---

typedef struct {
	bool can_rf;		// an FM APRS transmission could start now (nothing else on the air, no PTT, no scan ...)
	bool fm_rx;		// receiving FM (the receive path's audio is worth demodulating)
	uint32_t rx_hz;		// on this frequency: APRS receive runs only on the APRS frequency
	bool on_dmr;		// the channel is DMR with the call stack running (digital positions can go out)
	uint32_t own_dmr_id;
} aprs_radio_t;

// Start-up (radio_sm_init), after the settings are loaded
void aprs_init(uint32_t now_ms);
// Every radio poll
void aprs_tick(const aprs_radio_t *r, uint32_t now_ms);
// Our own transmission (PTT, not APRS) ended: the "after PTT" trigger
void aprs_ptt_end(uint32_t now_ms);
// The settings changed underneath (RREQ_RELOAD): read them again
void aprs_reload(void);


// RF job for the radio core
typedef struct {
	uint32_t hz;
	bool narrow;
	uint8_t level;
	const uint8_t *tones;
	uint16_t nbits;
} aprs_rf_job_t;
// The radio core starts it: fills job (tones stay valid until aprs_rf_done). False: nothing after all
bool aprs_rf_take(aprs_rf_job_t *job, uint32_t now_ms);
// It went out, or not: a busy channel, or a failure (aprs_rf_result_t)
void aprs_rf_done(aprs_rf_result_t r, uint32_t now_ms);
// Seconds a pending job may wait for a clear channel before it is dropped
#define APRS_CSMA_GIVE_UP_MS	10000u


#if CONFIG_APRS_RX
// The receive path's diagnostics (debug link): the largest sample since it started (0: only silence, the squelch never
// opened or no audio arrives), tone decisions, the HDLC decoder's good frames, FCS errors and aborts, and the starts
// the board's receive path refused
typedef struct {
	uint16_t peak;
	uint32_t bits, frames, fcs_errors, aborts, start_fails;
} aprs_rx_stats_t;
void aprs_rx_stats(aprs_rx_stats_t *out);
// Received audio (the radio task reads it from the board's receive path or the PCM stream while receiving FM)
void aprs_rx_audio(const int16_t *s, size_t n, uint32_t now_ms);
// A decoded AX.25 frame (without the FCS): from the demodulator, or the tests
void aprs_rx_frame(const uint8_t *frame, size_t n, uint32_t now_ms);
// A DMR caller's position (ETSI GPS Info) for the station list: named by its alias's callsign, else its DMR ID.
// Any task. Not gated or sent anywhere: the list only
void aprs_dmr_heard(uint32_t id, const char *alias, int32_t lat, int32_t lon, uint32_t now_ms);
#endif

// --- Any task ---

void aprs_set_lock(void (*lock)(void), void (*unlock)(void));
void aprs_get_view(aprs_view_t *v);
void aprs_get_cfg(aprs_cfg_t *c);
// Settings in effect now and saved (core/settings)
void aprs_set_cfg(const aprs_cfg_t *c, uint32_t now_ms);
// "Send now": a position beacon at once (on the configured paths)
bool aprs_send_now(void);
// Message options. Position appends a coordinate snapshot to the text; ACK adds a message number and retries.
#define APRS_MSG_ACK		0x01u
#define APRS_MSG_POSITION	0x02u
#if CONFIG_APRS_RX
uint8_t aprs_station_count(void);
bool aprs_station(uint8_t i, aprs_station_t *out);	// 0 = most recent
bool aprs_station_find(const char *call, aprs_station_t *out);	// exact callsign, one locked copy
uint8_t aprs_msg_count(void);
bool aprs_msg(uint8_t i, aprs_msg_t *out);		// 0 = newest; marks it read
bool aprs_msg_peek(uint8_t i, aprs_msg_t *out);	// leaves unread state alone
bool aprs_msg_delete(uint8_t i);
#endif

// --- The host link (aprs_host.c) ---

// The KISS setting is on: the host port is APRS's (KISS frames with the AX.25 half, NMEA positions in any build)
bool aprs_host_enabled(void);

// --- Calls into the AX.25 half (aprs_ax25.c, CONFIG_APRS_AFSK) that callers outside it use. Built without it they
// answer "nothing there", so a caller needs no switch of its own ---
#if CONFIG_APRS_AFSK
const char *aprs_path_text(uint8_t path);
// A storage job (it writes flash; not the radio or UI task): our path and SSID into the codeplug's APRS record 1, so
// the CPS reads what the radio uses. False when there is no such record or the write failed
bool aprs_to_codeplug(void);
// The level an RF transmission keys at: APRS's own setting (aprs_cfg_t.power: n = the board's level n - 1, capped at
// its highest) or, with none (0), the radio's current level
uint8_t aprs_tx_power(uint8_t current);
// A transmission is waiting (the radio core then checks it can start)
bool aprs_rf_pending(void);
// The radio can receive APRS: a build with CONFIG_APRS_RX on a board with an AFSK receive path (board.afsk->rx_*) or
// FM receive audio on its PCM stream (AUDIO_TAP_FM_RX)
bool aprs_can_receive(void);
// The receive path is running (the receiver must stay awake: no battery saving)
bool aprs_rx_listening(void);
// A status packet (">text")
bool aprs_send_status(const char *text);
// A message with acks and retries; false when one is still waiting or the text is unusable
bool aprs_send_message(const char *to, const char *text);
bool aprs_send_message_opts(const char *to, const char *text, uint8_t flags);
// RAM-only override for bench tests (debug link): the callsign ("" = back to the settings / codeplug)
void aprs_set_test_call(const char *call);
// A KISS data frame (AX.25 without FCS) from the host for RF: checked (our callsign as the source, rate limit)
bool aprs_host_frame(const uint8_t *frame, size_t n, uint32_t now_ms);
// An APRS-IS line from the host helper (KISS SetHardware 'G')
void aprs_host_is_line(const char *line, size_t n, uint32_t now_ms);
// The host's 'Q' query (KISS SetHardware 'Q', no data): answers with the queue, why RF is blocked, the mode and our
// callsign, without transmitting (aprs.md 9)
void aprs_host_status_query(void);
// KISS-encoded bytes for the host: copies up to max, returns the count (0 = nothing waiting)
size_t aprs_host_output(uint8_t *out, size_t max);
// The data link's SOS (aprs.md 9.3): SetHardware 'S' and 1 (alert) or 0 (cancelled) to the host, which raises or
// ends the alert (RadioLink: a 911 Alert in ATAK). false with KISS off: nobody to tell
bool aprs_host_sos(bool on);
bool aprs_host_sos_on(void);
// A SetHardware frame for the host (data starts with its tag), whole or not at all; false with KISS off or the ring
// full (the voice over the host port, aprs.md 9.5)
bool aprs_host_emit_hw(const uint8_t *data, size_t n);
#else
static inline bool aprs_host_sos(bool on) { (void)on; return false; }
static inline bool aprs_host_sos_on(void) { return false; }
static inline bool aprs_host_frame(const uint8_t *frame, size_t n, uint32_t now_ms)
{
	(void)frame;
	(void)n;
	(void)now_ms;
	return false;
}
static inline void aprs_host_is_line(const char *line, size_t n, uint32_t now_ms) { (void)line; (void)n; (void)now_ms; }
static inline void aprs_host_status_query(void) {}
static inline size_t aprs_host_output(uint8_t *out, size_t max) { (void)out; (void)max; return 0; }
static inline const char *aprs_path_text(uint8_t path) { (void)path; return ""; }
static inline bool aprs_to_codeplug(void) { return false; }
static inline uint8_t aprs_tx_power(uint8_t current) { return current; }
static inline bool aprs_rf_pending(void) { return false; }
static inline bool aprs_can_receive(void) { return false; }
static inline bool aprs_rx_listening(void) { return false; }
static inline bool aprs_send_status(const char *text) { (void)text; return false; }
static inline bool aprs_send_message(const char *to, const char *text) { (void)to; (void)text; return false; }
static inline bool aprs_send_message_opts(const char *to, const char *text, uint8_t flags)
{
	(void)to;
	(void)text;
	(void)flags;
	return false;
}
static inline void aprs_set_test_call(const char *call) { (void)call; }
#endif

// --- The data link over DMR (CONFIG_DATALINK_DMR, aprs.md 9.2): KISS data mode's host frames as UDP datagrams on the
// DMR channel instead of FM. The queue is here, the DMR side in aprs_dlink_dmr.c; radio task unless said ---
#if CONFIG_DATALINK_DMR
// Room in front of each queued frame for the transport's header (a UDP header), written in place by the taker
#define APRS_DLINK_HEADROOM	8u
#define APRS_DLINK_QUEUE	4u	// frames waiting, as the FM queue: the host sees the same free slots
typedef struct {
	uint8_t *buf;		// APRS_DLINK_HEADROOM octets, then the AX.25 frame; stays put until aprs_dlink_done
	uint16_t len;		// the frame's octets (after the headroom)
} aprs_dlink_frame_t;
// Data mode over DMR is on (the host's frames wait here, not in the FM queue)
bool aprs_dlink_on(void);
// The queued frames not yet taken, oldest first, at most max: they stay queued until aprs_dlink_done
int aprs_dlink_take(aprs_dlink_frame_t *out, int max);
// The first count frames taken last went out (sent) or failed; failed ones are tried again a few times, then dropped.
// count 0: none was started, all go back to the queue untouched
void aprs_dlink_done(int count, bool sent);
// What the DMR channel allows now (aprs_block_t: APRS_OK, APRS_NOT_DMR, APRS_NO_DMR_ID) and where the frames go
void aprs_dlink_channel(uint8_t block, uint32_t talk, bool private_call);
// A frame heard on the data link (any task): to the host as a received KISS frame, nothing else
void aprs_dlink_rx(const uint8_t *frame, size_t n, uint32_t now_ms);
// Plain DMR radios to the host in data mode (aprs.md 9.4; any task): a text message heard (to us or a group we listen
// to), and the host's text to one (aprs_dlink_dmr.c sends it as a DMR text message; false when it can't)
void aprs_dlink_dmr_text_heard(uint32_t src, const char *text);
bool aprs_dlink_dmr_text(uint32_t dst, bool group, const char *text, size_t n);
#endif

#endif
