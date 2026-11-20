// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// HR_C7000 FM receive audio for the APRS demodulator (fm_rx_audio.c): the modem's own copy of its demodulated FM
// audio, 8 kHz, into a ring the radio task drains (hal_afsk.h receive side, modem-hrc7000.md 8.5)
#ifndef HRC_FM_RX_AUDIO_H
#define HRC_FM_RX_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "modem/hrc7000/hrc7000.h"

#if CONFIG_APRS_RX || CONFIG_C4FM
// Once, from the radio init, after the modem is attached
void hrc_fm_rx_attach(hrc7000_t *modem);
// Its users: one at a time. C4FM (its receiver, on C4FM channels) comes first: its start takes the copy from APRS if
// APRS has it (a channel change from the APRS frequency to a C4FM channel, before APRS's next poll lets go); APRS's
// start fails while C4FM has it. A user's stop and read do nothing when the copy is another's
enum { HRC_FM_RX_APRS = 1, HRC_FM_RX_C4FM = 2 };
// Start and stop (task level). Start takes the ring from the RTOS heap and fails without it, or without the DMR
// glue's system interrupt (CONFIG_DMR off)
bool hrc_fm_rx_start(uint8_t who);
void hrc_fm_rx_stop(uint8_t who);
// Up to max samples since the last call, for each user (hal_afsk_t / hal_c4fm_t rx_read); 0 when the copy is another's
uint16_t hrc_fm_rx_read_aprs(int16_t *buf, uint16_t max);
uint16_t hrc_fm_rx_read_c4fm(int16_t *buf, uint16_t max);
// Halves taken and halves dropped because the ring was full
void hrc_fm_rx_stats(uint32_t *halves, uint32_t *dropped);
#else
static inline void hrc_fm_rx_attach(hrc7000_t *modem) { (void)modem; }
#endif

#endif
