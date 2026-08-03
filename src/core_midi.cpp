#include "core_midi.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace mpxadrv {
namespace {

std::string midiError(OSStatus status) {
  return std::to_string(static_cast<long>(status));
}

void requireMidi(OSStatus status, const char* operation) {
  if (status != noErr) {
    throw MidiError(std::string(operation) + " failed (CoreMIDI " +
                    midiError(status) + ")");
  }
}

std::string cfString(CFStringRef value) {
  if (value == nullptr) {
    return {};
  }
  const CFIndex length = CFStringGetLength(value);
  const CFIndex maximum =
      CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::vector<char> buffer(static_cast<std::size_t>(maximum), '\0');
  if (!CFStringGetCString(value, buffer.data(), maximum,
                          kCFStringEncodingUTF8)) {
    return {};
  }
  return buffer.data();
}

std::string endpointName(MIDIEndpointRef endpoint) {
  CFStringRef value = nullptr;
  if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &value) !=
          noErr ||
      value == nullptr) {
    if (MIDIObjectGetStringProperty(endpoint, kMIDIPropertyName, &value) !=
            noErr ||
        value == nullptr) {
      return "(unnamed destination)";
    }
  }
  const std::string name = cfString(value);
  CFRelease(value);
  return name.empty() ? "(unnamed destination)" : name;
}

std::string asciiLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

MIDIEndpointRef resolveDestination(const std::string& selector) {
  const ItemCount count = MIDIGetNumberOfDestinations();
  if (count == 0) {
    throw MidiError("no CoreMIDI destinations are available");
  }

  std::size_t consumed = 0;
  try {
    const unsigned long index = std::stoul(selector, &consumed, 10);
    if (consumed == selector.size()) {
      if (index >= count) {
        throw MidiError("MIDI destination index is out of range: " + selector);
      }
      return MIDIGetDestination(static_cast<ItemCount>(index));
    }
  } catch (const std::invalid_argument&) {
  } catch (const std::out_of_range&) {
  }

  const std::string target = asciiLower(selector);
  MIDIEndpointRef partial = 0;
  int partialMatches = 0;
  for (ItemCount index = 0; index < count; ++index) {
    const MIDIEndpointRef endpoint = MIDIGetDestination(index);
    const std::string name = asciiLower(endpointName(endpoint));
    if (name == target) {
      return endpoint;
    }
    if (name.find(target) != std::string::npos) {
      partial = endpoint;
      ++partialMatches;
    }
  }
  if (partialMatches == 1) {
    return partial;
  }
  if (partialMatches > 1) {
    throw MidiError("MIDI destination name is ambiguous: " + selector);
  }
  throw MidiError("MIDI destination not found: " + selector);
}

struct MidiHandles {
  MIDIClientRef client = 0;
  MIDIPortRef port = 0;

  ~MidiHandles() {
    if (port != 0) {
      MIDIPortDispose(port);
    }
    if (client != 0) {
      MIDIClientDispose(client);
    }
  }
};

void sendMessage(MIDIPortRef port, MIDIEndpointRef destination,
                 const std::vector<std::uint8_t>& message) {
  if (message.empty()) {
    return;
  }
  if (message.size() > std::numeric_limits<ByteCount>::max()) {
    throw MidiError("a MIDI message exceeds the CoreMIDI packet limit");
  }
  std::vector<std::uint8_t> storage(sizeof(MIDIPacketList) + message.size() + 64);
  auto* packets = reinterpret_cast<MIDIPacketList*>(storage.data());
  MIDIPacket* packet = MIDIPacketListInit(packets);
  packet = MIDIPacketListAdd(
      packets, storage.size(), packet, 0,
      static_cast<ByteCount>(message.size()), message.data());
  if (packet == nullptr) {
    throw MidiError("failed to build a CoreMIDI packet");
  }
  requireMidi(MIDISend(port, destination, packets), "MIDISend");
}

void allNotesOff(MIDIPortRef port, MIDIEndpointRef destination) noexcept {
  try {
    for (int channel = 0; channel < 16; ++channel) {
      sendMessage(port, destination,
                  {static_cast<std::uint8_t>(0xb0 | channel), 123, 0});
      sendMessage(port, destination,
                  {static_cast<std::uint8_t>(0xb0 | channel), 120, 0});
    }
  } catch (...) {
  }
}

}  // namespace

class CoreMidiPlayer::Impl {
 public:
  explicit Impl(const std::string& destinationSelector) {
    if (destinationSelector.empty()) {
      throw MidiError("--destination requires an index or name");
    }
    // Create the client first; destination enumeration is unreliable without it.
    requireMidi(MIDIClientCreate(CFSTR("mpxadrv"), nullptr, nullptr,
                                 &handles.client),
                "MIDIClientCreate");
    requireMidi(MIDIOutputPortCreate(handles.client, CFSTR("mpxadrv output"),
                                     &handles.port),
                "MIDIOutputPortCreate");
    destination = resolveDestination(destinationSelector);
  }

  void send(const std::vector<std::uint8_t>& message) {
    sendMessage(handles.port, destination, message);
  }

  void silence() noexcept { allNotesOff(handles.port, destination); }

  MidiHandles handles;
  MIDIEndpointRef destination = 0;
  std::vector<ScheduledMidiEvent> events;
  std::uint64_t loopStartUs = std::numeric_limits<std::uint64_t>::max();
  bool infinite = false;
};

CoreMidiPlayer::CoreMidiPlayer(const std::string& destinationSelector)
    : impl_(std::make_unique<Impl>(destinationSelector)) {}

CoreMidiPlayer::~CoreMidiPlayer() = default;

std::chrono::microseconds CoreMidiPlayer::latencyCompensation() const {
  return std::chrono::microseconds(0);
}

void CoreMidiPlayer::prepare(const MidiSequence& sequence, bool infinite,
                             int syncSampleRate) {
  impl_->events = scheduleMidiEvents(sequence, syncSampleRate);
  if (impl_->events.empty()) {
    throw MidiError("the song contains no events for CoreMIDI output");
  }
  impl_->infinite = infinite && sequence.hasSongLoop;
  impl_->loopStartUs =
      impl_->infinite
          ? midiTickMicroseconds(sequence, sequence.loopStartTick,
                                 syncSampleRate)
          : std::numeric_limits<std::uint64_t>::max();
}

void CoreMidiPlayer::playAt(const MidiSequence& sequence,
                            const std::function<bool()>& shouldStop,
                            std::chrono::steady_clock::time_point start,
                            bool infinite, int syncSampleRate) {
  prepare(sequence, infinite, syncSampleRate);
  playPreparedAt(shouldStop, start);
}

void CoreMidiPlayer::playPreparedAt(
    const std::function<bool()>& shouldStop,
    std::chrono::steady_clock::time_point start,
    const SongPositionClock& songClock, std::chrono::microseconds lead) {
  try {
    playScheduledMidiEvents(
        impl_->events, impl_->loopStartUs, impl_->infinite, start, shouldStop,
        [&](const std::vector<std::uint8_t>& bytes) { impl_->send(bytes); },
        songClock, lead);
  } catch (...) {
    impl_->silence();
    throw;
  }
  impl_->silence();
}

std::vector<std::string> midiDestinationNames() {
  MidiHandles handles;
  // A live client makes destination discovery reliable across USB plug events.
  if (MIDIClientCreate(CFSTR("mpxadrv-list"), nullptr, nullptr,
                       &handles.client) != noErr) {
    return {};
  }
  std::vector<std::string> names;
  const ItemCount count = MIDIGetNumberOfDestinations();
  names.reserve(static_cast<std::size_t>(count));
  for (ItemCount index = 0; index < count; ++index) {
    names.push_back(endpointName(MIDIGetDestination(index)));
  }
  return names;
}

void playMidiSequence(const MidiSequence& sequence,
                      const std::string& destinationSelector,
                      const std::function<bool()>& shouldStop, bool infinite) {
  CoreMidiPlayer player(destinationSelector);
  player.prepare(sequence, infinite);
  player.playPreparedAt(shouldStop, std::chrono::steady_clock::now() +
                                        std::chrono::milliseconds(100));
}

}  // namespace mpxadrv
