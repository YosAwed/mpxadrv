#include "mdr.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>

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
    case 0xe2:
    case 0xe3:
    case 0xe4:
    case 0xe5:
    case 0xe6:
      throw MdrError("unsupported MADRV command in MDR hardware track");
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
  while (position < end) {
    const std::uint8_t command = mdr.data[position];
    if (command == 0xe8) {
      ++position;
      continue;
    }
    if (command == 0xf1) {
      return std::nullopt;
    }
    if (command != 0xe0) {
      return midi ? std::nullopt : std::optional<int>(track & 0x0f);
    }
    const std::size_t length = e0CommandLength(mdr.data, position, end);
    const std::uint8_t subcommand = mdr.data[position + 1];
    if (subcommand == 0x08) {
      const std::uint8_t value = mdr.data[position + 2];
      midi = (value & 0x80) != 0;
      if (!midi) {
        return value & 0x0f;
      }
      return std::nullopt;
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

std::vector<std::uint8_t> neutralizedHardwareTrack(const MdrFile& mdr,
                                                   int track) {
  std::size_t position = static_cast<std::size_t>(mdr.trackOffsets[track]);
  const std::size_t end =
      track + 1 < MdrFile::kTrackCount
          ? static_cast<std::size_t>(mdr.trackOffsets[track + 1])
          : mdr.toneOffset;
  bool midi = track >= 16;
  std::vector<std::uint8_t> output;
  output.reserve(end - position);
  while (position < end) {
    const std::uint8_t command = mdr.data[position];
    const std::size_t length =
        standardCommandLength(mdr.data, position, end, midi);
    if (command == 0xe0) {
      if (mdr.data[position + 1] == 0x08) {
        midi = (mdr.data[position + 2] & 0x80) != 0;
      }
      appendNeutralCommands(output, length);
    } else if (command == 0xe8) {
      // The rebuilt MDX adds its own EX-PCM marker to the first track.
      output.push_back(0xf7);
    } else {
      output.insert(output.end(), mdr.data.begin() + position,
                    mdr.data.begin() + position + length);
    }
    position += length;
  }
  return output;
}

}  // namespace

MdrFile loadMdr(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw MdrError("MDR file not found: " + path.string());
  }

  MdrFile result;
  result.data.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
  if (result.data.empty()) {
    throw MdrError("MDR file is empty: " + path.string());
  }

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
  std::array<int, 16> sources{};
  sources.fill(-1);
  bool hasPcm = false;
  int hardwareTracks = 0;
  for (int track = 0; track < MdrFile::kTrackCount; ++track) {
    const std::optional<int> channel = initialHardwareChannel(mdr, track);
    if (!channel || *channel < 0 || *channel >= 16) {
      continue;
    }
    if (sources[*channel] < 0) {
      sources[*channel] = track;
      hasPcm = hasPcm || *channel >= 8;
      ++hardwareTracks;
    }
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
        neutralizedHardwareTrack(mdr, sources[channel]);
    result.insert(result.end(), track.begin(), track.end());
  }

  writeBigEndianWord(result, table, result.size() - table);
  result.insert(result.end(), mdr.data.begin() + mdr.toneOffset,
                mdr.data.end());
  return result;
}

}  // namespace mpxadrv
