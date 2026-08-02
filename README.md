# mpxadrv

`mpxadrv` is a native macOS command-line player for MADRV/MXDRV-compatible
`.MDX` music files and their `.PDX` samples. It uses Core Audio for playback
and CoreMIDI for external MIDI output, and can render songs directly to 16-bit
PCM WAV files or Standard MIDI Files. MADRV `.TDX` bank definitions can be
compiled or loaded transparently during playback.

The original Human68k source and reference music under `Reference/` are used
only as local compatibility references. They are intentionally excluded from
Git because the supplied `MADRVSRC/README.DOC` prohibits redistribution of the
source.

## Requirements

- macOS 11 or later (Apple Silicon or Intel)
- Xcode Command Line Tools
- CMake
- [mdxmini](https://github.com/mistydemeo/mdxmini) 2.0 or later

Install the build dependencies with Homebrew:

```sh
brew install cmake mdxmini
```

`mdxmini` is GPL-2.0-or-later software. A distributed build of this application
must comply with that license. This repository does not copy or vendor
`mdxmini`.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The executable is created at `build/mpxadrv`.

## Usage

Play an MDX file:

```sh
./build/mpxadrv song.mdx
```

Inspect its metadata and PDX lookup result:

```sh
./build/mpxadrv info song.mdx
```

`info` also scans command boundaries for MADRV-only opcodes (`0xE0` through
`0xE6`). Songs using them are reported before playback so MIDI or
exclusive-control tracks are not mistaken for ordinary MDX compatibility.

Render it to WAV:

```sh
./build/mpxadrv render song.mdx -o song.wav
```

Export MADRV MIDI tracks to a format-1 Standard MIDI File:

```sh
./build/mpxadrv midi song.mdx -o song.mid
```

List CoreMIDI destinations and send a song to one explicitly:

```sh
./build/mpxadrv midi-list
./build/mpxadrv midi-play song.mdx --destination 0
```

`--destination` also accepts an exact name or an unambiguous part of a name.
`midi-play` never chooses or opens an output implicitly. On interruption it
sends All Notes Off and All Sound Off on all 16 channels.

Play MADRV MIDI tracks directly through the software synthesizer included with
macOS—no external MIDI destination or IAC setup is required:

```sh
./build/mpxadrv midi-synth song.mdx
```

The default path uses Apple's multitimbral DLSMusicDevice and its built-in GM
bank. An SF2 SoundFont or DLS bank can be loaded explicitly; this path uses
Apple's AUMIDISynth because its sound-bank URL is writable:

```sh
./build/mpxadrv midi-synth song.mdx --soundfont instruments.sf2
```

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
-l, --loops <count>    number of loops (default: 1)
    --destination <id> CoreMIDI destination index or name (midi-play only)
    --soundfont <path>  SF2 or DLS bank (midi-synth only)
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
bank assembly, and MADRV-extension detection.

The native MIDI path can write SMF, send CoreMIDI, or play through the built-in
macOS software synthesizer. It understands MADRV's MIDI note/rest layout,
tempo and gate timing, program/pan/control changes, channel switching,
bend-range setup,
velocity control, RPN/NRPN, `E0` direct MIDI byte streams (including SysEx), and
`E1` polyphonic note stacking. Repeat commands and the requested `--loops`
count are followed during conversion. `E2` initialization for MT-32, CM-64,
and GS devices is converted to Roland DT1 SysEx with the original checksum
algorithm. Other model-specific Roland/CM-64/SC/U-110 macros remain
variable-length device commands; the converter reports the unsupported
subcommand and stops that track instead of guessing a boundary or sending
malformed SysEx. MADRV pitch-LFO/portamento behavior is also not yet expanded
into dense MIDI pitch-bend events.
