#include "remote.hpp"

#include <array>
#include <cstdio>
#include <string>

#include <sys/wait.h>

namespace mpxadrv {
namespace {

// MDR/MDX songs are well under 1 MiB and PDX banks under 2 MiB; the limit
// only guards against a misconfigured or hostile endpoint.
constexpr std::size_t kMaxRemoteBytes = 64 * 1024 * 1024;
constexpr int kCurlTimeoutSeconds = 60;

bool startsWithCaseInsensitive(const std::string& value, const char* prefix) {
  for (std::size_t index = 0; prefix[index] != '\0'; ++index) {
    if (index >= value.size()) {
      return false;
    }
    char c = value[index];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c + ('a' - 'A'));
    }
    if (c != prefix[index]) {
      return false;
    }
  }
  return true;
}

std::string stripUrlQuery(const std::string& url) {
  const std::size_t cut = url.find_first_of("?#");
  return cut == std::string::npos ? url : url.substr(0, cut);
}

}  // namespace

bool isRemoteUrl(const std::string& value) {
  return startsWithCaseInsensitive(value, "http://") ||
         startsWithCaseInsensitive(value, "https://");
}

std::string remoteUrlFilename(const std::string& url) {
  const std::string clean = stripUrlQuery(url);
  const std::size_t slash = clean.find_last_of('/');
  if (slash == std::string::npos) {
    return clean;
  }
  return clean.substr(slash + 1);
}

std::string remoteUrlDirectory(const std::string& url) {
  const std::string clean = stripUrlQuery(url);
  const std::size_t slash = clean.find_last_of('/');
  if (slash == std::string::npos) {
    return clean;
  }
  return clean.substr(0, slash);
}

std::string shellQuote(const std::string& value) {
  std::string quoted = "'";
  for (const char c : value) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted += c;
    }
  }
  quoted += "'";
  return quoted;
}

std::vector<std::uint8_t> fetchUrl(const std::string& url) {
  if (!isRemoteUrl(url)) {
    throw RemoteError("only http(s) URLs can be streamed: " + url);
  }
  const std::string command =
      "curl -fsSL --max-time " + std::to_string(kCurlTimeoutSeconds) +
      " --proto =http,https --proto-redir =http,https -- " + shellQuote(url);

  FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    throw RemoteError("could not start curl to download " + url);
  }

  std::vector<std::uint8_t> data;
  std::array<char, 16'384> chunk{};
  bool tooLarge = false;
  while (true) {
    const std::size_t count = std::fread(chunk.data(), 1, chunk.size(), pipe);
    if (count > 0) {
      if (data.size() + count > kMaxRemoteBytes) {
        tooLarge = true;
        break;
      }
      data.insert(data.end(), chunk.data(), chunk.data() + count);
    }
    if (count < chunk.size()) {
      break;
    }
  }
  const int status = pclose(pipe);

  if (tooLarge) {
    throw RemoteError("download exceeds the 64 MiB limit: " + url);
  }
  if (status == -1) {
    throw RemoteError("could not finish the curl download of " + url);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    const int exitCode =
        WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status) + 128;
    throw RemoteError("download failed for " + url + " (curl exit " +
                      std::to_string(exitCode) +
                      "; check the URL and credentials)");
  }
  return data;
}

}  // namespace mpxadrv
