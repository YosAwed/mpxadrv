#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mpxadrv {

struct MadrvExtension {
  int track = 0;
  std::uint8_t opcode = 0;
};

std::vector<MadrvExtension> scanMadrvExtensions(
    const std::uint8_t* data, std::size_t length, const int* trackOffsets,
    int trackCount);

}  // namespace mpxadrv
