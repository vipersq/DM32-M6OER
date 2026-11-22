// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The data link over DMR (docs/spec/aprs.md 9.2): KISS data mode's host frames as UDP datagrams to
// DMR_UDP_PORT_DATA_LINK on the DMR channel's talk target, through the DMR data service, and those heard back to the
// host. The frames wait in the APRS engine's data link queue (aprs.h aprs_dlink_*); this is the DMR side. Several
// frames waiting go in one transmission (dmrDataServiceSendIPChain), unconfirmed, the compressed UDP/IPv4 header.
// Radio task only (the data service's own context). Built with CONFIG_DATALINK_DMR
#ifndef RFOUNDRY_APRS_DLINK_DMR_H
#define RFOUNDRY_APRS_DLINK_DMR_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

typedef struct {
	bool dmr;		// on a DMR channel (or the DMR VFO) we may send data on now
	uint32_t own_id;	// our DMR ID, 0 none
	uint32_t talk;		// the channel's talk target, 0 none
	bool private_call;	// ... a private call, else a talkgroup
} aprs_dlink_dmr_channel_t;

#if CONFIG_DATALINK_DMR
// Every data service tick: the channel's state to the engine, then the queued frames to the service when it is free,
// and the end of the transmission under way back to the queue
void aprs_dlink_dmr_tick(const aprs_dlink_dmr_channel_t *ch, uint32_t now_ms);
#else
static inline void aprs_dlink_dmr_tick(const aprs_dlink_dmr_channel_t *ch, uint32_t now_ms)
{
	(void)ch;
	(void)now_ms;
}
#endif

#endif
