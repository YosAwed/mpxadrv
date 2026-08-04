#include "http.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>

namespace mpxadrv {
namespace {

bool hasCaseInsensitivePrefix(const std::string& value, const char* prefix) {
  const std::size_t length = std::strlen(prefix);
  if (value.size() < length) {
    return false;
  }
  for (std::size_t index = 0; index < length; ++index) {
    if (std::tolower(static_cast<unsigned char>(value[index])) !=
        std::tolower(static_cast<unsigned char>(prefix[index]))) {
      return false;
    }
  }
  return true;
}

std::string shellEscape(const std::string& value) {
  std::string escaped = "'";
  for (char character : value) {
    if (character == '\'') {
      escaped += "'\\''";
    } else {
      escaped.push_back(character);
    }
  }
  escaped.push_back('\'');
  return escaped;
}

}  // namespace

bool isHttpUrl(const std::string& value) {
  return hasCaseInsensitivePrefix(value, "http://") ||
         hasCaseInsensitivePrefix(value, "https://");
}

std::vector<std::uint8_t> fetchUrl(
    const std::string& url,
    std::optional<std::pair<std::size_t, std::size_t>> byteRange) {
  // Use system curl for reliable HTTPS without NSURLSession main-thread/
  // Objective-C runtime edge cases in this CLI.
  std::ostringstream command;
  command << "/usr/bin/curl --silent --show-error --fail --location "
             "--max-time 120";
  if (byteRange) {
    command << " --range " << byteRange->first << '-' << byteRange->second;
  }
  command << ' ' << shellEscape(url);

  FILE* pipe = popen(command.str().c_str(), "r");
  if (pipe == nullptr) {
    throw HttpError("failed to start curl for " + url);
  }

  std::vector<std::uint8_t> bytes;
  std::array<char, 16'384> buffer{};
  while (true) {
    const std::size_t read =
        std::fread(buffer.data(), 1, buffer.size(), pipe);
    if (read == 0) {
      break;
    }
    bytes.insert(bytes.end(), buffer.begin(),
                 buffer.begin() + static_cast<std::ptrdiff_t>(read));
  }
  const int status = pclose(pipe);
  if (status != 0) {
    throw HttpError("HTTP fetch failed for " + url + " (curl exit " +
                    std::to_string(status) + ")");
  }
  if (bytes.empty()) {
    throw HttpError("empty HTTP response for " + url);
  }
  return bytes;
}

}  // namespace mpxadrv
