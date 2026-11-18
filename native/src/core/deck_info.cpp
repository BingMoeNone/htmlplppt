#include "core/deck_info.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/config_issue.h"
#include "core/utf8_path.h"

namespace htmlppt {
namespace {

using json = nlohmann::json;

std::optional<int> ReadIndexValue(const json& value, const std::string& path,
                                  std::vector<ConfigIssue>& issues) {
  if (!value.is_number_integer()) {
    AddError(issues, path, "must be an integer");
    return std::nullopt;
  }
  if (value.is_number_unsigned()) {
    const auto raw = value.get<std::uint64_t>();
    if (raw > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      AddError(issues, path, "integer is out of 32-bit range");
      return std::nullopt;
    }
    return static_cast<int>(raw);
  }
  const auto raw = value.get<std::int64_t>();
  if (raw < std::numeric_limits<int>::min() ||
      raw > std::numeric_limits<int>::max()) {
    AddError(issues, path, "integer is out of 32-bit range");
    return std::nullopt;
  }
  return static_cast<int>(raw);
}

} // namespace

std::vector<std::string> ThumbnailFileNames(int slide_count,
                                           const std::filesystem::path& dir) {
  std::vector<std::string> names;
  if (slide_count <= 0) return names;
  // At least two digits, so slide-01..slide-09 sort naturally; a deck that
  // reaches three digits widens every name together instead of splitting the
  // ordering ("slide-100.bmp" must not sort before "slide-99.bmp").
  int width = 2;
  for (std::int64_t limit = 100; slide_count >= limit; limit *= 10) ++width;
  names.reserve(static_cast<std::size_t>(slide_count));
  for (int number = 1; number <= slide_count; ++number) {
    std::string index = std::to_string(number);
    if (static_cast<int>(index.size()) < width) {
      index.insert(0, static_cast<std::size_t>(width) - index.size(), '0');
    }
    names.push_back(Utf8Path(dir / ("slide-" + index + ".bmp")));
  }
  return names;
}

std::vector<SlideThumbnail> SlideThumbnails(int slide_count,
                                            const std::filesystem::path& dir) {
  const std::vector<std::string> names = ThumbnailFileNames(slide_count, dir);
  std::vector<SlideThumbnail> thumbnails;
  thumbnails.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    SlideThumbnail thumbnail;
    thumbnail.number = static_cast<int>(i) + 1;
    thumbnail.image_path = names[i];
    thumbnails.push_back(std::move(thumbnail));
  }
  return thumbnails;
}

std::string ComputeDeckFingerprint(const std::filesystem::path& deck_path) {
  if (deck_path.empty()) return {};
  std::error_code error;
  const auto size = std::filesystem::file_size(deck_path, error);
  if (error) return {};
  const auto write_time = std::filesystem::last_write_time(deck_path, error);
  if (error) return {};
  auto canonical = std::filesystem::weakly_canonical(deck_path, error);
  if (error) {
    error.clear();
    canonical = std::filesystem::absolute(deck_path, error);
    if (error) canonical = deck_path;
  }
  const std::string seed =
      canonical.string() + "|" + std::to_string(size) + "|" +
      std::to_string(static_cast<std::int64_t>(
          write_time.time_since_epoch().count()));
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char character : seed) {
    hash ^= character;
    hash *= 1099511628211ULL;
  }
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setw(16) << std::setfill('0')
      << hash;
  return out.str();
}

DeckInfoResult LoadDeckInfoFromJson(std::string_view text,
                                    const std::filesystem::path& deck_path) {
  DeckInfoResult result;
  json root;
  try {
    root = json::parse(text.begin(), text.end());
  } catch (const json::parse_error& error) {
    AddError(result.issues, "$", std::string("JSON parse error: ") + error.what());
    return result;
  }
  if (!root.is_object()) {
    AddError(result.issues, "$", "root must be an object");
    return result;
  }
  bool has_error = false;
  for (auto it = root.begin(); it != root.end(); ++it) {
    if (it.key() != "slides") {
      AddError(result.issues, it.key(), "unknown field");
      has_error = true;
    }
  }
  const auto slides = root.find("slides");
  if (slides == root.end()) {
    AddError(result.issues, "slides", "required array is missing");
    return result;
  }
  if (!slides->is_array()) {
    AddError(result.issues, "slides", "must be an array");
    return result;
  }
  if (slides->empty()) {
    AddError(result.issues, "slides", "must not be empty");
    return result;
  }

  DeckInfo info;
  info.path = deck_path;
  std::set<int> seen;
  for (std::size_t i = 0; i < slides->size(); ++i) {
    const auto& item = (*slides)[i];
    const std::string path = "slides[" + std::to_string(i) + "]";
    if (!item.is_object()) {
      AddError(result.issues, path, "must be an object");
      has_error = true;
      continue;
    }
    for (auto it = item.begin(); it != item.end(); ++it) {
      if (it.key() != "index" && it.key() != "title" && it.key() != "name") {
        AddError(result.issues, path + "." + it.key(), "unknown field");
        has_error = true;
      }
    }
    DeckSlideInfo slide;
    const auto index = item.find("index");
    if (index == item.end()) {
      AddError(result.issues, path + ".index", "required integer is missing");
      has_error = true;
      continue;
    }
    const auto parsed_index =
        ReadIndexValue(*index, path + ".index", result.issues);
    if (!parsed_index.has_value()) {
      has_error = true;
      continue;
    }
    slide.index = *parsed_index;
    if (slide.index < 1) {
      AddError(result.issues, path + ".index", "must be >= 1");
      has_error = true;
      continue;
    }
    if (!seen.insert(slide.index).second) {
      AddError(result.issues, path + ".index", "duplicate slide index");
      has_error = true;
      continue;
    }
    if (const auto title = item.find("title"); title != item.end()) {
      if (!title->is_string()) {
        AddError(result.issues, path + ".title", "must be a string");
        has_error = true;
      } else {
        slide.title = title->get<std::string>();
      }
    }
    if (const auto name = item.find("name"); name != item.end()) {
      if (!name->is_string()) {
        AddError(result.issues, path + ".name", "must be a string");
        has_error = true;
      } else {
        slide.name = name->get<std::string>();
      }
    }
    info.slides.push_back(std::move(slide));
  }
  if (has_error) return result;

  std::sort(info.slides.begin(), info.slides.end(),
            [](const DeckSlideInfo& left, const DeckSlideInfo& right) {
              return left.index < right.index;
            });
  for (std::size_t i = 0; i < info.slides.size(); ++i) {
    if (info.slides[i].index != static_cast<int>(i) + 1) {
      AddError(result.issues, "slides",
               "slide indices must be contiguous starting at 1 (missing index " +
                   std::to_string(i + 1) + ")");
      return result;
    }
  }
  result.info = std::move(info);
  return result;
}

DeckInfoResult LoadDeckInfoFile(const std::filesystem::path& deck_path,
                                const std::filesystem::path& info_path) {
  DeckInfoResult result;
  std::ifstream input(info_path, std::ios::binary);
  if (!input) {
    AddError(result.issues, "$",
             "cannot open deck info file: " + info_path.string());
    return result;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return LoadDeckInfoFromJson(buffer.str(), deck_path);
}

JsonDeckInfoProvider::JsonDeckInfoProvider(std::filesystem::path info_path)
    : info_path_(std::move(info_path)) {}

DeckInfoResult JsonDeckInfoProvider::Load(
    const std::filesystem::path& deck_path) {
  return LoadDeckInfoFile(deck_path, info_path_);
}

} // namespace htmlppt
