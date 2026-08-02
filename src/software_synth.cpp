#include "software_synth.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

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

}  // namespace

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop) {
  const std::vector<ScheduledMidiEvent> events = scheduleMidiEvents(sequence);
  if (events.empty()) {
    throw MidiError("the song contains no events for the software synthesizer");
  }

  SoftwareSynthGraph synth(soundFont);
  const auto start = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(100);
  try {
    for (const ScheduledMidiEvent& event : events) {
      if (shouldStop && shouldStop()) {
        break;
      }
      std::this_thread::sleep_until(
          start + std::chrono::microseconds(event.microseconds));
      if (shouldStop && shouldStop()) {
        break;
      }
      synth.send(event.bytes);
    }
    if (!(shouldStop && shouldStop())) {
      std::this_thread::sleep_for(std::chrono::milliseconds(750));
    }
  } catch (...) {
    synth.allNotesOff();
    throw;
  }
  synth.allNotesOff();
}

}  // namespace mpxadrv
