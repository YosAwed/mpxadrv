#include "compat.hpp"

namespace mpxadrv {

std::vector<MadrvExtension> scanMadrvExtensions(
    const std::uint8_t* data, std::size_t length, const int* trackOffsets,
    int trackCount) {
  std::vector<MadrvExtension> extensions;
  if (data == nullptr || trackOffsets == nullptr || length == 0 ||
      trackCount <= 0) {
    return extensions;
  }

  for (int track = 0; track < trackCount; ++track) {
    if (trackOffsets[track] < 0) {
      continue;
    }
    std::size_t position = static_cast<std::size_t>(trackOffsets[track]);
    while (position < length) {
      const std::uint8_t command = data[position];
      if (command >= 0xe0 && command <= 0xe6) {
        extensions.push_back({track, command});
        break;
      }

      std::size_t commandLength = 1;
      bool dataEnd = false;
      if (command >= 0x80 && command <= 0xdf) {
        commandLength = 2;
      } else {
        switch (command) {
          case 0xe7:
            if (position + 1 >= length) {
              position = length;
              continue;
            }
            commandLength = data[position + 1] == 1 ? 3 : 2;
            break;
          case 0xe8:
          case 0xee:
          case 0xf7:
          case 0xf9:
          case 0xfa:
            commandLength = 1;
            break;
          case 0xea:
          case 0xeb:
          case 0xec:
            if (position + 1 >= length) {
              position = length;
              continue;
            }
            commandLength = (data[position + 1] & 0x80) != 0 ? 2 : 6;
            break;
          case 0xf1:
            commandLength = 3;
            dataEnd = true;
            break;
          case 0xf2:
          case 0xf3:
          case 0xf4:
          case 0xf5:
          case 0xf6:
          case 0xfe:
            commandLength = 3;
            break;
          default:
            if (command >= 0xe9) {
              commandLength = 2;
            }
            break;
        }
      }
      if (commandLength > length - position) {
        break;
      }
      position += commandLength;
      if (dataEnd) {
        break;
      }
    }
  }
  return extensions;
}

}  // namespace mpxadrv
