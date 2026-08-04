#include "mdr.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

namespace mpxadrv {
namespace {

std::uint16_t readBigEndianWord(const std::vector<std::uint8_t>& bytes,
                                std::size_t position) {
  if (position + 2 > bytes.size()) {
    throw MdrError("truncated MDR offset table");
  }
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[position]) << 8) |
      bytes[position + 1]);
}

void writeBigEndianWord(std::vector<std::uint8_t>& bytes,
                        std::size_t position, std::size_t value) {
  if (value > 0xffff || position + 2 > bytes.size()) {
    throw MdrError("converted MDX exceeds its 16-bit offset limit");
  }
  bytes[position] = static_cast<std::uint8_t>(value >> 8);
  bytes[position + 1] = static_cast<std::uint8_t>(value & 0xff);
}

std::size_t e0CommandLength(const std::vector<std::uint8_t>& bytes,
                            std::size_t position, std::size_t end) {
  if (position + 2 > end) {
    throw MdrError("truncated E0 command in MDR hardware track");
  }
  const std::uint8_t subcommand = bytes[position + 1];
  if (subcommand == 0xff) {
    return 2;
  }
  if (subcommand > 0x1d) {
    throw MdrError("unsupported E0 command in MDR hardware track");
  }
  if (subcommand == 0x00) {
    if (position + 3 > end) {
      throw MdrError("truncated E0 distortion command");
    }
    return (bytes[position + 2] & 0x80) == 0 ? 7 : 3;
  }
  if (subcommand == 0x0e) {
    if (position + 3 > end) {
      throw MdrError("truncated E0 direct-MIDI command");
    }
    return 3 + static_cast<std::size_t>(bytes[position + 2]) + 1;
  }
  static constexpr std::array<int, 30> kParameterCounts = {
      0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0,
      2, 1, 0, 0, 1, 1, 3, 3, 4, 4, 1, 1, 1, 1, 1,
  };
  const std::size_t length =
      2 + static_cast<std::size_t>(kParameterCounts[subcommand]);
  if (position + length > end) {
    throw MdrError("truncated E0 command in MDR hardware track");
  }
  return length;
}

std::size_t e2CommandLength(const std::vector<std::uint8_t>& bytes,
                            std::size_t position, std::size_t end) {
  if (position + 2 > end) {
    throw MdrError("truncated E2 command in MDR MIDI track");
  }
  const std::uint8_t subcommand = bytes[position + 1];
  std::size_t length = 0;
  if (subcommand == 0x00 || subcommand == 0x14 || subcommand == 0x1c ||
      subcommand == 0x35 || subcommand == 0x36) {
    length = 2;
  } else if (subcommand == 0x1d ||
             (subcommand >= 0x23 && subcommand <= 0x27) ||
             subcommand == 0x2b ||
             (subcommand >= 0x2c && subcommand <= 0x30) ||
             (subcommand >= 0x38 && subcommand <= 0x3a)) {
    length = 3;
  } else if ((subcommand >= 0x1e && subcommand <= 0x22) ||
             subcommand == 0x32 || subcommand == 0x34 ||
             subcommand == 0x37) {
    length = 4;
  } else if (subcommand >= 0x28 && subcommand <= 0x2a) {
    if (position + 3 > end) {
      throw MdrError("truncated packed E2 command in MDR MIDI track");
    }
    length = 3 + static_cast<std::size_t>(bytes[position + 2]);
  } else if (subcommand == 0x31) {
    if (position + 6 > end) {
      throw MdrError("truncated E2 memory-write command in MDR MIDI track");
    }
    length = 6 + static_cast<std::size_t>(bytes[position + 5]);
  } else if (subcommand == 0x33) {
    std::size_t cursor = position + 2;
    while (cursor < end && bytes[cursor] != 0) {
      ++cursor;
    }
    if (cursor >= end) {
      throw MdrError("unterminated E2 display command in MDR MIDI track");
    }
    length = cursor - position + 1;
  } else {
    throw MdrError("unsupported E2 subcommand in MDR MIDI track");
  }
  if (position + length > end) {
    throw MdrError("truncated E2 command in MDR MIDI track");
  }
  return length;
}

std::size_t standardCommandLength(const std::vector<std::uint8_t>& bytes,
                                  std::size_t position, std::size_t end,
                                  bool midi) {
  if (position >= end) {
    throw MdrError("truncated MDR hardware track");
  }
  const std::uint8_t command = bytes[position];
  const std::uint8_t noteMinimum = midi ? 0x60 : 0x80;
  if (command < noteMinimum) {
    return 1;
  }
  if (command <= 0xdf) {
    if (position + 2 > end) {
      throw MdrError("truncated note in MDR hardware track");
    }
    return 2;
  }
  if (command == 0xe0) {
    return e0CommandLength(bytes, position, end);
  }
  if (command == 0xe2 && midi) {
    return e2CommandLength(bytes, position, end);
  }
  std::size_t length = 0;
  switch (command) {
    case 0xe1:
    case 0xe9:
    case 0xed:
    case 0xef:
    case 0xf0:
    case 0xf8:
    case 0xfb:
    case 0xfc:
    case 0xfd:
    case 0xff:
      length = 2;
      break;
    case 0xe7:
      if (position + 2 > end) {
        throw MdrError("truncated E7 command in MDR hardware track");
      }
      length = bytes[position + 1] == 1 ? 3 : 2;
      break;
    case 0xe8:
    case 0xee:
    case 0xf7:
    case 0xf9:
    case 0xfa:
      length = 1;
      break;
    case 0xea:
    case 0xeb:
    case 0xec:
      if (position + 2 > end) {
        throw MdrError("truncated LFO command in MDR hardware track");
      }
      length = (bytes[position + 1] & 0x80) == 0 ? 6 : 2;
      break;
    case 0xf1:
      if (position + 2 > end) {
        throw MdrError("truncated song loop in MDR hardware track");
      }
      length = bytes[position + 1] == 0 ? 2 : 3;
      break;
    case 0xf2:
    case 0xf3:
    case 0xf4:
    case 0xf5:
    case 0xf6:
    case 0xfe:
      length = 3;
      break;
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
      throw MdrError("unsupported MADRV command " +
                     std::to_string(static_cast<unsigned>(command)) +
                     " in MDR hardware track");
    default:
      throw MdrError("unknown command in MDR hardware track");
  }
  if (position + length > end) {
    throw MdrError("truncated command in MDR hardware track");
  }
  return length;
}

std::optional<int> initialHardwareChannel(const MdrFile& mdr, int track) {
  std::size_t position = static_cast<std::size_t>(mdr.trackOffsets[track]);
  const std::size_t end =
      track + 1 < MdrFile::kTrackCount
          ? static_cast<std::size_t>(mdr.trackOffsets[track + 1])
          : mdr.toneOffset;
  bool midi = track >= 16;
  int channel = track & 0x0f;
  while (position < end) {
    const std::uint8_t command = mdr.data[position];
    if (command == 0xf1) {
      return std::nullopt;
    }
    const std::uint8_t noteMinimum = midi ? 0x60 : 0x80;
    if (command >= noteMinimum && command <= 0xdf) {
      if (!midi) {
        return channel;
      }
    }
    const std::size_t length =
        standardCommandLength(mdr.data, position, end, midi);
    if (command == 0xe0 && mdr.data[position + 1] == 0x08) {
      const std::uint8_t value = mdr.data[position + 2];
      midi = (value & 0x80) != 0;
      channel = value & 0x0f;
    }
    position += length;
  }
  return std::nullopt;
}

void appendNeutralCommands(std::vector<std::uint8_t>& output,
                           std::size_t length) {
  while (length > 0) {
    if (length == 2 || length == 4) {
      output.push_back(0xf8);
      output.push_back(0x08);
      length -= 2;
    } else {
      output.push_back(0xf3);
      output.push_back(0x00);
      output.push_back(0x00);
      length -= 3;
    }
  }
}

// PCM8 v0.48 defines level 8 as the original sample volume and each level as
// a 2 dB step. libmdxmini instead applies the FM attenuation values linearly
// to PCM, which makes quiet PCM8 levels almost as loud as the highest one.
// Preserve libmdxmini's established default PCM balance at level 8 while
// retaining PCM8's 2 dB spacing below it. PCM8 levels above 8 require gain
// beyond libmdxmini's range, so those levels saturate at its maximum.
constexpr std::array<std::uint8_t, 16> kPcm8LinearGain = {
    17, 21, 27, 34, 42, 53, 67, 84,
    106, 127, 127, 127, 127, 127, 127, 127,
};

int pcm8Level(std::uint8_t volume) {
  if ((volume & 0x80) == 0) {
    return std::min<int>(volume, 15);
  }

  // MADRV's PCMVEL_TBL maps raw @v attenuation to PCM8's 16 levels.
  constexpr std::array<std::uint8_t, 15> kUpperBounds = {
      2, 5, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 34, 37, 40,
  };
  const int attenuation = volume & 0x7f;
  for (std::size_t index = 0; index < kUpperBounds.size(); ++index) {
    if (attenuation < kUpperBounds[index]) {
      return 15 - static_cast<int>(index);
    }
  }
  return 0;
}

std::uint8_t pcm8VolumeForMdxmini(std::uint8_t volume) {
  const std::uint8_t gain = kPcm8LinearGain[pcm8Level(volume)];
  // A raw MDX volume byte is decoded by libmdxmini as 255 - byte.
  return static_cast<std::uint8_t>(0xff - gain);
}

std::vector<std::uint8_t> neutralizedHardwareTrack(const MdrFile& mdr,
                                                   int track, bool pcm) {
  std::size_t position = static_cast<std::size_t>(mdr.trackOffsets[track]);
  const std::size_t end =
      track + 1 < MdrFile::kTrackCount
          ? static_cast<std::size_t>(mdr.trackOffsets[track + 1])
          : mdr.toneOffset;
  bool midi = track >= 16;
  std::vector<std::uint8_t> output;
  output.reserve(end - position);
  if (pcm) {
    // MADRV and PCM8 default to level 8. Establish the same default before a
    // track that starts playing without an explicit FB command.
    output.push_back(0xfb);
    output.push_back(pcm8VolumeForMdxmini(8));
  }
  while (position < end) {
    const std::uint8_t command = mdr.data[position];
    const std::size_t length =
        standardCommandLength(mdr.data, position, end, midi);
    if (command == 0xe0) {
      if (mdr.data[position + 1] == 0x08) {
        midi = (mdr.data[position + 2] & 0x80) != 0;
      }
      appendNeutralCommands(output, length);
    } else if (midi && command == 0xe2) {
      // MIDI system-exclusive setup is emitted by the separate MIDI player.
      appendNeutralCommands(output, length);
    } else if (command == 0xe8) {
      // The rebuilt MDX adds its own EX-PCM marker to the first track.
      output.push_back(0xf7);
    } else if (pcm && command == 0xfb) {
      output.push_back(command);
      output.push_back(pcm8VolumeForMdxmini(mdr.data[position + 1]));
    } else {
      output.insert(output.end(), mdr.data.begin() + position,
                    mdr.data.begin() + position + length);
    }
    position += length;
  }
  return output;
}

std::array<int, 16> findHardwareTrackSources(const MdrFile& mdr) {
  std::array<int, 16> sources{};
  sources.fill(-1);
  for (int track = 0; track < MdrFile::kTrackCount; ++track) {
    const std::optional<int> channel = initialHardwareChannel(mdr, track);
    if (!channel || *channel < 0 || *channel >= 16) {
      continue;
    }
    if (sources[*channel] < 0) {
      sources[*channel] = track;
    }
  }
  return sources;
}

}  // namespace

MdrFile loadMdr(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw MdrError("MDR file not found: " + path.string());
  }

  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                  std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    throw MdrError("MDR file is empty: " + path.string());
  }
  return parseMdr(std::move(bytes));
}

MdrFile parseMdr(std::vector<std::uint8_t> data) {
  if (data.empty()) {
    throw MdrError("MDR data is empty");
  }

  MdrFile result;
  result.data = std::move(data);

  const std::array<std::uint8_t, 3> titleEnd = {0x0d, 0x0a, 0x1a};
  const auto marker = std::search(result.data.begin(), result.data.end(),
                                  titleEnd.begin(), titleEnd.end());
  if (marker == result.data.end()) {
    throw MdrError("MDR title terminator was not found");
  }
  const std::size_t titleLength =
      static_cast<std::size_t>(marker - result.data.begin());
  result.title.assign(reinterpret_cast<const char*>(result.data.data()),
                      titleLength);

  const std::size_t pdxStart = titleLength + titleEnd.size();
  const auto pdxEnd = std::find(result.data.begin() + pdxStart,
                                result.data.end(), 0);
  if (pdxEnd == result.data.end()) {
    throw MdrError("MDR PCM filename is not terminated");
  }
  result.pdxName.assign(
      reinterpret_cast<const char*>(result.data.data() + pdxStart),
      static_cast<std::size_t>(pdxEnd - (result.data.begin() + pdxStart)));

  const std::size_t table =
      static_cast<std::size_t>(pdxEnd - result.data.begin()) + 1;
  result.dataOffset = table;
  constexpr std::size_t kOffsetTableBytes =
      2 * (MdrFile::kTrackCount + 1);  // Tone offset plus 32 tracks.
  if (table + kOffsetTableBytes > result.data.size()) {
    throw MdrError("MDR does not contain a complete 32-track offset table");
  }

  const std::size_t relativeToneOffset = readBigEndianWord(result.data, table);
  if (relativeToneOffset < kOffsetTableBytes ||
      relativeToneOffset > result.data.size() - table) {
    throw MdrError("MDR tone offset is outside the file");
  }
  result.toneOffset = table + relativeToneOffset;

  for (int track = 0; track < MdrFile::kTrackCount; ++track) {
    const std::size_t relative =
        readBigEndianWord(result.data, table + 2 + track * 2);
    if (relative < kOffsetTableBytes ||
        relative >= result.data.size() - table) {
      throw MdrError("MDR track " + std::to_string(track + 1) +
                     " offset is outside the file");
    }
    result.trackOffsets[track] = static_cast<int>(table + relative);
  }

  const std::size_t first =
      static_cast<std::size_t>(result.trackOffsets.front());
  const std::size_t signature =
      first < result.data.size() && result.data[first] == 0xe8 ? first + 1
                                                               : first;
  if (signature + 2 > result.data.size() ||
      result.data[signature] != 0xe0 || result.data[signature + 1] != 0xff) {
    throw MdrError("MDR E0 FF signature is missing from the first track");
  }

  for (int track = 0; track < MdrFile::kTrackCount; ++track) {
    const std::size_t begin =
        static_cast<std::size_t>(result.trackOffsets[track]);
    const std::size_t end =
        track + 1 < MdrFile::kTrackCount
            ? static_cast<std::size_t>(result.trackOffsets[track + 1])
            : result.toneOffset;
    if (end < begin) {
      throw MdrError("MDR track offsets are not ordered");
    }
    const std::size_t emptyLength =
        track == 0 ? (result.data[first] == 0xe8 ? 5 : 4) : 2;
    if (end - begin > emptyLength) {
      ++result.activeTracks;
    }
  }

  return result;
}

int countSeparableHardwareTracks(const MdrFile& mdr) {
  const std::array<int, 16> sources = findHardwareTrackSources(mdr);
  return static_cast<int>(
      std::count_if(sources.begin(), sources.end(),
                    [](int source) { return source >= 0; }));
}

std::vector<std::uint8_t> makeMdxCompatible(const MdrFile& mdr,
                                            bool includePdx) {
  if (mdr.dataOffset > mdr.data.size() || mdr.toneOffset > mdr.data.size()) {
    throw MdrError("MDR metadata is inconsistent");
  }
  const std::size_t first = static_cast<std::size_t>(mdr.trackOffsets[0]);
  const bool extendedPcm = first < mdr.data.size() && mdr.data[first] == 0xe8;
  const int trackCount = extendedPcm ? 16 : 9;
  const std::size_t tableBytes = 2 * static_cast<std::size_t>(trackCount + 1);

  std::size_t headerEnd = mdr.dataOffset;
  if (!includePdx && !mdr.pdxName.empty()) {
    if (mdr.pdxName.size() + 1 > headerEnd) {
      throw MdrError("MDR PCM filename is inconsistent");
    }
    headerEnd -= mdr.pdxName.size() + 1;
  }
  std::vector<std::uint8_t> result(mdr.data.begin(),
                                   mdr.data.begin() + headerEnd);
  if (!includePdx && !mdr.pdxName.empty()) {
    result.push_back(0);
  }
  const std::size_t table = result.size();
  result.resize(result.size() + tableBytes, 0);

  for (int track = 0; track < trackCount; ++track) {
    const std::size_t begin =
        static_cast<std::size_t>(mdr.trackOffsets[track]);
    const std::size_t end =
        static_cast<std::size_t>(mdr.trackOffsets[track + 1]);
    if (end < begin || end > mdr.data.size()) {
      throw MdrError("MDR track boundary is invalid");
    }
    writeBigEndianWord(result, table + 2 + static_cast<std::size_t>(track) * 2,
                       result.size() - table);
    if (track == 0) {
      const std::size_t signature = extendedPcm ? begin + 1 : begin;
      if (signature + 2 > end || mdr.data[signature] != 0xe0 ||
          mdr.data[signature + 1] != 0xff) {
        throw MdrError("cannot remove the MDR signature for MDX playback");
      }
      result.insert(result.end(), mdr.data.begin() + begin,
                    mdr.data.begin() + signature);
      result.insert(result.end(), mdr.data.begin() + signature + 2,
                    mdr.data.begin() + end);
    } else {
      result.insert(result.end(), mdr.data.begin() + begin,
                    mdr.data.begin() + end);
    }
  }

  writeBigEndianWord(result, table, result.size() - table);
  result.insert(result.end(), mdr.data.begin() + mdr.toneOffset,
                mdr.data.end());
  return result;
}

std::vector<std::uint8_t> makeMdxHardwareCompatible(
    const MdrFile& mdr, const std::vector<std::uint8_t>& conductorTrack) {
  const std::array<int, 16> sources = findHardwareTrackSources(mdr);
  bool hasPcm = false;
  int hardwareTracks = 0;
  for (int channel = 0; channel < 16; ++channel) {
    if (sources[channel] < 0) {
      continue;
    }
    hasPcm = hasPcm || channel >= 8;
    ++hardwareTracks;
  }
  if (hardwareTracks == 0) {
    throw MdrError("MDR contains no separable FM/PCM hardware tracks");
  }

  const int trackCount = hasPcm ? 16 : 9;
  int conductorChannel = -1;
  if (!conductorTrack.empty()) {
    for (int channel = 0; channel < trackCount; ++channel) {
      if (sources[channel] < 0) {
        conductorChannel = channel;
        break;
      }
    }
    if (conductorChannel < 0) {
      throw MdrError("MDR hardware mix has no free tempo-conductor track");
    }
  }
  const std::size_t tableBytes = 2 * static_cast<std::size_t>(trackCount + 1);
  std::vector<std::uint8_t> result(mdr.data.begin(),
                                   mdr.data.begin() + mdr.dataOffset);
  const std::size_t table = result.size();
  result.resize(result.size() + tableBytes, 0);

  for (int channel = 0; channel < trackCount; ++channel) {
    writeBigEndianWord(result,
                       table + 2 + static_cast<std::size_t>(channel) * 2,
                       result.size() - table);
    if (channel == 0 && hasPcm) {
      result.push_back(0xe8);
    }
    if (channel == conductorChannel) {
      result.insert(result.end(), conductorTrack.begin(),
                    conductorTrack.end());
      continue;
    }
    if (sources[channel] < 0) {
      result.push_back(0xf1);
      result.push_back(0x00);
      continue;
    }
    const std::vector<std::uint8_t> track =
        neutralizedHardwareTrack(mdr, sources[channel], channel >= 8);
    result.insert(result.end(), track.begin(), track.end());
  }

  writeBigEndianWord(result, table, result.size() - table);
  result.insert(result.end(), mdr.data.begin() + mdr.toneOffset,
                mdr.data.end());
  return result;
}

}  // namespace mpxadrv
