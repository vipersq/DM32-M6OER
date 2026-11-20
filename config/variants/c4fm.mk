# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
## title: C4FM
## summary: FM, DMR, C4FM (Yaesu System Fusion DN and VW), P25, NXDN and APRS in one firmware; DMR voice and SMS, no IP or network services.
##
## make BOARD=<board> VARIANT=c4fm. The GPL fork's build (docs/spec/c4fm.md): C4FM on beside FM and DMR (voice,
## channels, contacts, SMS), and off what a radio for QSOs doesn't need (IP, the cable's network services, the hotspot,
## the advanced DMR services), which also frees the RAM C4FM takes.

CONFIG_C4FM ?= y

# The roger sounds and tactical tones (core/radio/roger_sound.h, Settings > Roger & tones) take the place of the
# signaling tone chains: one or the other drives the encoder's tone input
CONFIG_SIG_TONES ?= n

# P25 and NXDN conventional stay on; the trunked systems (DMR Tier III, P25 trunking) are left out for the RAM
CONFIG_DMR_TRUNK ?= n
CONFIG_P25_TRUNK ?= n

# The advanced DMR services (SMS stays)
CONFIG_DMR_PRIVACY ?= n
CONFIG_DMR_GPS ?= n
CONFIG_DMR_ARS ?= n
CONFIG_DMR_LRRP ?= n
CONFIG_DMR_SIGNALING ?= n
CONFIG_DMR_EMERGENCY ?= n
CONFIG_DMR_ROAMING ?= n
CONFIG_DMR_IP_GATEWAY ?= n
CONFIG_DMR_CONTROL_STATION ?= n
CONFIG_ZONE_SHARE ?= n
CONFIG_SFR ?= n

# The hotspot and the network services on the programming cable
CONFIG_HOTSPOT ?= n
CONFIG_NET_PPP ?= n
CONFIG_SNMP ?= n

# APRS is on: it shares the HR_C7000's FM receive audio copy with the C4FM receiver, one at a time (APRS on the APRS
# frequency in FM, C4FM on C4FM channels; C4FM first, platform/hrc7000/fm_rx_audio.h)

# Receivers and extras this proof of concept leaves out
CONFIG_DUAL_WATCH ?= n
CONFIG_AM_RX ?= n
CONFIG_BCAST_FM ?= n
CONFIG_LANG_ES ?= n
CONFIG_LANG_IT ?= n
CONFIG_LANG_ZH ?= n
CONFIG_LANG_FR ?= n
