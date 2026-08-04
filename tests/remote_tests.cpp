#include "remote.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename Fetch>
void requireRemoteError(Fetch&& fetch, const char* message) {
  bool rejected = false;
  try {
    static_cast<void>(fetch());
  } catch (const mpxadrv::RemoteError&) {
    rejected = true;
  }
  require(rejected, message);
}

}  // namespace

int main() {
  try {
    require(mpxadrv::isRemoteUrl("https://example.com/song.mdr"),
            "https URL was not detected");
    require(mpxadrv::isRemoteUrl("http://192.168.0.2:8080/a.mdr"),
            "http URL was not detected");
    require(mpxadrv::isRemoteUrl("HTTPS://EXAMPLE.COM/X.MDX"),
            "uppercase URL scheme was not detected");
    require(!mpxadrv::isRemoteUrl("song.mdr"),
            "plain file name was detected as a URL");
    require(!mpxadrv::isRemoteUrl("/music/https://song.mdr"),
            "local path was detected as a URL");
    require(!mpxadrv::isRemoteUrl("ftp://example.com/song.mdr"),
            "ftp URL was accepted");
    require(!mpxadrv::isRemoteUrl("file:///tmp/song.mdr"),
            "file URL was accepted");

    require(mpxadrv::remoteUrlFilename("https://example.com/dir/song.mdr") ==
                "song.mdr",
            "URL file name was not extracted");
    require(mpxadrv::remoteUrlFilename(
                "https://example.com/dir/song.mdr?X-Amz-Signature=abc&b=1") ==
                "song.mdr",
            "presigned URL query leaked into the file name");
    require(mpxadrv::remoteUrlFilename("https://example.com/dir/song.mdr#f") ==
                "song.mdr",
            "URL fragment leaked into the file name");
    require(mpxadrv::remoteUrlFilename("https://example.com/dir/").empty(),
            "directory URL produced a file name");

    require(mpxadrv::remoteUrlDirectory("https://example.com/dir/song.mdr") ==
                "https://example.com/dir",
            "URL directory was not extracted");
    require(mpxadrv::remoteUrlDirectory("https://example.com/song.mdr#frag") ==
                "https://example.com",
            "URL fragment leaked into the directory");
    require(mpxadrv::remoteUrlDirectory(
                "https://example.com/a/b/c.pdx?sig=1") ==
                "https://example.com/a/b",
            "nested URL directory was not extracted");

    require(mpxadrv::shellQuote("plain") == "'plain'",
            "plain shell quoting is wrong");
    require(mpxadrv::shellQuote("a'b") == "'a'\\''b'",
            "single-quote escaping is wrong");
    require(mpxadrv::shellQuote("").empty() == false &&
                mpxadrv::shellQuote("") == "''",
            "empty shell quoting is wrong");
    require(mpxadrv::shellQuote("https://h/x y.mdr?a=1&b=2") ==
                "'https://h/x y.mdr?a=1&b=2'",
            "URL with spaces and ampersands was not quoted");

    // Unsupported schemes must be rejected before touching the network.
    requireRemoteError([] { return mpxadrv::fetchUrl("file:///etc/hosts"); },
                       "file:// download was not rejected");
    requireRemoteError(
        [] { return mpxadrv::fetchUrl("ftp://example.com/x.mdr"); },
        "ftp:// download was not rejected");

    // Optional live check: MPXADRV_TEST_URL points at a small known file.
    if (const char* live = std::getenv("MPXADRV_TEST_URL")) {
      const std::vector<std::uint8_t> data = mpxadrv::fetchUrl(live);
      require(!data.empty(), "live download returned no data");
      std::cout << "live download: " << data.size() << " bytes\n";
    }

    std::cout << "remote URL tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "remote URL test failed: " << error.what() << '\n';
    return 1;
  }
}
