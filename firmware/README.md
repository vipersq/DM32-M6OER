<!-- SPDX-License-Identifier: MIT -->
# Firmware images

| File | Built from | Radio |
|---|---|---|
| `DM32-M6OER-v1.1.0.bin` | tag `v1.1.0` (`make BOARD=dm32 VARIANT=c4fm`) | Baofeng DM-32UV |

**Baofeng DM-32UV only.** There is no image for any other radio. Do not flash it to another model.

Flash it with the RadioFoundry installer at radiofoundry.org: Select Features, "Use my own firmware build",
"Firmware file (.bin)". Untested builds can misbehave: keep your codeplug backed up.

The image is a GPL-2.0-or-later work (it contains `core/c4fm/`); its source is this repository at the tag above.
RadioFoundry's files are MIT ([LICENSE](../LICENSE)). All licenses, and what you may do when sharing it:
[LICENSING.md](../LICENSING.md). No warranty; the AMBE codec is not included.
