#include "catalog.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

}  // namespace

int main() {
  try {
    const char* json = R"({
      "version": 1,
      "songs": [
        {
          "id": "alpha",
          "title": "Alpha Song",
          "mdr": "https://example.test/alpha.mdr"
        },
        {
          "id": "beta",
          "title": "Beta Song",
          "mdr": "https://example.test/beta.mdr",
          "pdx": "https://example.test/bank.pdx"
        }
      ]
    })";

    const mpxadrv::Catalog catalog = mpxadrv::parseCatalogJson(json);
    require(catalog.songs.size() == 2, "catalog song count is wrong");
    require(catalog.songs[0].id == "alpha", "first song id is wrong");
    require(catalog.songs[0].title == "Alpha Song", "first song title is wrong");
    require(catalog.songs[0].mdrUrl == "https://example.test/alpha.mdr",
            "first song mdr URL is wrong");
    require(catalog.songs[0].pdxUrl.empty(), "first song pdx should be empty");
    require(catalog.songs[1].pdxUrl == "https://example.test/bank.pdx",
            "second song pdx URL is wrong");

    const char* tabby = R"({
      "version": 1,
      "songs": [
        {
          "id": "cam",
          "title": "CAMMY\tCHERRY",
          "mdr": "https://example.test/CAM_SC.MDR"
        }
      ]
    })";
    const mpxadrv::Catalog sanitized = mpxadrv::parseCatalogJson(tabby);
    require(sanitized.songs.size() == 1, "tab title catalog size wrong");
    require(sanitized.songs[0].title == "CAMMY CHERRY",
            "embedded TAB in title was not sanitized");

    bool rejected = false;
    try {
      static_cast<void>(mpxadrv::parseCatalogJson("{\"tracks\":[]}"));
    } catch (const mpxadrv::CatalogError&) {
      rejected = true;
    }
    require(rejected, "catalog without songs array was accepted");

    std::cout << "Catalog loader tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Catalog loader test failed: " << error.what() << '\n';
    return 1;
  }
}
