<!-- SPDX-License-Identifier: MIT -->
# Licensing of DM32-M6OER

DM32-M6OER is free software. You may use, copy, share, change and sell it, under the terms below.

## The firmware image as a whole

The firmware image (`firmware/DM32-M6OER-*.bin`, built with `make BOARD=dm32 VARIANT=c4fm`) is a
**GPL-2.0-or-later** work, because it contains `core/c4fm/` (GPL-2.0-or-later). The full license is in
[LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt).

**Source code:** the complete source for each image is this repository at the image's tag (for example `v1.1.0` for
`DM32-M6OER-v1.1.0.bin`), with the build command above. Anyone who receives the image may get, change and share the
source under the same license.

## What the image contains

These are all the licenses of the source files compiled into the image (read from the build's dependency files).

| Part | License | Copyright |
|---|---|---|
| RadioFoundry (most of the tree, FreeRTOS kernel) | MIT ([LICENSE](LICENSE), [LICENSES/MIT.txt](LICENSES/MIT.txt)) | The RadioFoundry Authors ([AUTHORS](AUTHORS)); FreeRTOS: Amazon.com, Inc. |
| C4FM / Yaesu System Fusion (`core/c4fm/`) | GPL-2.0-or-later ([LICENSES/GPL-2.0-or-later.txt](LICENSES/GPL-2.0-or-later.txt)) | its authors, see the file headers |
| IMBE voice decoder (`core/mbe/imbe_*`) | ISC ([LICENSES/ISC.txt](LICENSES/ISC.txt)) | mbelib Author |
| JetBrains Mono font (`core/ui/fonts/`) | SIL OFL 1.1 ([LICENSES/OFL-1.1.txt](LICENSES/OFL-1.1.txt)) | The JetBrains Mono Project Authors |
| This build's changes (DM32-M6OER, 2026) | the license of each file they are in | Kriss M6OER |

The DM32-M6OER changes are listed at the top of [README.md](README.md) and recorded in the git history.
Every file keeps its own `SPDX-License-Identifier` and copyright lines.

Other licenses in the tree (Apache-2.0, BSD-3-Clause in `third_party/` such as CMSIS and TinyUSB) belong to code that
the DM-32UV image does not compile.

## Not included

- **The AMBE+2 voice codec** (DVSI, proprietary) is not in this repository or in any image. It comes from your own
  radio's stock firmware ([codecs/README.md](codecs/README.md)).
- **Baofeng's stock firmware, bootloader or keys** are not included.

## Not affiliated, no warranty

DM32-M6OER is a personal build. It is not made, endorsed or supported by the RadioFoundry project, the RadioFoundry
C4FM project, Baofeng, Yaesu or DVSI. Names such as Baofeng, DM-32UV, Yaesu, System Fusion and ATAK belong to their
owners and are used only to say what this works with.

**There is no warranty**, as GPL-2.0 sections 11 and 12 and the MIT license state. Changing a radio's firmware may
void its warranty and its type approval. The firmware is for licensed amateur radio use; each user is responsible
for transmitting only where their licence allows.
