#include "midi.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>

namespace mpxadrv {
namespace {

constexpr std::array<std::uint8_t, 16> kVolumeTable = {
    0x2a, 0x28, 0x25, 0x22, 0x20, 0x1d, 0x1a, 0x18,
    0x15, 0x12, 0x10, 0x0d, 0x0a, 0x08, 0x05, 0x02,
};

std::int16_t readSignedWord(const std::vector<std::uint8_t>& data,
                            std::size_t position) {
  if (position + 2 > data.size()) {
    throw MidiError("truncated signed offset in MDX track");
  }
  const std::uint16_t value =
      static_cast<std::uint16_t>((data[position] << 8) | data[position + 1]);
  return static_cast<std::int16_t>(value);
}

std::size_t checkedAdvance(std::size_t position, std::size_t count,
                           std::size_t length) {
  if (count > length - position) {
    throw MidiError("truncated command in MDX track");
  }
  return position + count;
}

int midiMessageLength(std::uint8_t status) {
  if (status < 0x80) {
    return 0;
  }
  if (status < 0xf0) {
    return (status & 0xe0) == 0xc0 ? 2 : 3;
  }
  switch (status) {
    case 0xf1:
    case 0xf3:
      return 2;
    case 0xf2:
      return 3;
    case 0xf6:
    case 0xf8:
    case 0xfa:
    case 0xfb:
    case 0xfc:
    case 0xfe:
    case 0xff:
      return 1;
    default:
      return 0;
  }
}

std::string hexByte(std::uint8_t value) {
  constexpr char digits[] = "0123456789ABCDEF";
  std::string result = "0x00";
  result[2] = digits[value >> 4];
  result[3] = digits[value & 0x0f];
  return result;
}

struct TrackConverter {
  explicit TrackConverter(MidiSequence& value) : sequence(value) {}

  MidiSequence& sequence;
  MidiTrack result;
  std::vector<std::uint8_t> bytes;
  std::size_t position = 0;
  std::uint64_t tick = 0;
  std::uint64_t order = 0;
  int loopLimit = 1;
  int songLoops = 0;
  int channel = 0;
  int gate = 8;
  int nowVolume = 21;
  int nowVolumeCommand = 8;
  int velocityLevel = 0;
  int velocityCommand = 0;
  int bendSensitivity = 32768 / 12;
  std::uint8_t bendRange = 12;
  int bendResponse = 0;
  int bendResponseCounter = 0;
  int bendMode = 0;
  bool bendRangePending = false;
  std::uint32_t pitchAccumulator = 0x20000000u;
  std::int32_t portamentoStep = 0;
  std::int16_t keyDetune = 0;
  std::uint16_t lastPitch = 0xffff;
  bool suppressNextKeyOff = false;
  bool monophonicNoteActive = false;
  int monophonicBaseNote = 0;
  bool midi = false;
  bool emitted = false;
  bool stopped = false;
  // Cross-track MADRV sync (EF send / EE wait). When syncMode is set, rests and
  // notes deposit into pendingDuration instead of advancing tick immediately so
  // a shared scheduler can step every track on one Timer-B clock.
  bool syncMode = false;
  bool waitingEe = false;
  int pendingDuration = 0;
  std::array<bool, 32>* syncSignals = nullptr;
  std::uint8_t rolandDevice = 0x10;
  std::array<std::uint8_t, 16> scPartialReserve{};
  std::vector<std::pair<int, int>> polyNotes;

  void warning(const std::string& message) {
    std::ostringstream text;
    text << "track " << result.sourceTrack + 1 << ": " << message;
    sequence.warnings.push_back(text.str());
  }

  void event(std::vector<std::uint8_t> message, std::uint64_t at) {
    if (!midi || message.empty()) {
      return;
    }
    result.events.push_back({at, std::move(message), order++});
    emitted = true;
  }

  void rawEvent(std::vector<std::uint8_t> message, std::uint64_t at) {
    if (message.empty()) {
      return;
    }
    result.events.push_back({at, std::move(message), order++});
    emitted = true;
  }

  void channelEvent(std::uint8_t status, std::uint8_t first,
                    std::uint8_t second) {
    event({static_cast<std::uint8_t>(status | channel), first, second}, tick);
  }

  void control(std::uint8_t controller, std::uint8_t value) {
    channelEvent(0xb0, controller, static_cast<std::uint8_t>(value & 0x7f));
  }

  void emitBendRangeRpn(std::uint8_t range) {
    // Match MADRV KONOA09: CC100, CC101, then Data Entry. Also clear the
    // Data Entry LSB so modules that latch both bytes (SC-55) get a clean 24.
    control(100, 0);
    control(101, 0);
    control(6, range);
    control(38, 0);
  }

  void setBendRange(std::uint8_t range, bool emit) {
    const std::uint8_t clamped =
        range >= 2 && range <= 24 ? range : static_cast<std::uint8_t>(12);
    bendRange = clamped;
    bendSensitivity = 32768 / clamped;
    bendResponseCounter = 0;
    bendRangePending = midi && !emit;
    if (midi && emit) {
      emitBendRangeRpn(clamped);
    }
  }

  void emitPendingBendRange() {
    if (!bendRangePending) {
      return;
    }
    bendRangePending = false;
    emitBendRangeRpn(bendRange);
  }

  int noteVelocity() const { return (~nowVolume) & 0x7f; }

  int gateTicks(int duration) const {
    const int encoded = duration - 1;
    if (gate <= 8) {
      return (encoded * gate) / 8 + 1;
    }
    return std::max(1, duration + gate - 256);
  }

  void pitchBend(std::uint64_t at) {
    if (!midi) {
      return;
    }
    emitPendingBendRange();
    const std::uint16_t pitch = static_cast<std::uint16_t>(
        (pitchAccumulator >> 16) + static_cast<std::uint16_t>(keyDetune));
    if (pitch == lastPitch) {
      return;
    }
    if (bendResponseCounter > 0) {
      --bendResponseCounter;
      return;
    }
    bendResponseCounter = bendResponse;
    lastPitch = pitch;

    int value = pitch;
    if (static_cast<std::int16_t>(pitch) < 0) {
      value = 0;
    } else if (value > 0x3fff) {
      value = 0x3fff;
    }
    event({static_cast<std::uint8_t>(0xe0 | channel),
           static_cast<std::uint8_t>(value & 0x7f),
           static_cast<std::uint8_t>((value >> 7) & 0x7f)},
          at);
  }

  void advanceTime(int duration) {
    if (midi) {
      for (int elapsed = 0; elapsed < duration; ++elapsed) {
        pitchAccumulator += static_cast<std::uint32_t>(portamentoStep);
        pitchBend(tick + static_cast<std::uint64_t>(elapsed));
      }
    }
    if (syncMode) {
      pendingDuration = duration;
      portamentoStep = 0;
      return;
    }
    tick += static_cast<std::uint64_t>(duration);
    portamentoStep = 0;
  }

  void addPitchWord(std::int32_t amount) {
    const std::uint16_t high = static_cast<std::uint16_t>(
        (pitchAccumulator >> 16) + static_cast<std::uint16_t>(amount));
    pitchAccumulator = (static_cast<std::uint32_t>(high) << 16) |
                       (pitchAccumulator & 0xffffu);
  }

  void note(int key, int duration) {
    if (midi) {
      const int off = gateTicks(duration);
      for (const auto& poly : polyNotes) {
        event({static_cast<std::uint8_t>(0x90 | channel),
               static_cast<std::uint8_t>(poly.first & 0x7f),
               static_cast<std::uint8_t>(poly.second & 0x7f)},
              tick);
        event({static_cast<std::uint8_t>(0x80 | channel),
               static_cast<std::uint8_t>(poly.first & 0x7f), 0},
              tick + static_cast<std::uint64_t>(off));
      }
      polyNotes.clear();

      pitchAccumulator = 0x20000000u;
      if (monophonicNoteActive) {
        if (bendMode == 0) {
          const std::int32_t interval =
              static_cast<std::int32_t>(key - monophonicBaseNote) *
              bendSensitivity;
          addPitchWord(interval >> 2);
          bendResponseCounter = 0;
        }
      } else {
        event({static_cast<std::uint8_t>(0x90 | channel),
               static_cast<std::uint8_t>(key & 0x7f),
               static_cast<std::uint8_t>(noteVelocity())},
              tick);
        monophonicNoteActive = true;
        monophonicBaseNote = key;
      }
      if (!suppressNextKeyOff) {
        event({static_cast<std::uint8_t>(0x80 | channel),
               static_cast<std::uint8_t>(monophonicBaseNote & 0x7f), 0},
              tick + static_cast<std::uint64_t>(off));
      }
    }
    advanceTime(duration);
    if (midi && !suppressNextKeyOff) {
      monophonicNoteActive = false;
    }
    suppressNextKeyOff = false;
  }

  void directMidi(std::size_t start, std::size_t count) {
    const std::size_t end = checkedAdvance(start, count, bytes.size());
    std::size_t cursor = start;
    std::uint8_t runningStatus = 0;
    while (cursor < end) {
      std::uint8_t status = bytes[cursor];
      if (status < 0x80) {
        if (runningStatus == 0) {
          warning("direct MIDI data starts without a status byte");
          return;
        }
        status = runningStatus;
      } else {
        ++cursor;
        if (status < 0xf0) {
          runningStatus = status;
        } else {
          runningStatus = 0;
        }
      }

      if (status == 0xf0) {
        std::vector<std::uint8_t> sysex = {0xf0};
        while (cursor < end) {
          const std::uint8_t value = bytes[cursor++];
          sysex.push_back(value);
          if (value == 0xf7) {
            break;
          }
        }
        if (sysex.back() != 0xf7) {
          warning("unterminated SysEx in E0 direct-output command");
          sysex.push_back(0xf7);
        }
        rawEvent(std::move(sysex), tick);
        continue;
      }

      const int messageLength = midiMessageLength(status);
      if (messageLength == 0) {
        warning("unsupported direct MIDI status " + hexByte(status));
        return;
      }
      std::vector<std::uint8_t> message = {status};
      while (message.size() < static_cast<std::size_t>(messageLength)) {
        if (cursor >= end || bytes[cursor] >= 0x80) {
          warning("truncated direct MIDI message " + hexByte(status));
          return;
        }
        message.push_back(bytes[cursor++]);
      }
      if (status == 0xff) {
        warning("MIDI System Reset cannot be represented in an SMF and was omitted");
      } else {
        rawEvent(std::move(message), tick);
      }
    }
  }

  void rolandDt1(std::uint32_t packedAddress, std::uint8_t data) {
    const std::uint8_t model =
        static_cast<std::uint8_t>((packedAddress >> 24) & 0xff);
    const std::uint8_t address1 =
        static_cast<std::uint8_t>((packedAddress >> 16) & 0x7f);
    const std::uint8_t address2 =
        static_cast<std::uint8_t>((packedAddress >> 8) & 0x7f);
    const std::uint8_t address3 =
        static_cast<std::uint8_t>(packedAddress & 0x7f);
    const std::uint8_t checksum = static_cast<std::uint8_t>(
        (-(address1 + address2 + address3 + data)) & 0x7f);
    event({0xf0, 0x41, rolandDevice, model, 0x12, address1, address2,
           address3, data, checksum, 0xf7},
          tick);
  }

  void rolandPack(std::uint32_t packedAddress,
                  const std::vector<std::uint8_t>& data) {
    const std::uint8_t model =
        static_cast<std::uint8_t>((packedAddress >> 24) & 0xff);
    const std::uint8_t address1 =
        static_cast<std::uint8_t>((packedAddress >> 16) & 0x7f);
    const std::uint8_t address2 =
        static_cast<std::uint8_t>((packedAddress >> 8) & 0x7f);
    const std::uint8_t address3 =
        static_cast<std::uint8_t>(packedAddress & 0x7f);
    unsigned sum = address1 + address2 + address3;
    std::vector<std::uint8_t> message = {
        0xf0, 0x41, rolandDevice, model, 0x12,
        address1, address2, address3,
    };
    for (const std::uint8_t value : data) {
      const std::uint8_t sevenBit = value & 0x7f;
      message.push_back(sevenBit);
      sum += sevenBit;
    }
    message.push_back(static_cast<std::uint8_t>((-sum) & 0x7f));
    message.push_back(0xf7);
    event(std::move(message), tick);
  }

  std::vector<std::uint8_t> fixedParameters(std::size_t count) {
    const std::size_t start = position;
    position = checkedAdvance(position, count, bytes.size());
    return {bytes.begin() + static_cast<std::ptrdiff_t>(start),
            bytes.begin() + static_cast<std::ptrdiff_t>(position)};
  }

  std::vector<std::uint8_t> packedParameters() {
    position = checkedAdvance(position, 1, bytes.size());
    const std::size_t count = bytes[position - 1];
    return fixedParameters(count);
  }

  std::vector<std::uint8_t> terminatedParameters() {
    std::vector<std::uint8_t> result;
    while (position < bytes.size()) {
      const std::uint8_t value = bytes[position++];
      if (value == 0) {
        return result;
      }
      result.push_back(value & 0x7f);
    }
    throw MidiError("unterminated string in E2 command");
  }

  void extendedE2() {
    if (!midi) {
      warning(
          "E2 exclusive command appeared outside MIDI mode; track stopped safely");
      stopped = true;
      return;
    }
    position = checkedAdvance(position, 1, bytes.size());
    const std::uint8_t subcommand = bytes[position - 1];
    switch (subcommand) {
      case 0x00:  // MT:INIT
      case 0x14:  // CM64:INIT
        rolandDevice = 0x10;
        rolandDt1(0x167f0000, 0x00);
        break;
      case 0x1c:  // SC:INIT (GS reset)
        rolandDevice = 0x10;
        rolandDt1(0x4240007f, 0x00);
        break;
      case 0x1d: {  // SC:RM
        const auto parameters = fixedParameters(1);
        rolandDevice = 0x10;
        rolandDt1(0x42401015u +
                      (static_cast<std::uint32_t>(channel) << 8),
                  parameters[0]);
        break;
      }
      case 0x1e:
      case 0x1f:
      case 0x20:
      case 0x21:
      case 0x22: {  // SC rhythm NRPN controls.
        const auto parameters = fixedParameters(2);
        static constexpr std::array<std::uint8_t, 5> kNrpnMsb = {
            0x18, 0x1a, 0x1c, 0x1d, 0x1e,
        };
        control(99, kNrpnMsb[subcommand - 0x1e]);
        control(98, parameters[0]);
        control(6, parameters[1]);
        break;
      }
      case 0x23:
      case 0x24:
      case 0x25: {  // SC reverb macro/time/level.
        const auto parameters = fixedParameters(1);
        static constexpr std::array<std::uint32_t, 3> kAddresses = {
            0x42400130, 0x42400134, 0x42400133,
        };
        rolandDt1(kAddresses[subcommand - 0x23], parameters[0]);
        break;
      }
      case 0x26:
      case 0x27: {  // SC reverb/chorus send.
        const auto parameters = fixedParameters(1);
        control(subcommand == 0x26 ? 0x5b : 0x5d, parameters[0]);
        break;
      }
      case 0x28:
      case 0x29:
      case 0x2a: {  // SC packed effect/tone parameters.
        const auto parameters = packedParameters();
        std::uint32_t address = subcommand == 0x28 ? 0x42400130
                                : subcommand == 0x29 ? 0x42400138
                                                   : 0x42401030u +
                                                         (static_cast<std::uint32_t>(channel) << 8);
        rolandPack(address, parameters);
        break;
      }
      case 0x2b: {  // SC key shift.
        const auto parameters = fixedParameters(1);
        rolandDt1(0x42401016u +
                      (static_cast<std::uint32_t>(channel) << 8),
                  static_cast<std::uint8_t>(parameters[0] + 0x40));
        break;
      }
      case 0x2c:
      case 0x2d:
      case 0x2e:
      case 0x2f:
      case 0x30: {  // SC bank/portamento/pedal controls.
        const auto parameters = fixedParameters(1);
        static constexpr std::array<std::uint8_t, 5> kControllers = {
            0x00, 0x05, 0x41, 0x42, 0x43,
        };
        const std::uint8_t controller = kControllers[subcommand - 0x2c];
        control(controller, parameters[0]);
        if (controller == 0) {
          control(32, 0);
        }
        break;
      }
      case 0x31: {  // SC arbitrary memory write.
        const auto address = fixedParameters(3);
        const auto parameters = packedParameters();
        rolandPack(0x42000000u |
                       (static_cast<std::uint32_t>(address[0]) << 16) |
                       (static_cast<std::uint32_t>(address[1]) << 8) |
                       address[2],
                   parameters);
        break;
      }
      case 0x32:  // SC partial reserve updates a shared 16-byte table.
      {
        const auto parameters = fixedParameters(2);
        if (parameters[0] < scPartialReserve.size()) {
          scPartialReserve[parameters[0]] = parameters[1] & 0x7f;
        }
        rolandPack(0x42400110,
                   std::vector<std::uint8_t>(scPartialReserve.begin(),
                                             scPartialReserve.end()));
        break;
      }
      case 0x33:  // SC display text.
        rolandPack(0x45100000, terminatedParameters());
        break;
      case 0x34:  // SC dot-display buffer data.
        static_cast<void>(fixedParameters(2));
        break;
      case 0x35:  // SC dot-display clear.
      case 0x36:  // SC dot-display flush.
        break;
      case 0x37: {  // SC master tune (two bytes).
        rolandPack(0x42400000, fixedParameters(2));
        break;
      }
      case 0x38:
      case 0x39:
      case 0x3a: {  // SC master volume/key/pan.
        const auto parameters = fixedParameters(1);
        rolandDt1(0x42400001u + (subcommand - 0x38), parameters[0]);
        break;
      }
      default:
        warning("unsupported E2 subcommand " + hexByte(subcommand) +
                "; track stopped safely");
        stopped = true;
        break;
    }
  }

  void extendedE0() {
    position = checkedAdvance(position, 1, bytes.size());
    const std::uint8_t subcommand = bytes[position - 1];
    if (subcommand == 0xff) {
      return;
    }
    if (subcommand > 0x1d) {
      warning("unknown E0 subcommand " + hexByte(subcommand));
      stopped = true;
      return;
    }

    if (subcommand == 0x00) {
      position = checkedAdvance(position, 1, bytes.size());
      if ((bytes[position - 1] & 0x80) == 0) {
        position = checkedAdvance(position, 4, bytes.size());
      }
      return;
    }
    if (subcommand == 0x0e) {
      position = checkedAdvance(position, 1, bytes.size());
      const std::size_t count = static_cast<std::size_t>(bytes[position - 1]) + 1;
      directMidi(position, count);
      position = checkedAdvance(position, count, bytes.size());
      return;
    }

    static constexpr std::array<int, 30> parameterCounts = {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0,
        2, 1, 0, 0, 1, 1, 3, 3, 4, 4, 1, 1, 1, 1, 1,
    };
    const int count = parameterCounts[subcommand];
    const std::size_t parameters = position;
    position = checkedAdvance(position, static_cast<std::size_t>(count),
                              bytes.size());

    switch (subcommand) {
      case 0x08:
      {
        const bool wasMidi = midi;
        channel = bytes[parameters] & 0x0f;
        midi = (bytes[parameters] & 0x80) != 0;
        if (midi) {
          pitchAccumulator = 0x20000000u;
          lastPitch = 0xffff;
          if (!wasMidi) {
            setBendRange(12, false);
          }
        }
        break;
      }
      case 0x09:
        setBendRange(bytes[parameters], true);
        break;
      case 0x0a:
        if ((velocityCommand & 0x80) == 0) {
          if (velocityCommand < 15) {
            ++velocityCommand;
            velocityLevel = kVolumeTable[velocityCommand] * 2;
          }
        } else if (velocityCommand > 0x80) {
          --velocityCommand;
          velocityLevel = velocityCommand & 0x7f;
        }
        control(7, static_cast<std::uint8_t>((~velocityLevel) & 0x7f));
        break;
      case 0x0b:
        if ((velocityCommand & 0x80) == 0) {
          if (velocityCommand > 0) {
            --velocityCommand;
            velocityLevel = kVolumeTable[velocityCommand] * 2;
          }
        } else if (velocityCommand < 0xff) {
          ++velocityCommand;
          velocityLevel = velocityCommand & 0x7f;
        }
        control(7, static_cast<std::uint8_t>((~velocityLevel) & 0x7f));
        break;
      case 0x0c:
        velocityCommand = bytes[parameters];
        velocityLevel = (velocityCommand & 0x80) != 0
                            ? velocityCommand & 0x7f
                            : kVolumeTable[velocityCommand & 0x0f] * 2;
        control(7, static_cast<std::uint8_t>((~velocityLevel) & 0x7f));
        break;
      case 0x15:
      case 0x16:
        control(subcommand == 0x15 ? 99 : 101, bytes[parameters]);
        control(subcommand == 0x15 ? 98 : 100, bytes[parameters + 1]);
        control(6, bytes[parameters + 2]);
        break;
      case 0x17:
      case 0x18:
        control(subcommand == 0x17 ? 99 : 101, bytes[parameters]);
        control(subcommand == 0x17 ? 98 : 100, bytes[parameters + 1]);
        control(6, bytes[parameters + 2]);
        control(6, bytes[parameters + 3]);
        break;
      case 0x19:
        rolandDevice = bytes[parameters] & 0x7f;
        break;
      case 0x13:
        bendMode = bytes[parameters];
        break;
      case 0x14:
        bendResponse = bytes[parameters];
        bendResponseCounter = 0;
        break;
      case 0x1d:
        // Present as E0 1D 00 in later MDR samples. It selects a driver
        // timing mode and does not produce MIDI output.
        break;
      default:
        break;
    }
  }

  void standardVolume(std::uint8_t command) {
    nowVolumeCommand = command;
    nowVolume = (command & 0x80) != 0
                    ? command & 0x7f
                    : kVolumeTable[command & 0x0f] * (midi ? 2 : 1);
  }

  // Process commands until a duration is pending, EE is waiting, or the track
  // stops. Used by the shared Timer-B scheduler for EF/EE sync.
  void pumpUntilBlock() {
    constexpr std::uint64_t kMaximumCommands = 10'000'000;
    std::uint64_t commandCount = 0;
    while (!stopped && position < bytes.size()) {
      if (waitingEe || pendingDuration > 0) {
        return;
      }
      if (++commandCount > kMaximumCommands) {
        throw MidiError("MDX track exceeded the command safety limit");
      }
      processOneCommand();
    }
    stopped = true;
  }

  void run() {
    syncMode = false;
    syncSignals = nullptr;
    waitingEe = false;
    pendingDuration = 0;
    while (!stopped && position < bytes.size()) {
      processOneCommand();
    }
    stopped = true;
    result.endTick = tick;
  }

  void processOneCommand() {
      const std::size_t commandPosition = position;
      const std::uint8_t command = bytes[position++];

      const int noteMinimum = midi ? 0x60 : 0x80;
      if (command < noteMinimum) {
        advanceTime(static_cast<int>(command) + 1);
        suppressNextKeyOff = false;
        return;
      }
      if (command <= 0xdf) {
        position = checkedAdvance(position, 1, bytes.size());
        note(command - noteMinimum, static_cast<int>(bytes[position - 1]) + 1);
        return;
      }

      switch (command) {
        case 0xe0:
          extendedE0();
          break;
        case 0xe1:
          if (!midi) {
            warning("E1 polyphonic note appeared outside MIDI mode; track stopped safely");
            stopped = true;
            break;
          }
          position = checkedAdvance(position, 1, bytes.size());
          if (polyNotes.size() < 16) {
            polyNotes.emplace_back(bytes[position - 1] & 0x7f, noteVelocity());
          }
          break;
        case 0xe2:
          extendedE2();
          break;
        case 0xe3:
        case 0xe4:
        case 0xe5:
        case 0xe6:
          warning("unsupported MADRV command " + hexByte(command) +
                  "; track stopped safely");
          stopped = true;
          break;
        case 0xe7:
          position = checkedAdvance(position, 1, bytes.size());
          if (bytes[position - 1] == 1) {
            position = checkedAdvance(position, 1, bytes.size());
          } else if (bytes[position - 1] >= 2) {
            stopped = true;
          }
          break;
        case 0xe8:
          break;
        case 0xee:
          // MADRV CMD_EE: wait for TRACKSIGNAL (set by EF to this track).
          if (syncMode && syncSignals != nullptr) {
            const int self = result.sourceTrack;
            if (self >= 0 && self < 32 &&
                (*syncSignals)[static_cast<std::size_t>(self)]) {
              (*syncSignals)[static_cast<std::size_t>(self)] = false;
              break;
            }
            position = commandPosition;
            waitingEe = true;
            return;
          }
          break;
        case 0xea:
        case 0xeb:
        case 0xec:
          position = checkedAdvance(position, 1, bytes.size());
          if ((bytes[position - 1] & 0x80) == 0) {
            position = checkedAdvance(position, 4, bytes.size());
          }
          break;
        case 0xef:
          // MADRV CMD_EF: send sync to track N (parameter & 0x1f).
          position = checkedAdvance(position, 1, bytes.size());
          if (syncMode && syncSignals != nullptr) {
            const int target = bytes[position - 1] & 0x1f;
            (*syncSignals)[static_cast<std::size_t>(target)] = true;
          }
          break;
        case 0xe9:
        case 0xed:
        case 0xf0:
          position = checkedAdvance(position, 1, bytes.size());
          break;
        case 0xf1: {
          position = checkedAdvance(position, 1, bytes.size());
          if (bytes[position - 1] == 0) {
            stopped = true;
            break;
          }
          position = checkedAdvance(position, 1, bytes.size());
          const std::int16_t offset = readSignedWord(bytes, commandPosition + 1);
          if (songLoops >= loopLimit) {
            stopped = true;
          } else {
            if (songLoops == 0) {
              if (!sequence.hasSongLoop || tick < sequence.loopStartTick) {
                sequence.loopStartTick = tick;
                sequence.hasSongLoop = true;
              }
            }
            ++songLoops;
            const std::int64_t target = static_cast<std::int64_t>(position) + offset;
            if (target < 0 || target >= static_cast<std::int64_t>(bytes.size())) {
              throw MidiError("song-loop offset leaves MDX data");
            }
            position = static_cast<std::size_t>(target);
          }
          break;
        }
        case 0xf2: {
          position = checkedAdvance(position, 2, bytes.size());
          const std::int16_t amount = readSignedWord(bytes, commandPosition + 1);
          portamentoStep = static_cast<std::int32_t>(amount) * bendSensitivity;
          bendResponseCounter = 0;
          break;
        }
        case 0xf3: {
          position = checkedAdvance(position, 2, bytes.size());
          const std::int32_t scaled =
              static_cast<std::int32_t>(
                  readSignedWord(bytes, commandPosition + 1)) *
              bendSensitivity;
          keyDetune = static_cast<std::int16_t>(
              scaled >= 0 ? scaled / 256 : -(((-scaled) + 255) / 256));
          bendResponseCounter = 0;
          break;
        }
        case 0xf4: {
          position = checkedAdvance(position, 2, bytes.size());
          const std::int16_t outer = readSignedWord(bytes, commandPosition + 1);
          const std::int64_t nested = static_cast<std::int64_t>(position) + outer;
          if (nested < 0 || nested + 2 > static_cast<std::int64_t>(bytes.size())) {
            throw MidiError("repeat-break offset leaves MDX data");
          }
          const std::size_t nestedPosition = static_cast<std::size_t>(nested);
          const std::int16_t inner = readSignedWord(bytes, nestedPosition);
          const std::int64_t counter = nested + 1 + inner;
          if (counter < 0 || counter >= static_cast<std::int64_t>(bytes.size())) {
            throw MidiError("repeat-break counter leaves MDX data");
          }
          if (bytes[static_cast<std::size_t>(counter)] == 1) {
            position = nestedPosition + 2;
          }
          break;
        }
        case 0xf5: {
          position = checkedAdvance(position, 2, bytes.size());
          const std::int16_t offset = readSignedWord(bytes, commandPosition + 1);
          const std::int64_t counter =
              static_cast<std::int64_t>(position) + offset - 1;
          if (counter < 0 || counter >= static_cast<std::int64_t>(bytes.size())) {
            throw MidiError("repeat counter leaves MDX data");
          }
          std::uint8_t& value = bytes[static_cast<std::size_t>(counter)];
          --value;
          if (value != 0) {
            const std::int64_t target = static_cast<std::int64_t>(position) + offset;
            if (target < 0 || target >= static_cast<std::int64_t>(bytes.size())) {
              throw MidiError("repeat offset leaves MDX data");
            }
            position = static_cast<std::size_t>(target);
          }
          break;
        }
        case 0xf6:
          position = checkedAdvance(position, 2, bytes.size());
          bytes[position - 1] = bytes[position - 2];
          break;
        case 0xf7:
          suppressNextKeyOff = true;
          break;
        case 0xf8:
          position = checkedAdvance(position, 1, bytes.size());
          gate = bytes[position - 1];
          break;
        case 0xf9:
          if ((nowVolumeCommand & 0x80) == 0) {
            if (nowVolumeCommand < 15) {
              standardVolume(static_cast<std::uint8_t>(nowVolumeCommand + 1));
            }
          } else if (nowVolumeCommand > 0x80) {
            standardVolume(static_cast<std::uint8_t>(nowVolumeCommand - 1));
          }
          break;
        case 0xfa:
          if ((nowVolumeCommand & 0x80) == 0) {
            if (nowVolumeCommand > 0) {
              standardVolume(static_cast<std::uint8_t>(nowVolumeCommand - 1));
            }
          } else if (nowVolumeCommand < 0xff) {
            standardVolume(static_cast<std::uint8_t>(nowVolumeCommand + 1));
          }
          break;
        case 0xfb:
          position = checkedAdvance(position, 1, bytes.size());
          standardVolume(bytes[position - 1]);
          break;
        case 0xfc:
          position = checkedAdvance(position, 1, bytes.size());
          if (midi) {
            control(10, bytes[position - 1]);
          }
          break;
        case 0xfd:
          position = checkedAdvance(position, 1, bytes.size());
          if (midi) {
            event({static_cast<std::uint8_t>(0xc0 | channel),
                   static_cast<std::uint8_t>(bytes[position - 1] & 0x7f)},
                  tick);
          }
          break;
        case 0xfe:
          position = checkedAdvance(position, 2, bytes.size());
          if (midi) {
            const std::uint8_t controller = bytes[position - 2];
            const std::uint8_t value = bytes[position - 1];
            control(controller, value);
            // GS tone variations select with CC0 (MSB); always clear LSB so a
            // stale CC32 cannot pin the part to the wrong variation map.
            if (controller == 0) {
              control(32, 0);
            }
          }
          break;
        case 0xff:
          position = checkedAdvance(position, 1, bytes.size());
          sequence.tempos.push_back({tick, bytes[position - 1], order++});
          break;
        default:
          throw MidiError("internal MIDI conversion error");
      }
  }
};

void writeBigEndian(std::ostream& output, std::uint32_t value, int bytes) {
  for (int shift = (bytes - 1) * 8; shift >= 0; shift -= 8) {
    output.put(static_cast<char>((value >> shift) & 0xff));
  }
}

void writeVariable(std::vector<std::uint8_t>& output, std::uint64_t value) {
  if (value > 0x0fffffff) {
    throw MidiError("SMF delta time exceeds the 28-bit limit");
  }
  std::uint32_t buffer = static_cast<std::uint32_t>(value & 0x7f);
  while ((value >>= 7) != 0) {
    buffer <<= 8;
    buffer |= static_cast<std::uint32_t>((value & 0x7f) | 0x80);
  }
  for (;;) {
    output.push_back(static_cast<std::uint8_t>(buffer & 0xff));
    if ((buffer & 0x80) == 0) {
      break;
    }
    buffer >>= 8;
  }
}

void appendMeta(std::vector<std::uint8_t>& output, std::uint64_t delta,
                std::uint8_t type, const std::vector<std::uint8_t>& data) {
  writeVariable(output, delta);
  output.push_back(0xff);
  output.push_back(type);
  writeVariable(output, data.size());
  output.insert(output.end(), data.begin(), data.end());
}

void writeChunk(std::ostream& output, const std::vector<std::uint8_t>& data) {
  output.write("MTrk", 4);
  if (data.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw MidiError("SMF track is too large");
  }
  writeBigEndian(output, static_cast<std::uint32_t>(data.size()), 4);
  output.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

int eventPriority(const MidiEvent& event) {
  if (event.bytes.empty()) {
    return 1;
  }
  const std::uint8_t status = event.bytes.front() & 0xf0;
  if (status == 0x80 ||
      (status == 0x90 && event.bytes.size() >= 3 && event.bytes[2] == 0)) {
    return 0;
  }
  if (status == 0x90) {
    return 2;
  }
  return 1;
}

}  // namespace

MidiSequence convertMadrvMidiWithLimits(
    const std::uint8_t* data, std::size_t length, const int* trackOffsets,
    int trackCount, const std::vector<int>& loopLimits) {
  MidiSequence sequence;
  sequence.tempos.push_back({0, 0xc8, 0});

  struct TrackState {
    TrackConverter converter;
    explicit TrackState(MidiSequence& sequence) : converter(sequence) {}
  };

  std::vector<std::unique_ptr<TrackState>> states;
  states.reserve(static_cast<std::size_t>(trackCount));
  std::array<bool, 32> syncSignals{};
  bool anyTrack = false;

  for (int track = 0; track < trackCount; ++track) {
    if (trackOffsets[track] < 0 ||
        static_cast<std::size_t>(trackOffsets[track]) >= length) {
      continue;
    }
    auto state = std::make_unique<TrackState>(sequence);
    TrackConverter& converter = state->converter;
    converter.result.sourceTrack = track;
    converter.bytes.assign(data, data + length);
    converter.position = static_cast<std::size_t>(trackOffsets[track]);
    converter.loopLimit =
        track < static_cast<int>(loopLimits.size())
            ? loopLimits[static_cast<std::size_t>(track)]
            : 1;
    if (converter.loopLimit < 1) {
      converter.loopLimit = 1;
    }
    converter.midi = track >= 16;
    converter.channel = track & 0x0f;
    converter.syncMode = true;
    converter.syncSignals = &syncSignals;
    if (converter.midi) {
      converter.setBendRange(12, false);
    }
    states.push_back(std::move(state));
    anyTrack = true;
  }

  if (!anyTrack) {
    return sequence;
  }

  // Shared Timer-B clock: all tracks advance together so EF (sync send) can
  // release EE (sync wait) on the same tick order MADRV used (track 0..N).
  constexpr std::uint64_t kMaximumTicks = 10'000'000;
  for (std::uint64_t globalTick = 0; globalTick < kMaximumTicks; ++globalTick) {
    bool anyActive = false;
    for (auto& state : states) {
      if (!state->converter.stopped) {
        anyActive = true;
        break;
      }
    }
    if (!anyActive) {
      break;
    }

    bool progress = true;
    int round = 0;
    while (progress && ++round < 64) {
      progress = false;
      for (auto& state : states) {
        TrackConverter& converter = state->converter;
        if (converter.stopped) {
          continue;
        }
        converter.tick = globalTick;
        if (converter.pendingDuration > 0) {
          continue;
        }
        if (converter.waitingEe) {
          const int self = converter.result.sourceTrack;
          if (self >= 0 && self < 32 &&
              syncSignals[static_cast<std::size_t>(self)]) {
            syncSignals[static_cast<std::size_t>(self)] = false;
            converter.waitingEe = false;
            ++converter.position;
            progress = true;
          } else {
            continue;
          }
        }
        const std::size_t before = converter.position;
        const bool wasWaiting = converter.waitingEe;
        const int beforePending = converter.pendingDuration;
        converter.pumpUntilBlock();
        if (converter.stopped || converter.waitingEe != wasWaiting ||
            converter.pendingDuration != beforePending ||
            converter.position != before) {
          progress = true;
        }
      }
    }

    bool anyCountdown = false;
    bool anyWaiting = false;
    bool anyRunnable = false;
    for (auto& state : states) {
      TrackConverter& converter = state->converter;
      if (converter.stopped) {
        continue;
      }
      if (converter.pendingDuration > 0) {
        anyCountdown = true;
      } else if (converter.waitingEe) {
        anyWaiting = true;
      } else {
        anyRunnable = true;
      }
    }

    if (!anyCountdown && !anyRunnable && anyWaiting) {
      // Deadlock: every live track is parked on EE with no sender left.
      for (auto& state : states) {
        TrackConverter& converter = state->converter;
        if (converter.stopped || !converter.waitingEe) {
          continue;
        }
        converter.warning(
            "track sync wait (EE) never received EF; waiting abandoned");
        converter.waitingEe = false;
        ++converter.position;
        break;
      }
      continue;
    }

    if (!anyCountdown && !anyWaiting && !anyRunnable) {
      break;
    }

    // End of this Timer-B period: consume one tick of each sounding duration.
    for (auto& state : states) {
      TrackConverter& converter = state->converter;
      if (converter.pendingDuration > 0) {
        --converter.pendingDuration;
      }
    }
  }

  for (auto& state : states) {
    TrackConverter& converter = state->converter;
    if (!converter.stopped && converter.waitingEe) {
      converter.warning(
          "track still waiting on EE when conversion ended");
    }
    converter.result.endTick = converter.tick;
    sequence.endTick =
        std::max(sequence.endTick, converter.result.endTick);
    if (converter.emitted) {
      sequence.tracks.push_back(std::move(converter.result));
    }
  }

  std::stable_sort(sequence.tempos.begin(), sequence.tempos.end(),
                   [](const MidiTempo& left, const MidiTempo& right) {
                     if (left.tick != right.tick) {
                       return left.tick < right.tick;
                     }
                     return left.order < right.order;
                   });
  return sequence;
}

// Tracks often carry different L periods. Wrapping from the earliest F1 leaves
// short-period parts silent (and desyncs hybrid FM). Prefer the *majority*
// period as the song cycle, expand only compatible short tracks into that
// window, and wrap from that master loop point.
void alignSongLoopCycle(const std::uint8_t* data, std::size_t length,
                        const int* trackOffsets, int trackCount, int loops,
                        MidiSequence& sequence) {
  if (!sequence.hasSongLoop || loops < 1) {
    return;
  }

  std::vector<int> onceLimits(static_cast<std::size_t>(trackCount), 1);
  std::vector<int> twiceLimits(static_cast<std::size_t>(trackCount), 2);
  const MidiSequence once =
      convertMadrvMidiWithLimits(data, length, trackOffsets, trackCount,
                                 onceLimits);
  const MidiSequence twice =
      convertMadrvMidiWithLimits(data, length, trackOffsets, trackCount,
                                 twiceLimits);

  std::vector<std::uint64_t> endOnce(static_cast<std::size_t>(trackCount), 0);
  std::vector<std::uint64_t> period(static_cast<std::size_t>(trackCount), 0);
  for (const MidiTrack& track : once.tracks) {
    if (track.sourceTrack >= 0 && track.sourceTrack < trackCount) {
      endOnce[static_cast<std::size_t>(track.sourceTrack)] = track.endTick;
    }
  }
  for (const MidiTrack& track : twice.tracks) {
    if (track.sourceTrack < 0 || track.sourceTrack >= trackCount) {
      continue;
    }
    const std::size_t index = static_cast<std::size_t>(track.sourceTrack);
    if (track.endTick > endOnce[index]) {
      period[index] = track.endTick - endOnce[index];
    }
  }

  // Majority period (tie -> longer). Avoid one-off outliers such as a single
  // track whose measured period is twice everyone else's.
  std::map<std::uint64_t, int> votes;
  for (std::uint64_t value : period) {
    if (value > 0) {
      ++votes[value];
    }
  }
  std::uint64_t masterPeriod = 0;
  int bestVotes = 0;
  for (const auto& entry : votes) {
    const std::uint64_t value = entry.first;
    const int count = entry.second;
    if (count > bestVotes || (count == bestVotes && value > masterPeriod)) {
      bestVotes = count;
      masterPeriod = value;
    }
  }
  if (masterPeriod == 0) {
    return;
  }
  // If a longer period is an exact multiple of the majority (common in MDR
  // A/B phrase forms), wrap on that longer cycle so hybrid FM stays aligned.
  for (const auto& entry : votes) {
    const std::uint64_t value = entry.first;
    if (value > masterPeriod && value / masterPeriod <= 4 &&
        value % masterPeriod == 0) {
      masterPeriod = value;
    }
  }

  std::uint64_t masterLoopStart = sequence.loopStartTick;
  bool foundMaster = false;
  for (int track = 0; track < trackCount; ++track) {
    const std::size_t index = static_cast<std::size_t>(track);
    if (period[index] != masterPeriod || endOnce[index] < masterPeriod) {
      continue;
    }
    const std::uint64_t candidate = endOnce[index] - masterPeriod;
    if (!foundMaster || candidate < masterLoopStart) {
      masterLoopStart = candidate;
      foundMaster = true;
    }
  }
  if (!foundMaster) {
    return;
  }

  constexpr int kMaxExpand = 64;
  const std::uint64_t minExpandablePeriod =
      std::max<std::uint64_t>(1, masterPeriod / 32);
  const std::uint64_t targetEnd =
      masterLoopStart + masterPeriod * static_cast<std::uint64_t>(loops);
  std::vector<int> limits(static_cast<std::size_t>(trackCount), loops);
  bool needsRefill = masterLoopStart != sequence.loopStartTick;
  for (int track = 0; track < trackCount; ++track) {
    const std::size_t index = static_cast<std::size_t>(track);
    const std::uint64_t trackPeriod = period[index];
    if (trackPeriod == 0 || trackPeriod == masterPeriod) {
      continue;
    }
    // Tiny periods (e.g. 2-tick L) must not be unrolled across the master.
    if (trackPeriod < minExpandablePeriod) {
      continue;
    }
    int need = loops;
    bool reached = false;
    while (need <= kMaxExpand) {
      const std::uint64_t projected =
          endOnce[index] +
          static_cast<std::uint64_t>(need - 1) * trackPeriod;
      if (projected >= targetEnd) {
        reached = true;
        break;
      }
      ++need;
    }
    if (!reached) {
      continue;
    }
    if (need != loops) {
      needsRefill = true;
    }
    limits[index] = need;
  }
  if (!needsRefill) {
    sequence.loopStartTick = masterLoopStart;
    if (sequence.endTick > targetEnd) {
      sequence.endTick = targetEnd;
    }
    return;
  }

  sequence = convertMadrvMidiWithLimits(data, length, trackOffsets, trackCount,
                                        limits);
  sequence.hasSongLoop = true;
  sequence.loopStartTick = masterLoopStart;
  if (sequence.endTick > targetEnd) {
    sequence.endTick = targetEnd;
    for (MidiTrack& track : sequence.tracks) {
      track.events.erase(
          std::remove_if(track.events.begin(), track.events.end(),
                         [targetEnd](const MidiEvent& event) {
                           return event.tick > targetEnd;
                         }),
          track.events.end());
      if (track.endTick > targetEnd) {
        track.endTick = targetEnd;
      }
    }
    sequence.tempos.erase(
        std::remove_if(sequence.tempos.begin(), sequence.tempos.end(),
                       [targetEnd](const MidiTempo& tempo) {
                         return tempo.tick > targetEnd;
                       }),
        sequence.tempos.end());
  }
}

MidiSequence convertMadrvMidi(const std::uint8_t* data, std::size_t length,
                              const int* trackOffsets, int trackCount,
                              int loops) {
  if (data == nullptr || trackOffsets == nullptr || length == 0 ||
      trackCount <= 0) {
    throw MidiError("MDX has no track data");
  }
  if (loops < 1) {
    throw MidiError("MIDI loop count must be positive");
  }

  std::vector<int> limits(static_cast<std::size_t>(trackCount), loops);
  MidiSequence sequence = convertMadrvMidiWithLimits(
      data, length, trackOffsets, trackCount, limits);
  alignSongLoopCycle(data, length, trackOffsets, trackCount, loops, sequence);
  return sequence;
}

namespace {

// OPM Timer-B period in microseconds: 256 * (256 - tempo).
// mdxmini converts each period to samples with integer truncation:
//   samples += (sampleRate * periodUs) / 1'000'000
// Hybrid MIDI must accumulate in that sample domain, then convert back.
std::uint64_t samplesPerTick(std::uint8_t tempo, int sampleRate) {
  const std::uint64_t usPerTick =
      256u * (256u - static_cast<unsigned>(tempo));
  return (static_cast<std::uint64_t>(sampleRate) * usPerTick) / 1'000'000u;
}

std::uint64_t samplesToMicroseconds(std::uint64_t samples, int sampleRate) {
  return (samples * 1'000'000u) / static_cast<std::uint64_t>(sampleRate);
}

}  // namespace

std::vector<ScheduledMidiEvent> scheduleMidiEvents(
    const MidiSequence& sequence, int sampleRate) {
  struct PendingEvent {
    std::uint64_t tick = 0;
    std::uint64_t order = 0;
    int priority = 0;
    std::vector<std::uint8_t> bytes;
  };

  std::vector<PendingEvent> pending;
  std::uint64_t trackOrder = 0;
  for (const MidiTrack& track : sequence.tracks) {
    for (const MidiEvent& event : track.events) {
      pending.push_back({event.tick, (trackOrder << 32) + event.order,
                         eventPriority(event), event.bytes});
    }
    ++trackOrder;
  }
  std::stable_sort(pending.begin(), pending.end(),
                   [](const PendingEvent& left, const PendingEvent& right) {
                     if (left.tick != right.tick) {
                       return left.tick < right.tick;
                     }
                     if (left.priority != right.priority) {
                       return left.priority < right.priority;
                     }
                     return left.order < right.order;
                   });

  std::vector<MidiTempo> tempos = sequence.tempos;
  std::stable_sort(tempos.begin(), tempos.end(),
                   [](const MidiTempo& left, const MidiTempo& right) {
                     if (left.tick != right.tick) {
                       return left.tick < right.tick;
                     }
                     return left.order < right.order;
                   });

  std::vector<ScheduledMidiEvent> scheduled;
  scheduled.reserve(pending.size());
  std::uint64_t elapsedUs = 0;
  std::uint64_t elapsedSamples = 0;
  std::uint64_t previousTick = 0;
  std::uint8_t tempo = 0xc8;
  std::size_t tempoIndex = 0;

  auto advance = [&](std::uint64_t ticks) {
    if (sampleRate > 0) {
      elapsedSamples += ticks * samplesPerTick(tempo, sampleRate);
    } else {
      elapsedUs +=
          ticks * (256u * (256u - static_cast<unsigned>(tempo)));
    }
  };

  for (PendingEvent& event : pending) {
    while (tempoIndex < tempos.size() &&
           tempos[tempoIndex].tick <= event.tick) {
      const MidiTempo& change = tempos[tempoIndex++];
      advance(change.tick - previousTick);
      previousTick = change.tick;
      tempo = change.value;
    }
    // Peek time at event without committing tempo-segment state beyond it.
    std::uint64_t eventTime = 0;
    if (sampleRate > 0) {
      eventTime = samplesToMicroseconds(
          elapsedSamples +
              (event.tick - previousTick) * samplesPerTick(tempo, sampleRate),
          sampleRate);
    } else {
      eventTime =
          elapsedUs + (event.tick - previousTick) *
                          (256u * (256u - static_cast<unsigned>(tempo)));
    }
    scheduled.push_back({eventTime, std::move(event.bytes)});
  }
  return scheduled;
}

std::uint64_t midiDurationMicroseconds(const MidiSequence& sequence,
                                       int sampleRate) {
  return midiTickMicroseconds(sequence, sequence.endTick, sampleRate);
}

std::uint64_t midiTickMicroseconds(const MidiSequence& sequence,
                                   std::uint64_t tick, int sampleRate) {
  std::vector<MidiTempo> tempos = sequence.tempos;
  std::stable_sort(tempos.begin(), tempos.end(),
                   [](const MidiTempo& left, const MidiTempo& right) {
                     if (left.tick != right.tick) {
                       return left.tick < right.tick;
                     }
                     return left.order < right.order;
                   });

  std::uint64_t elapsedUs = 0;
  std::uint64_t elapsedSamples = 0;
  std::uint64_t previousTick = 0;
  std::uint8_t tempo = 0xc8;
  for (const MidiTempo& change : tempos) {
    if (change.tick > tick) {
      break;
    }
    const std::uint64_t span = change.tick - previousTick;
    if (sampleRate > 0) {
      elapsedSamples += span * samplesPerTick(tempo, sampleRate);
    } else {
      elapsedUs += span * (256u * (256u - static_cast<unsigned>(tempo)));
    }
    previousTick = change.tick;
    tempo = change.value;
  }
  const std::uint64_t remain = tick - previousTick;
  if (sampleRate > 0) {
    return samplesToMicroseconds(
        elapsedSamples + remain * samplesPerTick(tempo, sampleRate),
        sampleRate);
  }
  return elapsedUs +
         remain * (256u * (256u - static_cast<unsigned>(tempo)));
}

std::vector<std::vector<std::uint8_t>> midiLoopRestoreMessages(
    const MidiSequence& sequence) {
  struct ChannelState {
    int bankMsb = -1;
    int bankLsb = -1;
    int program = -1;
    int volume = -1;
    int expression = -1;
    int pan = -1;
    int reverb = -1;
    int chorus = -1;
  };
  std::array<ChannelState, 16> channels{};
  if (!sequence.hasSongLoop) {
    return {};
  }
  for (const MidiTrack& track : sequence.tracks) {
    for (const MidiEvent& event : track.events) {
      if (event.tick >= sequence.loopStartTick || event.bytes.empty()) {
        continue;
      }
      const std::uint8_t status = event.bytes[0];
      if (status < 0x80 || status >= 0xf0) {
        continue;
      }
      const int channel = status & 0x0f;
      const std::uint8_t kind = status & 0xf0;
      ChannelState& state = channels[static_cast<std::size_t>(channel)];
      if (kind == 0xc0 && event.bytes.size() >= 2) {
        state.program = event.bytes[1] & 0x7f;
      } else if (kind == 0xb0 && event.bytes.size() >= 3) {
        const int controller = event.bytes[1];
        const int value = event.bytes[2] & 0x7f;
        switch (controller) {
          case 0:
            state.bankMsb = value;
            break;
          case 32:
            state.bankLsb = value;
            break;
          case 7:
            state.volume = value;
            break;
          case 11:
            state.expression = value;
            break;
          case 10:
            state.pan = value;
            break;
          case 91:
            state.reverb = value;
            break;
          case 93:
            state.chorus = value;
            break;
          default:
            break;
        }
      }
    }
  }

  std::vector<std::vector<std::uint8_t>> messages;
  for (int channel = 0; channel < 16; ++channel) {
    const ChannelState& state = channels[static_cast<std::size_t>(channel)];
    const std::uint8_t cc = static_cast<std::uint8_t>(0xb0 | channel);
    const std::uint8_t pc = static_cast<std::uint8_t>(0xc0 | channel);
    if (state.bankMsb >= 0) {
      messages.push_back({cc, 0, static_cast<std::uint8_t>(state.bankMsb)});
    }
    if (state.bankLsb >= 0) {
      messages.push_back({cc, 32, static_cast<std::uint8_t>(state.bankLsb)});
    }
    if (state.program >= 0) {
      messages.push_back({pc, static_cast<std::uint8_t>(state.program)});
    }
    if (state.volume >= 0) {
      messages.push_back({cc, 7, static_cast<std::uint8_t>(state.volume)});
    }
    if (state.expression >= 0) {
      messages.push_back({cc, 11, static_cast<std::uint8_t>(state.expression)});
    }
    if (state.pan >= 0) {
      messages.push_back({cc, 10, static_cast<std::uint8_t>(state.pan)});
    }
    if (state.reverb >= 0) {
      messages.push_back({cc, 91, static_cast<std::uint8_t>(state.reverb)});
    }
    if (state.chorus >= 0) {
      messages.push_back({cc, 93, static_cast<std::uint8_t>(state.chorus)});
    }
  }
  return messages;
}

void playScheduledMidiEvents(
    const std::vector<ScheduledMidiEvent>& events,
    std::uint64_t loopStartUs, bool infinite,
    std::chrono::steady_clock::time_point start,
    const std::function<bool()>& shouldStop,
    const std::function<void(const std::vector<std::uint8_t>&)>& send,
    const SongPositionClock& songClock, std::chrono::microseconds lead,
    const std::vector<std::vector<std::uint8_t>>& loopRestore,
    std::uint64_t loopEndUs) {
  if (events.empty()) {
    return;
  }

  const std::uint64_t cycleEndUs =
      loopEndUs != std::numeric_limits<std::uint64_t>::max() &&
              loopEndUs > loopStartUs
          ? loopEndUs
          : events.back().microseconds;
  const bool canLoop =
      infinite && loopStartUs != std::numeric_limits<std::uint64_t>::max() &&
      cycleEndUs > loopStartUs;
  std::size_t loopIndex = 0;
  if (canLoop) {
    while (loopIndex < events.size() &&
           events[loopIndex].microseconds < loopStartUs) {
      ++loopIndex;
    }
    if (loopIndex >= events.size()) {
      return;
    }
  }

  const std::int64_t leadUs = lead.count();
  auto timeOrigin = start;
  std::int64_t cycleBase = 0;
  bool useAudioClock = false;
  std::size_t index = 0;
  bool firstPass = true;
  // Track live note-ons so loop wraps only need a few Note Offs instead of a
  // large All-Notes-Off + program restore flush (which occasionally delays the
  // next downbeat, often a crash cymbal).
  std::array<std::array<bool, 128>, 16> sounding{};
  (void)loopRestore;

  auto waitForEvent = [&](std::uint64_t eventUs) {
    const std::int64_t songTarget =
        static_cast<std::int64_t>(eventUs) - leadUs;
    while (!(shouldStop && shouldStop())) {
      if (songClock) {
        const std::int64_t audioUs = songClock();
        if (audioUs >= 0) {
          if (!useAudioClock) {
            // AudioQueue sample time 0 is song time 0 (both sides share `start`).
            useAudioClock = true;
            cycleBase = 0;
          }
          const std::int64_t target = cycleBase + songTarget;
          if (audioUs >= target) {
            return;
          }
          const std::int64_t remainUs = target - audioUs;
          const auto sleepFor = std::chrono::microseconds(
              std::clamp<std::int64_t>(remainUs / 2, 200, 2'000));
          std::this_thread::sleep_for(sleepFor);
          continue;
        }
      }
      // wall-clock fallback before the audio device clock is available
      const auto deadline =
          timeOrigin + std::chrono::microseconds(eventUs) - lead;
      std::this_thread::sleep_until(deadline);
      return;
    }
  };

  auto observeMessage = [&](const std::vector<std::uint8_t>& message) {
    if (message.size() < 2) {
      return;
    }
    const std::uint8_t status = message[0];
    if (status < 0x80 || status >= 0xf0) {
      return;
    }
    const int channel = status & 0x0f;
    const std::uint8_t kind = status & 0xf0;
    if (kind == 0x90 && message.size() >= 3) {
      const int key = message[1] & 0x7f;
      sounding[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)] =
          message[2] != 0;
    } else if (kind == 0x80 && message.size() >= 2) {
      const int key = message[1] & 0x7f;
      sounding[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)] =
          false;
    }
  };

  // Songs like MEGALITH dump 100+ CC/RPN messages on the first tick before any
  // note-on. Sending that burst in the same instant as the downbeat makes many
  // external modules drop the first notes (and hybrid OPM alone then sounds
  // like the opening was cut). Deliver pre-note setup slightly before start.
  std::size_t firstNoteOn = events.size();
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& bytes = events[i].bytes;
    if (bytes.size() >= 3 && (bytes[0] & 0xf0) == 0x90 && bytes[2] != 0) {
      firstNoteOn = i;
      break;
    }
  }
  if (firstNoteOn > 0 && !(shouldStop && shouldStop())) {
    const auto setupDeadline =
        start - std::chrono::milliseconds(40) - lead;
    if (setupDeadline > std::chrono::steady_clock::now()) {
      std::this_thread::sleep_until(setupDeadline);
    }
    for (; index < firstNoteOn; ++index) {
      if (shouldStop && shouldStop()) {
        return;
      }
      send(events[index].bytes);
      observeMessage(events[index].bytes);
    }
  }

  auto resetForLoop = [&] {
    // Light wrap: only release notes that are still marked on. Leave the drum
    // channel alone so open crash/ride at the loop point is not choked.
    for (int channel = 0; channel < 16; ++channel) {
      if (channel == 9) {
        continue;
      }
      for (int key = 0; key < 128; ++key) {
        if (!sounding[static_cast<std::size_t>(channel)]
                     [static_cast<std::size_t>(key)]) {
          continue;
        }
        send({static_cast<std::uint8_t>(0x80 | channel),
              static_cast<std::uint8_t>(key), 0x00});
        sounding[static_cast<std::size_t>(channel)]
                [static_cast<std::size_t>(key)] = false;
      }
    }
  };

  while (true) {
    if (shouldStop && shouldStop()) {
      break;
    }
    if (index >= events.size()) {
      if (!canLoop) {
        break;
      }
      // Gate < 8 ends the last Note Off before F1. Wait out that trailing
      // silence so each wrap matches the OPM/MML cycle (endTick), not the
      // last MIDI message.
      waitForEvent(cycleEndUs);
      if (shouldStop && shouldStop()) {
        break;
      }
      // Re-anchor using the audio clock from *before* the wrap flush so reset
      // latency cannot accumulate into later cycles. Fall back to the musical
      // period when the audio clock is unavailable.
      if (useAudioClock && songClock) {
        const std::int64_t wrapAudio = songClock();
        if (wrapAudio >= 0) {
          cycleBase =
              wrapAudio - static_cast<std::int64_t>(loopStartUs);
        } else {
          cycleBase += static_cast<std::int64_t>(cycleEndUs) -
                       static_cast<std::int64_t>(loopStartUs);
        }
      } else {
        timeOrigin += std::chrono::microseconds(cycleEndUs - loopStartUs);
      }
      resetForLoop();
      index = loopIndex;
      firstPass = false;
      continue;
    }
    const ScheduledMidiEvent& event = events[index++];
    if (!firstPass && event.microseconds < loopStartUs) {
      continue;
    }
    waitForEvent(event.microseconds);
    if (shouldStop && shouldStop()) {
      break;
    }
    send(event.bytes);
    observeMessage(event.bytes);
  }
  if (!(shouldStop && shouldStop()) && !canLoop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
  }
}

void writeStandardMidi(const MidiSequence& sequence,
                       const std::filesystem::path& path,
                       const std::string& title) {
  if (sequence.tracks.size() >= std::numeric_limits<std::uint16_t>::max()) {
    throw MidiError("too many tracks for an SMF");
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw MidiError("cannot create MIDI file: " + path.string());
  }

  output.write("MThd", 4);
  writeBigEndian(output, 6, 4);
  writeBigEndian(output, 1, 2);
  writeBigEndian(output,
                 static_cast<std::uint32_t>(sequence.tracks.size() + 1), 2);
  writeBigEndian(output, static_cast<std::uint32_t>(sequence.ppqn), 2);

  std::vector<std::uint8_t> conductor;
  if (!title.empty()) {
    appendMeta(conductor, 0, 0x03,
               std::vector<std::uint8_t>(title.begin(), title.end()));
  }
  std::uint64_t previousTick = 0;
  std::uint64_t lastTempoTick = std::numeric_limits<std::uint64_t>::max();
  std::uint8_t lastTempoValue = 0;
  for (const MidiTempo& tempo : sequence.tempos) {
    if (tempo.tick == lastTempoTick && tempo.value == lastTempoValue) {
      continue;
    }
    const std::uint32_t microsPerQuarter =
        256u * (256u - static_cast<unsigned>(tempo.value)) *
        static_cast<unsigned>(sequence.ppqn);
    const std::vector<std::uint8_t> value = {
        static_cast<std::uint8_t>((microsPerQuarter >> 16) & 0xff),
        static_cast<std::uint8_t>((microsPerQuarter >> 8) & 0xff),
        static_cast<std::uint8_t>(microsPerQuarter & 0xff),
    };
    appendMeta(conductor, tempo.tick - previousTick, 0x51, value);
    previousTick = tempo.tick;
    lastTempoTick = tempo.tick;
    lastTempoValue = tempo.value;
  }
  appendMeta(conductor, sequence.endTick - previousTick, 0x2f, {});
  writeChunk(output, conductor);

  for (const MidiTrack& source : sequence.tracks) {
    std::vector<MidiEvent> events = source.events;
    std::stable_sort(events.begin(), events.end(),
                     [](const MidiEvent& left, const MidiEvent& right) {
                       if (left.tick != right.tick) {
                         return left.tick < right.tick;
                       }
                       const int leftPriority = eventPriority(left);
                       const int rightPriority = eventPriority(right);
                       if (leftPriority != rightPriority) {
                         return leftPriority < rightPriority;
                       }
                       return left.order < right.order;
                     });

    std::vector<std::uint8_t> track;
    const std::string name = "MADRV track " +
                             std::to_string(source.sourceTrack + 1);
    appendMeta(track, 0, 0x03,
               std::vector<std::uint8_t>(name.begin(), name.end()));
    previousTick = 0;
    for (const MidiEvent& event : events) {
      if (event.bytes.empty()) {
        continue;
      }
      writeVariable(track, event.tick - previousTick);
      if (event.bytes.front() == 0xf0 || event.bytes.front() == 0xf7) {
        track.push_back(event.bytes.front());
        writeVariable(track, event.bytes.size() - 1);
        track.insert(track.end(), event.bytes.begin() + 1, event.bytes.end());
      } else {
        track.insert(track.end(), event.bytes.begin(), event.bytes.end());
      }
      previousTick = event.tick;
    }
    appendMeta(track, source.endTick > previousTick ? source.endTick - previousTick
                                                    : 0,
               0x2f, {});
    writeChunk(output, track);
  }
  if (!output) {
    throw MidiError("failed while writing MIDI file: " + path.string());
  }
}

void resolveSoftwareSynthPreset(int bankMsb, int program, int& outBank,
                                int& outProgram) {
  const int sanitizedProgram = program & 0x7f;
  // ScummVM's shared MT-32 → GM table. Used when an SC-55 SoundFont lacks the
  // hardware's variation-127 (MT-32/CM-64) tone map.
  static constexpr std::array<std::uint8_t, 128> kMt32ToGm = {
      // clang-format off
      //  0    1    2    3    4    5    6    7    8    9    A    B    C    D    E    F
          0,   1,   0,   4,   4,   5,   5,   3,  16,  17,  18,  16,  16,  19,  20,  21,  // 0x
          6,   6,   6,   7,   7,   7,   8, 112,  62,  62,  63,  63,  38,  38,  39,  39,  // 1x
         88,  95,  52,  98,  97,  99,  14,  54, 102,  96,  53, 102,  81, 100,  14,  80,  // 2x
         48,  48,  49,  45,  41,  40,  42,  42,  43,  46,  45,  24,  25,  28,  27, 104,  // 3x
         32,  32,  34,  33,  36,  37,  35,  35,  79,  73,  72,  72,  74,  75,  64,  65,  // 4x
         66,  67,  71,  71,  68,  69,  70,  22,  56,  59,  57,  57,  60,  60,  58,  61,  // 5x
         61,  11,  11,  98,  14,   9,  14,  13,  12, 107, 107,  77,  78,  78,  76,  76,  // 6x
         47, 117, 127, 118, 118, 116, 115, 119, 115, 112,  55, 124, 123,   0,  14, 117,  // 7x
      // clang-format on
  };

  if (bankMsb == 127) {
    // Prefer E.Piano 1/2 for the MT-32 set's electric pianos (SCB-55 names),
    // matching GS capitals present in SC-55-style SoundFonts.
    outBank = 0;
    outProgram = kMt32ToGm[static_cast<std::size_t>(sanitizedProgram)];
    return;
  }
  if (bankMsb == 126) {
    // CM-32P map is also absent from the lightweight SF2; fall back to GM.
    outBank = 0;
    outProgram = sanitizedProgram;
    return;
  }
  outBank = bankMsb & 0x7f;
  outProgram = sanitizedProgram;
}

int resolveRhythmProgram(int program) {
  static constexpr std::array<int, 10> kGsDrumKits = {
      0, 8, 16, 24, 25, 32, 40, 48, 56, 127,
  };
  const int sanitized = program & 0x7f;
  const auto isKit = [&](int value) {
    return std::find(kGsDrumKits.begin(), kGsDrumKits.end(), value) !=
           kGsDrumKits.end();
  };
  if (isKit(sanitized)) {
    return sanitized;
  }
  // Some MADRV files store Roland's 1-based display numbers (17=Power, ...).
  // Do not treat 57 as SFX (56): that kit lacks normal kick/snare/hat notes, so
  // grooves that used FluidSynth's old "missing → Standard" fallback went silent.
  if (sanitized > 0 && isKit(sanitized - 1)) {
    if (sanitized - 1 == 56) {
      return 0;
    }
    return sanitized - 1;
  }
  return 0;
}

}  // namespace mpxadrv
