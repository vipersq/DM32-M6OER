#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 The RadioFoundry Authors
# The C4FM build's menus (docs/spec/c4fm.md 6) in the simulator built with CONFIG_C4FM (uiSimC4fm): each page and
# screen opens and draws at every display class. Menu: 0 Switch to VFO/Memory, 1 Zones, 2 Channels, 3 This channel,
# 4 Calls, 5 Wires-X, 6 Tools, 7 Settings. Settings: 1 Sound, 2 Roger & tones, 3 Display, 4 Tactical, 5 Talk & voice,
# 6 My radio, 7 Modes, 8 Location, 9 Battery & keys, 0 Advanced. Calls: Last caller, Recent DMR, Last heard C4FM,
# Contacts, Messages
set -e
fail=0
check() {	# script, expected screen
	for g in 240x320 160x128 160x120i5 128x64; do
		if ! ./uiSimC4fm --geom "$g" --script "$1 expect:screen=$2" --out /dev/null >/dev/null 2>&1; then
			echo "FAIL c4fm menu [$g]: '$1' should reach $2"
			fail=1
		fi
	done
}
check "ok" hub
check "ok 3" group
check "ok 4" group
check "ok 4 down ok" recent
check "ok 4 down down ok" c4fm-heard
check "ok 4 down down down ok" contacts
# Wires-X from Menu: a C4FM channel only (elsewhere a toast, Menu stays)
check "ok 5" hub
check "ok ok quick 4 back ok 5" group
# Wires-X: # on Home on a C4FM VFO (Switch to VFO, Quick tile 4 = mode -> C4FM) opens its page; a room and Connect
check "ok ok quick 4 back ctx" group
check "ok ok quick 4 back ctx down down down down ok 2 8 6 1 7 ok down ok" group
check "ok ok quick 4 back ctx down down down ok" wx-search
check "ok ok quick 4 back ctx down down down ok 7 ok" wx-rooms
check "ok ok quick 4 back ctx down down ok" wx-rooms
check "ok ok quick 4 back ctx down down ok down ok" group
check "ok 7" settings
for n in 1 2 3 4 5 6 7 8 9 0; do
	check "ok 7 $n" group
done
check "ok 7 6 down ok" c4fm-call
check "ok 7 7 down down down down down ok" group
check "ok 1" zones
check "ok 6" tools
check "ok ok" home
[ $fail = 0 ] && echo "c4fmUiSmoke: passed" || { echo "c4fmUiSmoke: FAILED"; exit 1; }
