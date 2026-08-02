#include "tdx.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void writeBigEndian(std::vector<std::uint8_t>& bytes, std::size_t position,
                    std::uint32_t value) {
  bytes[position] = static_cast<std::uint8_t>(value >> 24);
  bytes[position + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[position + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[position + 3] = static_cast<std::uint8_t>(value);
}
std::uint32_t readBigEndian(const std::vector<std::uint8_t>& bytes,
                            std::size_t position) {
  return (static_cast<std::uint32_t>(bytes[position]) << 24) |
         (static_cast<std::uint32_t>(bytes[position + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[position + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[position + 3]);
}

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void writeFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  require(static_cast<bool>(output), "failed to write test binary");
}

void writeText(const fs::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  output << text;
  require(static_cast<bool>(output), "failed to write test text");
}

class TestDirectory {
 public:
  TestDirectory() {
    path_ = fs::temp_directory_path() / "mpxadrv-tdx-tests";
    std::error_code error;
    fs::remove_all(path_, error);
    fs::create_directories(path_);
  }

  ~TestDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

}  // namespace

int main() {
  try {
    TestDirectory directory;

    std::vector<std::uint8_t> source(768 + 6, 0);
    writeBigEndian(source, 0, 768);
    writeBigEndian(source, 4, 4);
    writeBigEndian(source, 8, 772);
    writeBigEndian(source, 12, 2);
    source[768] = 0x12;
    source[769] = 0x34;
    source[770] = 0x56;
    source[771] = 0x78;
    source[772] = 0x9a;
    source[773] = 0xbc;
    writeFile(directory.path() / "SOURCE.PDX", source);

    const fs::path definition = directory.path() / "valid.tdx";
    writeText(definition,
              "; comment before initialization\r\n"
              "# 2\r\n"
              "+ source.pdx\r\n"
              "@0\r\n"
              "N0 = N0\r\n"
              "@1\r\n"
              "C1 = N1\r\n"
              "&N1 = 0 N0\r\n");

    const mpxadrv::TdxResult result = mpxadrv::compileTdx(definition);
    require(result.banks == 2, "wrong bank count");
    require(result.assignments == 3, "wrong assignment count");
    require(result.sourceFiles.size() == 1, "wrong source file count");
    require(result.pdxData.size() == 1542, "wrong compiled PDX size");

    require(readBigEndian(result.pdxData, 0) == 1536,
            "bank 0 sample offset is wrong");
    require(readBigEndian(result.pdxData, 4) == 4,
            "bank 0 sample length is wrong");
    const std::size_t bank1C1 = 768 + 8 * 8;
    require(readBigEndian(result.pdxData, bank1C1) == 1540,
            "bank 1 C1 sample offset is wrong");
    require(readBigEndian(result.pdxData, bank1C1 + 4) == 2,
            "bank 1 C1 sample length is wrong");
    const std::size_t bank1N1 = 768 + 8;
    require(readBigEndian(result.pdxData, bank1N1) == 1536,
            "alias sample offset is wrong");
    require(readBigEndian(result.pdxData, bank1N1 + 4) == 4,
            "alias sample length is wrong");
    require(result.pdxData[1536] == 0x12 && result.pdxData[1541] == 0xbc,
            "sample payload is wrong");

    const fs::path output = directory.path() / "compiled.pdx";
    mpxadrv::writeTdxResult(result, output);
    require(fs::file_size(output) == result.pdxData.size(),
            "written PDX size is wrong");

    const fs::path duplicate = directory.path() / "duplicate.tdx";
    writeText(duplicate,
              "#1\n+SOURCE.PDX\nN0=N0\nN0=N1\n");
    bool rejectedDuplicate = false;
    try {
      (void)mpxadrv::compileTdx(duplicate);
    } catch (const mpxadrv::TdxError&) {
      rejectedDuplicate = true;
    }
    require(rejectedDuplicate, "duplicate assignment was accepted");

    const fs::path missing = directory.path() / "missing.tdx";
    writeText(missing, "#1\n+SOURCE.PDX\nN0=N95\n");
    bool rejectedMissing = false;
    try {
      (void)mpxadrv::compileTdx(missing);
    } catch (const mpxadrv::TdxError&) {
      rejectedMissing = true;
    }
    require(rejectedMissing, "missing source sample was accepted");

    std::cout << "TDX compiler tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "TDX compiler test failed: " << error.what() << '\n';
    return 1;
  }
}
