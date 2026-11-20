// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The HR_C7000's FM audio copy shared by APRS and the C4FM receiver (platform/hrc7000/fm_rx_audio.c): one user at a
// time; C4FM's start takes it from APRS; a user's read and stop do nothing when the copy is another's
#include <stdio.h>
#include <string.h>
#include "fm_rx_audio.h"

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void (*hook)(void);
bool hrc_dmr_fm_rx_hook(void (*fn)(void)) { hook = fn; return true; }
static bool irq_on;
void hrc7000_fm_rx_audio_irq(hrc7000_t *d, bool on) { (void)d; irq_on = on; }

static hrc7000_t modem;
static uint8_t ram[0x10000];
static uint32_t regs[0x1000];

// One half of audio, sample value v (the modem's RX RAM names the half in FM_ADDR_SW)
static void half(int16_t v)
{
	modem.buf = ram;
	modem.regs = regs;
	regs[C7_FM_ADDR_SW / 4u] = 1;	// the first half is the CPU's
	for (unsigned i = 0; i < HRC7000_FM_RX_SAMPLES; i++) {
		ram[C7_FM_RX_AUDIO + 2u * i] = (uint8_t)((uint16_t)v >> 8);	// high byte first
		ram[C7_FM_RX_AUDIO + 2u * i + 1] = (uint8_t)(v & 0xFF);
	}
	if (hook != NULL)
		hook();
}

int main(void)
{
	int16_t buf[1024];
	hrc_fm_rx_attach(&modem);
	// APRS alone: it reads what comes in; C4FM's read gets nothing of it
	CHECK(hrc_fm_rx_start(HRC_FM_RX_APRS) && irq_on);
	half(7);
	CHECK(hrc_fm_rx_read_c4fm(buf, 1024) == 0);
	uint16_t n = hrc_fm_rx_read_aprs(buf, 1024);
	CHECK(n == HRC7000_FM_RX_SAMPLES && buf[0] == 7 && buf[n - 1] == 7);
	// C4FM starts (a channel change) before APRS has let go: C4FM takes the copy
	CHECK(hrc_fm_rx_start(HRC_FM_RX_C4FM) && irq_on);
	CHECK(hrc_fm_rx_read_aprs(buf, 1024) == 0);
	half(9);
	CHECK(hrc_fm_rx_read_aprs(buf, 1024) == 0 && hrc_fm_rx_read_c4fm(buf, 1024) == HRC7000_FM_RX_SAMPLES && buf[0] == 9);
	// APRS's late stop and a new start of its own leave C4FM alone
	hrc_fm_rx_stop(HRC_FM_RX_APRS);
	CHECK(irq_on && !hrc_fm_rx_start(HRC_FM_RX_APRS));
	// C4FM stops: APRS can have it again
	hrc_fm_rx_stop(HRC_FM_RX_C4FM);
	CHECK(!irq_on && hrc_fm_rx_start(HRC_FM_RX_APRS));
	// APRS never takes it from C4FM, and a second start of the same user is refused (it has it)
	CHECK(!hrc_fm_rx_start(HRC_FM_RX_APRS));
	hrc_fm_rx_stop(HRC_FM_RX_APRS);
	CHECK(!irq_on && hrc_fm_rx_read_aprs(buf, 1024) == 0);
	printf("fmRxShareTest: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
