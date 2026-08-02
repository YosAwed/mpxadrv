#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>

#include "midi.hpp"

namespace mpxadrv {

class SoftwareSynthPlayer {
 public:
  explicit SoftwareSynthPlayer(const std::filesystem::path& soundFont);
  ~SoftwareSynthPlayer();

  SoftwareSynthPlayer(const SoftwareSynthPlayer&) = delete;
  SoftwareSynthPlayer& operator=(const SoftwareSynthPlayer&) = delete;

  // syncSampleRate: when non-zero (hybrid FM+MIDI), schedule against mdxmini's
  // sample-quantised timeline at that rate so MIDI does not drift ahead.
  void prepare(const MidiSequence& sequence, bool infinite = false,
               int syncSampleRate = 0);
  void playAt(const MidiSequence& sequence,
              const std::function<bool()>& shouldStop,
              std::chrono::steady_clock::time_point start,
              bool infinite = false, int syncSampleRate = 0);
  void playPreparedAt(const std::function<bool()>& shouldStop,
                      std::chrono::steady_clock::time_point start);
  std::chrono::microseconds latencyCompensation() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop,
                       bool infinite = false);

void playSoftwareSynthAt(
    const MidiSequence& sequence, const std::filesystem::path& soundFont,
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start, bool infinite = false);

}  // namespace mpxadrv
