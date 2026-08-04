#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mpxadrv {

class RemoteError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// True for http:// and https:// URLs (case-insensitive scheme).
bool isRemoteUrl(const std::string& value);

// Last path segment of the URL, without the query string or fragment.
// Empty when the URL ends with a slash.
std::string remoteUrlFilename(const std::string& url);

// URL of the directory containing the file, without a trailing slash and
// without the query string or fragment. "https://host/dir/song.mdr" becomes
// "https://host/dir".
std::string remoteUrlDirectory(const std::string& url);

// Single-quote escaping for safe use inside a /bin/sh command line.
std::string shellQuote(const std::string& value);

// Downloads the URL into memory with the system curl and returns the bytes.
// Nothing is written to disk. ~/.netrc and other curl configuration apply,
// so private hosts (tokens, basic auth) work without new options.
// Throws RemoteError on HTTP errors, unsupported schemes, timeouts, or when
// the response exceeds the built-in size limit.
std::vector<std::uint8_t> fetchUrl(const std::string& url);

}  // namespace mpxadrv
