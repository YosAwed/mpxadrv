#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace mpxadrv {

class TdxError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct TdxResult {
  std::vector<std::uint8_t> pdxData;
  int banks = 0;
  int assignments = 0;
  std::vector<std::filesystem::path> sourceFiles;
};

TdxResult compileTdx(const std::filesystem::path& input,
                     const std::filesystem::path& additionalPdxDirectory = {});

void writeTdxResult(const TdxResult& result,
                    const std::filesystem::path& output);

}  // namespace mpxadrv
