#pragma once

#include <functional>
#include <string>
#include <vector>

#include "midi.hpp"

namespace mpxadrv {

std::vector<std::string> midiDestinationNames();

void playMidiSequence(const MidiSequence& sequence,
                      const std::string& destinationSelector,
                      const std::function<bool()>& shouldStop);

}  // namespace mpxadrv
