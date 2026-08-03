#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mpxadrv {

class MidiError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct MidiEvent {
  std::uint64_t tick = 0;
  std::vector<std::uint8_t> bytes;
  std::uint64_t order = 0;
};

struct MidiTempo {
  std::uint64_t tick = 0;
  std::uint8_t value = 0;
  std::uint64_t order = 0;
};

struct MidiTrack {
  int sourceTrack = 0;
  std::vector<MidiEvent> events;
  std::uint64_t endTick = 0;
};

struct MidiSequence {
  int ppqn = 48;
  std::vector<MidiTempo> tempos;
  std::vector<MidiTrack> tracks;
  std::vector<std::string> warnings;
  std::uint64_t endTick = 0;
  // Tick where the song's L (F1) first jumped back during conversion.
  // Used by software/CoreMIDI players to repeat from that point forever.
  std::uint64_t loopStartTick = 0;
  bool hasSongLoop = false;
};

struct ScheduledMidiEvent {
  std::uint64_t microseconds = 0;
  std::vector<std::uint8_t> bytes;
};

MidiSequence convertMadrvMidi(const std::uint8_t* data, std::size_t length,
                              const int* trackOffsets, int trackCount,
                              int loops);

// When sampleRate > 0, event times match mdxmini's integer sample quantisation
// at that rate (see mdx_calc_sample). Hybrid FM+MIDI playback must pass the
// FM stream rate so the two clocks do not drift apart over long songs.
std::vector<ScheduledMidiEvent> scheduleMidiEvents(
    const MidiSequence& sequence, int sampleRate = 0);

std::uint64_t midiDurationMicroseconds(const MidiSequence& sequence,
                                       int sampleRate = 0);

std::uint64_t midiTickMicroseconds(const MidiSequence& sequence,
                                   std::uint64_t tick, int sampleRate = 0);

// Plays scheduled events once, or forever from loopStartUs when infinite is set
// and loopStartUs is within the sequence. send() delivers each MIDI message.
//
// songClock: optional hybrid master clock returning audible song position in
// microseconds since AudioQueue start, or <0 if not running yet. When provided,
// MIDI locks to that clock after it becomes available (avoids wall-clock drift
// against FM/PCM). lead advances MIDI relative to the clock for soft-synth
// buffer compensation.
using SongPositionClock = std::function<std::int64_t()>;

void playScheduledMidiEvents(
    const std::vector<ScheduledMidiEvent>& events,
    std::uint64_t loopStartUs, bool infinite,
    std::chrono::steady_clock::time_point start,
    const std::function<bool()>& shouldStop,
    const std::function<void(const std::vector<std::uint8_t>&)>& send,
    const SongPositionClock& songClock = {},
    std::chrono::microseconds lead = std::chrono::microseconds(0));

void writeStandardMidi(const MidiSequence& sequence,
                       const std::filesystem::path& path,
                       const std::string& title);

// SC-55 bank MSB 127 is the MT-32/CM-64 tone map. Lightweight SC-55 SoundFonts
// (and Apple's GM bank) omit that variation, so software playback remaps those
// programs onto GS capital tones. Bank MSB 126 (CM-32P) falls back to bank 0
// with the same program number. Other banks are left unchanged.
void resolveSoftwareSynthPreset(int bankMsb, int program, int& outBank,
                                int& outProgram);

// MADRV drum tracks sometimes store Roland's 1-based kit numbers (17=Power,
// 57=SFX, ...). Map those onto the 0-based GS kit list used by SoundFonts.
int resolveRhythmProgram(int program);

}  // namespace mpxadrv
