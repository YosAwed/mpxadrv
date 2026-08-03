#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "midi.hpp"

namespace mpxadrv {

std::vector<std::string> midiDestinationNames();

// CoreMIDI output player for USB / physical MIDI destinations. Raw bytes are
// sent as authored (no software-synth bank or velocity remapping).
class CoreMidiPlayer {
 public:
  explicit CoreMidiPlayer(const std::string& destinationSelector);
  ~CoreMidiPlayer();

  CoreMidiPlayer(const CoreMidiPlayer&) = delete;
  CoreMidiPlayer& operator=(const CoreMidiPlayer&) = delete;

  // syncSampleRate: when non-zero (hybrid FM+MIDI), schedule against mdxmini's
  // sample-quantised timeline at that rate so MIDI does not drift ahead.
  void prepare(const MidiSequence& sequence, bool infinite = false,
               int syncSampleRate = 0);
  void playAt(const MidiSequence& sequence,
              const std::function<bool()>& shouldStop,
              std::chrono::steady_clock::time_point start,
              bool infinite = false, int syncSampleRate = 0);
  void playPreparedAt(const std::function<bool()>& shouldStop,
                      std::chrono::steady_clock::time_point start,
                      const SongPositionClock& songClock = {},
                      std::chrono::microseconds lead = std::chrono::microseconds(
                          0));
  // Physical outs do not need FluidSynth-style buffer lead-in.
  std::chrono::microseconds latencyCompensation() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

void playMidiSequence(const MidiSequence& sequence,
                      const std::string& destinationSelector,
                      const std::function<bool()>& shouldStop,
                      bool infinite = false);

}  // namespace mpxadrv
