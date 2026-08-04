#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mpxadrv {

class HttpError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

bool isHttpUrl(const std::string& value);

// Downloads a URL. When byteRange is set, sends a Range request for the
// inclusive [first, last] byte offsets.
std::vector<std::uint8_t> fetchUrl(
    const std::string& url,
    std::optional<std::pair<std::size_t, std::size_t>> byteRange = std::nullopt);

}  // namespace mpxadrv
