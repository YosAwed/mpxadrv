#include "software_synth.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fluidsynth.h>

#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace mpxadrv {
namespace {

constexpr int kRhythmChannel = 9;  // MIDI channel 10

// The lightweight SC-55 SF2's drum kits sit quieter than its melodic presets,
// especially under hybrid FM+MIDI mixes. Lift rhythm note-ons modestly.
int boostRhythmVelocity(int velocity) {
  if (velocity <= 0) {
    return velocity;
  }
  return std::min(127, (velocity * 3 + 1) / 2);  // 1.5x, capped
}

std::string audioUnitError(OSStatus status) {
  char code[5] = {};
  const std::uint32_t swapped =
      CFSwapInt32HostToBig(static_cast<std::uint32_t>(status));
  std::memcpy(code, &swapped, 4);
  bool printable = true;
  for (int index = 0; index < 4; ++index) {
    printable = printable && code[index] >= 32 && code[index] <= 126;
  }
  return printable ? "'" + std::string(code, 4) + "'"
                   : std::to_string(static_cast<long>(status));
}

void requireAudioUnit(OSStatus status, const char* operation) {
  if (status != noErr) {
    throw MidiError(std::string(operation) + " failed (Audio Unit " +
                    audioUnitError(status) + ")");
  }
}

class SoftwareSynthGraph {
 public:
  explicit SoftwareSynthGraph(const std::filesystem::path& soundFont) {
    requireAudioUnit(NewAUGraph(&graph_), "NewAUGraph");
    try {

      AudioComponentDescription synthDescription{};
      synthDescription.componentType = kAudioUnitType_MusicDevice;
      synthDescription.componentSubType =
          soundFont.empty() ? kAudioUnitSubType_DLSSynth
                            : kAudioUnitSubType_MIDISynth;
      synthDescription.componentManufacturer = kAudioUnitManufacturer_Apple;

      AudioComponentDescription outputDescription{};
      outputDescription.componentType = kAudioUnitType_Output;
      outputDescription.componentSubType = kAudioUnitSubType_DefaultOutput;
      outputDescription.componentManufacturer = kAudioUnitManufacturer_Apple;

      requireAudioUnit(AUGraphAddNode(graph_, &synthDescription, &synthNode_),
                       "AUGraphAddNode(DLS synth)");
      requireAudioUnit(AUGraphAddNode(graph_, &outputDescription, &outputNode_),
                       "AUGraphAddNode(default output)");
      requireAudioUnit(AUGraphOpen(graph_), "AUGraphOpen");
      requireAudioUnit(AUGraphNodeInfo(graph_, synthNode_, nullptr, &synth_),
                       "AUGraphNodeInfo(DLS synth)");

      if (!soundFont.empty()) {
        const std::string path = soundFont.string();
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(
            kCFAllocatorDefault,
            reinterpret_cast<const UInt8*>(path.data()), path.size(), false);
        if (url == nullptr) {
          throw MidiError("cannot create a SoundFont URL: " + path);
        }
        const OSStatus status = AudioUnitSetProperty(
            synth_, kMusicDeviceProperty_SoundBankURL, kAudioUnitScope_Global,
            0, &url, sizeof(url));
        CFRelease(url);
        requireAudioUnit(status, "AudioUnitSetProperty(SoundBankURL)");
      }

      requireAudioUnit(
          AUGraphConnectNodeInput(graph_, synthNode_, 0, outputNode_, 0),
          "AUGraphConnectNodeInput");
      requireAudioUnit(AUGraphInitialize(graph_), "AUGraphInitialize");
      requireAudioUnit(AUGraphStart(graph_), "AUGraphStart");
      started_ = true;
    } catch (...) {
      DisposeAUGraph(graph_);
      graph_ = nullptr;
      throw;
    }
  }

  SoftwareSynthGraph(const SoftwareSynthGraph&) = delete;
  SoftwareSynthGraph& operator=(const SoftwareSynthGraph&) = delete;

  ~SoftwareSynthGraph() {
    if (graph_ != nullptr) {
      if (started_) {
        AUGraphStop(graph_);
      }
      AUGraphUninitialize(graph_);
      DisposeAUGraph(graph_);
    }
  }

  void send(const std::vector<std::uint8_t>& message) {
    if (message.empty()) {
      return;
    }
    const std::uint8_t status = message[0];
    if (status >= 0xf0) {
      if (message.size() > std::numeric_limits<UInt32>::max()) {
        throw MidiError("a system MIDI message is too large for DLSMusicDevice");
      }
      requireAudioUnit(
          MusicDeviceSysEx(synth_, message.data(),
                           static_cast<UInt32>(message.size())),
          "MusicDeviceSysEx");
      return;
    }
    const std::uint8_t data1 = message.size() > 1 ? message[1] : 0;
    const std::uint8_t data2 = message.size() > 2 ? message[2] : 0;
    const int channel = status & 0x0f;
    if ((status & 0xf0) == 0xb0 && data1 == 0) {
      bankMsb_[channel] = data2;
      // Channel 10 is the GS rhythm part; bank selects must not move it onto
      // a melodic bank or drum notes play as piano pitches.
      if (channel == kRhythmChannel) {
        return;
      }
      int outBank = 0;
      int outProgram = 0;
      resolveSoftwareSynthPreset(data2, 0, outBank, outProgram);
      if (outBank != data2) {
        // CM-64/MT-32 maps are missing from the built-in GM bank; keep the
        // logical bank for later program changes and send a GS capital bank.
        requireAudioUnit(
            MusicDeviceMIDIEvent(synth_, status, 0, outBank, 0),
            "MusicDeviceMIDIEvent");
        return;
      }
    }
    if ((status & 0xf0) == 0xc0) {
      if (channel == kRhythmChannel) {
        requireAudioUnit(
            MusicDeviceMIDIEvent(synth_, status, resolveRhythmProgram(data1),
                                 0, 0),
            "MusicDeviceMIDIEvent");
        return;
      }
      int outBank = 0;
      int outProgram = 0;
      resolveSoftwareSynthPreset(bankMsb_[channel], data1, outBank,
                                 outProgram);
      if (outBank != bankMsb_[channel] || outProgram != data1) {
        requireAudioUnit(
            MusicDeviceMIDIEvent(synth_, 0xb0 | channel, 0, outBank, 0),
            "MusicDeviceMIDIEvent");
      }
      requireAudioUnit(
          MusicDeviceMIDIEvent(synth_, status, outProgram, 0, 0),
          "MusicDeviceMIDIEvent");
      return;
    }
    if ((status & 0xf0) == 0x90 && channel == kRhythmChannel && data2 > 0) {
      requireAudioUnit(
          MusicDeviceMIDIEvent(synth_, status, data1,
                               boostRhythmVelocity(data2), 0),
          "MusicDeviceMIDIEvent");
      return;
    }
    requireAudioUnit(MusicDeviceMIDIEvent(synth_, status, data1, data2, 0),
                     "MusicDeviceMIDIEvent");
  }

  void allNotesOff() noexcept {
    for (int channel = 0; channel < 16; ++channel) {
      MusicDeviceMIDIEvent(synth_, 0xb0 | channel, 123, 0, 0);
      MusicDeviceMIDIEvent(synth_, 0xb0 | channel, 120, 0, 0);
    }
  }

 private:
  AUGraph graph_ = nullptr;
  AUNode synthNode_ = 0;
  AUNode outputNode_ = 0;
  AudioUnit synth_ = nullptr;
  bool started_ = false;
  std::array<int, 16> bankMsb_{};
};

class FluidSynthGraph {
 public:
  FluidSynthGraph(const std::filesystem::path& soundFont, double sampleRate,
                  bool startDriver) {
    settings_ = new_fluid_settings();
    if (settings_ == nullptr) {
      throw MidiError("new_fluid_settings failed");
    }
    try {
      fluid_settings_setnum(settings_, "synth.sample-rate", sampleRate);
      fluid_settings_setint(settings_, "synth.polyphony", 512);
      fluid_settings_setnum(settings_, "synth.gain", 0.5);
      if (!startDriver) {
        // Offline hybrid WAV render: sample clock, no realtime pinning.
        fluid_settings_setint(settings_, "synth.lock-memory", 0);
      } else {
        fluid_settings_setstr(settings_, "audio.driver", "coreaudio");
        // Retain enough buffering for dense arrangements. Hybrid playback
        // compensates for this known queue depth by sending MIDI ahead of the
        // FM/PCM AudioQueue instead of risking underruns with a tiny buffer.
        fluid_settings_setint(settings_, "audio.period-size", 64);
        fluid_settings_setint(settings_, "audio.periods", 16);
      }
      synth_ = new_fluid_synth(settings_);
      if (synth_ == nullptr) {
        throw MidiError("new_fluid_synth failed");
      }
      if (fluid_synth_sfload(synth_, soundFont.string().c_str(), 1) < 0) {
        throw MidiError("FluidSynth could not load SoundFont: " +
                        soundFont.string());
      }
      // Full channel volume on the rhythm part; SF2 kits are still soft, so
      // note-ons are additionally boosted in send().
      fluid_synth_cc(synth_, kRhythmChannel, 7, 127);
      fluid_synth_cc(synth_, kRhythmChannel, 11, 127);
      if (startDriver) {
        driver_ = new_fluid_audio_driver(settings_, synth_);
        if (driver_ == nullptr) {
          throw MidiError("FluidSynth could not open the Core Audio output");
        }
      }
    } catch (...) {
      cleanup();
      throw;
    }
  }

  explicit FluidSynthGraph(const std::filesystem::path& soundFont)
      : FluidSynthGraph(soundFont, 48000.0, true) {}

  FluidSynthGraph(const FluidSynthGraph&) = delete;
  FluidSynthGraph& operator=(const FluidSynthGraph&) = delete;

  ~FluidSynthGraph() { cleanup(); }

  void send(const std::vector<std::uint8_t>& message) {
    if (message.empty()) {
      return;
    }
    const int status = message[0];
    if (status == 0xf0) {
      if (message.size() <= 2) {
        return;
      }
      const bool terminated = message.back() == 0xf7;
      const int length = static_cast<int>(message.size()) -
                         (terminated ? 2 : 1);
      int handled = 0;
      fluid_synth_sysex(
          synth_, reinterpret_cast<const char*>(message.data() + 1), length,
          nullptr, nullptr, &handled, 0);
      return;
    }
    const int channel = status & 0x0f;
    const int data1 = message.size() > 1 ? message[1] : 0;
    const int data2 = message.size() > 2 ? message[2] : 0;
    switch (status & 0xf0) {
      case 0x80:
        fluid_synth_noteoff(synth_, channel, data1);
        break;
      case 0x90:
        if (data2 == 0) {
          fluid_synth_noteoff(synth_, channel, data1);
        } else if (channel == kRhythmChannel) {
          fluid_synth_noteon(synth_, channel, data1,
                             boostRhythmVelocity(data2));
        } else {
          fluid_synth_noteon(synth_, channel, data1, data2);
        }
        break;
      case 0xa0:
        fluid_synth_key_pressure(synth_, channel, data1, data2);
        break;
      case 0xb0:
        if (data1 == 0) {
          bankMsb_[channel] = data2;
          // Keep MIDI channel 10 as FluidSynth's drum channel. Selecting a
          // melodic bank here turns kit notes into piano pitches.
          if (channel == kRhythmChannel) {
            break;
          }
          int outBank = 0;
          int outProgram = 0;
          resolveSoftwareSynthPreset(data2, 0, outBank, outProgram);
          // Keep CM-64/MT-32 maps logical, but ask FluidSynth for a bank the
          // SoundFont actually contains to avoid substitution warnings.
          fluid_synth_cc(synth_, channel, 0, outBank);
          break;
        }
        fluid_synth_cc(synth_, channel, data1, data2);
        break;
      case 0xc0: {
        if (channel == kRhythmChannel) {
          fluid_synth_program_change(synth_, channel,
                                     resolveRhythmProgram(data1));
          break;
        }
        int outBank = 0;
        int outProgram = 0;
        resolveSoftwareSynthPreset(bankMsb_[channel], data1, outBank,
                                   outProgram);
        fluid_synth_bank_select(synth_, channel, outBank);
        fluid_synth_program_change(synth_, channel, outProgram);
        break;
      }
      case 0xd0:
        fluid_synth_channel_pressure(synth_, channel, data1);
        break;
      case 0xe0:
        fluid_synth_pitch_bend(synth_, channel, data1 | (data2 << 7));
        break;
      default:
        break;
    }
  }

  void allNotesOff() noexcept {
    for (int channel = 0; channel < 16; ++channel) {
      fluid_synth_cc(synth_, channel, 123, 0);
      fluid_synth_cc(synth_, channel, 120, 0);
    }
  }

  void writeInterleavedStereo(std::int16_t* interleaved, int frames) {
    if (frames <= 0) {
      return;
    }
    fluid_synth_write_s16(synth_, frames, interleaved, 0, 2, interleaved, 1, 2);
  }

 private:
  void cleanup() noexcept {
    if (driver_ != nullptr) {
      delete_fluid_audio_driver(driver_);
      driver_ = nullptr;
    }
    if (synth_ != nullptr) {
      delete_fluid_synth(synth_);
      synth_ = nullptr;
    }
    if (settings_ != nullptr) {
      delete_fluid_settings(settings_);
      settings_ = nullptr;
    }
  }

  fluid_settings_t* settings_ = nullptr;
  fluid_synth_t* synth_ = nullptr;
  fluid_audio_driver_t* driver_ = nullptr;
  std::array<int, 16> bankMsb_{};
};

}  // namespace

class SoftwareSynthPlayer::Impl {
 public:
  explicit Impl(const std::filesystem::path& soundFont) {
    if (soundFont.empty()) {
      apple = std::make_unique<SoftwareSynthGraph>(soundFont);
    } else {
      fluid = std::make_unique<FluidSynthGraph>(soundFont);
    }
  }

  void send(const std::vector<std::uint8_t>& message) {
    if (fluid) {
      fluid->send(message);
    } else {
      apple->send(message);
    }
  }

  void allNotesOff() noexcept {
    if (fluid) {
      fluid->allNotesOff();
    } else {
      apple->allNotesOff();
    }
  }

  std::unique_ptr<SoftwareSynthGraph> apple;
  std::unique_ptr<FluidSynthGraph> fluid;
  std::vector<ScheduledMidiEvent> events;
  std::vector<std::vector<std::uint8_t>> loopRestore;
  std::uint64_t loopStartUs = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t loopEndUs = std::numeric_limits<std::uint64_t>::max();
  bool infinite = false;
};

SoftwareSynthPlayer::SoftwareSynthPlayer(
    const std::filesystem::path& soundFont)
    : impl_(std::make_unique<Impl>(soundFont)) {}

SoftwareSynthPlayer::~SoftwareSynthPlayer() = default;

std::chrono::microseconds SoftwareSynthPlayer::latencyCompensation() const {
  // FluidSynth's Core Audio driver queues 64 frames in each of 16 periods at
  // 48 kHz. Core Audio begins consuming the queue while it is primed, and in
  // hybrid playback a full 14-period lead made MIDI audibly early against the
  // FM/PCM AudioQueue — use ten periods (~13.3 ms) instead.
  return impl_->fluid ? std::chrono::microseconds(13'333)
                      : std::chrono::microseconds(0);
}

void SoftwareSynthPlayer::prepare(const MidiSequence& sequence, bool infinite,
                                  int syncSampleRate) {
  impl_->events = scheduleMidiEvents(sequence, syncSampleRate);
  if (impl_->events.empty()) {
    throw MidiError("the song contains no events for the software synthesizer");
  }
  impl_->infinite = infinite && sequence.hasSongLoop;
  impl_->loopStartUs =
      impl_->infinite
          ? midiTickMicroseconds(sequence, sequence.loopStartTick,
                                 syncSampleRate)
          : std::numeric_limits<std::uint64_t>::max();
  impl_->loopEndUs =
      impl_->infinite
          ? midiTickMicroseconds(sequence, sequence.endTick, syncSampleRate)
          : std::numeric_limits<std::uint64_t>::max();
  impl_->loopRestore =
      impl_->infinite ? midiLoopRestoreMessages(sequence)
                      : std::vector<std::vector<std::uint8_t>>{};
}

void SoftwareSynthPlayer::playAt(
    const MidiSequence& sequence, const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start, bool infinite,
    int syncSampleRate) {
  prepare(sequence, infinite, syncSampleRate);
  playPreparedAt(shouldStop, start);
}

void SoftwareSynthPlayer::playPreparedAt(
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start,
    const SongPositionClock& songClock, std::chrono::microseconds lead) {
  try {
    playScheduledMidiEvents(
        impl_->events, impl_->loopStartUs, impl_->infinite, start, shouldStop,
        [&](const std::vector<std::uint8_t>& bytes) { impl_->send(bytes); },
        songClock, lead, impl_->loopRestore, impl_->loopEndUs);
  } catch (...) {
    impl_->allNotesOff();
    throw;
  }
  impl_->allNotesOff();
}

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop, bool infinite) {
  SoftwareSynthPlayer player(soundFont);
  player.prepare(sequence, infinite);
  player.playPreparedAt(shouldStop, std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(100));
}

void playSoftwareSynthAt(
    const MidiSequence& sequence, const std::filesystem::path& soundFont,
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start, bool infinite) {
  SoftwareSynthPlayer player(soundFont);
  player.playAt(sequence, shouldStop, start, infinite);
}

class OfflineFluidRenderer::Impl {
 public:
  Impl(const std::filesystem::path& soundFont, int sampleRate)
      : sampleRate_(sampleRate),
        fluid_(soundFont, static_cast<double>(sampleRate), false) {
    if (sampleRate < 8'000 || sampleRate > 192'000) {
      throw MidiError("offline FluidSynth sample rate is out of range");
    }
  }

  void prepare(const MidiSequence& sequence) {
    events_ = scheduleMidiEvents(sequence, sampleRate_);
    if (events_.empty()) {
      throw MidiError("the song contains no events for offline FluidSynth render");
    }
    eventIndex_ = 0;
    samplePosition_ = 0;
  }

  void render(std::int16_t* interleavedStereo, int frames) {
    if (interleavedStereo == nullptr || frames <= 0) {
      return;
    }
    int produced = 0;
    while (produced < frames) {
      const std::uint64_t nowUs =
          (samplePosition_ * 1'000'000ull) /
          static_cast<std::uint64_t>(sampleRate_);
      while (eventIndex_ < events_.size() &&
             events_[eventIndex_].microseconds <= nowUs) {
        fluid_.send(events_[eventIndex_].bytes);
        ++eventIndex_;
      }

      std::uint64_t nextBoundaryUs = nowUs + 1'000'000ull;
      if (eventIndex_ < events_.size()) {
        nextBoundaryUs = events_[eventIndex_].microseconds;
      }
      const std::uint64_t nextBoundarySample =
          (nextBoundaryUs * static_cast<std::uint64_t>(sampleRate_) +
           999'999ull) /
          1'000'000ull;
      int chunk = frames - produced;
      if (eventIndex_ < events_.size() &&
          nextBoundarySample > samplePosition_) {
        const std::uint64_t untilEvent = nextBoundarySample - samplePosition_;
        if (untilEvent < static_cast<std::uint64_t>(chunk)) {
          chunk = static_cast<int>(untilEvent);
        }
      }
      if (chunk <= 0) {
        chunk = 1;
      }
      fluid_.writeInterleavedStereo(interleavedStereo + produced * 2, chunk);
      samplePosition_ += static_cast<std::uint64_t>(chunk);
      produced += chunk;
    }
  }

 private:
  int sampleRate_ = 0;
  FluidSynthGraph fluid_;
  std::vector<ScheduledMidiEvent> events_;
  std::size_t eventIndex_ = 0;
  std::uint64_t samplePosition_ = 0;
};

OfflineFluidRenderer::OfflineFluidRenderer(
    const std::filesystem::path& soundFont, int sampleRate)
    : impl_(std::make_unique<Impl>(soundFont, sampleRate)) {}

OfflineFluidRenderer::~OfflineFluidRenderer() = default;

void OfflineFluidRenderer::prepare(const MidiSequence& sequence) {
  impl_->prepare(sequence);
}

void OfflineFluidRenderer::render(std::int16_t* interleavedStereo, int frames) {
  impl_->render(interleavedStereo, frames);
}

}  // namespace mpxadrv
