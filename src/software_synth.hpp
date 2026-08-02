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

  void prepare(const MidiSequence& sequence);
  void playAt(const MidiSequence& sequence,
              const std::function<bool()>& shouldStop,
              std::chrono::steady_clock::time_point start);
  void playPreparedAt(const std::function<bool()>& shouldStop,
                      std::chrono::steady_clock::time_point start);

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop);

void playSoftwareSynthAt(
    const MidiSequence& sequence, const std::filesystem::path& soundFont,
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start);

}  // namespace mpxadrv
