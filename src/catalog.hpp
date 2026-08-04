#pragma once

#include <string>
#include <vector>

namespace mpxadrv {

struct CatalogSong {
  std::string id;
  std::string title;
  std::string mdrUrl;
  std::string pdxUrl;
};

struct Catalog {
  int version = 1;
  std::vector<CatalogSong> songs;
};

class CatalogError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

Catalog parseCatalogJson(const std::string& json);
Catalog loadCatalog(const std::string& pathOrUrl);

}  // namespace mpxadrv
