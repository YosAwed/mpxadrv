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

std::uint16_t readWord(const std::vector<std::uint8_t>& bytes,
                       std::size_t position) {
  return static_cast<std::uint16_t>((bytes[position] << 8) |
                                    bytes[position + 1]);
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
    for (int track = 0; track < mpxadrv::MdrFile::kTrackCount; ++track) {
      writeWord(bytes, table + 2 + track * 2, offset);
      if (track == 0) {
        bytes.insert(bytes.end(), {0xe0, 0xff, 0xf1, 0x00});
        offset += 4;
      } else if (track == 1) {
        // A transient hardware selection followed by a MIDI-only track must
        // not reserve hardware channel 1.
        bytes.insert(bytes.end(),
                     {0xe0, 0x08, 0x01, 0x00, 0xe0, 0x08, 0x81,
                      0xe2, 0x1c, 0xf1, 0x00});
        offset += 11;
      } else if (track == 16) {
        bytes.insert(bytes.end(),
                     {0xe0, 0x08, 0x08, 0xfb, 0x0c,
                      0x80, 0x03, 0xf1, 0x00});
        offset += 9;
      } else {
        bytes.insert(bytes.end(), {0xf1, 0x00});
        offset += 2;
      }
    }
    writeWord(bytes, table, offset);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();

    const mpxadrv::MdrFile mdr = mpxadrv::loadMdr(path);
    require(mdr.title == "Test", "MDR title is wrong");
    require(mdr.pdxName == "BANK", "MDR PDX name is wrong");
    require(mdr.trackOffsets[0] == static_cast<int>(table + 66),
            "first MDR track offset is wrong");
    require(mdr.trackOffsets[31] ==
                static_cast<int>(table + 66 + 4 + 30 * 2 + 7 + 9),
            "last MDR track offset is wrong");
    require(mdr.activeTracks == 2, "active MDR tracks were not found");
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
    require(mpxadrv::countSeparableHardwareTracks(mdr) == 1,
            "separable hardware track was not counted");
    const std::vector<std::uint8_t> hardwareMdx =
        mpxadrv::makeMdxHardwareCompatible(
            mdr, {0xff, 0xc8, 0x0f, 0xf1, 0x00});
    const std::vector<std::uint8_t> neutralized = {
        0xfb, 0x95,        // PCM8's default v8, preserving the prior balance.
        0xf3, 0x00, 0x00,  // Neutralized E0 08 08 channel selection.
        0xfb, 0x80,        // PCM8 v12 saturates at libmdxmini's maximum.
        0x80, 0x03, 0xf1, 0x00,
    };
    require(std::search(hardwareMdx.begin(), hardwareMdx.end(),
                        neutralized.begin(), neutralized.end()) !=
                hardwareMdx.end(),
            "MDR hardware track was not converted to MDX");
    const std::size_t hardwareTable = mdr.dataOffset;
    const std::size_t channel1 =
        hardwareTable + readWord(hardwareMdx, hardwareTable + 4);
    require(hardwareMdx[channel1] == 0xf1 &&
                hardwareMdx[channel1 + 1] == 0x00,
            "transient hardware selection occupied a hardware channel");

    // MIDI-only MDR with empty FM/PCM channel stubs (as in _SBMA8_SC.MDR)
    // must not be treated as a hybrid candidate.
    std::vector<std::uint8_t> midiOnly = {
        'M', 'I', 'D', 'I', 0x0d, 0x0a, 0x1a, 0x00,
    };
    const std::size_t midiTable = midiOnly.size();
    midiOnly.resize(midiTable + 66, 0);
    std::uint16_t midiOffset = 66;
    for (int track = 0; track < mpxadrv::MdrFile::kTrackCount; ++track) {
      writeWord(midiOnly, midiTable + 2 + track * 2, midiOffset);
      if (track == 0) {
        midiOnly.insert(midiOnly.end(),
                        {0xe0, 0xff, 0xe0, 0x08, 0x80, 0x80, 0x03, 0xf1, 0x00});
        midiOffset += 9;
      } else if (track >= 16 && track <= 30) {
        const std::uint8_t channel =
            static_cast<std::uint8_t>(track - 16);  // hardware stub
        midiOnly.insert(midiOnly.end(),
                        {0xe0, 0x08, channel, 0x2f, 0xf1, 0x00});
        midiOffset += 6;
      } else {
        midiOnly.insert(midiOnly.end(), {0xf1, 0x00});
        midiOffset += 2;
      }
    }
    writeWord(midiOnly, midiTable, midiOffset);
    std::ofstream midiOnlyFile(path, std::ios::binary | std::ios::trunc);
    midiOnlyFile.write(reinterpret_cast<const char*>(midiOnly.data()),
                       static_cast<std::streamsize>(midiOnly.size()));
    midiOnlyFile.close();
    const mpxadrv::MdrFile midiOnlyMdr = mpxadrv::loadMdr(path);
    require(midiOnlyMdr.activeTracks > 1,
            "MIDI-only stub tracks should still count as active");
    require(mpxadrv::countSeparableHardwareTracks(midiOnlyMdr) == 0,
            "empty hardware stubs were counted as separable tracks");
    bool rejectedHardware = false;
    try {
      static_cast<void>(mpxadrv::makeMdxHardwareCompatible(midiOnlyMdr));
    } catch (const mpxadrv::MdrError&) {
      rejectedHardware = true;
    }
    require(rejectedHardware,
            "MIDI-only MDR with empty stubs produced a hardware MDX");

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
