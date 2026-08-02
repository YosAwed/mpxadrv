#include "midi.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <unistd.h>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    const std::vector<std::uint8_t> track = {
        0xe0, 0x08, 0x80,        // Switch track to MIDI channel 1.
        0xfd, 0x05,              // Program 5.
        0xfb, 0x80,              // Raw volume 0 -> velocity 127.
        0xfc, 0x40,              // Pan 64.
        0xfe, 0x01, 0x20,        // Modulation 32.
        0xe1, 0x3c,              // Polyphonic C4.
        0xa0, 0x07,              // E4, eight clocks.
        0x03,                    // Four-clock rest.
        0xe0, 0x09, 0x0c,        // Bend range 12.
        0xe0, 0x0e, 0x01, 0xc1, 0x09,  // Direct Program Change.
        0xff, 0xbe,              // Tempo 190.
        0xf1, 0x00, 0x00,        // End.
    };
    const int offsets[] = {0};
    const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
        track.data(), track.size(), offsets, 1, 1);
    require(sequence.tracks.size() == 1, "MIDI track was not emitted");
    require(sequence.tracks[0].endTick == 12, "track timing is wrong");
    require(sequence.tempos.size() == 2, "tempo event count is wrong");
    require(sequence.tempos[1].tick == 12 && sequence.tempos[1].value == 0xbe,
            "tempo event is wrong");

    const auto& events = sequence.tracks[0].events;
    require(events.size() == 11, "MIDI event count is wrong");
    require(events[0].bytes == std::vector<std::uint8_t>({0xc0, 0x05}),
            "program change is wrong");
    require(events[3].bytes ==
                std::vector<std::uint8_t>({0x90, 0x3c, 0x7f}),
            "polyphonic note-on is wrong");
    require(events[4].tick == 8 && events[4].bytes[0] == 0x80,
            "note-off timing is wrong");
    require(events.back().bytes ==
                std::vector<std::uint8_t>({0xc1, 0x09}),
            "direct MIDI command is wrong");

    mpxadrv::MidiSequence timing;
    timing.tempos = {{0, 0xc8, 0}, {10, 0xbe, 1}};
    timing.tracks.push_back(
        {0, {{0, {0x90, 60, 100}, 0}, {12, {0x80, 60, 0}, 1}}, 12});
    const auto scheduled = mpxadrv::scheduleMidiEvents(timing);
    require(scheduled.size() == 2, "scheduled MIDI event count is wrong");
    require(scheduled[0].microseconds == 0,
            "first scheduled MIDI event is late");
    require(scheduled[1].microseconds == 177152,
            "tempo-aware MIDI scheduling is wrong");

    const std::filesystem::path output =
        std::filesystem::temp_directory_path() /
        ("mpxadrv-midi-test-" + std::to_string(getpid()) + ".mid");
    mpxadrv::writeStandardMidi(sequence, output, "MIDI test");
    std::ifstream input(output, std::ios::binary);
    const std::vector<char> written((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    std::error_code error;
    std::filesystem::remove(output, error);
    require(written.size() > 32, "SMF output is too short");
    require(std::string(written.data(), 4) == "MThd", "SMF header is missing");
    require(std::string(written.data() + 14, 4) == "MTrk",
            "SMF conductor track is missing");

    const std::vector<std::uint8_t> unsupported = {
        0xe0, 0x08, 0x80, 0xe2, 0x00,
    };
    const mpxadrv::MidiSequence safe = mpxadrv::convertMadrvMidi(
        unsupported.data(), unsupported.size(), offsets, 1, 1);
    require(!safe.warnings.empty(), "E2 safety warning is missing");
    require(safe.tracks.empty(), "unsupported E2 emitted unexpected MIDI");

    std::cout << "MIDI conversion tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "MIDI conversion test failed: " << error.what() << '\n';
    return 1;
  }
}
