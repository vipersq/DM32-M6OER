<!-- SPDX-License-Identifier: MIT -->
# C4FM (Yaesu System Fusion DN voice)

The C4FM fork's radio mode: DN voice (V/D mode 2) beside FM, on one frequency. Proof of concept, untested on air.

| Contents | |
|---|---|
| 1 | Scope and license |
| 2 | Why the radio's DMR parts carry C4FM |
| 3 | Layer 2 (core/c4fm/ysf.c) |
| 4 | Receiver and modulator (core/c4fm/c4fm_dsp.c) |
| 5 | Calls (core/c4fm/c4fm_call.c) |
| 6 | Radio core, settings, UI |
| 7 | The board's modem (hal_c4fm.h) |
| 8 | Diagnostics and bench registers |
| 9 | Open questions |

## 1. Scope and license

| Item | |
|---|---|
| Voice | DN: AMBE+2 3600x2450, the frame DMR carries. VW (voice full rate, IMBE): received and played (section "VW"), not sent. V/D mode 1 is shown, not played |
| Identity | A callsign: the radio name's first word (`radio_cwid_callsign`), and a five-character radio ID from the chip's unique ID (`c4fm_radio_id`). No contacts, no talk groups |
| Selector | DG-ID 0-99 (Settings > My radio > C4FM DG-ID). 00 sends with DG-ID 0 and hears every transmission |
| Channels | a codeplug channel's mode byte 2 is C4FM, its DG-ID (0-99) in the color code byte; the VFO uses the DG-ID setting |
| Not done | GPS (DT1 / DT2), data FR mode, picture and text, repeater (Fusion link) features, the chip's own 4FSK demodulator |
| License | `core/c4fm/*` and its tests are GPL-2.0-or-later: the layout facts come from MMDVMHost and MMDVM_CM. The rest of the change is MIT, but a build with `CONFIG_C4FM=y` is a GPL work, and none of it can go upstream (docs/CLEANROOM.md) |

## 2. Why the radio's DMR parts carry C4FM

| C4FM | DMR | So |
|---|---|---|
| 4FSK, 4800 Bd, root raised cosine alpha 0.2 | same | the DMR modem's physical layer modulates C4FM bits |
| dibits 01 +3, 00 +1, 10 -1, 11 -3 | same | the bits go to the modem as they are |
| outer deviation +-2700 Hz, inner +-900 Hz | +-1944 / +-648 Hz | modulation amplitude x 1.39 while transmitting C4FM |
| DN voice: AMBE+2 2450 bit/s, 49 parameter bits per 20 ms | same codec rate and parameter order | the radio's own DMR codec (hal_vocoder_t) speaks C4FM DN; only the FEC around the 49 bits differs |
| 100 ms frames, continuous | 30 ms TDMA slots | the stream is cut into slots back to back (section 7) |

The 49 parameter bits are in the DMR stack's order (`ambeFec.h`): bits 0-11 Golay(24,12) data, 12-23 Golay(23,12)
data, 24-48 unprotected. C4FM sends the same words: MMDVM_CM converts DMR to DN voice by re-coding the FEC only.

## 3. Layer 2

One frame: 120 bytes, 960 bits, 480 symbols, 100 ms. Bits numbered from the MSB of byte 0.

| Bytes | Part | Coding |
|---|---|---|
| 0-4 | sync `D4 71 C9 63 4D` | 20 symbols |
| 5-29 | FICH | 32 bits + CRC-16 -> 4 x Golay(24,12) -> 96 + 4 tail -> rate 1/2 K=5 -> 200 bits, dibit interleave 20 x 5 |
| 30-119 | payload | five 144-bit blocks |

| Payload | Per block | Channel |
|---|---|---|
| Header, terminator | 72 bits CSD1 + 72 bits CSD2 | each: 20 bytes, whitened, CRC-16, rate 1/2 (180 symbols), interleave 20 x 9. CSD1 = destination + source, CSD2 = downlink + uplink (10 characters each) |
| Communications, DN | 40 bits DCH + 104 bits VCH | DCH: 10 bytes, whitened, CRC-16, rate 1/2 (100 symbols), interleave 20 x 5. VCH: 49 bits, bits 0-26 three times, 27-48 once, one zero bit; whitened; bit interleave 26 x 4 |

FICH fields: FI (0 header, 1 communications, 2 terminator), CS 2, CM 1, BN / BT 0, FN 0..FT, FT 6, DEV 0,
MR 0 (direct), DT 2 (DN), DG-ID. CRC-16: polynomial 0x1021, init 0, inverted, big endian. Convolutional code:
G1 = 1 + D^3 + D^4, G2 = 1 + D + D^2 + D^4. The whitening sequence and the interleave tables are in `ysf.c`.

The fields are sent as a Yaesu FT-70D sends them (recorded on air, 2026-11-14), so that repeaters and Wires-X take
the transmission as they take that radio's:

| Field | Content |
|---|---|
| destination (CSD1, DCH FN 0) | `*****` and the radio ID, e.g. `*****K7Q2Z` |
| source (CSD1, DCH FN 1) | the callsign, space padded |
| downlink, uplink (CSD2, DCH FN 2-3), remarks 1-2 (FN 4) | spaces |
| remarks 3-4 (DCH FN 5) | five spaces and the radio ID |
| data (DCH FN 6) | `01 22 61 5F 2B 03 11 00 00 00`, the FT-70D's (meaning unknown) |

The radio ID: five characters 0-9 A-Z, a hash of the chip's unique ID (`hal_system_t.chip`; the callsign without
one): the same across restarts, different between radios. Received, the caller's ID is taken from a destination
`*****ID` or the end of FN 5 (`c4fm.src_id`).

`tests/host/c4fmYsfTest.c` checks every part bit for bit against vectors generated from MMDVM_CM's encoder
(`c4fmOracle.h`).

## 4. Receiver and modulator

| Step | |
|---|---|
| Input | discriminator audio, 8 kHz, 16 bit (the board's modem, section 7) |
| Matched filter | interpolation by 3 through the RRC (alpha 0.2, 8 symbols, 41 taps): 24 kHz, 5 samples per symbol. 8 kHz is enough: the signal's band ends at 2.88 kHz |
| Search | every sample: the last 20 symbols' samples correlated with the sync, DC removed, normalized by their mean absolute deviation. Above 0.75 of the ideal: the best point of the next 5 samples is the frame start |
| Levels | least squares fit of the 20 sync samples to their levels: gain and center. A negative gain is an inverted discriminator; nothing else changes |
| First lock | at most 4 of the 40 sync bits wrong. Tentative until the next sync turns up where it is due: meanwhile a sync with at most 2 errors elsewhere takes over. Receiver noise passes 4 errors every 3-5 minutes (SR5WI: 45 in 3 h), and such a lock used to hold the receiver for 400 ms (3 flywheel frames) while a real transmission's header went by. A stricter first lock would cost a low-passed channel half its frames, and the noise correlates like a weak signal |
| Tracking | the next sync is looked for within +-2 samples of where it is due, above 0.5 of the ideal and within 6 dB of the last level |
| Loss | a missed sync is bridged on the old timing (flywheel frame) up to 3 times, then the receiver searches again |
| Tone filter | some repeaters send a CTCSS tone under the 4FSK. Each data symbol's slicing error less the one a frame earlier at the same place (what repeats every frame is a comb at multiples of 10 Hz, not a tone) runs through a Goertzel filter per standard tone (50, 67.0-254.1 Hz) over blocks of 4 frames (2.5 Hz resolution; syncs and flywheel frames as zeros; each bin weighted for the difference's gain on noise there); a tone 6 times above the mean of the others (its neighbors left out) in two blocks running selects a notch (Q 20) on the 8 kHz input; a neighbor of the tone in use takes over only at twice its power. Every new lock starts with no tone. Always on (debug link SETTING 43 turns it off for an A/B) |
| Equalizer | 7 taps a symbol apart around each data symbol, decision-directed normalized LMS (step 2^-6) towards the decided level, DC left out; the center tap held within 0.7-1.2, a side tap past 0.5 resets it to a pass-through; not adapted on flywheel frames; every new lock starts from a pass-through. Always on (debug link SETTING 42 turns it off for an A/B) |

Integer arithmetic only. About 14 multiply-adds per 24 kHz sample for the filter, 20 more while searching.
`c4fmDspTest.c`: frames bit exact through timing offsets, DC, inversion, a 300-unit level; the FICH survives noise at
0.35 of the inner level; no false frame in 10 s of noise; with the equalizer, a clean signal stays bit exact and a
low-passed noisy one loses about a third of the bit errors it had without.

The equalizer: a signal through a repeater reaches the discriminator with less of its upper band than a radio's
direct signal (a least squares equalizer fitted to the SR5WI captures boosts the band edge, alternating taps; the
direct captures need none). That is inter-symbol interference, which the matched filter alone leaves in.
On the eleven repeater captures of 2026-11-14 (`c4fmReplay --eq 7,5,6`) the voice BER drops from 5.6 % to 3.5 % and
15 % more FICHs decode with hard decisions. Symbol timing is not the problem: the sync spacing is exact and the best
sampling instant does not drift inside a frame.

The tone filter: SR5WI sends 127.3 Hz under its C4FM (the slicing error's spectrum has a line there, up to 49 times
the floor, on every capture through it; none on a direct one). Yaesu receivers ignore it; this receiver took it for
level errors. On the ten voice captures (`c4fmReplay --tone --eq 7,5,6`, the tone found in each within the 2 s) the
voice BER drops from 5.6 % to 2.2 % with the first version (a tone from one block, kept from call to call); with a
notch from the first frame, to 1.7 %. That version failed on the bench (2026-11-15): a direct signal sending the same
voice frame over and over (silence) leaves a comb of lines at multiples of 10 Hz in the errors, which the 2.5 Hz bins
took for 131.8 / 199.5 Hz; and a notch, once in, cuts its band out of the signal, which the errors show as its tone,
so a wrong pick stayed until a restart. Now the errors are differenced frame to frame, a tone has to win two blocks,
and each lock starts afresh: the 2 s captures through SR5WI come to 1.6 % (the tone is notched from about 0.8 s on),
no tone is found on the direct ones. A tone within about 1 Hz of a multiple of 10 Hz (100.0, 179.9, 189.9, 199.5 Hz)
is differenced away as well and is not found. `c4fmDspTest` checks that a 127.3 Hz tone of 0.4 of a level is found
and that none is picked without one.

The equalizer's gain is the sync fit's: with a free center tap it had a second resting point near 0.4, where the
outer levels fall on the inner ones, every decision is +-1 and the updates hold it there. A radio on the bench sat at
0.355 and decoded no FICH, from the strongest signal, until a restart (nothing reset the weights but a transmission).
`c4fmDspTest` starts it at 0.38 and expects a clean decode.

Soft decisions: with each frame the receiver hands over a soft value per bit (`c4fm_frame_info_t.soft`: the
sample's distance from the decision threshold, 32 to a level unit; the dibit's MSB is -z, its LSB |z| - 2). The
`ysf*DecodeSoft` decoders run a soft decision Viterbi on the FICH, CSD and DCH and a weighted majority on the VCH's
three copies of bits 0-26. They are the only decoders the radio uses (the hard decision ones remain for the host tests).
The demodulator itself is the software one in all cases. On five transmissions captured through a repeater (raw BER 5-8 %) they decode 90-100 % of the FICHs
where hard decisions decode 50-60 %, and nearly every DCH; `c4fmDspTest` keeps the gain at noise of half a level.
The VCH's bits 27-48 have no FEC, so a noisy channel still degrades the voice: the radio's DMR demodulator (4FSK in
the HR_C7000) is the next step for weak signals.

The modulator (RRC at 24 kHz, decimated to 8 kHz) serves the host tests.

## 5. Calls

| Phase | Transmit | Receive |
|---|---|---|
| Start | 100 ms of `0x77` (+3 -3 ...), header (CSD1 `*****` + radio ID / our callsign) | a header, or a communications frame whose FICH decodes (late entry) |
| Voice | a frame per five voice frames; DCH cycling by FN | DN: DCH by FN (FN 1 the caller's callsign), five AMBE frames to the voice path. Another DT: shown, not played. Playback starts 60 ms after the call's first frames (`ambeVoiceSetRxPrimeWait`): the five come at once, at the first poll (every 25 ms) after the frame's end, and played on arrival the queue ran dry whenever a frame came a poll late (a repeated 20 ms each time) |
| Gaps | the modem kept 100-200 ms ahead; a frame still short of voice when the modem runs low is padded with silence | a frame whose FICH fails inside a call is taken as the communications frame due |
| End | PTT up: the voice left (padded), the terminator, unkey once it has played out | the terminator, a lost signal, or 400 ms without a frame |

DG-ID: with ours at 0 every transmission plays; else only those carrying ours (the rest show as busy).

The voice path is the DMR stack's (`ambeVoice`: vocoder, PCM, concealment). The engine converts 49 bits to and from
the 72-bit DMR air frame with `ambeFec`, which is lossless both ways. `c4fmCallTest.c`: two radios through the
modulator and receiver; every voice frame arrives in order and unchanged, the header and late entry give the caller,
another DG-ID doesn't play.

### 5.1 Wires-X

A radio controls its Wires-X node with short data transmissions: data FR mode (FICH DT 1), the same frame layout
both ways. The header, FN 0 and the terminator carry CSD1 / CSD2 (as a DN header); FN 1 carries CSD3 (remarks 1-4:
spaces, our radio ID last) and data bytes 0-19; each next FN 40 bytes. FT is the last FN. The two 20-byte channels
of a frame are coded as CSD1 / CSD2 (`ysfCsdEncode`).

| Data | |
|---|---|
| Command (ours) | `00`, the command (DX `5D 71 5F` status, CONNECT `5D 23 5F`, DISCONNECT `5D 2A 5F`, ALL `5D 66 5F`), `26`, the parameters in ASCII (CONNECT: the room ID in 5 digits and a space), `03`, the byte sum of all before it |
| Reply (the node's) | `[1..4]` the reply code (`5D 51 5F 26` DX, `5D 41 5F 26` connect / disconnect), `[5..9]` the node's ID, `[10..19]` its callsign, `[20..33]` its name, `[34..35]` `15` in a room / `12` not, `[36..40]` the room's ID, `[41..56]` its name; `03` and the byte sum |
| Search | ALL's command with the parameters `11`, the first entry and the text (16 characters, upper case); the reply is the list's, with `02` at `[5..6]`, `1` at `[22]` and the count in `[23..24]` (`01` and none: nothing found) |
| Room list (ALL) | the command's parameters `01` and the first entry in 3 digits; the reply `5D 46 5F 26`, `[22..24]` rooms on the page, `[25..27]` rooms in all, then 50 bytes a room (`[1..5]` ID, `[6..21]` name, `[22..24]` users). About 1030 bytes: several blocks, each next block's FN 1 starting with a `00` before its 19 bytes; BN has 2 bits, so the receiver counts blocks as BN moves on (a fifth block wraps to 0) |

The layout follows a node's implementation (MMDVM_CM `WiresX.cpp`): a node checks the command at `[1..3]` and the
parameters from `[5]`. Bytes `[0]` and `[4]` of a command are not checked there and copy the replies' layout; a Yaesu
radio's command captured with `c4fm_bench.py wx` settles them. Tried with an MMDVM hotspot (YSFGateway): status,
connect and the reply work.

| UI | |
|---|---|
| # on Home on a C4FM channel or VFO | asks the node for its status (DX) and opens Menu > C4FM > Wires-X |
| Wires-X page | Node (its name; OK asks again), Room (the room it is in, or none; "waiting..." / "no answer" after a command), Rooms (the node's list, 20 a page: OK on a room connects to it, the last row asks for the next page), Search (a name or its start; the matches as the list), Room ID (five digits, kept), Connect, Disconnect |
| Keying | a command keys the radio as a short PTT and is refused as PTT would be (band, RX only, no callsign); the refusal clears by itself after 2 s |

`c4fmCallTest` sends CONNECT through the modulator and checks the frames, then plays a node's replies into the
receiver: the connect reply (the room) and a room list of five blocks.

## 6. Radio core, settings, UI

| Item | |
|---|---|
| Mode | `RADIO_MODE_C4FM`, offered with `board.c4fm`. The VFO's Mode tile cycles FM -> C4FM -> DMR -> P25 -> NXDN (P25 and NXDN where the board runs them). `RADIO_MODE_C4FM` is 4, after RadioFoundry's P25 (2) and NXDN (3); the codeplug's mode byte stays 2 |
| Board | `set_mode(C4FM)`: the RF side as for DMR, then the C4FM modem. PTT keys the board as for DMR (`key_tx` at 12.5 kHz); the engine starts the stream; the core unkeys once the engine is off the air |
| State | RX / TX follow the engine's phase; the time-out stops the transmission with its terminator |
| Settings | `settings_t`: `wx_room` (the Wires-X room ID entered), `c4fm_dgid`, `c4fm_tx_level` (%, 0 = 100), `c4fm_preamble` (100 ms units, 0 = 1), `voice_rx_gain` (dB), `c4fm_flags` (bit 0 plain fields, bits 1-2 the decoder), `c4fm_call` ("" = the radio name's first word). An older record reads 0: the defaults |
| Power | every level of the board, as in DMR |
| Channel editor | Mode cycles FM -> DMR -> C4FM; a C4FM channel shows DG-ID (0-99) in place of DMR's and FM's rows. The web codeplug editor (`site/static/js/editor`) has the same mode and field |
| Home | talk row: our callsign and DG-ID, or the caller's while receiving; chips: DN, power, 12.5k; activity: the caller and the mode heard, then "Last heard" |
| Build | `CONFIG_C4FM` (needs `CONFIG_DMR`); `make VARIANT=c4fm`: FM, DMR (voice, channels, contacts, SMS), C4FM and APRS, without IP, the cable's network services, the hotspot and the advanced DMR services. APRS and the C4FM receiver share the HR_C7000's FM audio copy one at a time, C4FM first (`platform/hrc7000/fm_rx_audio.h`) |

The menus in the C4FM build (`core/ui/ui_menus.c`), by what one does; every setting has one place:

| Menu row | Opens |
|---|---|
| Switch to VFO / Memory | toggles, back to Home |
| Zones, Channels | as in RadioFoundry |
| This channel | the channel or VFO in use: 25 kHz, RX / TX tone (CTCSS / DCS), the VFO's repeater offset, DG-ID; rows of another mode dimmed. The row shows the mode |
| Calls | Last caller, Recent DMR, Last heard C4FM, Contacts, Messages (the unread count on the row) |
| Wires-X | the Wires-X page (5.1) on a C4FM channel, else "C4FM only" |
| Tools | the tools this build has, numbered in a row (Scan, Position, APRS, Battery); Diagnostics is in Settings > Advanced |
| Settings | Sound; Roger & tones (roger sound, key-up / unkey / RX tones, their style); Display; Tactical (everything tactical at once, red screen, red brightness, LEDs off); Talk & voice (TX timeout, mic gain / AGC / noise reduction, voice volume); My radio (DMR ID and name, C4FM callsign); Modes (FM squelch and busy lockout, DMR reply window / monitor / talker alias / data and messages, C4FM TX deviation / preamble / FT-70D / test tone); Location; Battery & keys; Advanced (About, codeplug, diagnostics, reset, crash log, VFO and broadcast steps, dual watch, SFR, RX past bands, frequency trim) |

`tests/host/c4fmUiSmoke.sh` opens every page at each display class in the simulator built with `CONFIG_C4FM`.

## 7. The board's modem

`hal_c4fm.h`: a byte stream at 9600 bit/s out (DMR dibit mapping, C4FM deviation, idle `0x77` when the queue runs
dry), discriminator audio in at `rx_rate_hz`. The HR_C7000 version (`drivers/modem/hrc7000/hrc7000_c4fm.c`):

| Direction | How |
|---|---|
| Transmit | the physical layer continuous mode (WORK_MODE 0x02, LAYER2_CONTROL 0x82), 36 bytes per 30 ms slot to both TX RAM halves at each slot boundary (the first PHY_RDY or timeslot interrupt), as the split duplex TX half does ([modem-hrc7000.md](modem-hrc7000.md) 2.10). RF_MOD_BIAS_CTRL's two modulation amplitudes x 1.39 while keyed. RF chip and PA keyed once at the first RF TX interrupt; 200 ms without a slot interrupt drops the PA |
| Receive | the chip in FM copies its AF input to the RX RAM at 8 kHz ([modem-hrc7000.md](modem-hrc7000.md) 8.5), the voice band-pass off (FM_BANDWIDTH 0x0F); the RF chip in its DMR set-up (flat discriminator). FM playback off: the speaker gets the decoded voice through the prompt FIFO |
| Speaker | the prompt FIFO also feeds the modulator: its gate closes while C4FM transmits (`hrc_dmr_tx_active`) |

## 8. Diagnostics and bench registers

Debug link view, section `c4fm`:

| Key | |
|---|---|
| `c4fm.phase`, `src`, `dst`, `dgid`, `dt`, `late` | the engine (phase: 0 idle, 1 RX voice, 2 RX busy, 3 TX, 4 TX ending, 5 denied) |
| `c4fm.dsp_syncs`, `dsp_losts`, `dsp_flywheels` | syncs found, signals lost, frames bridged |
| `c4fm.rx_gain`, `rx_center`, `rx_inverted` | the last level fit: gain per level unit (inner = 1), DC, polarity |
| `c4fm.rx_eq` | the equalizer is on (SETTING 42 sets it until the next configuration) |
| `c4fm.rx_tone_on`, `rx_tone` | the tone filter is on (SETTING 43), the tone it notches in Hz (0.0 none found) |
| `c4fm.rx_frames`, `rx_fich_bad`, `rx_vch_errors`, `rx_ambe` | frames taken, FICH failures, voice triplets corrected, voice frames played |
| `c4fm.rx_fich_fec` | FICH bits the Viterbi decoder corrected: raw bit error rate about `rx_fich_fec / (200 * good FICHs)` |
| `c4fm.tx_frames`, `tx_padded` | frames sent, voice frames padded with silence |
| `c4fm.drv_*` | the driver: key-ups, slots, slots from the stream / with idle, stalls, interrupts, level, RF_MOD_BIAS_CTRL for DMR and for C4FM |

Section `c4fm_raw`: the last call's fields as another radio sent them, cleared when a call starts. `c4fm.raw_fich`
(fi,cs,cm,bn,bt,fn,ft,dev,mr,voip,dt,sql,dgid of the last good FICH), `raw_hdr_csd1`, `raw_hdr_csd2`,
`raw_term_csd1`, `raw_term_csd2` (20 bytes in hex), `raw_dch0` .. `raw_dch7` (10 bytes, by FN); `-` for a part not
received with a good CRC. `c4fm.raw_serial` counts captures.

The voice settings are the DMR stack's and apply to C4FM: mic gain (SETTING 29), encode options (SETTING 31),
the test tone (RADIO op 33); their state is in the `dmr` and `features` sections (`voice.*`, `set.mic_gain`).

Section `c4fm_cap`: the bench capture (`c4fm_capture_t`). The receiver's input audio goes through a 2 s ring
(16000 int16 samples at 8 kHz) and the last 32 frames' figures (sync errors, levels, FICH, FEC) into records. SETTING
40 arms it (1: freeze 1.5 s after the next call starts), freezes it now (2) or lets it run (0); the keys give the
state and the RAM addresses, which READ_MEM reads. `tools/c4fm_capture.py` does it per transmission and
`tests/host/c4fmReplay` runs the receiver on a capture on the host.

`tools/c4fm_bench.py` records overs with these keys, works out error rates and decodes the fields (the boards'
bench procedures in `docs/testing/`).

CHIP_REG (`rfoundry_debug.py reg CHIP REG [VALUE] --page PAGE`), boards that build C4FM:

| Chip | Registers |
|---|---|
| 1 | the HR_C7000's modem registers, offset = page << 8 \| reg: RF_MOD_BIAS_CTRL 0x114 (live, also while keyed), RF_MODE 0x104 (b23:16 AF gain), FM_BANDWIDTH 0x500 (receive filters, rewritten at every receive set-up) |
| 2 | register 0: the C4FM TX level in percent of DMR's modulation (139), from the next key-up |

## 9. Open questions

1. The deviation the scaled RF_MOD_BIAS_CTRL gives: linear? Measured against a C4FM receiver's or a service monitor's.
2. Measured: the FM receive copy is taken before the chip's FM filters (FM_BANDWIDTH doesn't change it). Through
   a repeater at -83..-100 dBm the receiver gave a raw BER of 4-8 % (direct, strong: 0.3 %), where Yaesu radios
   decode cleanly. Most of it was the repeater's CTCSS tone under the 4FSK (the tone filter, section 4) and
   inter-symbol interference (the equalizer): with both, about 1-2 %. The RF chip's other set-ups were tried and
   dropped: its own FM demodulator gives voice-filtered, de-emphasized audio (no-signal noise 75 % at 300-1000 Hz,
   2 % under 300 Hz) on which the receiver finds no sync; the 25 kHz filter gained nothing. The DMR set-up is fixed.
   Next, if weak signals still need it: the chip's own 4FSK demodulator.
3. Whether mic PCM and the vocoder keep their timing in the physical layer mode (the split duplex TX half fed its
   voice from the host).
4. C4FM receivers' lock on our header after 100 ms of preamble (Yaesu radios key with a longer one).

## Roger sounds (all modes)

`core/radio/roger_sound.c` (MIT): one table of end-of-transmission sounds, chosen in the Roger sound row of the FM, DMR
and C4FM pages (one setting: `radio_settings_t.roger_beep` on/off and `roger_sound`, stored in `settings_t.roger_sound`).
Off, Classic (1200 / 900 Hz, the original), Beep, Double beep, Two-tone up / down, Triple chirp, Sweep up / down,
Bee-boo, Morse K, Morse R, Quindar. Steps are on the radio core's 25 ms poll grid.

| Mode | How it is sent |
|---|---|
| FM | tones on the carrier through `hal_radio_t.tx_tone`, one step per poll (`roger_poll`), then the unkey |
| DMR, C4FM | on PTT release the AMBE+2 encoder takes the sound in place of the mic (`ambeVoiceTxTone`); the stack keeps PTT down until the encoder has played it and `RADIO_DIG_ROGER_GUARD_MS` (200 ms) of queued frames have gone out (`dig_roger_poll`), then releases as usual (DMR: terminator after the superframe; C4FM: terminator frame). PTT again during it cancels the sound and the over goes on |

Host tests: radioFeaturesTest `test_roger_sounds` (FM steps, the table, the generator), radioCoreTest
`test_dmr_roger` (DMR through the encoder). C4FM shares the encoder path; it has no radio core host test.

## VW (voice full rate)

`core/c4fm/ysf_vw.c`: a VW frame (FICH DT 3) carries the full-rate vocoder's 144-bit words at payload bytes 0, 18, 36,
54 and 72, the same words as a P25 LDU's voice frames (four Golay(23,12), three Hamming(15,11), seven bits bare, PN
masks from the first word); the first frame after the header (FN 0 of FT 1) has data in its first 45 bytes and voice in
the last two words only. The bit map is RadioFoundry's P25 one (core/p25/p25_unit.c), checked bit for bit against the
YSF layout. The words go through `c4fm_call_set_imbe` to `ambeVoiceImbeFrame`, which queues them with the DN frames;
the audio callback decodes them with RadioFoundry's full-rate decoder (`core/mbe`, MIT, tables from mbelib under ISC),
which repeats and then mutes on bad frames. RAM: about 3.4 KB (the decoder's state and scratch). Receive only.
Host test: `c4fmVwTest`.

## Tactical mode and tones

Settings > Tactical (the last group in Settings), with a Tone style (Deep, the default; Bloop; Low chirp; High chirp; Harris-like, measured from a recording: key-up 1000 Hz 200 ms, unkey 1000 Hz 130 ms with a buzz, reception start / end a buzzing 640 Hz 90 / 140 ms, the buzz `BEEP_HZ_BUZZ` = the 3rd and 5th harmonics; `beep_tac_style`, a change plays the key-up tone at once): Tactical mode (the screen in shades of red: `gfx_set_red`, each palette
color sent to the display as red of the same brightness, the dimmest lifted to stay visible; the 16-color display's
palette is the one place colors leave, so icons, meters and anti-aliased text follow), Red brightness (the backlight
in tactical mode, Normal = the usual brightness; `settings_t.tac_ui`, `tac_bright`), LEDs off (`radio_settings_t.leds_off`:
the lamps stay dark, the view still has what they would show) and the three tones below. Host tests: gfxTest
`test_red`, radioFeaturesTest `test_tactical_tones`.

The tones: Key-up tone, Unkey tone, RX tone, each on its own, off by default
(`radio_settings_t.tac_*`, `settings_t.tac_flags`). Short speaker sounds in the style of a military handheld, every mode
(`beep.c` BEEP_TAC_*). The key-up tone plays before keying: `ptt_down` holds (`tac_hold`) and `tac_service` keys once
the tone is over (or after its length + 100 ms if the stream never ran it); PTT let go meanwhile keys nothing. The unkey
tone follows the end of a transmission (after the roger sound), the RX tone the squelch or a call opening. None on a
no-beep channel. Host test: radioFeaturesTest `test_tactical_tones`.

## RAM

`CONFIG_C4FM_CAPTURE` (off by default) holds the bench capture's 2 s input ring (32 KB): off, the frame records stay
and the ring is one sample. The C4FM build's RAM went from 94 % to 79 % with it off.

## Roger & tones per mode, the reception end tone

Settings > Roger & tones has, beside the roger sound and the tactical tones, a switch per mode for each: Roger in FM /
DMR / C4FM and Tones in FM / DMR / C4FM (`radio_settings_t.roger_off`, `tones_off`: a bit per `radio_mode_t`, set =
off; `settings_t.tac_modes`; all on by default). The mode is the channel's in use. RX end tone (`tac_rxend`,
`BEEP_TAC_RXEND`, in every style) sounds when a reception ends (the squelch or the call closing), not when our own
key-up cuts it. Everything tactical includes it. Host tests: radioFeaturesTest `test_tactical_tones` (FM),
radioCoreTest `test_dmr_roger` (Roger in DMR off).

## APRS beside C4FM

The C4FM variant has APRS on again (Tools > APRS, Settings > Location). The HR_C7000 copies its FM audio to one user
at a time (`platform/hrc7000/fm_rx_audio.c`, an owner per start): APRS on the APRS frequency in FM, the C4FM receiver
on C4FM channels. C4FM's start takes the copy from APRS (a channel change before APRS's next poll lets go); APRS's read
and stop do nothing while C4FM has it, its start fails. Host test: `fmRxShareTest`. RAM with APRS: 86 %.


## P25 and NXDN beside C4FM

The fork follows RadioFoundry's P25 and NXDN conventional modes (docs/spec/p25.md, nxdn.md): channels of those kinds,
the VFO's Mode tile, the channel editor's Mode row (FM, DMR, C4FM, P25, NXDN). The C4FM variant leaves the trunked
systems out (`CONFIG_DMR_TRUNK`, `CONFIG_P25_TRUNK`) and keeps the roger sounds in place of RadioFoundry's signaling
tone chains (`CONFIG_SIG_TONES=n`: one or the other drives the encoder's tone input; `ambeVoiceRogerTone` here).
Settings > Roger & tones has Tones in P25 and Tones in NXDN; `settings_t.tac_modes` and `roger_modes` are a bit per
`radio_mode_t` each. A settings record from the fork before this (its fields at bytes 395-423) loads with every field
where it is now (settings.c `fork_record_move`); the stored power levels go back to the defaults once.

## Quick pages, DMR receive, mic gain, dual watch

- Quick (`ui_quick_pages`): # turns pages. FM, C4FM, P25 and NXDN have two, DMR three. The mode's own page comes
  first; C4FM's has DG-ID, P25 and NXDN's voice volume. DMR's second page is receive: Both slots, Any TG, Any CC,
  RX TG, digital monitor, Talk to, voice volume, slot. The last page is sound and tactical: roger, tactical tones, red
  screen, mic, tone style, the reception end tone, LEDs. Tile 4 stays Mode (Copy to VFO on a channel).
- DMR receive (`radio_settings_t.dmr_rx`, `dmr_rx_tg`; `settings_t.dmr_rx`, `dmr_rx_tg[3]`): both timeslots (the
  first call heard plays until it ends: one vocoder), every talkgroup, any color code, each on top of the channel and
  the digital monitor; and one talkgroup heard on every DMR channel (the stack's `group_ok`). Home shows "TS1+2" and
  "CC1*"; a call shows its slot, its color code where it isn't the channel's, and its talkgroup. PTT stays on the
  channel's slot, color code and talk target. Also in Settings > Modes.
- Mic gain: the DM-32 now takes Settings > Mic gain in FM too (`hal_radio_t.mic_gain`), and starts 5 dB (FM) and
  3 dB (digital) above the old fixed level.
- Dual watch is on in the C4FM variant (Quick tile 7); the UI stack grows by 320 bytes and the RTOS heap by 2 KB.

## ID database callers, mic compressor, side keys

- The DMR ID database (the RadioID list the editor uploads, `dmr_iddb.h`): a DMR call shows the words after the
  callsign (name, city) on a line of their own when no talker alias is shown (`radio_dmr_call_t.src_info`, from
  find_name's lookup cache: no extra flash read). A P25 call shows the callsign for its unit ID where the database
  knows it (amateur P25 uses DMR IDs); NXDN's own numbers are not in it.
- Mic compressor (Settings > Talk & voice, `settings_t.mic_comp`, `ambeVoiceSetCompressor`): before the vocoder
  (DMR, C4FM, NXDN). Off, Light (threshold -12 dBFS, 3:1, +5 dB) or Strong (-18 dBFS, 5:1, +9 dB); 5 ms attack,
  150 ms release, a gate near -40 dBFS keeps the background where it was, a soft limit above -2.5 dBFS. FM keeps the
  board's mic gain; with a board mic gain Mic AGC no longer overrides it.
- Side keys (Settings > Keys): Tactical, Tactical tones, Roger on/off, APRS position, numbered 200-203 in the
  codeplug's key slots so RadioFoundry's own later numbers never read as them.
- APRS messaging and SmartBeaconing are RadioFoundry's (Tools > APRS, Calls > Messages, Settings > Location).

## Data link for RadioLink (ATAK)

- Tools > Data link (the last tool): KISS on USB (Off, On, Data; the same setting as Tools > APRS), the data
  frequency and its bandwidth (12.5 kHz unless set to 25). Press 2 while the radio is on the data frequency (an FM
  channel or the VFO) to take it; hold 2 to go back to the APRS frequency. The body says whether the radio sends
  only, or sends and listens (on that frequency in FM). APRS beacons stay on the APRS frequency ([aprs.md 9](aprs.md)).
- The host's 'Q' query (KISS SetHardware) answers the queue, why the data frequency is blocked, the KISS mode and the
  callsign, without transmitting; RadioLink's "Check radio" uses it.
- Mode (row 4): FM, or DMR: the host's frames go as DMR data on the DMR channel the radio is on, to its talkgroup
  (UDP port 4200, compressed header, unconfirmed), and those heard there come back to the host
  ([aprs.md 9.2](aprs.md#92-data-link-over-dmr)). The body names the talkgroup, or says what is missing (a DMR channel
  with a talkgroup, a DMR ID). The status answer's mode is then 3.
- Relayed frames, the Data link SOS key and plain DMR radios' positions and texts for the host:
  [aprs.md 9.3, 9.4](aprs.md#93-relayed-frames-and-the-sos-key).
