#pragma once

#include <filesystem>
#include <functional>

#include "midi.hpp"

namespace mpxadrv {

void playSoftwareSynth(const MidiSequence& sequence,
                       const std::filesystem::path& soundFont,
                       const std::function<bool()>& shouldStop);

}  // namespace mpxadrv
