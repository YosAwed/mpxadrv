#include "software_synth.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <fluidsynth.h>

#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace mpxadrv {
namespace {

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
};

class FluidSynthGraph {
 public:
  explicit FluidSynthGraph(const std::filesystem::path& soundFont) {
    settings_ = new_fluid_settings();
    if (settings_ == nullptr) {
      throw MidiError("new_fluid_settings failed");
    }
    try {
      fluid_settings_setstr(settings_, "audio.driver", "coreaudio");
      fluid_settings_setnum(settings_, "synth.sample-rate", 48000.0);
      fluid_settings_setnum(settings_, "synth.gain", 0.5);
      synth_ = new_fluid_synth(settings_);
      if (synth_ == nullptr) {
        throw MidiError("new_fluid_synth failed");
      }
      if (fluid_synth_sfload(synth_, soundFont.string().c_str(), 1) < 0) {
        throw MidiError("FluidSynth could not load SoundFont: " +
                        soundFont.string());
      }
      driver_ = new_fluid_audio_driver(settings_, synth_);
      if (driver_ == nullptr) {
        throw MidiError("FluidSynth could not open the Core Audio output");
      }
    } catch (...) {
      cleanup();
      throw;
    }
  }

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
        } else {
          fluid_synth_noteon(synth_, channel, data1, data2);
        }
        break;
      case 0xa0:
        fluid_synth_key_pressure(synth_, channel, data1, data2);
        break;
      case 0xb0:
        fluid_synth_cc(synth_, channel, data1, data2);
        break;
      case 0xc0:
        fluid_synth_program_change(synth_, channel, data1);
        break;
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
};

SoftwareSynthPlayer::SoftwareSynthPlayer(
    const std::filesystem::path& soundFont)
    : impl_(std::make_unique<Impl>(soundFont)) {}

SoftwareSynthPlayer::~SoftwareSynthPlayer() = default;

void SoftwareSynthPlayer::prepare(const MidiSequence& sequence) {
  impl_->events = scheduleMidiEvents(sequence);
  if (impl_->events.empty()) {
    throw MidiError("the song contains no events for the software synthesizer");
  }
}

void SoftwareSynthPlayer::playAt(
    const MidiSequence& sequence, const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start) {
  prepare(sequence);
  playPreparedAt(shouldStop, start);
}

void SoftwareSynthPlayer::playPreparedAt(
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start) {
  try {
    for (const ScheduledMidiEvent& event : impl_->events) {
      if (shouldStop && shouldStop()) {
        break;
      }
      std::this_thread::sleep_until(
          start + std::chrono::microseconds(event.microseconds));
      if (shouldStop && shouldStop()) {
        break;
      }
      impl_->send(event.bytes);
    }
    if (!(shouldStop && shouldStop())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
  } catch (...) {
    impl_->allNotesOff();
    throw;
  }
  impl_->allNotesOff();
}

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop) {
  SoftwareSynthPlayer player(soundFont);
  player.prepare(sequence);
  player.playPreparedAt(shouldStop, std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(100));
}

void playSoftwareSynthAt(
    const MidiSequence& sequence, const std::filesystem::path& soundFont,
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start) {
  SoftwareSynthPlayer player(soundFont);
  player.playAt(sequence, shouldStop, start);
}

}  // namespace mpxadrv
