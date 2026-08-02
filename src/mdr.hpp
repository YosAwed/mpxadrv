#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace mpxadrv {

class MdrError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct MdrFile {
  static constexpr int kTrackCount = 32;

  std::vector<std::uint8_t> data;
  std::string title;
  std::string pdxName;
  std::array<int, kTrackCount> trackOffsets{};
  std::size_t dataOffset = 0;
  std::size_t toneOffset = 0;
  int activeTracks = 0;
};

MdrFile loadMdr(const std::filesystem::path& path);

// Counts FM/PCM channels that actually emit notes before the song loop.
// Empty hardware-channel stubs (common in MIDI-only MDR files) are ignored.
int countSeparableHardwareTracks(const MdrFile& mdr);

std::vector<std::uint8_t> makeMdxCompatible(const MdrFile& mdr,
                                            bool includePdx = true);

std::vector<std::uint8_t> makeMdxHardwareCompatible(
    const MdrFile& mdr,
    const std::vector<std::uint8_t>& conductorTrack = {});

}  // namespace mpxadrv
