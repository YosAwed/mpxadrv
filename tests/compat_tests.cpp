#include "compat.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
}  // namespace

int main() {
  try {
    const std::vector<std::uint8_t> extended = {
        0x80, 0xe0,        // Note; 0xe0 is its duration, not an opcode.
        0xfb, 0xe1,        // Volume; 0xe1 is its parameter.
        0xfe, 0x01, 0xe3,  // Register write with an extension-looking value.
        0xe2,              // Actual MADRV extension command.
    };
    const int firstTrack[] = {0};
    const auto found = mpxadrv::scanMadrvExtensions(
        extended.data(), extended.size(), firstTrack, 1);
    require(found.size() == 1, "extension count is wrong");
    require(found[0].track == 0, "extension track is wrong");
    require(found[0].opcode == 0xe2, "extension opcode is wrong");

    const std::vector<std::uint8_t> standard = {
        0xe8,              // PCM8 mode.
        0xff, 0xe0,        // Tempo with an extension-looking parameter.
        0xec, 0x80,        // LFO off.
        0xf1, 0x00, 0x00,  // End.
        0xe0,              // Bytes after end must not be scanned.
    };
    const auto absent = mpxadrv::scanMadrvExtensions(
        standard.data(), standard.size(), firstTrack, 1);
    require(absent.empty(), "false-positive extension was reported");

    const int tracks[] = {0, 3};
    const std::vector<std::uint8_t> twoTracks = {
        0xf1, 0x00, 0x00, 0xe1,
    };
    const auto second = mpxadrv::scanMadrvExtensions(
        twoTracks.data(), twoTracks.size(), tracks, 2);
    require(second.size() == 1 && second[0].track == 1 &&
                second[0].opcode == 0xe1,
            "multiple-track scan is wrong");

    std::cout << "Compatibility scanner tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Compatibility scanner test failed: " << error.what() << '\n';
    return 1;
  }
}
