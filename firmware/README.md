<!-- SPDX-License-Identifier: MIT -->
# Firmware images

| File | Built from | Radio |
|---|---|---|
| `DM32-M6OER-v1.0.0.bin` | tag `v1.0.0` (`make BOARD=dm32 VARIANT=c4fm`) | Baofeng DM-32UV |

Flash it with the RadioFoundry installer at radiofoundry.org: Select Features, "Use my own firmware build",
"Firmware file (.bin)". Untested builds can misbehave: keep your codeplug backed up.

The image is a GPL-2.0-or-later work (it contains `core/c4fm/`); its source is this repository at the tag above.
RadioFoundry's files are MIT ([LICENSE](../LICENSE)).
