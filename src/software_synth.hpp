#pragma once

#include <chrono>
#include <cstdint>
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

// Offline FluidSynth renderer for hybrid WAV export (no Core Audio driver).
// Requires an SF2 SoundFont. Timeline matches mdxmini when sampleRate is set.
class OfflineFluidRenderer {
 public:
  OfflineFluidRenderer(const std::filesystem::path& soundFont, int sampleRate);
  ~OfflineFluidRenderer();

  OfflineFluidRenderer(const OfflineFluidRenderer&) = delete;
  OfflineFluidRenderer& operator=(const OfflineFluidRenderer&) = delete;

  void prepare(const MidiSequence& sequence);
  // Advances the song timeline by `frames` and writes interleaved stereo s16.
  void render(std::int16_t* interleavedStereo, int frames);

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
