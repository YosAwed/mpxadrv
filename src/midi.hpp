#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
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
};

struct ScheduledMidiEvent {
  std::uint64_t microseconds = 0;
  std::vector<std::uint8_t> bytes;
};

MidiSequence convertMadrvMidi(const std::uint8_t* data, std::size_t length,
                              const int* trackOffsets, int trackCount,
                              int loops);

std::vector<ScheduledMidiEvent> scheduleMidiEvents(
    const MidiSequence& sequence);

std::uint64_t midiDurationMicroseconds(const MidiSequence& sequence);

void writeStandardMidi(const MidiSequence& sequence,
                       const std::filesystem::path& path,
                       const std::string& title);

}  // namespace mpxadrv
