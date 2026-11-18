#include "core/mapping_wizard.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <string_view>
#include <utility>

namespace htmlppt {
namespace {

constexpr char kSeparators[] = "/\\";

std::string_view FileName(std::string_view path) {
  const std::size_t separator = path.find_last_of(kSeparators);
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

std::string LowerAscii(std::string_view text) {
  std::string lower;
  lower.reserve(text.size());
  for (const char value : text) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
  }
  return lower;
}

bool IsHtmlFile(const std::string& path) {
  const std::string_view name = FileName(path);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos) return false;
  const std::string extension = LowerAscii(name.substr(dot));
  return extension == ".html" || extension == ".htm";
}

// The trailing run of ASCII digits in the file stem (the name without its
// final extension), or -1 when the stem does not end in a number. A number too
// large for an int is treated as no match rather than wrapped.
int TrailingStemNumber(std::string_view path) {
  std::string_view stem = FileName(path);
  const std::size_t dot = stem.find_last_of('.');
  if (dot != std::string_view::npos) stem = stem.substr(0, dot);
  std::size_t first = stem.size();
  while (first > 0 &&
         std::isdigit(static_cast<unsigned char>(stem[first - 1])) != 0) {
    --first;
  }
  if (first == stem.size()) return -1;
  int number = 0;
  for (std::size_t index = first; index < stem.size(); ++index) {
    const int digit = stem[index] - '0';
    if (number > (std::numeric_limits<int>::max() - digit) / 10) return -1;
    number = number * 10 + digit;
  }
  return number;
}

std::vector<std::string> HtmlFiles(const std::vector<std::string>& files) {
  std::vector<std::string> html;
  for (const std::string& file : files) {
    if (IsHtmlFile(file)) html.push_back(file);
  }
  return html;
}

std::vector<WizardPair> ByFileName(const std::vector<WizardSlide>& slides,
                                   const std::vector<std::string>& html_files) {
  // First file wins per slide number: later files with the same number are
  // extra matches and stay unused.
  std::map<int, std::string> chosen;
  for (const std::string& file : html_files) {
    if (!IsHtmlFile(file)) continue;
    const int number = TrailingStemNumber(file);
    if (number < 1) continue;
    chosen.emplace(number, file);
  }
  // Emit in deck order, which is the order the preview table shows.
  std::vector<WizardPair> pairs;
  for (const WizardSlide& slide : slides) {
    const auto found = chosen.find(slide.number);
    if (found == chosen.end()) continue;
    WizardPair pair;
    pair.slide = slide.number;
    pair.title = slide.title;
    pair.html = found->second;
    pairs.push_back(std::move(pair));
    // A deck with a repeated slide number must not propose the same file twice.
    chosen.erase(found);
  }
  return pairs;
}

std::vector<WizardPair> ByFileOrder(const std::vector<WizardSlide>& slides,
                                    const std::vector<std::string>& html_files) {
  std::vector<std::string> files = HtmlFiles(html_files);
  std::sort(files.begin(), files.end());
  const std::size_t count = std::min(slides.size(), files.size());
  std::vector<WizardPair> pairs;
  pairs.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    WizardPair pair;
    pair.slide = slides[index].number;
    pair.title = slides[index].title;
    pair.html = files[index];
    pairs.push_back(std::move(pair));
  }
  return pairs;
}

// Splits a directory string on both separators. `absolute` reports a leading
// separator and `is_drive` a leading "C:" segment, so roots of different kinds
// are never mixed into one bogus common prefix.
std::vector<std::string> DirectorySegments(std::string_view directory,
                                           bool* absolute, bool* is_drive) {
  *absolute = false;
  *is_drive = false;
  std::size_t position = 0;
  while (position < directory.size() &&
         (directory[position] == '/' || directory[position] == '\\')) {
    *absolute = true;
    ++position;
  }
  std::vector<std::string> segments;
  std::string segment;
  for (; position < directory.size(); ++position) {
    const char value = directory[position];
    if (value == '/' || value == '\\') {
      if (!segment.empty()) segments.push_back(std::move(segment));
      segment.clear();
    } else {
      segment.push_back(value);
    }
  }
  if (!segment.empty()) segments.push_back(std::move(segment));
  if (!segments.empty()) {
    const std::string& first = segments.front();
    *is_drive = first.size() == 2 &&
                std::isalpha(static_cast<unsigned char>(first[0])) != 0 &&
                first[1] == ':';
  }
  return segments;
}

std::string JoinSegments(const std::vector<std::string>& segments,
                         std::size_t first) {
  std::string joined;
  for (std::size_t index = first; index < segments.size(); ++index) {
    if (!joined.empty()) joined.push_back('/');
    joined += segments[index];
  }
  return joined;
}

} // namespace

std::vector<WizardPair> ProposePairs(const std::vector<WizardSlide>& slides,
                                     const std::vector<std::string>& html_files,
                                     PairingMode mode) {
  switch (mode) {
    case PairingMode::ByFileName:
      return ByFileName(slides, html_files);
    case PairingMode::ByFileOrder:
      return ByFileOrder(slides, html_files);
  }
  return {};
}

std::string SuggestHtmlRoot(const std::vector<std::string>& html_files) {
  if (html_files.empty()) return {};
  std::vector<std::string> directories;
  directories.reserve(html_files.size());
  for (const std::string& file : html_files) {
    const std::size_t separator = file.find_last_of(kSeparators);
    directories.push_back(separator == std::string::npos
                              ? std::string{}
                              : file.substr(0, separator));
  }
  bool absolute = false;
  bool is_drive = false;
  std::vector<std::string> common =
      DirectorySegments(directories.front(), &absolute, &is_drive);
  for (std::size_t index = 1; index < directories.size(); ++index) {
    bool other_absolute = false;
    bool other_drive = false;
    const std::vector<std::string> segments =
        DirectorySegments(directories[index], &other_absolute, &other_drive);
    // A relative path, a rooted path, and a drive path never share a root.
    if (other_absolute != absolute || other_drive != is_drive) return {};
    std::size_t same = 0;
    const std::size_t limit = std::min(common.size(), segments.size());
    while (same < limit && common[same] == segments[same]) ++same;
    common.resize(same);
    if (common.empty()) return {};
  }
  if (common.empty()) return {};
  const std::string rest = JoinSegments(common, is_drive ? 1 : 0);
  if (is_drive) {
    return rest.empty() ? common.front() + "/" : common.front() + "/" + rest;
  }
  if (absolute) return rest.empty() ? "/" : "/" + rest;
  return rest;
}

} // namespace htmlppt
