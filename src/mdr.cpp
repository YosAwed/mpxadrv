#include "mdr.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

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

}  // namespace mpxadrv
