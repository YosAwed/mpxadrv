# mpxadrv

`mpxadrv` is a native macOS command-line player for MADRV/MXDRV-compatible
`.MDX` music files, MADRV `.MDR` extended MIDI songs, and their `.PDX` samples.
It uses Core Audio for playback and CoreMIDI for external MIDI output, and can
render MDX and FM/PCM-only MDR songs directly to 16-bit PCM WAV files or export
MIDI to Standard MIDI Files. MADRV `.TDX` bank definitions can be compiled or
loaded transparently during MDX playback.

The original Human68k source and reference music under `Reference/` are used
only as local compatibility references. They are intentionally excluded from
Git because the supplied `MADRVSRC/README.DOC` prohibits redistribution of the
source.

At startup the CLI preserves the original compiler attribution and identifies
the derivative macOS implementation separately:

```text
Based on MADRV MUSIC CONVERTER Version 1.10 (c)1991,92 Konoa
macOS CLI adaptation developed by Awed (c)2026
```

## Requirements

- macOS 11 or later (Apple Silicon or Intel)
- Xcode Command Line Tools
- CMake
- [mdxmini](https://github.com/mistydemeo/mdxmini) 2.0 or later
- [FluidSynth](https://www.fluidsynth.org/) 2.5 or later

Install the build dependencies with Homebrew:

```sh
brew install cmake mdxmini fluid-synth
```

`mdxmini` is GPL-2.0-or-later software and FluidSynth is
LGPL-2.1-or-later. A distributed build of this application must comply with
those licenses. This repository does not copy or vendor either dependency.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is created at `build/mpxadrv`.

### Interactive song menu

To select songs without typing each filename, change to a folder containing
MDR/MDX files and run the terminal menu:

```sh
cd Reference/MDR
../../scripts/mpxadrv-player.command
```

The menu lists the current folder, plays the selected number, and returns to
the list when playback finishes. `r` refreshes the folder and `q` exits. A
folder can also be passed explicitly:

```sh
scripts/mpxadrv-player.command /path/to/music
```

The script automatically uses `build/mpxadrv` and the local ignored
`SoundFonts/Roland_SC-55.sf2` when available. Override the binary or SoundFont
with `MPXADRV_BIN` or `MPXADRV_SOUNDFONT`. To send MIDI to a USB / physical
module instead of the software synth, set `MPXADRV_DESTINATION` to a
`midi-list` index or destination name (this disables the SoundFont path).

## Usage

Play an MDX file:

```sh
./build/mpxadrv song.mdx
```

Play an MDR file through the multitimbral software MIDI synthesizer included
with macOS:

```sh
./build/mpxadrv song.mdr
```

MDR is MADRV's `#EX-MDR` form: it retains the MDX title/PCM-name header, uses a
32-track offset table, and marks the first track with `E0 FF` (or `E8 E0 FF`
when `#EX-PCM` is also enabled). The dedicated loader validates this structure
instead of relying on mdxmini's 9/16-track MDX limit.

Inspect its metadata and PDX lookup result:

```sh
./build/mpxadrv info song.mdx
./build/mpxadrv info song.mdr
```

`info` also scans command boundaries for MADRV-only opcodes (`0xE0` through
`0xE6`). Songs using them are reported before playback so MIDI or
exclusive-control tracks are not mistaken for ordinary MDX compatibility.

Render it to WAV:

```sh
./build/mpxadrv render song.mdx -o song.wav
```

Hybrid MDR songs can be rendered offline to a single stereo WAV (FM/PCM +
FluidSynth MIDI). An SF2 SoundFont is required:

```sh
./build/mpxadrv render song.mdr -o song.wav \
  --soundfont SoundFonts/Roland_SC-55.sf2 -l 1
```

Export MADRV MIDI tracks to a format-1 Standard MIDI File:

```sh
./build/mpxadrv midi song.mdr -o song.mid
```

List CoreMIDI destinations and send MIDI to a USB / physical module:

```sh
./build/mpxadrv midi-list
./build/mpxadrv play song.mdr --destination 0
./build/mpxadrv midi-play song.mdr --destination 0
```

`--destination` accepts an index from `midi-list`, an exact name, or an
unambiguous part of a name. With `play`, hybrid MDR songs keep FM/PCM on the
Mac and route only the MIDI tracks to the selected CoreMIDI output. Without
`--destination`, MDR play uses the built-in software synthesizer (SF2 via
`--soundfont`, or macOS DLSMusicDevice). `--destination` and `--soundfont`
cannot be combined. On interruption the player sends All Notes Off and All
Sound Off on all 16 channels.

Play MADRV MIDI tracks directly through the software synthesizer included with
macOS—no external MIDI destination or IAC setup is required:

```sh
./build/mpxadrv midi-synth song.mdr
```

The default path uses Apple's multitimbral DLSMusicDevice and its built-in GM
bank. An SF2 SoundFont can be loaded explicitly; custom banks use FluidSynth
instead of Apple's AUMIDISynth so that sample tuning, modulators, and pitch-bend
range follow the SoundFont specification:

```sh
./build/mpxadrv midi-synth song.mdx --soundfont instruments.sf2
```

### Optional SC-55-style SoundFont

For MADRV files authored for the Roland SC-55, the lightweight SC-55-style
bank distributed with ScummVM is a useful local reference. The bank is not
included in this repository. Download the pinned copy into the ignored
`SoundFonts/` directory:

```sh
mkdir -p SoundFonts
curl -L \
  https://raw.githubusercontent.com/scummvm/scummvm/24813e60febcab5da73f870f035b7675e88b1d39/dists/soundfonts/Roland_SC-55.sf2 \
  -o SoundFonts/Roland_SC-55.sf2
shasum -a 256 SoundFonts/Roland_SC-55.sf2
```

The expected SHA-256 is
`fca3e514b635a21789d4224e84865d2954a2a914d46b64aa8219ddb565c44869`.
Use it for hybrid MDR playback with the software synthesizer:

```sh
./build/mpxadrv play song.mdr \
  --soundfont SoundFonts/Roland_SC-55.sf2
```

Or send the MIDI half to a connected SC-55 / USB MIDI interface while FM/PCM
still play locally:

```sh
./build/mpxadrv midi-list
./build/mpxadrv play song.mdr --destination 0
```

ScummVM identifies the file as `Copyright (c) 2015 deemster`, licensed under
GPL-3.0-or-later; see its
[copyright notice](https://github.com/scummvm/scummvm/blob/master/dists/soundfonts/COPYRIGHT.Roland_SC-55).
`SoundFonts/` is excluded by `.gitignore`, so the downloaded bank is never
added to this project's GitHub repository. It is an SC-55-oriented software
approximation, not a bit-exact replacement for Roland hardware.

Compile a MADRV TDX definition into a multi-bank PDX:

```sh
./build/mpxadrv tdx samples.tdx -o samples.pdx
```

An MDX file that names a `.TDX` PCM file is compiled automatically. A TDX can
also override the PDX named by an existing song:

```sh
./build/mpxadrv play song.mdx --tdx-file samples.tdx
```

Useful options:

```text
-p, --pdx-dir <path>   additional PDX search directory
    --tdx-file <path>  override the song's PDX with a TDX definition
-r, --rate <hz>        sample rate (default: 48000)
-l, --loops <count>    song L repeats: 0=forever (default), 1-100=finite
    --destination <id> CoreMIDI USB/physical out (play, midi-play)
    --soundfont <path>  SF2/DLS soft synth (play, render, midi-synth)
```

Run `./build/mpxadrv --help` for the complete command reference.

## TDX syntax

The native loader follows `MAC_PLAY.S`:

```text
# 2                 number of destination banks (1-16)
+ drums.pdx         select a source PDX
* 0                 select a source bank
@ 0                 select a destination bank
N0 = N12            assign numeric sample keys
C4 = D4             pitch-name keys are also accepted
@ 1
&N0 = 0 N0          alias bank 0, key N0 without copying sample data
```

Blank lines and lines beginning with `;` are ignored. Source filenames are
resolved relative to the TDX file first and then `--pdx-dir`. The original
multi-assignment routine appears to use the current-bank pointer where its
comment and parsed bank operand indicate the selected-bank pointer; this port
uses the explicit bank operand.

## Current scope

The current version supports MDX metadata, 8-channel YM2151 FM playback, and up
to 8 PCM channels through `mdxmini`, including normal PDX lookup, MADRV TDX
bank assembly, and MADRV-extension detection. MDR files are parsed independently
as 32-track MADRV containers; their MIDI tracks can be played by the macOS
software synthesizer, sent through CoreMIDI, or exported as SMF.

The native MIDI path can write SMF, send CoreMIDI, or play through the built-in
macOS software synthesizer. It understands MADRV's MIDI note/rest layout,
tempo and gate timing, program/pan/control changes, channel switching,
bend-range setup,
velocity control, RPN/NRPN, `E0` direct MIDI byte streams (including SysEx), and
`E1` polyphonic note stacking. Repeat commands and the requested `--loops`
count are followed during conversion. `E2` initialization for MT-32, CM-64,
and GS devices is converted to Roland DT1 SysEx with the original checksum
algorithm. SC-55 reset, packed DT1 SysEx, rhythm NRPN, partial-reserve,
effects, bank, and pedal commands are converted. Other model-specific
Roland/CM-64/U-110 macros remain variable-length device commands; the converter
reports the unsupported subcommand and stops that track instead of guessing a
boundary or sending malformed SysEx. MADRV detune and portamento commands are
expanded into MIDI pitch-bend events at the original one-clock resolution.
Pitch LFO commands are not yet expanded into MIDI pitch-bend events.

Hybrid MDR playback separates hardware tracks into a temporary MDX stream and
starts it in sync with the software MIDI synthesizer. A tempo-conductor track
keeps both sides aligned through tempo changes and song loops. An SC-55-style
SF2 bank can be selected with `play song.mdr --soundfont bank.sf2`; custom
banks are rendered by FluidSynth. The built-in macOS DLS bank is GM-compatible
but does not reproduce every SC-55 timbre. The same SF2 path is used by
`render song.mdr --soundfont bank.sf2` to mix FM/PCM and MIDI into one WAV
offline. An MDR with no MIDI events uses the same 9/16-track MDX compatibility
path for playback and WAV rendering; if its PDX/TDX dependencies are missing,
the command warns and continues with its FM portion.
