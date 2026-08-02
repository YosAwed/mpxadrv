#include <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <iconv.h>
extern "C" {
#include <libmdxmini/mdxmini.h>
}

#include "tdx.hpp"
#include "compat.hpp"
#include "core_midi.hpp"
#include "midi.hpp"
#include "software_synth.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.4.0";
constexpr int kDefaultRate = 48'000;
constexpr int kDefaultLoops = 1;
constexpr std::uint32_t kFramesPerBuffer = 2'048;
constexpr int kQueueBufferCount = 3;

volatile std::sig_atomic_t gInterrupted = 0;

void handleSignal(int) {
  gInterrupted = 1;
}

class CliError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::string formatDuration(int seconds) {
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;
  std::ostringstream result;
  if (hours > 0) {
    result << hours << ':' << std::setfill('0') << std::setw(2);
  }
  result << minutes << ':' << std::setfill('0') << std::setw(2) << secs;
  return result.str();
}

std::string asciiLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    if (c >= 'A' && c <= 'Z') {
      return static_cast<char>(c + ('a' - 'A'));
    }
    return static_cast<char>(c);
  });
  return value;
}

std::string shiftJisToUtf8(const char* text) {
  if (text == nullptr || *text == '\0') {
    return {};
  }

  iconv_t converter = iconv_open("UTF-8", "CP932");
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return text;
  }

  const std::size_t inputLength = std::strlen(text);
  std::vector<char> output(inputLength * 4 + 1, '\0');
  char* input = const_cast<char*>(text);
  char* destination = output.data();
  std::size_t inputRemaining = inputLength;
  std::size_t outputRemaining = output.size() - 1;

  const std::size_t result =
      iconv(converter, &input, &inputRemaining, &destination, &outputRemaining);
  iconv_close(converter);
  if (result == static_cast<std::size_t>(-1)) {
    return text;
  }
  return output.data();
}

fs::path findCaseInsensitive(const fs::path& directory,
                             const std::string& filename) {
  std::error_code error;
  if (directory.empty() || !fs::is_directory(directory, error)) {
    return {};
  }
  const std::string target = asciiLower(filename);
  for (const auto& item : fs::directory_iterator(directory, error)) {
    if (error) {
      return {};
    }
    if (asciiLower(item.path().filename().string()) == target) {
      return item.path();
    }
  }
  return {};
}

struct Options {
  std::string command = "play";
  fs::path input;
  fs::path output;
  fs::path pdxDirectory;
  fs::path tdxFile;
  fs::path soundFont;
  std::string midiDestination;
  int rate = kDefaultRate;
  int loops = kDefaultLoops;
};

int parseInteger(const std::string& value, const char* option) {
  std::size_t consumed = 0;
  long parsed = 0;
  try {
    parsed = std::stol(value, &consumed, 10);
  } catch (const std::exception&) {
    throw CliError(std::string(option) + " requires an integer: " + value);
  }
  if (consumed != value.size() || parsed > std::numeric_limits<int>::max() ||
      parsed < std::numeric_limits<int>::min()) {
    throw CliError(std::string(option) + " requires an integer: " + value);
  }
  return static_cast<int>(parsed);
}

void printUsage(std::ostream& stream) {
  stream
      << "mpxadrv " << kVersion << " - MADRV/MDX player for macOS\n\n"
      << "Usage:\n"
      << "  mpxadrv <file.mdx> [options]\n"
      << "  mpxadrv play <file.mdx> [options]\n"
      << "  mpxadrv info <file.mdx> [options]\n"
      << "  mpxadrv render <file.mdx> -o <file.wav> [options]\n\n"
      << "  mpxadrv midi <file.mdx> [-o <file.mid>] [options]\n"
      << "  mpxadrv midi-synth <file.mdx> [--soundfont <file.sf2|file.dls>] [options]\n"
      << "  mpxadrv midi-list\n"
      << "  mpxadrv midi-play <file.mdx> --destination <index-or-name> [options]\n"
      << "  mpxadrv tdx <file.tdx> [-o <file.pdx>] [options]\n\n"
      << "Options:\n"
      << "  -o, --output <path>    WAV, MIDI, or compiled PDX output path\n"
      << "  -p, --pdx-dir <path>   Additional PDX search directory\n"
      << "      --tdx-file <path>  Override the song's PDX with a TDX definition\n"
      << "      --destination <id> CoreMIDI destination index or name\n"
      << "      --soundfont <path>  SF2 or DLS bank for midi-synth\n"
      << "  -r, --rate <hz>        Sample rate, 8000-192000 (default: 48000)\n"
      << "  -l, --loops <count>    Number of song loops, 1-100 (default: 1)\n"
      << "  -h, --help             Show this help\n"
      << "      --version          Show version\n";
}

Options parseArguments(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(std::cerr);
    throw CliError("an MDX file is required");
  }

  Options options;
  int index = 1;
  const std::string first = argv[index];
  if (first == "--help" || first == "-h") {
    printUsage(std::cout);
    std::exit(0);
  }
  if (first == "--version") {
    std::cout << "mpxadrv " << kVersion << '\n';
    std::exit(0);
  }
  if (first == "play" || first == "info" || first == "render" ||
      first == "midi" || first == "midi-synth" || first == "midi-list" ||
      first == "midi-play" || first == "tdx") {
    options.command = first;
    ++index;
  }

  auto requireValue = [&](const char* option) -> std::string {
    if (++index >= argc) {
      throw CliError(std::string(option) + " requires a value");
    }
    return argv[index];
  };

  for (; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      printUsage(std::cout);
      std::exit(0);
    }
    if (argument == "--version") {
      std::cout << "mpxadrv " << kVersion << '\n';
      std::exit(0);
    }
    if (argument == "-o" || argument == "--output") {
      options.output = requireValue(argument.c_str());
    } else if (argument == "-p" || argument == "--pdx-dir") {
      options.pdxDirectory = requireValue(argument.c_str());
    } else if (argument == "--tdx-file") {
      options.tdxFile = requireValue(argument.c_str());
    } else if (argument == "--destination") {
      options.midiDestination = requireValue(argument.c_str());
    } else if (argument == "--soundfont") {
      options.soundFont = requireValue(argument.c_str());
    } else if (argument == "-r" || argument == "--rate") {
      options.rate = parseInteger(requireValue(argument.c_str()), argument.c_str());
    } else if (argument == "-l" || argument == "--loops") {
      options.loops = parseInteger(requireValue(argument.c_str()), argument.c_str());
    } else if (!argument.empty() && argument[0] == '-') {
      throw CliError("unknown option: " + argument);
    } else if (options.input.empty()) {
      options.input = argument;
    } else {
      throw CliError("only one MDX file can be processed at a time");
    }
  }

  if (options.input.empty() && options.command != "midi-list") {
    throw CliError("an MDX file is required");
  }
  if (options.command == "midi-list" && !options.input.empty()) {
    throw CliError("midi-list does not accept an input file");
  }
  if (options.rate < 8'000 || options.rate > 192'000) {
    throw CliError("sample rate must be between 8000 and 192000 Hz");
  }
  if (options.loops < 1 || options.loops > 100) {
    throw CliError("loops must be between 1 and 100");
  }
  if (options.command == "render" && options.output.empty()) {
    options.output = options.input;
    options.output.replace_extension(".wav");
  }
  if (options.command == "tdx" && options.output.empty()) {
    options.output = options.input;
    options.output.replace_extension(".pdx");
  }
  if (options.command == "midi" && options.output.empty()) {
    options.output = options.input;
    options.output.replace_extension(".mid");
  }
  if (options.command != "render" && options.command != "midi" &&
      options.command != "tdx" &&
      !options.output.empty()) {
    throw CliError("--output can only be used with render, midi, or tdx");
  }
  if (!options.midiDestination.empty() && options.command != "midi-play") {
    throw CliError("--destination can only be used with midi-play");
  }
  if (options.command == "midi-play" && options.midiDestination.empty()) {
    throw CliError("midi-play requires --destination <index-or-name>");
  }
  if (!options.soundFont.empty() && options.command != "midi-synth") {
    throw CliError("--soundfont can only be used with midi-synth");
  }
  return options;
}

std::string readMdxPdxName(const fs::path& input) {
  std::ifstream stream(input, std::ios::binary);
  if (!stream) {
    return {};
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
  std::size_t position = 0;
  while (position + 2 < bytes.size()) {
    if (static_cast<unsigned char>(bytes[position]) == 0x0d &&
        static_cast<unsigned char>(bytes[position + 1]) == 0x0a &&
        static_cast<unsigned char>(bytes[position + 2]) == 0x1a) {
      position += 3;
      break;
    }
    ++position;
  }
  if (position >= bytes.size()) {
    return {};
  }
  const std::size_t start = position;
  while (position < bytes.size() && bytes[position] != '\0') {
    ++position;
  }
  if (position >= bytes.size()) {
    return {};
  }
  const std::string encoded(bytes.data() + start, position - start);
  return shiftJisToUtf8(encoded.c_str());
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (fs::temp_directory_path() / "mpxadrv.XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = mkdtemp(writable.data());
    if (created == nullptr) {
      throw CliError("cannot create a temporary directory");
    }
    path_ = created;
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  ~TemporaryDirectory() {
    std::error_code error;
    if (!path_.empty()) {
      fs::remove_all(path_, error);
    }
  }

  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

struct TdxStats {
  int banks = 0;
  int assignments = 0;
  fs::path source;
};

class Song {
 public:
  explicit Song(const Options& options) : rate_(options.rate) {
    std::error_code error;
    const fs::path absoluteInput = fs::absolute(options.input, error);
    if (error || !fs::is_regular_file(absoluteInput, error)) {
      throw CliError("MDX file not found: " + options.input.string());
    }
    input_ = absoluteInput;

    fs::path playbackInput = input_;
    fs::path playbackPdxDirectory = options.pdxDirectory;
    referencedPdxName_ = readMdxPdxName(input_);
    const bool referencesTdx =
        asciiLower(fs::path(referencedPdxName_).extension().string()) == ".tdx";
    if (referencesTdx || !options.tdxFile.empty()) {
      fs::path tdxPath;
      if (!options.tdxFile.empty()) {
        tdxPath = fs::absolute(options.tdxFile);
        if (!fs::is_regular_file(tdxPath, error)) {
          throw CliError("TDX file not found: " + options.tdxFile.string());
        }
      } else {
        tdxPath = findCaseInsensitive(input_.parent_path(), referencedPdxName_);
        if (tdxPath.empty() && !options.pdxDirectory.empty()) {
          tdxPath = findCaseInsensitive(fs::absolute(options.pdxDirectory),
                                        referencedPdxName_);
        }
      }
      if (tdxPath.empty()) {
        throw CliError("TDX file not found: " + referencedPdxName_);
      }
      if (referencedPdxName_.empty()) {
        throw CliError("--tdx-file requires an MDX song with a PCM filename");
      }

      mpxadrv::TdxResult compiled =
          mpxadrv::compileTdx(tdxPath, options.pdxDirectory);
      tdxStats_ = TdxStats{compiled.banks, compiled.assignments, tdxPath};
      temporaryDirectory_ = std::make_unique<TemporaryDirectory>();
      playbackInput = temporaryDirectory_->path() / input_.filename();
      fs::copy_file(input_, playbackInput, fs::copy_options::overwrite_existing,
                    error);
      if (error) {
        throw CliError("failed to prepare MDX for TDX playback: " +
                       error.message());
      }
      const fs::path compiledPdx =
          temporaryDirectory_->path() / fs::path(referencedPdxName_);
      mpxadrv::writeTdxResult(compiled, compiledPdx);
      playbackPdxDirectory = temporaryDirectory_->path();
    }

    mdx_set_rate(rate_);
    std::string inputString = playbackInput.string();
    std::string pdxString;
    char* pdxDirectory = nullptr;
    if (!playbackPdxDirectory.empty()) {
      pdxString = fs::absolute(playbackPdxDirectory).string();
      pdxDirectory = pdxString.data();
    }
    if (mdx_open(&context_, inputString.data(), pdxDirectory) != 0) {
      throw CliError("failed to parse MDX file: " + input_.string());
    }
    opened_ = true;
    extensions_ = mpxadrv::scanMadrvExtensions(
        context_.mdx->data, static_cast<std::size_t>(context_.mdx->length),
        context_.mdx->mml_data_offset, context_.mdx->tracks);
    mdx_set_max_loop(&context_, options.loops);
    duration_ = mdx_get_length(&context_);
    if (duration_ <= 0) {
      mdx_close(&context_);
      opened_ = false;
      throw CliError("the MDX song has no playable duration");
    }
    channels_ = context_.channels;
    if (channels_ < 1 || channels_ > 2) {
      mdx_close(&context_);
      opened_ = false;
      throw CliError("unsupported audio channel count: " +
                     std::to_string(channels_));
    }
  }

  Song(const Song&) = delete;
  Song& operator=(const Song&) = delete;

  ~Song() {
    if (opened_) {
      mdx_close(&context_);
    }
  }

  int render(std::int16_t* samples, int frames) {
    return mdx_calc_sample(&context_, samples, frames);
  }

  int rate() const { return rate_; }
  int channels() const { return channels_; }
  int duration() const { return duration_; }
  int tracks() const { return mdx_get_tracks(const_cast<t_mdxmini*>(&context_)); }
  std::uint64_t totalFrames() const {
    return static_cast<std::uint64_t>(duration_) * static_cast<std::uint64_t>(rate_);
  }

  std::string title() const {
    char title[MDX_MAX_TITLE_LENGTH] = {};
    mdx_get_title(const_cast<t_mdxmini*>(&context_), title);
    return shiftJisToUtf8(title);
  }

  std::string pdxName() const {
    if (context_.mdx == nullptr) {
      return {};
    }
    return context_.mdx->pdx_name;
  }

  const std::optional<TdxStats>& tdxStats() const { return tdxStats_; }
  const std::vector<mpxadrv::MadrvExtension>& extensions() const {
    return extensions_;
  }

  const std::uint8_t* mdxData() const {
    return context_.mdx == nullptr ? nullptr : context_.mdx->data;
  }

  std::size_t mdxLength() const {
    return context_.mdx == nullptr
               ? 0
               : static_cast<std::size_t>(context_.mdx->length);
  }

  const int* trackOffsets() const {
    return context_.mdx == nullptr ? nullptr : context_.mdx->mml_data_offset;
  }

  int mdxTrackCount() const {
    return context_.mdx == nullptr ? 0 : context_.mdx->tracks;
  }

  fs::path locatePdx(const fs::path& extraDirectory) const {
    const std::string name = pdxName();
    if (name.empty()) {
      return {};
    }
    if (fs::path found = findCaseInsensitive(input_.parent_path(), name);
        !found.empty()) {
      return found;
    }
    if (!extraDirectory.empty()) {
      return findCaseInsensitive(fs::absolute(extraDirectory), name);
    }
    return {};
  }

 private:
  t_mdxmini context_{};
  bool opened_ = false;
  int rate_ = 0;
  int channels_ = 0;
  int duration_ = 0;
  fs::path input_;
  std::string referencedPdxName_;
  std::unique_ptr<TemporaryDirectory> temporaryDirectory_;
  std::optional<TdxStats> tdxStats_;
  std::vector<mpxadrv::MadrvExtension> extensions_;
};

void writeLittleEndian(std::ostream& output, std::uint32_t value,
                       int byteCount) {
  for (int i = 0; i < byteCount; ++i) {
    output.put(static_cast<char>((value >> (i * 8)) & 0xff));
  }
}

class WavWriter {
 public:
  WavWriter(const fs::path& path, int rate, int channels)
      : path_(path), rate_(rate), channels_(channels) {
    output_.open(path_, std::ios::binary | std::ios::trunc);
    if (!output_) {
      throw CliError("cannot create WAV file: " + path_.string());
    }
    writeHeader(0);
  }

  ~WavWriter() {
    if (output_.is_open()) {
      finalize();
    }
  }

  void write(const std::int16_t* samples, std::size_t frames) {
    const std::size_t bytes = frames * static_cast<std::size_t>(channels_) *
                              sizeof(std::int16_t);
    if (dataBytes_ + bytes > std::numeric_limits<std::uint32_t>::max() - 44) {
      throw CliError("WAV output exceeds the 4 GiB RIFF limit");
    }
    output_.write(reinterpret_cast<const char*>(samples),
                  static_cast<std::streamsize>(bytes));
    if (!output_) {
      throw CliError("failed while writing WAV file: " + path_.string());
    }
    dataBytes_ += static_cast<std::uint32_t>(bytes);
  }

  void finalize() {
    if (!output_.is_open()) {
      return;
    }
    output_.seekp(0);
    writeHeader(dataBytes_);
    output_.close();
  }

 private:
  void writeHeader(std::uint32_t dataBytes) {
    output_.write("RIFF", 4);
    writeLittleEndian(output_, 36 + dataBytes, 4);
    output_.write("WAVEfmt ", 8);
    writeLittleEndian(output_, 16, 4);
    writeLittleEndian(output_, 1, 2);
    writeLittleEndian(output_, static_cast<std::uint32_t>(channels_), 2);
    writeLittleEndian(output_, static_cast<std::uint32_t>(rate_), 4);
    const std::uint32_t bytesPerSecond =
        static_cast<std::uint32_t>(rate_ * channels_ * sizeof(std::int16_t));
    writeLittleEndian(output_, bytesPerSecond, 4);
    writeLittleEndian(output_,
                      static_cast<std::uint32_t>(channels_ * sizeof(std::int16_t)),
                      2);
    writeLittleEndian(output_, 16, 2);
    output_.write("data", 4);
    writeLittleEndian(output_, dataBytes, 4);
  }

  fs::path path_;
  int rate_;
  int channels_;
  std::ofstream output_;
  std::uint32_t dataBytes_ = 0;
};

std::string audioError(OSStatus status) {
  char code[5] = {};
  const std::uint32_t swapped = CFSwapInt32HostToBig(static_cast<std::uint32_t>(status));
  std::memcpy(code, &swapped, 4);
  bool printable = true;
  for (int i = 0; i < 4; ++i) {
    printable = printable && code[i] >= 32 && code[i] <= 126;
  }
  if (printable) {
    return "'" + std::string(code, 4) + "'";
  }
  return std::to_string(status);
}

void requireAudio(OSStatus status, const char* operation) {
  if (status != noErr) {
    throw CliError(std::string(operation) + " failed (Core Audio " +
                   audioError(status) + ")");
  }
}

class AudioPlayer {
 public:
  explicit AudioPlayer(Song& song)
      : song_(song), remainingFrames_(song.totalFrames()) {
    AudioStreamBasicDescription format{};
    format.mSampleRate = song.rate();
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                          kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = song.channels() * sizeof(std::int16_t);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = song.channels() * sizeof(std::int16_t);
    format.mChannelsPerFrame = song.channels();
    format.mBitsPerChannel = 16;

    requireAudio(AudioQueueNewOutput(&format, outputCallback, this, nullptr, nullptr,
                                     0, &queue_),
                 "AudioQueueNewOutput");
  }

  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  ~AudioPlayer() {
    if (queue_ != nullptr) {
      AudioQueueStop(queue_, true);
      AudioQueueDispose(queue_, true);
    }
  }

  void play() {
    const std::uint32_t bytes =
        kFramesPerBuffer * song_.channels() * sizeof(std::int16_t);
    for (int i = 0; i < kQueueBufferCount && !reachedEnd_; ++i) {
      AudioQueueBufferRef buffer = nullptr;
      requireAudio(AudioQueueAllocateBuffer(queue_, bytes, &buffer),
                   "AudioQueueAllocateBuffer");
      fillAndEnqueue(buffer);
    }
    requireAudio(AudioQueueStart(queue_, nullptr), "AudioQueueStart");

    while (!finished_.load() && !gInterrupted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (gInterrupted) {
      AudioQueueStop(queue_, true);
    }
  }

 private:
  static void outputCallback(void* userData, AudioQueueRef,
                             AudioQueueBufferRef buffer) {
    auto* player = static_cast<AudioPlayer*>(userData);
    --player->buffersInFlight_;
    if (!player->reachedEnd_ && !gInterrupted) {
      player->fillAndEnqueue(buffer);
    } else if (player->buffersInFlight_ == 0) {
      player->finished_.store(true);
    }
  }

  void fillAndEnqueue(AudioQueueBufferRef buffer) noexcept {
    const std::uint32_t frames = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(kFramesPerBuffer, remainingFrames_));
    if (frames == 0) {
      reachedEnd_ = true;
      return;
    }
    const int hasMore = song_.render(
        static_cast<std::int16_t*>(buffer->mAudioData), static_cast<int>(frames));
    buffer->mAudioDataByteSize =
        frames * song_.channels() * sizeof(std::int16_t);
    remainingFrames_ -= frames;
    reachedEnd_ = !hasMore || remainingFrames_ == 0;
    if (AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr) == noErr) {
      ++buffersInFlight_;
    } else {
      reachedEnd_ = true;
      finished_.store(true);
    }
  }

  Song& song_;
  AudioQueueRef queue_ = nullptr;
  std::uint64_t remainingFrames_ = 0;
  std::atomic<bool> finished_{false};
  int buffersInFlight_ = 0;
  bool reachedEnd_ = false;
};

void printSongInfo(const Song& song, const Options& options) {
  const std::string title = song.title();
  std::cout << "Title: " << (title.empty() ? "(untitled)" : title) << '\n'
            << "Duration: " << formatDuration(song.duration()) << " ("
            << song.duration() << " seconds)\n"
            << "Tracks: " << song.tracks() << " (8 FM";
  if (song.tracks() > 8) {
    std::cout << ", " << song.tracks() - 8 << " PCM";
  }
  std::cout << ")\n";

  const std::string pdxName = song.pdxName();
  if (pdxName.empty()) {
    std::cout << "PDX: none\n";
  } else if (song.tdxStats()) {
    std::cout << "PDX: " << pdxName << " (compiled from TDX)\n";
  } else {
    const fs::path pdx = song.locatePdx(options.pdxDirectory);
    std::cout << "PDX: " << pdxName;
    if (pdx.empty()) {
      std::cout << " (not found)\n";
    } else {
      std::cout << " (" << pdx.string() << ")\n";
    }
  }
  if (song.tdxStats()) {
    const TdxStats& stats = *song.tdxStats();
    std::cout << "TDX: " << stats.banks << " bank"
              << (stats.banks == 1 ? "" : "s") << ", " << stats.assignments
              << " assignment" << (stats.assignments == 1 ? "" : "s")
              << " (" << stats.source.string() << ")\n";
  }
  if (song.extensions().empty()) {
    std::cout << "Compatibility: standard MDX command set\n";
  } else {
    std::cout << "MADRV extensions:";
    for (const mpxadrv::MadrvExtension& extension : song.extensions()) {
      std::cout << " track " << extension.track + 1 << "=0x" << std::hex
                << std::uppercase << static_cast<int>(extension.opcode)
                << std::dec << std::nouppercase;
    }
    std::cout << " (MIDI export available with the midi command)\n";
  }
  std::cout << "Audio: " << song.rate() << " Hz, " << song.channels()
            << " channel" << (song.channels() == 1 ? "" : "s") << '\n';
}

void renderSong(Song& song, const fs::path& outputPath) {
  WavWriter writer(outputPath, song.rate(), song.channels());
  std::vector<std::int16_t> buffer(
      kFramesPerBuffer * static_cast<std::size_t>(song.channels()));
  std::uint64_t remaining = song.totalFrames();
  while (remaining > 0 && !gInterrupted) {
    const int frames = static_cast<int>(
        std::min<std::uint64_t>(kFramesPerBuffer, remaining));
    const int hasMore = song.render(buffer.data(), frames);
    writer.write(buffer.data(), static_cast<std::size_t>(frames));
    remaining -= static_cast<std::uint64_t>(frames);
    if (!hasMore) {
      break;
    }
  }
  writer.finalize();
  if (gInterrupted) {
    throw CliError("rendering interrupted; partial WAV file was kept");
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  try {
    const Options options = parseArguments(argc, argv);

    if (options.command == "tdx") {
      const mpxadrv::TdxResult compiled =
          mpxadrv::compileTdx(options.input, options.pdxDirectory);
      mpxadrv::writeTdxResult(compiled, options.output);
      std::cout << "Compiled " << compiled.banks << " bank"
                << (compiled.banks == 1 ? "" : "s") << ", "
                << compiled.assignments << " assignment"
                << (compiled.assignments == 1 ? "" : "s") << '\n';
      for (const fs::path& source : compiled.sourceFiles) {
        std::cout << "Source: " << source.string() << '\n';
      }
      std::cout << "Wrote " << fs::absolute(options.output).string() << '\n';
      return 0;
    }

    if (options.command == "midi-list") {
      const std::vector<std::string> destinations =
          mpxadrv::midiDestinationNames();
      if (destinations.empty()) {
        std::cout << "No CoreMIDI destinations found.\n";
      } else {
        for (std::size_t index = 0; index < destinations.size(); ++index) {
          std::cout << index << ": " << destinations[index] << '\n';
        }
      }
      return 0;
    }

    Song song(options);

    if (options.command == "info") {
      printSongInfo(song, options);
      return 0;
    }

    if (options.command == "midi") {
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), options.loops);
      mpxadrv::writeStandardMidi(sequence, options.output, song.title());
      for (const std::string& warning : sequence.warnings) {
        std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
      }
      std::cout << "Exported " << sequence.tracks.size() << " MIDI track"
                << (sequence.tracks.size() == 1 ? "" : "s") << '\n'
                << "Wrote " << fs::absolute(options.output).string() << '\n';
      return 0;
    }

    if (options.command == "midi-synth") {
      fs::path soundFont;
      if (!options.soundFont.empty()) {
        std::error_code error;
        soundFont = fs::absolute(options.soundFont, error);
        if (error || !fs::is_regular_file(soundFont, error)) {
          throw CliError("SoundFont file not found: " +
                         options.soundFont.string());
        }
        const std::string extension =
            asciiLower(soundFont.extension().string());
        if (extension != ".sf2" && extension != ".dls") {
          throw CliError("--soundfont requires an .sf2 or .dls file");
        }
      }
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), options.loops);
      for (const std::string& warning : sequence.warnings) {
        std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
      }
      if (sequence.tracks.empty()) {
        throw CliError("the song contains no convertible MIDI events");
      }
      std::cout << "Playing with the macOS "
                << (soundFont.empty() ? "DLSMusicDevice"
                                      : "AUMIDISynth")
                << "... press Ctrl-C to stop.\n";
      mpxadrv::playSoftwareSynth(sequence, soundFont,
                                 [] { return gInterrupted != 0; });
      std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
      return 0;
    }

    if (options.command == "midi-play") {
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), options.loops);
      for (const std::string& warning : sequence.warnings) {
        std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
      }
      if (sequence.tracks.empty()) {
        throw CliError("the song contains no convertible MIDI events");
      }
      std::cout << "Sending MIDI... press Ctrl-C to stop.\n";
      mpxadrv::playMidiSequence(sequence, options.midiDestination,
                                [] { return gInterrupted != 0; });
      std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
      return 0;
    }

    const std::string title = song.title();
    std::cout << (title.empty() ? options.input.filename().string() : title)
              << "  [" << formatDuration(song.duration()) << "]\n";

    if (options.command == "render") {
      renderSong(song, options.output);
      std::cout << "Wrote " << fs::absolute(options.output).string() << '\n';
      return 0;
    }

    std::cout << "Playing... press Ctrl-C to stop.\n";
    AudioPlayer player(song);
    player.play();
    std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
    return 0;
  } catch (const CliError& error) {
    std::cerr << "mpxadrv: " << error.what() << '\n';
    return 2;
  } catch (const mpxadrv::TdxError& error) {
    std::cerr << "mpxadrv: TDX: " << error.what() << '\n';
    return 2;
  } catch (const mpxadrv::MidiError& error) {
    std::cerr << "mpxadrv: MIDI: " << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "mpxadrv: unexpected error: " << error.what() << '\n';
    return 1;
  }
}
