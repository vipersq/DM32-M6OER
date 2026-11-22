// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// DMR voice over the host port (core/aprs/aprs_voice.c, adpcm.c, docs/spec/aprs.md 9.5): IMA ADPCM, then the KISS
// 'V' session against the real shared PCM stream (audio_mux.c) and a fake board stream, radio core and KISS output
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "board.h"
#include "aprs/adpcm.h"
#include "aprs/aprs.h"
#include "aprs/aprs_voice.h"
#include "hostlink/hostlink_view.h"
#include "radio/radio.h"
#include "usbaudio/audio_mux.h"

static int failures;
static int checks;

#define CHECK(cond) do { checks++; if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint32_t now;
uint32_t hal_millis(void) { return now; }

// ---- the board's stream ----

static struct {
	bool streaming;
	audio_block_cb_t cb;
} hw;
static bool a_start(audio_block_cb_t cb, void *ctx) { (void)ctx; hw.cb = cb; hw.streaming = true; return true; }
static void a_stop(void) { hw.streaming = false; hw.cb = NULL; }
static void a_pcm(bool on) { (void)on; }
static const hal_audio_t fake_audio = { .start = a_start, .stop = a_stop, .speaker_pcm = a_pcm };

static int16_t mic[AUDIO_BLOCK_SAMPLES], spk[AUDIO_BLOCK_SAMPLES];
static uint32_t phase;

// The vocoder: plays a 500 Hz tone to the speaker, keeps what it got as the microphone
static int16_t enc_mic[AUDIO_BLOCK_SAMPLES];
static void vocoder(const int16_t *m, int16_t *s, void *ctx)
{
	(void)ctx;
	memcpy(enc_mic, m, sizeof enc_mic);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		s[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * 500.0 * (double)(phase++) / 8000.0));
	}
}

static hal_audio_tap_t tap = { .caps = AUDIO_TAP_DMR };
const board_t board = { .name = "fake", .audio = &fake_audio, .audio_tap = &tap };

// ---- the radio core ----

static radio_view_t view;
static int host_downs, host_ups;
void radio_get_view(radio_view_t *v) { *v = view; }
void radio_req_ptt_host(bool down) { if (down) { host_downs++; } else { host_ups++; } }

// ---- KISS output: SetHardware frames as they would go to the host ----

#define MAX_OUT 256
static struct {
	size_t n;
	uint8_t d[96];
} out[MAX_OUT];
static int nout;
static bool kiss_on = true;
bool aprs_host_emit_hw(const uint8_t *data, size_t n)
{
	if (!kiss_on) {
		return false;
	}
	if (nout < MAX_OUT && n <= sizeof out[0].d) {
		out[nout].n = n;
		memcpy(out[nout].d, data, n);
	}
	nout++;
	return true;
}

static int count(char sub)
{
	int c = 0;
	for (int i = 0; i < nout && i < MAX_OUT; i++) {
		c += out[i].d[0] == 'V' && out[i].d[1] == (uint8_t)sub;
	}
	return c;
}

static int last(char sub)
{
	for (int i = (nout < MAX_OUT ? nout : MAX_OUT) - 1; i >= 0; i--) {
		if (out[i].d[0] == 'V' && out[i].d[1] == (uint8_t)sub) {
			return i;
		}
	}
	return -1;
}

static const hal_audio_t *radio_audio;

static void hw_frame(const char *s, size_t n)
{
	aprs_voice_hw((const uint8_t *)s, n, now);
}

static void reset(void)
{
	hw_frame("R\0", 2);
	hw_frame("U", 1);
	audio_mux_reset();
	memset(&hw, 0, sizeof hw);
	memset(&view, 0, sizeof view);
	view.ch.mode = RADIO_MODE_DMR;
	view.dmr.active = true;
	view.dmr.codec = true;
	view.state = RS_IDLE;
	view.dmr.state = RDMR_IDLE;
	host_downs = host_ups = 0;
	now += 5000;
	aprs_voice_pump(now);
	nout = 0;
	radio_audio = audio_mux_wrap(&fake_audio);
}

// 20 ms: the radio task's service and the host task's pump every 5 ms, then a block of the stream
static void tick(void)
{
	for (int i = 0; i < 4; i++) {
		now += 5;
		audio_mux_service(now);
		aprs_voice_pump(now);
	}
	if (hw.streaming && hw.cb != NULL) {
		hw.cb(mic, spk, NULL);
	}
}

// ---- ADPCM ----

static double snr_db(const int16_t *a, const int16_t *b, size_t n)
{
	double s = 0, e = 0;
	for (size_t i = 0; i < n; i++) {
		s += (double)a[i] * a[i];
		e += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
	}
	return 10.0 * log10(s / (e > 0 ? e : 1e-9));
}

static void test_adpcm(void)
{
	enum { N = 1600 };
	static int16_t x[N], y[N];
	for (int i = 0; i < N; i++) {
		// A voice-like mix: 300 Hz and 1.1 kHz, rising level
		double a = 2000.0 + 10.0 * i;
		x[i] = (int16_t)(a * (sin(2 * M_PI * 300.0 * i / 8000.0) + 0.5 * sin(2 * M_PI * 1100.0 * i / 8000.0)));
	}
	adpcm_state_t st = { 0, 0 };
	uint8_t f[KISS_VOICE_FRAME];
	size_t got = 0;
	for (int k = 0; k < N; k += KISS_VOICE_SAMPLES) {
		size_t n = adpcm_encode(&st, x + k, KISS_VOICE_SAMPLES, f);
		CHECK(n == KISS_VOICE_FRAME);
		got += adpcm_decode(f, n, y + k, KISS_VOICE_SAMPLES);
	}
	CHECK(got == N);
	// Skip the first frame (the step size adapting from rest)
	double snr = snr_db(x + KISS_VOICE_SAMPLES, y + KISS_VOICE_SAMPLES, N - KISS_VOICE_SAMPLES);
	CHECK(snr > 20.0);

	// A frame decodes alone: its header carries the state, so a lost frame costs only itself
	adpcm_state_t s2 = { 0, 0 };
	uint8_t f1[KISS_VOICE_FRAME], f2[KISS_VOICE_FRAME];
	adpcm_encode(&s2, x + 800, KISS_VOICE_SAMPLES, f1);
	adpcm_encode(&s2, x + 960, KISS_VOICE_SAMPLES, f2);
	int16_t z[KISS_VOICE_SAMPLES];
	CHECK(adpcm_decode(f2, sizeof f2, z, KISS_VOICE_SAMPLES) == KISS_VOICE_SAMPLES);
	CHECK(snr_db(x + 960, z, KISS_VOICE_SAMPLES) > 20.0);

	// Too short, a bad step index, a small output buffer
	CHECK(adpcm_decode(f1, 2, z, KISS_VOICE_SAMPLES) == 0);
	f1[2] = 89;
	CHECK(adpcm_decode(f1, sizeof f1, z, KISS_VOICE_SAMPLES) == 0);
	CHECK(adpcm_decode(f2, sizeof f2, z, 10) == 10);

	// Full scale doesn't wrap around
	int16_t big[KISS_VOICE_SAMPLES];
	for (int i = 0; i < (int)KISS_VOICE_SAMPLES; i++) {
		big[i] = (i / 8) % 2 ? 32767 : -32768;
	}
	adpcm_state_t s3 = { 0, 0 };
	for (int r = 0; r < 4; r++) {
		adpcm_encode(&s3, big, KISS_VOICE_SAMPLES, f1);
	}
	CHECK(adpcm_decode(f1, sizeof f1, z, KISS_VOICE_SAMPLES) == KISS_VOICE_SAMPLES);
	CHECK(z[4] < -20000 && z[12] > 20000);
}

// ---- the session ----

static void test_rx(void)
{
	reset();
	radio_audio->start(vocoder, NULL);
	// No session: nothing to the host, and the stream isn't the host's
	for (int i = 0; i < 10; i++) {
		tick();
	}
	CHECK(nout == 0);
	hw_frame("R\1", 2);
	CHECK(aprs_voice_rx_on());
	// Listening, no call: the speaker's samples are read and dropped
	for (int i = 0; i < 10; i++) {
		tick();
	}
	CHECK(count('A') == 0 && count('C') == 0);
	// A call plays: 'C' with the caller, then 20 ms frames of what the speaker gets
	view.dmr.state = RDMR_RX;
	view.dmr.call.src = 2351234;
	view.dmr.call.dst = 235;
	view.dmr.call.group = true;
	strcpy(view.dmr.call.src_name, "M6OER");
	for (int i = 0; i < 50; i++) {
		tick();
	}
	int c = last('C');
	CHECK(c >= 0 && out[c].n == 11 + 5);
	CHECK(c >= 0 && out[c].d[2] == 0x00 && out[c].d[3] == 0x23 && out[c].d[4] == 0xE0 && out[c].d[5] == 0x82);
	CHECK(c >= 0 && out[c].d[9] == 235 && out[c].d[10] == 1 && memcmp(out[c].d + 11, "M6OER", 5) == 0);
	int a = count('A');
	CHECK(a >= 45 && a <= 50);	// one a block, less the refresh's latency
	int k = last('A');
	CHECK(k >= 0 && out[k].n == 2 + KISS_VOICE_FRAME);
	int16_t pcm[KISS_VOICE_SAMPLES];
	CHECK(k >= 0 && adpcm_decode(out[k].d + 2, out[k].n - 2, pcm, KISS_VOICE_SAMPLES) == KISS_VOICE_SAMPLES);
	int peak = 0;
	for (int i = 0; i < (int)KISS_VOICE_SAMPLES; i++) {
		peak = pcm[i] > peak ? pcm[i] : peak;
	}
	CHECK(peak > 6500 && peak < 9500);
	// The call ends: 'E', and no more audio
	view.dmr.state = RDMR_IDLE;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	CHECK(count('E') == 1);
	int a2 = count('A');
	for (int i = 0; i < 20; i++) {
		tick();
	}
	CHECK(count('A') == a2);
	// Receive audio off: the host link's share of the stream ends
	hw_frame("R\0", 2);
	CHECK(!aprs_voice_rx_on());
	radio_audio->stop();
	audio_mux_service(now);
	CHECK(!hw.streaming);
}

static void host_audio(int16_t level)
{
	static int16_t pcm[KISS_VOICE_SAMPLES];
	static adpcm_state_t st;
	uint8_t f[1 + KISS_VOICE_FRAME];
	for (int i = 0; i < (int)KISS_VOICE_SAMPLES; i++) {
		pcm[i] = level;
	}
	f[0] = 'A';
	size_t n = adpcm_encode(&st, pcm, KISS_VOICE_SAMPLES, f + 1);
	aprs_voice_hw(f, n + 1u, now);
}

static void test_tx(void)
{
	reset();
	// Refused: FM, hotspot, no vocoder; the host hears why
	view.ch.mode = RADIO_MODE_FM;
	hw_frame("K", 1);
	int t = last('T');
	CHECK(t >= 0 && out[t].d[2] == 0 && out[t].d[3] == KISS_V_NOT_DMR && host_downs == 0);
	view.ch.mode = RADIO_MODE_DMR;
	view.hotspot = true;
	hw_frame("K", 1);
	t = last('T');
	CHECK(t >= 0 && out[t].d[3] == KISS_V_HOTSPOT && host_downs == 0);
	view.hotspot = false;
	view.dmr.codec = false;
	hw_frame("K", 1);
	t = last('T');
	CHECK(t >= 0 && out[t].d[3] == KISS_V_NO_CODEC && host_downs == 0 && !aprs_voice_tx_on());
	view.dmr.codec = true;

	// Keyed: PTT with the host's audio, which replaces the microphone at the encoder
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
		mic[i] = -5;
	}
	radio_audio->start(vocoder, NULL);
	hw_frame("R\1", 2);
	hw_frame("K", 1);
	CHECK(host_downs == 1 && aprs_voice_tx_on());
	hw_frame("K", 1);
	CHECK(host_downs == 1);		// a second key changes nothing
	view.state = RS_TX;
	view.dmr.state = RDMR_TX;
	for (int i = 0; i < 10; i++) {
		host_audio(3000);
		tick();
	}
	t = last('T');
	CHECK(t >= 0 && out[t].d[2] == 1 && out[t].d[4] == 1);		// on air, the host's transmission
	CHECK(enc_mic[0] > 2500 && enc_mic[0] < 3500 && enc_mic[100] > 2500);
	CHECK(count('A') == 0);		// half duplex: our own transmission isn't sent back
	// Audio frames without a key are dropped
	hw_frame("U", 1);
	CHECK(host_ups == 1 && !aprs_voice_tx_on());
	view.state = RS_IDLE;
	view.dmr.state = RDMR_IDLE;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	t = last('T');
	CHECK(t >= 0 && out[t].d[2] == 0 && out[t].d[4] == 0);
	CHECK(enc_mic[0] == -5);	// the microphone again
	audio_mux_stats_t st0, st1;
	audio_mux_stats(&st0);
	host_audio(3000);
	audio_mux_stats(&st1);
	CHECK(st1.tx_pushes == st0.tx_pushes);

	// The host goes quiet while keyed: unkeyed after KISS_VOICE_STALE_MS, never a stuck transmitter
	hw_frame("K", 1);
	CHECK(host_downs == 2);
	host_audio(1000);
	for (int i = 0; i < (int)(KISS_VOICE_STALE_MS / 20u) - 2; i++) {
		tick();
	}
	CHECK(aprs_voice_tx_on());
	for (int i = 0; i < 4; i++) {
		tick();
	}
	CHECK(!aprs_voice_tx_on() && host_ups == 2);

	// The radio's own PTT: the host sees the transmitter, not as its own
	view.state = RS_TX;
	for (int i = 0; i < 4; i++) {
		tick();
	}
	t = last('T');
	CHECK(t >= 0 && out[t].d[2] == 1 && out[t].d[4] == 0);
	view.state = RS_IDLE;
	hw_frame("R\0", 2);
	radio_audio->stop();
}

static void test_garbage(void)
{
	reset();
	aprs_voice_hw((const uint8_t *)"", 0, now);
	hw_frame("Z", 1);
	hw_frame("A", 1);	// audio unkeyed, too short
	hw_frame("R", 1);	// no flag: off
	CHECK(!aprs_voice_rx_on() && !aprs_voice_tx_on() && nout == 0);
	// KISS off: frames go nowhere, nothing breaks
	kiss_on = false;
	view.ch.mode = RADIO_MODE_FM;
	hw_frame("K", 1);
	CHECK(nout == 0 && host_downs == 0);
	kiss_on = true;
}

int main(void)
{
	test_adpcm();
	test_rx();
	test_tx();
	test_garbage();
	printf("kissVoiceTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
