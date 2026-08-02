#include "midi.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
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

  void setBendRange(std::uint8_t range, bool emit) {
    if (midi && emit) {
      control(100, 0);
      control(101, 0);
      control(6, range);
    }
    bendSensitivity = range >= 2 && range <= 24 ? 32768 / range : 0;
    bendResponseCounter = 0;
    bendRangePending = midi && !emit;
  }

  void emitPendingBendRange() {
    if (!bendRangePending) {
      return;
    }
    bendRangePending = false;
    control(100, 0);
    control(101, 0);
    control(6, 12);
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
        control(kControllers[subcommand - 0x2c], parameters[0]);
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

  void run() {
    constexpr std::uint64_t kMaximumCommands = 10'000'000;
    std::uint64_t commandCount = 0;
    while (!stopped && position < bytes.size()) {
      if (++commandCount > kMaximumCommands) {
        throw MidiError("MDX track exceeded the command safety limit");
      }
      const std::size_t commandPosition = position;
      const std::uint8_t command = bytes[position++];

      const int noteMinimum = midi ? 0x60 : 0x80;
      if (command < noteMinimum) {
        advanceTime(static_cast<int>(command) + 1);
        suppressNextKeyOff = false;
        continue;
      }
      if (command <= 0xdf) {
        position = checkedAdvance(position, 1, bytes.size());
        note(command - noteMinimum, static_cast<int>(bytes[position - 1]) + 1);
        continue;
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
        case 0xee:
          break;
        case 0xea:
        case 0xeb:
        case 0xec:
          position = checkedAdvance(position, 1, bytes.size());
          if ((bytes[position - 1] & 0x80) == 0) {
            position = checkedAdvance(position, 4, bytes.size());
          }
          break;
        case 0xe9:
        case 0xed:
        case 0xef:
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
            control(bytes[position - 2], bytes[position - 1]);
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
    result.endTick = tick;
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

  MidiSequence sequence;
  sequence.tempos.push_back({0, 0xc8, 0});
  for (int track = 0; track < trackCount; ++track) {
    if (trackOffsets[track] < 0 ||
        static_cast<std::size_t>(trackOffsets[track]) >= length) {
      continue;
    }
    TrackConverter converter(sequence);
    converter.result.sourceTrack = track;
    converter.bytes.assign(data, data + length);
    converter.position = static_cast<std::size_t>(trackOffsets[track]);
    converter.loopLimit = loops;
    converter.midi = track >= 16;
    converter.channel = track & 0x0f;
    if (converter.midi) {
      converter.setBendRange(12, false);
    }
    converter.run();
    sequence.endTick = std::max(sequence.endTick, converter.result.endTick);
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

void playScheduledMidiEvents(
    const std::vector<ScheduledMidiEvent>& events,
    std::uint64_t loopStartUs, bool infinite,
    std::chrono::steady_clock::time_point start,
    const std::function<bool()>& shouldStop,
    const std::function<void(const std::vector<std::uint8_t>&)>& send) {
  if (events.empty()) {
    return;
  }

  const bool canLoop =
      infinite && loopStartUs != std::numeric_limits<std::uint64_t>::max() &&
      events.back().microseconds > loopStartUs;
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

  auto timeOrigin = start;
  std::size_t index = 0;
  bool firstPass = true;
  while (true) {
    if (shouldStop && shouldStop()) {
      break;
    }
    if (index >= events.size()) {
      if (!canLoop) {
        break;
      }
      // Replay from the song's L point without silencing hanging notes.
      index = loopIndex;
      firstPass = false;
      timeOrigin = std::chrono::steady_clock::now() -
                   std::chrono::microseconds(loopStartUs);
      continue;
    }
    const ScheduledMidiEvent& event = events[index++];
    if (!firstPass && event.microseconds < loopStartUs) {
      continue;
    }
    std::this_thread::sleep_until(
        timeOrigin + std::chrono::microseconds(event.microseconds));
    if (shouldStop && shouldStop()) {
      break;
    }
    send(event.bytes);
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
