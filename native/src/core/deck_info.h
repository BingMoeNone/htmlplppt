#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/config.h"

namespace htmlppt {

struct DeckSlideInfo {
  int index = 0;      // 1-based slide index
  std::string title;  // title placeholder text, may be empty
  std::string name;   // Slide.Name, may be empty
};

struct DeckInfo {
  std::filesystem::path path;
  std::vector<DeckSlideInfo> slides;  // sorted by index, contiguous 1..N
};

struct DeckInfoResult {
  std::optional<DeckInfo> info;
  std::vector<ConfigIssue> issues;
};

class DeckInfoProvider {
 public:
  virtual ~DeckInfoProvider() = default;
  virtual DeckInfoResult Load(const std::filesystem::path& deck_path) = 0;
};

// Diagnostic provider used on Linux/WSL and in CI. The WPS COM provider is a
// later plan and implements the same interface.
class JsonDeckInfoProvider final : public DeckInfoProvider {
 public:
  explicit JsonDeckInfoProvider(std::filesystem::path info_path);
  DeckInfoResult Load(const std::filesystem::path& deck_path) override;

 private:
  std::filesystem::path info_path_;
};

// Expected document:
// { "slides": [ { "index": 1, "title": "Cover", "name": "Slide1" } ] }
DeckInfoResult LoadDeckInfoFromJson(std::string_view text,
                                    const std::filesystem::path& deck_path);
DeckInfoResult LoadDeckInfoFile(const std::filesystem::path& deck_path,
                                const std::filesystem::path& info_path);

// 16 lowercase hex characters derived from the canonical path, file size, and
// last write time. Returns an empty string when the deck cannot be stat'ed.
std::string ComputeDeckFingerprint(const std::filesystem::path& deck_path);

} // namespace htmlppt
