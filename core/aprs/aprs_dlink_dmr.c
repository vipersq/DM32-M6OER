// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The data link over DMR (aprs_dlink_dmr.h)
#include "config.h"
#include "aprs/aprs_dlink_dmr.h"
#if CONFIG_DATALINK_DMR	// the whole file
#include "aprs/aprs.h"
#include "dmr/dmrData.h"
#include "dmr/dmrDataService.h"
#include "dmr/dmr_data_api.h"

#define UDP_HEADER	8u
_Static_assert(UDP_HEADER == APRS_DLINK_HEADROOM, "the queue leaves room for the UDP header");

static bool sending;		// a transmission of ours is under way
static int sending_n;		// ... carrying the first this many frames taken

// The UDP header in the frame's headroom: from and to the data link's port, the checksum left 0 for the IPv4 layer
// (the service computes it, as for the radio's other datagrams)
static void udp_header(uint8_t *h, uint16_t frame_len)
{
	uint16_t n = (uint16_t)(UDP_HEADER + frame_len);
	h[0] = (uint8_t)(DMR_UDP_PORT_DATA_LINK >> 8);
	h[1] = (uint8_t)DMR_UDP_PORT_DATA_LINK;
	h[2] = (uint8_t)(DMR_UDP_PORT_DATA_LINK >> 8);
	h[3] = (uint8_t)DMR_UDP_PORT_DATA_LINK;
	h[4] = (uint8_t)(n >> 8);
	h[5] = (uint8_t)n;
	h[6] = 0;
	h[7] = 0;
}

void aprs_dlink_dmr_tick(const aprs_dlink_dmr_channel_t *ch, uint32_t now_ms)
{
	(void)now_ms;
	uint8_t block = (!ch->dmr || ch->talk == 0) ? APRS_NOT_DMR : ch->own_id == 0 ? APRS_NO_DMR_ID : APRS_OK;
	aprs_dlink_channel(block, ch->dmr ? ch->talk : 0, ch->private_call);
	if (sending) {
		if (dmrDataServiceIpBusy()) {
			return;
		}
		sending = false;
		aprs_dlink_done(sending_n, dmrDataServiceIpResult() == DMR_REQ_OK);
		return;
	}
	if (block != APRS_OK || dmrDataServiceIpBusy() || dmrDataServiceIsBusy()) {
		return;
	}
	aprs_dlink_frame_t f[APRS_DLINK_QUEUE];
	int n = aprs_dlink_take(f, (int)APRS_DLINK_QUEUE);
	if (n == 0) {
		return;
	}
	dmrDataChainPacket_t p[APRS_DLINK_QUEUE];
	for (int i = 0; i < n; i++) {
		udp_header(f[i].buf, f[i].len);
		p[i].protocol = DMR_IP_PROTO_UDP;
		p[i].delivery = DMR_IPS_UNCONFIRMED;	// to a talkgroup there is no one to confirm: repeats are the host's
		p[i].l4 = f[i].buf;
		p[i].length = (int)(UDP_HEADER + f[i].len);
	}
	// As many as fit one transmission; the rest wait for the next. 0: the service can't send now (the queue keeps them)
	int k = dmrDataServiceSendIPChain(!ch->private_call, ch->talk, DMR_DATA_IP_OURS, DMR_DATA_IP_OURS,
					  DMR_IP_TTL_DEFAULT, p, n);
	if (k <= 0) {
		aprs_dlink_done(0, false);
		return;
	}
	sending = true;
	sending_n = k;
}

void dmrDataServiceDataLinkRx(uint32_t src, bool group, const uint8_t *payload, int length, uint32_t nowMs)
{
	(void)src;
	(void)group;
	if (length > 0) {
		aprs_dlink_rx(payload, (size_t)length, nowMs);
	}
}

#endif
