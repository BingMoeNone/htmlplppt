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

// The size of every exported slide thumbnail. The BMP export and the list's
// image list share these values, so a thumbnail is never rescaled at draw time.
inline constexpr int kThumbnailWidth = 96;
inline constexpr int kThumbnailHeight = 72;

// One slide's exported thumbnail: the 1-based slide number and the UTF-8 path
// of its BMP. The export writes the file and the editor's list reads it back,
// so the path is the contract between the two.
struct SlideThumbnail {
  int number = 0;
  std::string image_path;
};

// One name per slide, in slide order, already joined to `dir`: slide-01.bmp,
// slide-02.bmp, ... A deck of 100+ slides widens the zero-padded index so the
// names still sort in slide order. A slide count of zero or less yields an
// empty list.
std::vector<std::string> ThumbnailFileNames(int slide_count,
                                           const std::filesystem::path& dir);
// The same list paired with each slide's 1-based number: the export uses the
// pair to know which file belongs to which slide.
std::vector<SlideThumbnail> SlideThumbnails(int slide_count,
                                            const std::filesystem::path& dir);

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
