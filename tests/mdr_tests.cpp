#include "mdr.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void writeWord(std::vector<std::uint8_t>& bytes, std::size_t position,
               std::uint16_t value) {
  bytes[position] = static_cast<std::uint8_t>(value >> 8);
  bytes[position + 1] = static_cast<std::uint8_t>(value & 0xff);
}

}  // namespace

int main() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("mpxadrv-mdr-test-" + std::to_string(getpid()) + ".mdr");
  try {
    std::vector<std::uint8_t> bytes = {
        'T', 'e', 's', 't', 0x0d, 0x0a, 0x1a,
        'B', 'A', 'N', 'K', 0x00,
    };
    const std::size_t table = bytes.size();
    bytes.resize(table + 66, 0);
    std::uint16_t offset = 66;
    writeWord(bytes, table, static_cast<std::uint16_t>(66 + 4 + 31 * 2));
    for (int track = 0; track < mpxadrv::MdrFile::kTrackCount; ++track) {
      writeWord(bytes, table + 2 + track * 2, offset);
      if (track == 0) {
        bytes.insert(bytes.end(), {0xe0, 0xff, 0xf1, 0x00});
        offset += 4;
      } else {
        bytes.insert(bytes.end(), {0xf1, 0x00});
        offset += 2;
      }
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();

    const mpxadrv::MdrFile mdr = mpxadrv::loadMdr(path);
    require(mdr.title == "Test", "MDR title is wrong");
    require(mdr.pdxName == "BANK", "MDR PDX name is wrong");
    require(mdr.trackOffsets[0] == static_cast<int>(table + 66),
            "first MDR track offset is wrong");
    require(mdr.trackOffsets[31] == static_cast<int>(table + 66 + 4 + 30 * 2),
            "last MDR track offset is wrong");
    require(mdr.activeTracks == 0, "empty MDR tracks were reported active");
    const std::vector<std::uint8_t> mdx = mpxadrv::makeMdxCompatible(mdr);
    require(mdx[table + 2] == 0x00 && mdx[table + 3] == 0x14,
            "converted MDX first-track offset is wrong");
    require(mdx[table + 20] == 0xf1 && mdx[table + 21] == 0x00,
            "converted MDX did not remove the MDR signature");
    const std::vector<std::uint8_t> mdxWithoutPdx =
        mpxadrv::makeMdxCompatible(mdr, false);
    require(mdxWithoutPdx[7] == 0 && mdxWithoutPdx[8] == 0x00 &&
                mdxWithoutPdx[9] != 0,
            "converted MDX did not remove its PDX name");

    bytes[table + 66] = 0xf1;
    std::ofstream invalid(path, std::ios::binary | std::ios::trunc);
    invalid.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    invalid.close();
    bool rejected = false;
    try {
      static_cast<void>(mpxadrv::loadMdr(path));
    } catch (const mpxadrv::MdrError&) {
      rejected = true;
    }
    require(rejected, "MDR without E0 FF signature was accepted");

    bytes[table + 66] = 0xe8;
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(table + 67),
                 0xe0);
    bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(table + 68),
                 0xff);
    for (int track = 1; track < mpxadrv::MdrFile::kTrackCount; ++track) {
      const std::uint16_t previous = static_cast<std::uint16_t>(
          (bytes[table + 2 + track * 2] << 8) |
          bytes[table + 3 + track * 2]);
      writeWord(bytes, table + 2 + track * 2,
                static_cast<std::uint16_t>(previous + 2));
    }
    writeWord(bytes, table, static_cast<std::uint16_t>(
                                ((bytes[table] << 8) | bytes[table + 1]) + 2));
    std::ofstream expcm(path, std::ios::binary | std::ios::trunc);
    expcm.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    expcm.close();
    require(mpxadrv::loadMdr(path).trackOffsets[0] ==
                static_cast<int>(table + 66),
            "EX-PCM MDR signature was rejected");

    std::error_code error;
    std::filesystem::remove(path, error);
    std::cout << "MDR loader tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::cerr << "MDR loader test failed: " << error.what() << '\n';
    return 1;
  }
}
