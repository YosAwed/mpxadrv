#include "tdx.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

#include <iconv.h>

namespace fs = std::filesystem;

namespace mpxadrv {
namespace {

constexpr std::size_t kPdxBankHeaderSize = 0x300;
constexpr int kSamplesPerBank = 96;
constexpr int kMaximumBanks = 16;

struct Entry {
  std::uint32_t offset = 0;
  std::uint32_t length = 0;

  bool present() const { return offset != 0 && length != 0; }
};

struct SourcePdx {
  fs::path path;
  std::vector<std::uint8_t> bytes;
  int bank = 0;
};

std::string trim(std::string value) {
  const auto isSpace = [](unsigned char character) {
    return character == ' ' || character == '\t';
  };
  while (!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         (isSpace(static_cast<unsigned char>(value.back())) ||
          value.back() == '\r')) {
    value.pop_back();
  }
  return value;
}

std::string asciiLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string cp932ToUtf8(const std::string& text) {
  if (text.empty()) {
    return text;
  }
  iconv_t converter = iconv_open("UTF-8", "CP932");
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return text;
  }

  std::vector<char> output(text.size() * 4 + 1, '\0');
  char* input = const_cast<char*>(text.data());
  char* destination = output.data();
  std::size_t inputRemaining = text.size();
  std::size_t outputRemaining = output.size() - 1;
  const std::size_t status =
      iconv(converter, &input, &inputRemaining, &destination, &outputRemaining);
  iconv_close(converter);
  if (status == static_cast<std::size_t>(-1)) {
    return text;
  }
  return output.data();
}

std::vector<std::uint8_t> readBinaryFile(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw TdxError("cannot open file: " + path.string());
  }
  input.seekg(0, std::ios::end);
  const std::streamoff size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) >
                      std::numeric_limits<std::size_t>::max()) {
    throw TdxError("cannot determine file size: " + path.string());
  }
  input.seekg(0);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), size);
  }
  if (!input) {
    throw TdxError("failed to read file: " + path.string());
  }
  return bytes;
}

fs::path findCaseInsensitive(const fs::path& path) {
  std::error_code error;
  if (fs::is_regular_file(path, error)) {
    return path;
  }
  const fs::path directory = path.parent_path().empty() ? fs::path(".")
                                                        : path.parent_path();
  if (!fs::is_directory(directory, error)) {
    return {};
  }
  const std::string target = asciiLower(path.filename().string());
  for (const auto& item : fs::directory_iterator(directory, error)) {
    if (error) {
      break;
    }
    if (asciiLower(item.path().filename().string()) == target &&
        item.is_regular_file(error)) {
      return item.path();
    }
  }
  return {};
}

fs::path resolvePdxPath(std::string filename, const fs::path& tdxDirectory,
                        const fs::path& additionalDirectory) {
  filename = trim(cp932ToUtf8(trim(filename)));
  if (filename.size() >= 2 && filename.front() == '"' &&
      filename.back() == '"') {
    filename = filename.substr(1, filename.size() - 2);
  }
  fs::path name(filename);
  if (name.extension().empty()) {
    name += ".PDX";
  }

  std::vector<fs::path> candidates;
  if (name.is_absolute()) {
    candidates.push_back(name);
  } else {
    candidates.push_back(tdxDirectory / name);
    if (!additionalDirectory.empty()) {
      candidates.push_back(additionalDirectory / name);
    }
  }
  for (const fs::path& candidate : candidates) {
    if (fs::path found = findCaseInsensitive(candidate); !found.empty()) {
      return fs::absolute(found);
    }
  }
  throw TdxError("source PDX file not found: " + filename);
}

std::uint32_t readBigEndian(const std::vector<std::uint8_t>& bytes,
                            std::size_t position) {
  if (position + 4 > bytes.size()) {
    throw TdxError("truncated PDX header");
  }
  return (static_cast<std::uint32_t>(bytes[position]) << 24) |
         (static_cast<std::uint32_t>(bytes[position + 1]) << 16) |
         (static_cast<std::uint32_t>(bytes[position + 2]) << 8) |
         static_cast<std::uint32_t>(bytes[position + 3]);
}

void writeBigEndian(std::vector<std::uint8_t>& bytes, std::size_t position,
                    std::uint32_t value) {
  bytes[position] = static_cast<std::uint8_t>(value >> 24);
  bytes[position + 1] = static_cast<std::uint8_t>(value >> 16);
  bytes[position + 2] = static_cast<std::uint8_t>(value >> 8);
  bytes[position + 3] = static_cast<std::uint8_t>(value);
}

void skipSpaces(const std::string& line, std::size_t& position) {
  while (position < line.size() &&
         (line[position] == ' ' || line[position] == '\t')) {
    ++position;
  }
}

int parseUnsigned(const std::string& line, std::size_t& position,
                  std::size_t lineNumber, const char* description) {
  skipSpaces(line, position);
  const std::size_t start = position;
  unsigned long value = 0;
  while (position < line.size() && line[position] >= '0' &&
         line[position] <= '9') {
    value = value * 10 + static_cast<unsigned long>(line[position] - '0');
    if (value > static_cast<unsigned long>(std::numeric_limits<int>::max())) {
      throw TdxError("line " + std::to_string(lineNumber) + ": " + description +
                     " is too large");
    }
    ++position;
  }
  if (position == start) {
    throw TdxError("line " + std::to_string(lineNumber) + ": expected " +
                   description);
  }
  return static_cast<int>(value);
}

int parseKey(const std::string& line, std::size_t& position,
             std::size_t lineNumber) {
  skipSpaces(line, position);
  if (position >= line.size()) {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": expected a sample key");
  }

  const char command = static_cast<char>(
      std::toupper(static_cast<unsigned char>(line[position++])));
  int key = -1;
  if (command == 'N') {
    key = parseUnsigned(line, position, lineNumber, "numeric key");
  } else if (command >= 'A' && command <= 'G') {
    constexpr std::array<int, 7> bases = {9, 11, 0, 2, 4, 5, 7};
    int base = bases[static_cast<std::size_t>(command - 'A')];
    skipSpaces(line, position);
    if (position < line.size() && line[position] == '+') {
      ++base;
      ++position;
    } else if (position < line.size() && line[position] == '-') {
      --base;
      ++position;
    }
    const int octave = parseUnsigned(line, position, lineNumber, "octave");
    key = octave * 12 + base - 4;
  } else {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": invalid sample key");
  }

  if (key < 0 || key >= kSamplesPerBank) {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": sample key is outside N0..N95");
  }
  return key;
}

void expectEquals(const std::string& line, std::size_t& position,
                  std::size_t lineNumber) {
  skipSpaces(line, position);
  if (position >= line.size() || line[position] != '=') {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": expected '='");
  }
  ++position;
}

Entry sourceEntry(const SourcePdx& source, int key, std::size_t lineNumber) {
  const std::size_t position =
      static_cast<std::size_t>(source.bank) * kPdxBankHeaderSize +
      static_cast<std::size_t>(key) * 8;
  if (position + 8 > source.bytes.size()) {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": source PDX bank header is truncated");
  }
  Entry entry{readBigEndian(source.bytes, position),
              readBigEndian(source.bytes, position + 4)};
  if (!entry.present()) {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": source sample does not exist");
  }
  const std::uint64_t end =
      static_cast<std::uint64_t>(entry.offset) + entry.length;
  if (end > source.bytes.size()) {
    throw TdxError("line " + std::to_string(lineNumber) +
                   ": source sample extends past the PDX file");
  }
  return entry;
}

std::vector<std::string> readLines(const fs::path& input) {
  std::ifstream stream(input, std::ios::binary);
  if (!stream) {
    throw TdxError("TDX file not found: " + input.string());
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
  }
  if (!stream.eof()) {
    throw TdxError("failed to read TDX file: " + input.string());
  }
  return lines;
}

}  // namespace

TdxResult compileTdx(const fs::path& input,
                     const fs::path& additionalPdxDirectory) {
  const fs::path absoluteInput = fs::absolute(input);
  const std::vector<std::string> lines = readLines(absoluteInput);

  int bankCount = 0;
  std::size_t firstCommand = 0;
  for (; firstCommand < lines.size(); ++firstCommand) {
    const std::string line = trim(lines[firstCommand]);
    if (!line.empty() && line.front() == '#') {
      std::size_t position = 1;
      bankCount = parseUnsigned(line, position, firstCommand + 1, "bank count");
      ++firstCommand;
      break;
    }
  }
  if (bankCount < 1 || bankCount > kMaximumBanks) {
    throw TdxError("TDX bank count must be between 1 and 16");
  }

  const std::size_t headerSize =
      static_cast<std::size_t>(bankCount) * kPdxBankHeaderSize;
  std::vector<Entry> entries(static_cast<std::size_t>(bankCount) *
                             kSamplesPerBank);
  std::vector<std::uint8_t> payload;
  std::optional<SourcePdx> source;
  int destinationBank = 0;
  int assignments = 0;
  std::vector<fs::path> sourceFiles;
  std::unordered_set<std::string> knownSources;

  const fs::path tdxDirectory = absoluteInput.parent_path();
  for (std::size_t index = firstCommand; index < lines.size(); ++index) {
    const std::string line = trim(lines[index]);
    const std::size_t lineNumber = index + 1;
    if (line.empty() || line.front() == ';') {
      continue;
    }
    std::size_t position = 0;
    const char command = line[position];

    if (command == '+') {
      const fs::path sourcePath =
          resolvePdxPath(line.substr(1), tdxDirectory, additionalPdxDirectory);
      source = SourcePdx{sourcePath, readBinaryFile(sourcePath), 0};
      if (source->bytes.size() < kPdxBankHeaderSize) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": source PDX is smaller than one header bank");
      }
      if (knownSources.insert(sourcePath.string()).second) {
        sourceFiles.push_back(sourcePath);
      }
      continue;
    }
    if (command == '*') {
      if (!source) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": no source PDX has been selected");
      }
      position = 1;
      const int bank =
          parseUnsigned(line, position, lineNumber, "source bank number");
      if (bank < 0 ||
          (static_cast<std::size_t>(bank) + 1) * kPdxBankHeaderSize >
              source->bytes.size()) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": source bank is outside the PDX file");
      }
      source->bank = bank;
      continue;
    }
    if (command == '@') {
      position = 1;
      destinationBank =
          parseUnsigned(line, position, lineNumber, "destination bank number");
      if (destinationBank < 0 || destinationBank >= bankCount) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": destination bank is outside the TDX bank count");
      }
      continue;
    }

    bool alias = false;
    if (command == '&') {
      alias = true;
      position = 1;
    }
    const int destinationKey = parseKey(line, position, lineNumber);
    expectEquals(line, position, lineNumber);
    const std::size_t destinationIndex =
        static_cast<std::size_t>(destinationBank) * kSamplesPerBank +
        destinationKey;
    if (entries[destinationIndex].present()) {
      throw TdxError("line " + std::to_string(lineNumber) +
                     ": destination sample is already assigned");
    }

    if (alias) {
      const int sourceBank =
          parseUnsigned(line, position, lineNumber, "assigned bank number");
      const int sourceKey = parseKey(line, position, lineNumber);
      if (sourceBank < 0 || sourceBank >= bankCount) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": assigned bank is outside the TDX bank count");
      }
      const Entry entry =
          entries[static_cast<std::size_t>(sourceBank) * kSamplesPerBank +
                  sourceKey];
      if (!entry.present()) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": assigned sample does not exist yet");
      }
      entries[destinationIndex] = entry;
    } else {
      if (!source) {
        throw TdxError("line " + std::to_string(lineNumber) +
                       ": no source PDX has been selected");
      }
      const int sourceKey = parseKey(line, position, lineNumber);
      const Entry original = sourceEntry(*source, sourceKey, lineNumber);
      const std::uint64_t destinationOffset = headerSize + payload.size();
      if (destinationOffset > std::numeric_limits<std::uint32_t>::max() ||
          destinationOffset + original.length >
              std::numeric_limits<std::uint32_t>::max()) {
        throw TdxError("compiled PDX exceeds the 4 GiB format limit");
      }
      entries[destinationIndex] = {
          static_cast<std::uint32_t>(destinationOffset), original.length};
      payload.insert(payload.end(), source->bytes.begin() + original.offset,
                     source->bytes.begin() + original.offset + original.length);
    }
    ++assignments;
  }

  std::vector<std::uint8_t> output(headerSize + payload.size(), 0);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (!entries[index].present()) {
      continue;
    }
    writeBigEndian(output, index * 8, entries[index].offset);
    writeBigEndian(output, index * 8 + 4, entries[index].length);
  }
  std::copy(payload.begin(), payload.end(), output.begin() + headerSize);

  return {std::move(output), bankCount, assignments, std::move(sourceFiles)};
}

void writeTdxResult(const TdxResult& result, const fs::path& output) {
  std::error_code error;
  if (!output.parent_path().empty()) {
    fs::create_directories(output.parent_path(), error);
    if (error) {
      throw TdxError("cannot create output directory: " +
                     output.parent_path().string());
    }
  }
  std::ofstream stream(output, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw TdxError("cannot create compiled PDX: " + output.string());
  }
  if (!result.pdxData.empty()) {
    stream.write(reinterpret_cast<const char*>(result.pdxData.data()),
                 static_cast<std::streamsize>(result.pdxData.size()));
  }
  if (!stream) {
    throw TdxError("failed to write compiled PDX: " + output.string());
  }
}

}  // namespace mpxadrv
