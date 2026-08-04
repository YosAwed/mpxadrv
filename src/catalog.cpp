#include "catalog.hpp"

#include "http.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace mpxadrv {
namespace {

void skipWhitespace(const std::string& json, std::size_t& position) {
  while (position < json.size() &&
         std::isspace(static_cast<unsigned char>(json[position]))) {
    ++position;
  }
}

bool matchLiteral(const std::string& json, std::size_t& position,
                  const char* literal) {
  const std::size_t length = std::strlen(literal);
  if (position + length > json.size() ||
      json.compare(position, length, literal) != 0) {
    return false;
  }
  position += length;
  return true;
}

std::string parseJsonString(const std::string& json, std::size_t& position) {
  skipWhitespace(json, position);
  if (position >= json.size() || json[position] != '"') {
    throw CatalogError("expected JSON string");
  }
  ++position;
  std::string result;
  while (position < json.size()) {
    const char character = json[position++];
    if (character == '"') {
      return result;
    }
    if (character == '\\') {
      if (position >= json.size()) {
        throw CatalogError("truncated JSON escape");
      }
      const char escaped = json[position++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          result.push_back(escaped);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        default:
          throw CatalogError("unsupported JSON escape");
      }
      continue;
    }
    result.push_back(character);
  }
  throw CatalogError("unterminated JSON string");
}

std::string readLocalFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw CatalogError("catalog file not found: " + path);
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::string loadCatalogText(const std::string& pathOrUrl) {
  if (isHttpUrl(pathOrUrl)) {
    const std::vector<std::uint8_t> bytes = fetchUrl(pathOrUrl);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return readLocalFile(pathOrUrl);
}

std::size_t findSongsArray(const std::string& json) {
  const std::string key = "\"songs\"";
  const std::size_t keyPosition = json.find(key);
  if (keyPosition == std::string::npos) {
    throw CatalogError("catalog JSON is missing a songs array");
  }
  std::size_t position = keyPosition + key.size();
  skipWhitespace(json, position);
  if (!matchLiteral(json, position, ":")) {
    throw CatalogError("catalog JSON is missing a songs array");
  }
  skipWhitespace(json, position);
  if (!matchLiteral(json, position, "[")) {
    throw CatalogError("catalog JSON is missing a songs array");
  }
  return position;
}

std::optional<std::string> readObjectString(const std::string& object,
                                            const char* key) {
  const std::string quoted = std::string("\"") + key + "\"";
  const std::size_t keyPosition = object.find(quoted);
  if (keyPosition == std::string::npos) {
    return std::nullopt;
  }
  std::size_t position = keyPosition + quoted.size();
  skipWhitespace(object, position);
  if (!matchLiteral(object, position, ":")) {
    return std::nullopt;
  }
  return parseJsonString(object, position);
}

CatalogSong parseSongObject(const std::string& object) {
  CatalogSong song;
  if (const std::optional<std::string> id = readObjectString(object, "id")) {
    song.id = *id;
  }
  if (const std::optional<std::string> title =
          readObjectString(object, "title")) {
    song.title = *title;
  }
  if (const std::optional<std::string> mdr = readObjectString(object, "mdr")) {
    song.mdrUrl = *mdr;
  }
  if (const std::optional<std::string> pdx = readObjectString(object, "pdx")) {
    song.pdxUrl = *pdx;
  }
  if (song.mdrUrl.empty()) {
    throw CatalogError("catalog song is missing an mdr URL");
  }
  if (song.title.empty()) {
    song.title = song.id.empty() ? song.mdrUrl : song.id;
  }
  return song;
}

std::optional<std::string> extractNextObject(const std::string& json,
                                             std::size_t& position) {
  skipWhitespace(json, position);
  if (position >= json.size()) {
    return std::nullopt;
  }
  if (json[position] == ']') {
    return std::nullopt;
  }
  if (!matchLiteral(json, position, "{")) {
    throw CatalogError("invalid catalog song entry");
  }
  const std::size_t start = position - 1;
  int depth = 1;
  bool inString = false;
  bool escaped = false;
  while (position < json.size() && depth > 0) {
    const char character = json[position++];
    if (inString) {
      if (escaped) {
        escaped = false;
        continue;
      }
      if (character == '\\') {
        escaped = true;
        continue;
      }
      if (character == '"') {
        inString = false;
      }
      continue;
    }
    if (character == '"') {
      inString = true;
      continue;
    }
    if (character == '{') {
      ++depth;
    } else if (character == '}') {
      --depth;
    }
  }
  if (depth != 0) {
    throw CatalogError("unterminated catalog song object");
  }
  return json.substr(start, position - start);
}

}  // namespace

Catalog parseCatalogJson(const std::string& json) {
  Catalog catalog;
  std::size_t position = findSongsArray(json);
  while (true) {
    const std::optional<std::string> object =
        extractNextObject(json, position);
    if (!object) {
      break;
    }
    catalog.songs.push_back(parseSongObject(*object));
    skipWhitespace(json, position);
    if (position < json.size() && json[position] == ',') {
      ++position;
      continue;
    }
    skipWhitespace(json, position);
    if (position < json.size() && json[position] == ']') {
      break;
    }
  }
  if (catalog.songs.empty()) {
    throw CatalogError("catalog does not contain any songs");
  }
  return catalog;
}

Catalog loadCatalog(const std::string& pathOrUrl) {
  return parseCatalogJson(loadCatalogText(pathOrUrl));
}

}  // namespace mpxadrv
