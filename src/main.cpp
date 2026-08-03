#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/HostTime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include "mdr.hpp"
#include "midi.hpp"
#include "software_synth.hpp"

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.6.1";
constexpr int kDefaultRate = 48'000;
constexpr int kDefaultLoops = 0;  // 0 = follow the song's L forever
constexpr std::uint32_t kFramesPerBuffer = 2'048;
constexpr int kQueueBufferCount = 3;

volatile std::sig_atomic_t gInterrupted = 0;

void handleSignal(int) {
  gInterrupted = 1;
}

void printStartupBanner(std::ostream& stream) {
  stream << "mpxadrv " << kVersion << '\n'
         << "Based on MADRV MUSIC CONVERTER Version 1.10 "
            "(c)1991,92 Konoa\n"
         << "macOS CLI adaptation developed by Awed (c)2026\n\n";
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

int expansionLoops(const Options& options) {
  // SMF/WAV and the MIDI converter need a finite expansion. Infinite playback
  // expands one L cycle and restarts from that point in the player.
  return options.loops == 0 ? 1 : options.loops;
}

bool infinitePlayback(const Options& options) {
  return options.loops == 0 &&
         (options.command == "play" || options.command == "midi-synth" ||
          options.command == "midi-play");
}

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
      << "  mpxadrv <file.mdx|file.mdr> [options]\n"
      << "  mpxadrv play <file.mdx|file.mdr> [options]\n"
      << "  mpxadrv info <file.mdx|file.mdr> [options]\n"
      << "  mpxadrv render <file.mdx|file.mdr> -o <file.wav> [options]\n\n"
      << "  mpxadrv midi <file.mdx|file.mdr> [-o <file.mid>] [options]\n"
      << "  mpxadrv midi-synth <file.mdx|file.mdr> [--soundfont <file.sf2|file.dls>] [options]\n"
      << "  mpxadrv midi-list\n"
      << "  mpxadrv midi-play <file.mdx|file.mdr> --destination <index-or-name> [options]\n"
      << "  mpxadrv tdx <file.tdx> [-o <file.pdx>] [options]\n\n"
      << "Options:\n"
      << "  -o, --output <path>    WAV, MIDI, or compiled PDX output path\n"
      << "  -p, --pdx-dir <path>   Additional PDX search directory\n"
      << "      --tdx-file <path>  Override the song's PDX with a TDX definition\n"
      << "      --destination <id> CoreMIDI USB/physical out (play, midi-play)\n"
      << "      --soundfont <path>  SF2/DLS soft synth (play/render/midi-synth)\n"
      << "  -r, --rate <hz>        Sample rate, 8000-192000 (default: 48000)\n"
      << "  -l, --loops <count>    Song L repeats: 0=forever (default), 1-100=finite\n"
      << "  -h, --help             Show this help\n"
      << "      --version          Show version\n";
}

Options parseArguments(int argc, char* argv[]) {
  if (argc < 2) {
    printUsage(std::cerr);
    throw CliError("an MDX or MDR file is required");
  }

  Options options;
  int index = 1;
  const std::string first = argv[index];
  if (first == "--help" || first == "-h") {
    printUsage(std::cout);
    std::exit(0);
  }
  if (first == "--version") {
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
      throw CliError("only one MDX or MDR file can be processed at a time");
    }
  }

  if (options.input.empty() && options.command != "midi-list") {
    throw CliError("an MDX or MDR file is required");
  }
  if (options.command == "midi-list" && !options.input.empty()) {
    throw CliError("midi-list does not accept an input file");
  }
  if (options.rate < 8'000 || options.rate > 192'000) {
    throw CliError("sample rate must be between 8000 and 192000 Hz");
  }
  if (options.loops < 0 || options.loops > 100) {
    throw CliError("loops must be between 0 (forever) and 100");
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
  if (!options.midiDestination.empty() && options.command != "midi-play" &&
      options.command != "play") {
    throw CliError("--destination can only be used with play or midi-play");
  }
  if (options.command == "midi-play" && options.midiDestination.empty()) {
    throw CliError("midi-play requires --destination <index-or-name>");
  }
  if (!options.midiDestination.empty() && options.command == "play" &&
      asciiLower(options.input.extension().string()) != ".mdr") {
    throw CliError("--destination with play requires an MDR file");
  }
  const bool mdrSoundFont =
      (options.command == "play" || options.command == "render") &&
      asciiLower(options.input.extension().string()) == ".mdr";
  if (!options.soundFont.empty() && options.command != "midi-synth" &&
      !mdrSoundFont) {
    throw CliError(
        "--soundfont can only be used with midi-synth, MDR play, or MDR render");
  }
  if (!options.midiDestination.empty() && !options.soundFont.empty()) {
    throw CliError(
        "--destination and --soundfont cannot be used together; "
        "choose external MIDI or the software synthesizer");
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
    infiniteLoops_ = infinitePlayback(options);
    // Duration uses one L cycle when looping forever so info/WAV bounds stay
    // finite; playback then sets max_infinite_loops to 0 (mdxmini: never stop).
    mdx_set_max_loop(&context_, expansionLoops(options));
    duration_ = mdx_get_length(&context_);
    if (duration_ <= 0) {
      mdx_close(&context_);
      opened_ = false;
      throw CliError("the MDX song has no playable duration");
    }
    if (infiniteLoops_) {
      mdx_set_max_loop(&context_, 0);
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
  bool infiniteLoops() const { return infiniteLoops_; }
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
  bool infiniteLoops_ = false;
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
      : song_(song),
        rate_(song.rate()),
        remainingFrames_(song.infiniteLoops()
                             ? std::numeric_limits<std::uint64_t>::max()
                             : song.totalFrames()),
        infinite_(song.infiniteLoops()) {
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
    playAt(std::chrono::steady_clock::time_point{}, {});
  }

  void prepare() {
    if (prepared_) {
      return;
    }
    const std::uint32_t bytes =
        kFramesPerBuffer * song_.channels() * sizeof(std::int16_t);
    for (int i = 0; i < kQueueBufferCount && !reachedEnd_; ++i) {
      AudioQueueBufferRef buffer = nullptr;
      requireAudio(AudioQueueAllocateBuffer(queue_, bytes, &buffer),
                   "AudioQueueAllocateBuffer");
      fillAndEnqueue(buffer);
    }
    prepared_ = true;
  }

  // Audible song position in microseconds since AudioQueueStart, or -1 if the
  // device clock is not running yet. Safe to call from the MIDI thread.
  std::int64_t playbackMicroseconds() const {
    if (queue_ == nullptr || !started_.load() || rate_ <= 0) {
      return -1;
    }
    AudioTimeStamp stamp{};
    Boolean discontinuity = false;
    const OSStatus status =
        AudioQueueGetCurrentTime(queue_, nullptr, &stamp, &discontinuity);
    if (status != noErr ||
        (stamp.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
      return -1;
    }
    if (stamp.mSampleTime < 0) {
      return 0;
    }
    return static_cast<std::int64_t>((stamp.mSampleTime * 1'000'000.0) /
                                     static_cast<double>(rate_));
  }

  void playAt(std::chrono::steady_clock::time_point start,
              std::function<bool()> shouldStop) {
    shouldStop_ = std::move(shouldStop);
    prepare();
    if (shouldStop()) {
      return;
    }
    AudioTimeStamp startStamp{};
    const AudioTimeStamp* requestedStart = nullptr;
    if (start != std::chrono::steady_clock::time_point{}) {
      const auto delay = std::chrono::duration_cast<std::chrono::nanoseconds>(
          start - std::chrono::steady_clock::now());
      if (delay.count() > 0) {
        startStamp.mFlags = kAudioTimeStampHostTimeValid;
        startStamp.mHostTime =
            AudioGetCurrentHostTime() +
            AudioConvertNanosToHostTime(static_cast<UInt64>(delay.count()));
        requestedStart = &startStamp;
      }
    }
    requireAudio(AudioQueueStart(queue_, requestedStart), "AudioQueueStart");
    started_.store(true);

    while (!finished_.load() && !shouldStop()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (shouldStop()) {
      AudioQueueStop(queue_, true);
    }
  }

 private:
  static void outputCallback(void* userData, AudioQueueRef,
                             AudioQueueBufferRef buffer) {
    auto* player = static_cast<AudioPlayer*>(userData);
    --player->buffersInFlight_;
    if (!player->reachedEnd_ && !player->shouldStop()) {
      player->fillAndEnqueue(buffer);
    } else if (player->buffersInFlight_ == 0) {
      player->finished_.store(true);
    }
  }

  bool shouldStop() const {
    return gInterrupted || (shouldStop_ && shouldStop_());
  }

  void fillAndEnqueue(AudioQueueBufferRef buffer) noexcept {
    std::uint32_t frames = kFramesPerBuffer;
    if (!infinite_) {
      frames = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(kFramesPerBuffer, remainingFrames_));
      if (frames == 0) {
        reachedEnd_ = true;
        return;
      }
    }
    const int hasMore = song_.render(
        static_cast<std::int16_t*>(buffer->mAudioData), static_cast<int>(frames));
    buffer->mAudioDataByteSize =
        frames * song_.channels() * sizeof(std::int16_t);
    if (!infinite_) {
      remainingFrames_ -= frames;
    }
    reachedEnd_ = !hasMore || (!infinite_ && remainingFrames_ == 0);
    if (AudioQueueEnqueueBuffer(queue_, buffer, 0, nullptr) == noErr) {
      ++buffersInFlight_;
    } else {
      reachedEnd_ = true;
      finished_.store(true);
    }
  }

  Song& song_;
  int rate_ = 0;
  AudioQueueRef queue_ = nullptr;
  std::uint64_t remainingFrames_ = 0;
  std::atomic<bool> finished_{false};
  std::atomic<bool> started_{false};
  std::function<bool()> shouldStop_;
  int buffersInFlight_ = 0;
  bool reachedEnd_ = false;
  bool prepared_ = false;
  bool infinite_ = false;
};

void printSongInfo(const Song& song, const Options& options) {
  const std::string title = song.title();
  std::cout << "Title: " << (title.empty() ? "(untitled)" : title) << '\n'
            << "Duration: " << formatDuration(song.duration()) << " ("
            << song.duration() << " seconds)"
            << (options.loops == 0 ? " per loop cycle; loops forever" : "")
            << '\n'
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

std::int16_t mixSample(std::int32_t left, std::int32_t right) {
  const std::int32_t mixed = left + right;
  if (mixed > 32767) {
    return 32767;
  }
  if (mixed < -32768) {
    return -32768;
  }
  return static_cast<std::int16_t>(mixed);
}

void renderHybridSong(Song& hardwareSong, const mpxadrv::MidiSequence& sequence,
                      const fs::path& soundFont, const fs::path& outputPath) {
  if (soundFont.empty() ||
      asciiLower(soundFont.extension().string()) != ".sf2") {
    throw CliError(
        "hybrid MDR render requires --soundfont <file.sf2> (FluidSynth offline)");
  }
  const int rate = hardwareSong.rate();
  const int channels = hardwareSong.channels();
  if (channels < 1 || channels > 2) {
    throw CliError("unsupported hybrid render channel count");
  }

  mpxadrv::OfflineFluidRenderer midi(soundFont, rate);
  midi.prepare(sequence);

  WavWriter writer(outputPath, rate, 2);
  std::vector<std::int16_t> fmBuffer(
      kFramesPerBuffer * static_cast<std::size_t>(channels));
  std::vector<std::int16_t> midiBuffer(kFramesPerBuffer * 2);
  std::vector<std::int16_t> mixBuffer(kFramesPerBuffer * 2);
  std::uint64_t remaining = hardwareSong.totalFrames();

  while (remaining > 0 && !gInterrupted) {
    const int frames = static_cast<int>(
        std::min<std::uint64_t>(kFramesPerBuffer, remaining));
    const int hasMore = hardwareSong.render(fmBuffer.data(), frames);
    midi.render(midiBuffer.data(), frames);
    for (int frame = 0; frame < frames; ++frame) {
      const std::int16_t fmLeft =
          channels == 1 ? fmBuffer[static_cast<std::size_t>(frame)]
                        : fmBuffer[static_cast<std::size_t>(frame) * 2];
      const std::int16_t fmRight =
          channels == 1
              ? fmLeft
              : fmBuffer[static_cast<std::size_t>(frame) * 2 + 1];
      mixBuffer[static_cast<std::size_t>(frame) * 2] = mixSample(
          fmLeft, midiBuffer[static_cast<std::size_t>(frame) * 2]);
      mixBuffer[static_cast<std::size_t>(frame) * 2 + 1] = mixSample(
          fmRight, midiBuffer[static_cast<std::size_t>(frame) * 2 + 1]);
    }
    writer.write(mixBuffer.data(), static_cast<std::size_t>(frames));
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

fs::path locateMdrPdx(const mpxadrv::MdrFile& mdr, const fs::path& input,
                      const fs::path& extraDirectory) {
  if (mdr.pdxName.empty()) {
    return {};
  }
  std::vector<std::string> names = {mdr.pdxName};
  if (fs::path(mdr.pdxName).extension().empty()) {
    names.push_back(mdr.pdxName + ".pdx");
  }
  for (const std::string& name : names) {
    if (fs::path found = findCaseInsensitive(input.parent_path(), name);
        !found.empty()) {
      return found;
    }
    if (!extraDirectory.empty()) {
      if (fs::path found =
              findCaseInsensitive(fs::absolute(extraDirectory), name);
          !found.empty()) {
        return found;
      }
    }
  }
  return {};
}

fs::path checkedSoundFont(const fs::path& requested) {
  if (requested.empty()) {
    return {};
  }
  std::error_code error;
  const fs::path soundFont = fs::absolute(requested, error);
  if (error || !fs::is_regular_file(soundFont, error)) {
    throw CliError("SoundFont file not found: " + requested.string());
  }
  const std::string extension = asciiLower(soundFont.extension().string());
  if (extension != ".sf2" && extension != ".dls") {
    throw CliError("--soundfont requires an .sf2 or .dls file");
  }
  return soundFont;
}

void printMidiWarnings(const mpxadrv::MidiSequence& sequence) {
  for (const std::string& warning : sequence.warnings) {
    std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
  }
}

std::vector<std::uint8_t> makeMdxTempoConductor(
    const mpxadrv::MidiSequence& sequence) {
  std::vector<std::uint8_t> track;
  std::uint64_t tick = 0;
  auto appendRest = [&](std::uint64_t clocks) {
    while (clocks > 0) {
      const std::uint8_t chunk =
          static_cast<std::uint8_t>(std::min<std::uint64_t>(clocks, 128));
      track.push_back(static_cast<std::uint8_t>(chunk - 1));
      clocks -= chunk;
    }
  };
  for (const mpxadrv::MidiTempo& tempo : sequence.tempos) {
    if (tempo.tick > sequence.endTick) {
      break;
    }
    appendRest(tempo.tick - tick);
    tick = tempo.tick;
    track.push_back(0xff);
    track.push_back(tempo.value);
  }
  appendRest(sequence.endTick - tick);
  track.push_back(0xf1);
  track.push_back(0x00);
  return track;
}

bool canLoadMdrPdx(const mpxadrv::MdrFile& mdr, const fs::path& pdx) {
  bool includePdx = mdr.pdxName.empty() || !pdx.empty();
  if (includePdx && !pdx.empty() &&
      asciiLower(pdx.extension().string()) == ".tdx") {
    try {
      static_cast<void>(mpxadrv::compileTdx(pdx, pdx.parent_path()));
    } catch (const mpxadrv::TdxError& tdxError) {
      includePdx = false;
      std::cerr << "mpxadrv: MDR warning: " << tdxError.what()
                << "; continuing with FM only\n";
    }
  } else if (!includePdx) {
    std::cerr << "mpxadrv: MDR warning: PCM definition was not found; "
                 "continuing with FM only\n";
  }
  return includePdx;
}

fs::path writeTemporaryMdx(const TemporaryDirectory& temporary,
                           const fs::path& input,
                           const std::vector<std::uint8_t>& mdx) {
  fs::path filename = input.filename();
  filename.replace_extension(".mdx");
  const fs::path path = temporary.path() / filename;
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char*>(mdx.data()),
               static_cast<std::streamsize>(mdx.size()));
  if (!output) {
    throw CliError("failed to prepare MDR FM/PCM playback");
  }
  output.close();
  return path;
}

int processMdr(const Options& options) {
  std::error_code error;
  const fs::path input = fs::absolute(options.input, error);
  if (error || !fs::is_regular_file(input, error)) {
    throw CliError("MDR file not found: " + options.input.string());
  }
  if (!options.tdxFile.empty()) {
    throw CliError("--tdx-file is not supported for MDR input");
  }

  const mpxadrv::MdrFile mdr = mpxadrv::loadMdr(input);
  const std::string title = shiftJisToUtf8(mdr.title.c_str());
  const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
      mdr.data.data(), mdr.data.size(), mdr.trackOffsets.data(),
      mpxadrv::MdrFile::kTrackCount, expansionLoops(options));
  const bool loopForever = infinitePlayback(options) && sequence.hasSongLoop;
  const bool infoLoopsForever = options.loops == 0 && sequence.hasSongLoop;
  const std::uint64_t durationUs =
      mpxadrv::midiDurationMicroseconds(sequence);
  const int durationSeconds = static_cast<int>(std::min<std::uint64_t>(
      (durationUs + 999'999) / 1'000'000,
      static_cast<std::uint64_t>(std::numeric_limits<int>::max())));

  if (options.command == "info") {
    std::cout << "Title: " << (title.empty() ? "(untitled)" : title) << '\n'
              << "Format: MADRV MDR (32-track EX-MDR)\n"
              << "Duration: " << formatDuration(durationSeconds) << " ("
              << durationSeconds << " seconds)"
              << (infoLoopsForever ? " per loop cycle; loops forever" : "")
              << '\n'
              << "Tracks: " << mpxadrv::MdrFile::kTrackCount << " capacity, "
              << mdr.activeTracks << " active\n"
              << "MIDI tracks: " << sequence.tracks.size() << '\n';
    if (mdr.pdxName.empty()) {
      std::cout << "PDX: none\n";
    } else {
      const fs::path pdx = locateMdrPdx(mdr, input, options.pdxDirectory);
      std::cout << "PDX: " << shiftJisToUtf8(mdr.pdxName.c_str());
      if (pdx.empty()) {
        std::cout << " (not found)\n";
      } else {
        std::cout << " (" << pdx.string() << ")\n";
      }
    }
    if (sequence.tracks.empty()) {
      std::cout << "Playback: FM/PCM compatibility path\n";
    } else if (mpxadrv::countSeparableHardwareTracks(mdr) > 0) {
      std::cout << "Playback: synchronized MIDI + FM/PCM hybrid\n";
    } else {
      std::cout << "Playback: macOS software MIDI synthesizer";
      if (!mdr.pdxName.empty()) {
        std::cout << " (MIDI portion; FM/PCM mixing is not available)";
      }
      std::cout << '\n';
    }
    printMidiWarnings(sequence);
    return 0;
  }

  if (sequence.tracks.empty() &&
      (options.command == "play" || options.command == "render")) {
    const fs::path pdx = locateMdrPdx(mdr, input, options.pdxDirectory);
    const bool includePdx = canLoadMdrPdx(mdr, pdx);
    const std::vector<std::uint8_t> mdx =
        mpxadrv::makeMdxCompatible(mdr, includePdx);
    TemporaryDirectory temporary;
    const fs::path mdxPath = writeTemporaryMdx(temporary, input, mdx);

    Options compatible = options;
    compatible.input = mdxPath;
    compatible.pdxDirectory =
        !includePdx || pdx.empty() ? options.pdxDirectory : pdx.parent_path();
    Song song(compatible);
    std::cout << (title.empty() ? input.filename().string() : title) << "  ["
              << formatDuration(song.duration()) << "]\n";
    if (options.command == "render") {
      renderSong(song, options.output);
      std::cout << "Wrote " << fs::absolute(options.output).string() << '\n';
      return 0;
    }
    std::cout << "Playing MDR FM/PCM... press Ctrl-C to stop.\n";
    AudioPlayer player(song);
    player.play();
    std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
    return 0;
  }

  if ((options.command == "play" || options.command == "render") &&
      !sequence.tracks.empty() &&
      mpxadrv::countSeparableHardwareTracks(mdr) > 0) {
    try {
      const std::vector<std::uint8_t> mdx =
          mpxadrv::makeMdxHardwareCompatible(mdr,
                                             makeMdxTempoConductor(sequence));
      const fs::path pdx = locateMdrPdx(mdr, input, options.pdxDirectory);
      const bool includePdx = canLoadMdrPdx(mdr, pdx);
      if (!includePdx && !mdr.pdxName.empty()) {
        throw mpxadrv::MdrError(
            "hybrid MDR hardware tracks require their PCM sample bank");
      }
      if (options.command == "render") {
        const fs::path soundFont = checkedSoundFont(options.soundFont);
        if (soundFont.empty() ||
            asciiLower(soundFont.extension().string()) != ".sf2") {
          throw CliError(
              "hybrid MDR render requires --soundfont <file.sf2> "
              "(FluidSynth offline)");
        }
        TemporaryDirectory temporary;
        const fs::path mdxPath = writeTemporaryMdx(temporary, input, mdx);
        Options compatible = options;
        compatible.input = mdxPath;
        compatible.pdxDirectory =
            pdx.empty() ? options.pdxDirectory : pdx.parent_path();
        Song hardwareSong(compatible);
        printMidiWarnings(sequence);
        std::cout << (title.empty() ? input.filename().string() : title)
                  << "  [" << formatDuration(durationSeconds) << "]\n"
                  << "Rendering MDR MIDI + FM/PCM to WAV...\n";
        renderHybridSong(hardwareSong, sequence, soundFont, options.output);
        std::cout << "Wrote " << fs::absolute(options.output).string() << '\n';
        return 0;
      }

      TemporaryDirectory temporary;
      const fs::path mdxPath = writeTemporaryMdx(temporary, input, mdx);
      Options compatible = options;
      compatible.input = mdxPath;
      compatible.pdxDirectory =
          pdx.empty() ? options.pdxDirectory : pdx.parent_path();
      Song hardwareSong(compatible);
      printMidiWarnings(sequence);
      const bool useExternalMidi = !options.midiDestination.empty();
      std::cout << (title.empty() ? input.filename().string() : title) << "  ["
                << formatDuration(durationSeconds) << "]\n"
                << "Playing MDR "
                << (useExternalMidi ? "CoreMIDI + FM/PCM"
                                    : "MIDI + FM/PCM")
                << "... press Ctrl-C to stop.\n";

      // Match mdxmini's per-tick sample truncation so MIDI does not creep
      // ahead of FM/PCM over a multi-minute hybrid song.
      std::unique_ptr<mpxadrv::SoftwareSynthPlayer> softMidi;
      std::unique_ptr<mpxadrv::CoreMidiPlayer> coreMidi;
      std::chrono::microseconds midiLead{0};
      if (useExternalMidi) {
        coreMidi = std::make_unique<mpxadrv::CoreMidiPlayer>(
            options.midiDestination);
        coreMidi->prepare(sequence, loopForever, hardwareSong.rate());
        midiLead = coreMidi->latencyCompensation();
      } else {
        softMidi = std::make_unique<mpxadrv::SoftwareSynthPlayer>(
            checkedSoundFont(options.soundFont));
        softMidi->prepare(sequence, loopForever, hardwareSong.rate());
        midiLead = softMidi->latencyCompensation();
      }
      AudioPlayer player(hardwareSong);
      player.prepare();
      std::atomic<bool> hybridStop{false};
      std::exception_ptr midiFailure;
      const auto start = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(150);
      const mpxadrv::SongPositionClock songClock = [&]() -> std::int64_t {
        return player.playbackMicroseconds();
      };
      std::thread midiThread([&] {
        try {
          const auto shouldStop = [&] {
            return gInterrupted != 0 || hybridStop.load();
          };
          // Share the FM/PCM AudioQueue clock so external/soft MIDI cannot
          // drift away from OPM over long looping songs such as BCheck.
          if (coreMidi) {
            coreMidi->playPreparedAt(shouldStop, start, songClock, midiLead);
          } else {
            softMidi->playPreparedAt(shouldStop, start, songClock, midiLead);
          }
        } catch (...) {
          midiFailure = std::current_exception();
          hybridStop.store(true);
        }
      });
      try {
        player.playAt(start, [&] { return hybridStop.load(); });
      } catch (...) {
        hybridStop.store(true);
        midiThread.join();
        throw;
      }
      midiThread.join();
      if (midiFailure) {
        std::rethrow_exception(midiFailure);
      }
      std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
      return 0;
    } catch (const mpxadrv::MdrError& hardwareError) {
      std::cerr << "mpxadrv: MDR warning: " << hardwareError.what()
                << "; "
                << (options.command == "render" ? "rendering" : "playing")
                << " MIDI only\n";
    }
  }

  if (options.command == "render") {
    if (sequence.tracks.empty()) {
      throw CliError("the MDR file contains no convertible MIDI events");
    }
    const fs::path soundFont = checkedSoundFont(options.soundFont);
    if (soundFont.empty() ||
        asciiLower(soundFont.extension().string()) != ".sf2") {
      throw CliError(
          "MDR MIDI WAV rendering requires --soundfont <file.sf2>");
    }
    printMidiWarnings(sequence);
    std::cout << (title.empty() ? input.filename().string() : title) << "  ["
              << formatDuration(durationSeconds) << "]\n"
              << "Rendering MDR MIDI to WAV...\n";
    // Finite render: rebuild FM-less duration from the already-expanded
    // sequence and write FluidSynth alone as stereo PCM.
    mpxadrv::OfflineFluidRenderer midi(soundFont, options.rate);
    midi.prepare(sequence);
    WavWriter writer(options.output, options.rate, 2);
    const std::uint64_t totalFrames =
        (durationUs * static_cast<std::uint64_t>(options.rate) + 999'999ull) /
        1'000'000ull;
    std::vector<std::int16_t> buffer(kFramesPerBuffer * 2);
    std::uint64_t remaining = totalFrames;
    while (remaining > 0 && !gInterrupted) {
      const int frames = static_cast<int>(
          std::min<std::uint64_t>(kFramesPerBuffer, remaining));
      midi.render(buffer.data(), frames);
      writer.write(buffer.data(), static_cast<std::size_t>(frames));
      remaining -= static_cast<std::uint64_t>(frames);
    }
    writer.finalize();
    if (gInterrupted) {
      throw CliError("rendering interrupted; partial WAV file was kept");
    }
    std::cout << "Wrote " << fs::absolute(options.output).string() << '\n';
    return 0;
  }
  if (sequence.tracks.empty()) {
    throw CliError("the MDR file contains no convertible MIDI events");
  }
  printMidiWarnings(sequence);

  if (options.command == "midi") {
    mpxadrv::writeStandardMidi(sequence, options.output, title);
    std::cout << "Exported " << sequence.tracks.size() << " MIDI track"
              << (sequence.tracks.size() == 1 ? "" : "s") << '\n'
              << "Wrote " << fs::absolute(options.output).string() << '\n';
    return 0;
  }
  if (options.command == "midi-play" || !options.midiDestination.empty()) {
    std::cout << "Sending MDR MIDI to CoreMIDI... press Ctrl-C to stop.\n";
    mpxadrv::playMidiSequence(sequence, options.midiDestination,
                              [] { return gInterrupted != 0; }, loopForever);
    std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
    return 0;
  }

  const fs::path soundFont = checkedSoundFont(options.soundFont);
  std::cout << (title.empty() ? input.filename().string() : title) << "  ["
            << formatDuration(durationSeconds) << "]\n"
            << "Playing MDR MIDI with the "
            << (soundFont.empty() ? "macOS DLSMusicDevice"
                                  : "FluidSynth SoundFont")
            << "... press Ctrl-C to stop.\n";
  mpxadrv::playSoftwareSynth(sequence, soundFont,
                             [] { return gInterrupted != 0; }, loopForever);
  std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  printStartupBanner(std::cout);
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

    if (asciiLower(options.input.extension().string()) == ".mdr") {
      return processMdr(options);
    }

    Song song(options);

    if (options.command == "info") {
      printSongInfo(song, options);
      return 0;
    }

    if (options.command == "midi") {
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), expansionLoops(options));
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
      const fs::path soundFont = checkedSoundFont(options.soundFont);
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), expansionLoops(options));
      for (const std::string& warning : sequence.warnings) {
        std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
      }
      if (sequence.tracks.empty()) {
        throw CliError("the song contains no convertible MIDI events");
      }
      const bool loopForever =
          infinitePlayback(options) && sequence.hasSongLoop;
      std::cout << "Playing with the "
                << (soundFont.empty() ? "macOS DLSMusicDevice"
                                      : "FluidSynth SoundFont")
                << "... press Ctrl-C to stop.\n";
      mpxadrv::playSoftwareSynth(sequence, soundFont,
                                 [] { return gInterrupted != 0; },
                                 loopForever);
      std::cout << (gInterrupted ? "Stopped.\n" : "Finished.\n");
      return 0;
    }

    if (options.command == "midi-play") {
      const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
          song.mdxData(), song.mdxLength(), song.trackOffsets(),
          song.mdxTrackCount(), expansionLoops(options));
      for (const std::string& warning : sequence.warnings) {
        std::cerr << "mpxadrv: MIDI warning: " << warning << '\n';
      }
      if (sequence.tracks.empty()) {
        throw CliError("the song contains no convertible MIDI events");
      }
      const bool loopForever =
          infinitePlayback(options) && sequence.hasSongLoop;
      std::cout << "Sending MIDI... press Ctrl-C to stop.\n";
      mpxadrv::playMidiSequence(sequence, options.midiDestination,
                                [] { return gInterrupted != 0; },
                                loopForever);
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
  } catch (const mpxadrv::MdrError& error) {
    std::cerr << "mpxadrv: MDR: " << error.what() << '\n';
    return 2;
  } catch (const mpxadrv::MidiError& error) {
    std::cerr << "mpxadrv: MIDI: " << error.what() << '\n';
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "mpxadrv: unexpected error: " << error.what() << '\n';
    return 1;
  }
}
