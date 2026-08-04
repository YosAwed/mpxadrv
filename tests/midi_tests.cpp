#include "midi.hpp"

#include <algorithm>
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
        0xf1, 0x00,              // End.
    };
    const int offsets[] = {0};
    const mpxadrv::MidiSequence sequence = mpxadrv::convertMadrvMidi(
        track.data(), track.size(), offsets, 1, 1);
    require(sequence.tracks.size() == 1, "MIDI track was not emitted");
    require(sequence.tracks[0].endTick == 12, "track timing is wrong");
    require(sequence.tempos.size() == 2, "tempo event count is wrong");
    require(sequence.tempos[1].tick == 12 && sequence.tempos[1].value == 0xbe,
            "tempo event is wrong");

    const std::vector<std::uint8_t> loopTrack = {
        0xe0, 0x08, 0x80,  // MIDI channel 1.
        0x03,              // Four-clock rest at loop start.
        0xf1, 0xff, 0xfc,  // Loop back four bytes once.
    };
    const mpxadrv::MidiSequence loopSequence = mpxadrv::convertMadrvMidi(
        loopTrack.data(), loopTrack.size(), offsets, 1, 1);
    require(loopSequence.endTick == 8,
            "one requested song loop was not replayed");
    require(loopSequence.hasSongLoop && loopSequence.loopStartTick == 4,
            "song loop start tick was not recorded");

    // Two tracks with L periods 4 and 8: wrap from the longer cycle, and keep
    // the short track expanded through that whole cycle.
    const std::vector<std::uint8_t> multiData = {
        // Track 0: intro note (4) + body note (8).
        0xe0, 0x08, 0x80, 0xfb, 0x80, 0xa0, 0x03, 0xa1, 0x07, 0xf1, 0xff,
        0xfb,
        // Track 1: period-4 note loop.
        0xe0, 0x08, 0x81, 0xfb, 0x80, 0xa2, 0x03, 0xf1, 0xff, 0xfb,
        // Overrun guard if a loop offset is wrong during the test.
        0xf1, 0x00,
    };
    const int multiOffsets[] = {0, 12};
    const mpxadrv::MidiSequence multi = mpxadrv::convertMadrvMidi(
        multiData.data(), multiData.size(), multiOffsets, 2, 1);
    require(multi.hasSongLoop, "multi-period song loop missing");
    require(multi.loopStartTick == 12,
            "master loop should start at the long-period F1");
    require(multi.endTick - multi.loopStartTick == 8,
            "aligned cycle length should equal the long period");
    bool shortFilled = false;
    for (const auto& track : multi.tracks) {
      if (track.sourceTrack == 1 && track.endTick >= multi.endTick) {
        shortFilled = true;
      }
    }
    require(shortFilled, "short-period track was not expanded to master cycle");

    const auto& events = sequence.tracks[0].events;
    require(events.size() == 17, "MIDI event count is wrong");
    const auto bendRangeLsb = std::find_if(
        events.begin(), events.end(), [](const mpxadrv::MidiEvent& event) {
          return event.bytes ==
                 std::vector<std::uint8_t>({0xb0, 38, 0});
        });
    require(bendRangeLsb != events.end(),
            "bend-range Data Entry LSB was not cleared");
    const auto program = std::find_if(
        events.begin(), events.end(), [](const mpxadrv::MidiEvent& event) {
          return event.bytes == std::vector<std::uint8_t>({0xc0, 0x05});
        });
    require(program != events.end(),
            "program change is wrong");
    const auto polyphonic = std::find_if(
        events.begin(), events.end(), [](const mpxadrv::MidiEvent& event) {
          return event.bytes ==
                 std::vector<std::uint8_t>({0x90, 0x3c, 0x7f});
        });
    require(polyphonic != events.end(),
            "polyphonic note-on is wrong");
    const auto polyphonicOff = std::find_if(
        events.begin(), events.end(), [](const mpxadrv::MidiEvent& event) {
          return event.tick == 8 && event.bytes ==
                 std::vector<std::uint8_t>({0x80, 0x3c, 0x00});
        });
    require(polyphonicOff != events.end(),
            "note-off timing is wrong");
    require(events.back().bytes ==
                std::vector<std::uint8_t>({0xc1, 0x09}),
            "direct MIDI command is wrong");

    const std::vector<std::uint8_t> bankTrack = {
        0xe0, 0x08, 0x80,  // MIDI channel 1.
        0xfe, 0x00, 0x08,  // Bank MSB 8 (GS variation).
        0xfd, 0x50,        // Program 80.
        0xf1, 0x00,
    };
    const mpxadrv::MidiSequence bankSelect = mpxadrv::convertMadrvMidi(
        bankTrack.data(), bankTrack.size(), offsets, 1, 1);
    const auto bankMsb = std::find_if(
        bankSelect.tracks[0].events.begin(), bankSelect.tracks[0].events.end(),
        [](const mpxadrv::MidiEvent& event) {
          return event.bytes ==
                 std::vector<std::uint8_t>({0xb0, 0x00, 0x08});
        });
    const auto bankLsb = std::find_if(
        bankSelect.tracks[0].events.begin(), bankSelect.tracks[0].events.end(),
        [](const mpxadrv::MidiEvent& event) {
          return event.bytes ==
                 std::vector<std::uint8_t>({0xb0, 0x20, 0x00});
        });
    require(bankMsb != bankSelect.tracks[0].events.end() &&
                bankLsb != bankSelect.tracks[0].events.end() &&
                bankLsb->order == bankMsb->order + 1,
            "GS bank select did not clear CC32 after CC0");

    const std::vector<std::uint8_t> portamentoTrack = {
        0xe0, 0x08, 0x80,  // Switch track to MIDI channel 1.
        0xe0, 0x09, 0x0c,  // Bend range 12 semitones.
        0xf2, 0x10, 0x00,  // Rise one semitone over four clocks.
        0xf7,              // Keep the MIDI note on while it bends.
        0x9c, 0x03,        // C4, four clocks.
        0xf2, 0x00, 0x00,  // Stop the portamento.
        0x9d, 0x03,        // Continue logically at C#4, then key off.
        0xf1, 0x00,
    };
    const mpxadrv::MidiSequence portamento = mpxadrv::convertMadrvMidi(
        portamentoTrack.data(), portamentoTrack.size(), offsets, 1, 1);
    require(portamento.tracks.size() == 1,
            "portamento track was not emitted");
    std::vector<mpxadrv::MidiEvent> bends;
    int noteOns = 0;
    int noteOffs = 0;
    for (const auto& event : portamento.tracks[0].events) {
      const std::uint8_t status = event.bytes.empty() ? 0 : event.bytes[0] & 0xf0;
      if (status == 0xe0) {
        bends.push_back(event);
      } else if (status == 0x90 && event.bytes.size() == 3 &&
                 event.bytes[2] != 0) {
        ++noteOns;
      } else if (status == 0x80 ||
                 (status == 0x90 && event.bytes.size() == 3 &&
                  event.bytes[2] == 0)) {
        ++noteOffs;
        require(event.tick == 8, "tied portamento note ended too early");
      }
    }
    require(bends.size() == 4,
            "portamento did not produce one pitch bend per clock");
    require(bends.front().tick == 0 && bends.back().tick == 3,
            "portamento pitch-bend timing is wrong");
    require(bends.front().bytes ==
                std::vector<std::uint8_t>({0xe0, 0x2a, 0x41}) &&
                bends.back().bytes ==
                    std::vector<std::uint8_t>({0xe0, 0x2a, 0x45}),
            "portamento pitch-bend values are wrong");
    require(noteOns == 1 && noteOffs == 1,
            "tied portamento retriggered the MIDI note");

    const std::vector<std::uint8_t> detuneTrack = {
        0xe0, 0x08, 0x80,  // Switch track to MIDI channel 1.
        0xf3, 0x00, 0x40,  // One-semitone MADRV detune at range 12.
        0x9c, 0x00,        // C4, one clock.
        0xf1, 0x00,
    };
    const mpxadrv::MidiSequence detune = mpxadrv::convertMadrvMidi(
        detuneTrack.data(), detuneTrack.size(), offsets, 1, 1);
    const auto detuneBend = std::find_if(
        detune.tracks[0].events.begin(), detune.tracks[0].events.end(),
        [](const mpxadrv::MidiEvent& event) {
          return !event.bytes.empty() && (event.bytes[0] & 0xf0) == 0xe0;
        });
    require(detuneBend != detune.tracks[0].events.end() &&
                detuneBend->bytes ==
                    std::vector<std::uint8_t>({0xe0, 0x2a, 0x45}),
            "MADRV detune was not converted to MIDI pitch bend");

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
    timing.endTick = 20;
    require(mpxadrv::midiDurationMicroseconds(timing) == 312320,
            "tempo-aware MIDI duration is wrong");
    // At 48 kHz, mdxmini truncates each Timer-B period to an integer sample
    // count. Hybrid playback must use the same quantisation.
    const auto quantised = mpxadrv::scheduleMidiEvents(timing, 48000);
    require(quantised.size() == 2, "quantised MIDI event count is wrong");
    require(quantised[0].microseconds == 0, "first quantised event is late");
    require(quantised[1].microseconds == 177125,
            "48 kHz-quantised MIDI scheduling is wrong");
    require(mpxadrv::midiDurationMicroseconds(timing, 48000) == 312291,
            "48 kHz-quantised MIDI duration is wrong");
    require(quantised[1].microseconds < scheduled[1].microseconds,
            "quantised schedule should be slightly shorter than exact us");

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
        0xe0, 0x08, 0x80, 0xe2, 0x01,
    };
    const mpxadrv::MidiSequence safe = mpxadrv::convertMadrvMidi(
        unsupported.data(), unsupported.size(), offsets, 1, 1);
    require(!safe.warnings.empty(), "E2 safety warning is missing");
    require(safe.tracks.empty(), "unsupported E2 emitted unexpected MIDI");

    const std::vector<std::uint8_t> resets = {
        0xe0, 0x08, 0x80,  // Switch to MIDI channel 1.
        0xe2, 0x00,        // MT-32 reset.
        0xe2, 0x14,        // CM-64 reset.
        0xe2, 0x1c,        // GS reset.
        0xf1, 0x00,
    };
    const mpxadrv::MidiSequence resetSequence = mpxadrv::convertMadrvMidi(
        resets.data(), resets.size(), offsets, 1, 1);
    require(resetSequence.warnings.empty(),
            "supported E2 reset produced a warning");
    require(resetSequence.tracks.size() == 1,
            "E2 reset track was not emitted");
    const auto& resetEvents = resetSequence.tracks[0].events;
    require(resetEvents.size() == 3, "E2 reset event count is wrong");
    const std::vector<std::uint8_t> mt32Reset = {
        0xf0, 0x41, 0x10, 0x16, 0x12, 0x7f,
        0x00, 0x00, 0x00, 0x01, 0xf7,
    };
    const std::vector<std::uint8_t> gsReset = {
        0xf0, 0x41, 0x10, 0x42, 0x12, 0x40,
        0x00, 0x7f, 0x00, 0x41, 0xf7,
    };
    require(resetEvents[0].bytes == mt32Reset,
            "MT-32 reset SysEx is wrong");
    require(resetEvents[1].bytes == mt32Reset,
            "CM-64 reset SysEx is wrong");
    require(resetEvents[2].bytes == gsReset, "GS reset SysEx is wrong");

    const std::vector<std::uint8_t> laterMdr = {
        0xe0, 0x1d, 0x00,              // Later MDR timing-mode marker.
        0xe0, 0x08, 0x80,              // Switch to MIDI channel 1.
        0xe2, 0x28, 0x02, 0x03, 0x04,  // SC reverb packed data.
        0xe2, 0x32, 0x00, 0x08,        // SC partial reserve.
        0xf1, 0x00,
    };
    const mpxadrv::MidiSequence laterSequence = mpxadrv::convertMadrvMidi(
        laterMdr.data(), laterMdr.size(), offsets, 1, 1);
    require(laterSequence.warnings.empty(),
            "supported later MDR/SC command produced a warning");
    require(laterSequence.tracks.size() == 1,
            "later MDR/SC commands did not emit a track");
    require(laterSequence.tracks[0].events.size() == 2,
            "SC packed SysEx event count is wrong");
    require(laterSequence.tracks[0].events[0].bytes[3] == 0x42,
            "SC packed SysEx model is wrong");

    const std::filesystem::path resetOutput =
        std::filesystem::temp_directory_path() /
        ("mpxadrv-reset-test-" + std::to_string(getpid()) + ".mid");
    mpxadrv::writeStandardMidi(resetSequence, resetOutput, "Reset test");
    std::ifstream resetInput(resetOutput, std::ios::binary);
    const std::vector<std::uint8_t> resetFile(
        (std::istreambuf_iterator<char>(resetInput)),
        std::istreambuf_iterator<char>());
    std::filesystem::remove(resetOutput, error);
    const std::vector<std::uint8_t> encodedMt32Reset = {
        0xf0, 0x0a, 0x41, 0x10, 0x16, 0x12,
        0x7f, 0x00, 0x00, 0x00, 0x01, 0xf7,
    };
    require(std::search(resetFile.begin(), resetFile.end(),
                        encodedMt32Reset.begin(), encodedMt32Reset.end()) !=
                resetFile.end(),
            "SMF SysEx encoding is wrong");

    int bank = -1;
    int preset = -1;
    mpxadrv::resolveSoftwareSynthPreset(127, 3, bank, preset);
    require(bank == 0 && preset == 4,
            "MT-32 map electric piano was not remapped to GS E.Piano 1");
    mpxadrv::resolveSoftwareSynthPreset(0, 65, bank, preset);
    require(bank == 0 && preset == 65,
            "ordinary GS bank/program must stay unchanged");
    mpxadrv::resolveSoftwareSynthPreset(8, 48, bank, preset);
    require(bank == 8 && preset == 48,
            "GS variation bank must stay unchanged");
    mpxadrv::resolveSoftwareSynthPreset(126, 10, bank, preset);
    require(bank == 0 && preset == 10,
            "CM-32P map should fall back to bank 0");
    require(mpxadrv::resolveRhythmProgram(57) == 0,
            "bogus PC 57 should fall back to Standard, not SFX");
    require(mpxadrv::resolveRhythmProgram(56) == 56,
            "0-based SFX drum kit must stay unchanged");
    require(mpxadrv::resolveRhythmProgram(17) == 16,
            "1-based Power drum kit was not remapped");
    require(mpxadrv::resolveRhythmProgram(0) == 0,
            "Standard drum kit must stay unchanged");
    require(mpxadrv::resolveRhythmProgram(3) == 0,
            "unknown drum program should fall back to Standard");

    std::cout << "MIDI conversion tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "MIDI conversion test failed: " << error.what() << '\n';
    return 1;
  }
}
