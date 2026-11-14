# C++ Watchdog Resolve and CLI Implementation Plan (Plan 2B)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the platform-neutral deck-info provider, target resolution (slide index and title/name), HTML path/query handling, deterministic auto stage and Edge-profile names, and the `--validate` / `--resolve` / `--print-config` / `--print-schema` CLI.

**Architecture:** `core/deck_info` defines `DeckInfoProvider` plus a diagnostic JSON provider and the deck fingerprint used for auto names. `core/resolve` consumes the Plan 2A `Config` model and resolves every enabled target to a concrete slide, filesystem path, stage name, and Edge profile, reporting field-path issues. `app/cli` parses arguments, loads the config and optional deck info, and formats the diagnostic commands. No Windows headers or COM calls are introduced; the WPS provider plugs into the same interface in a later plan.

**Tech Stack:** C++20, CMake/CTest, nlohmann/json 3.11.3, the Plan 1 `htmlppt_core` library and test harness, the Plan 2A config model.

## Plan Split

This document is Plan 2B, the second half of the approved Plan 2 configuration work. Plan 2A delivered the typed config model, validation, legacy migration, profiles, canonical serialization, and the embedded JSON Schema. Plan 2B consumes that model; it does not change config parsing semantics except for the shared issue-helper refactor in Task 1.

Later plans own the WPS COM `DeckInfoProvider` implementation, the state machine, staging, the watchdog runtime, and the config UI.

## Global Constraints

- Target Windows 10/11 x64; Plan 2B must build and test on Linux/WSL.
- C++20; MSVC `/W4 /permissive- /utf-8`; GCC/Clang `-Wall -Wextra -Wpedantic`.
- No third-party runtime dependencies beyond the vendored nlohmann/json.
- All new code lives under `native/`; do not modify existing scripts, root tests, or production docs.
- Project-authored source must remain ASCII-only. Configuration and deck-info content is UTF-8 at runtime.
- `core/` must not include Windows-specific headers. No COM or Win32 calls in this plan.
- Field paths in issues are root-relative, matching Plan 2A: `targets[0].html`, `deck`, `slides[1].index`. Only JSON parse errors and non-object roots use `$`.
- Existing tests (57 unit cases, CTest 3/3) must stay green.
- `npm test` is not modified by this plan. In an environment with Chrome it stays 3/3; if Chrome is absent, only the existing contract smoke test fails with `Chrome did not start`, which is an environment limitation.

## File Structure

Create:

- `native/src/core/config_issue.h` - shared issue helpers used by config, deck-info, and resolve code.
- `native/src/core/deck_info.h` - deck-info model, provider interface, JSON provider, fingerprint.
- `native/src/core/deck_info.cpp` - JSON parsing, validation, file provider, FNV-1a fingerprint.
- `native/src/core/resolve.h` - resolve options, resolved target model, public entry points.
- `native/src/core/resolve.cpp` - slide/title resolution, path containment, query merge, auto names, conflicts, table formatting.
- `native/src/app/cli.h` - CLI entry point used by `main.cpp`.
- `native/src/app/cli.cpp` - argument parser and diagnostic commands.
- `native/tests/unit/deck_info_test.cpp` - deck-info and fingerprint unit tests.
- `native/tests/unit/resolve_test.cpp` - resolution unit tests.
- `native/tests/fixtures/watcher.json` - valid CLI fixture.
- `native/tests/fixtures/watcher-missing-html.json` - failing CLI fixture.
- `native/tests/fixtures/deck-info.json` - valid deck-info fixture.
- `native/tests/fixtures/deck.pptx` - dummy deck file for fingerprint and existence checks.
- `native/tests/fixtures/page1.html`, `native/tests/fixtures/page2.html` - fixture HTML files.

Modify:

- `native/src/core/config.cpp` - remove the four local issue helpers and include `core/config_issue.h`.
- `native/src/app/main.cpp` - delegate to `RunCli`.
- `native/CMakeLists.txt` - add `src/app/cli.cpp` and the four new CTest cases.
- `native/README.md` - document the diagnostic CLI and deck-info JSON.

---

### Task 1: Shared Issue Helpers and Deck Info

**Files:**
- Create: `native/src/core/config_issue.h`
- Create: `native/src/core/deck_info.h`
- Create: `native/src/core/deck_info.cpp`
- Create: `native/tests/unit/deck_info_test.cpp`
- Modify: `native/src/core/config.cpp`

**Interfaces:**
- Consumes: `ConfigIssue` and `IssueSeverity` from `core/config.h`; nlohmann/json.
- Produces:
  - `void AddIssue(std::vector<ConfigIssue>&, IssueSeverity, std::string path, std::string message)`
  - `void AddError(std::vector<ConfigIssue>&, std::string path, std::string message)`
  - `void AddWarning(std::vector<ConfigIssue>&, std::string path, std::string message)`
  - `bool HasErrorIssue(const std::vector<ConfigIssue>&)`
  - `struct DeckSlideInfo { int index; std::string title; std::string name; }`
  - `struct DeckInfo { std::filesystem::path path; std::vector<DeckSlideInfo> slides; }`
  - `struct DeckInfoResult { std::optional<DeckInfo> info; std::vector<ConfigIssue> issues; }`
  - `class DeckInfoProvider { virtual DeckInfoResult Load(const std::filesystem::path&) = 0; }`
  - `class JsonDeckInfoProvider final : public DeckInfoProvider`
  - `DeckInfoResult LoadDeckInfoFromJson(std::string_view text, const std::filesystem::path& deck_path)`
  - `DeckInfoResult LoadDeckInfoFile(const std::filesystem::path& deck_path, const std::filesystem::path& info_path)`
  - `std::string ComputeDeckFingerprint(const std::filesystem::path& deck_path)`

- [ ] **Step 1: Write the failing deck-info tests**

Create `native/tests/unit/deck_info_test.cpp`:

```cpp
#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/deck_info.h"

using htmlppt::DeckInfoResult;
using htmlppt::IssueSeverity;

namespace {

DeckInfoResult ParseDeckInfo(std::string_view text) {
  return htmlppt::LoadDeckInfoFromJson(text, std::filesystem::path{"deck.pptx"});
}

bool HasError(const DeckInfoResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasIssueAt(const DeckInfoResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(deck_info_parses_valid_json) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 2, "title": "Second", "name": "Slide2" },
      { "index": 1, "title": "First", "name": "Slide1" }
    ]
  })");
  HTMLPPT_CHECK(result.info.has_value());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.info->slides.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.info->slides[0].index, 1);
  HTMLPPT_CHECK_EQ(result.info->slides[0].title, std::string{"First"});
  HTMLPPT_CHECK_EQ(result.info->slides[1].index, 2);
  HTMLPPT_CHECK_EQ(result.info->slides[1].name, std::string{"Slide2"});
}

HTMLPPT_TEST_CASE(deck_info_requires_contiguous_indices) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 1, "title": "One" },
      { "index": 3, "title": "Three" }
    ]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_duplicate_index) {
  const auto result = ParseDeckInfo(R"({
    "slides": [
      { "index": 1, "title": "One" },
      { "index": 1, "title": "Again" }
    ]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides[1].index"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_missing_slides) {
  const auto result = ParseDeckInfo(R"({ "version": 1 })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_empty_slides) {
  const auto result = ParseDeckInfo(R"({ "slides": [] })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_unknown_fields) {
  const auto result = ParseDeckInfo(R"({
    "slides": [{ "index": 1, "title": "One", "mystery": true }]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "slides[0].mystery"));
}

HTMLPPT_TEST_CASE(deck_info_rejects_unknown_root_field) {
  const auto result = ParseDeckInfo(R"({
    "mystery": true,
    "slides": [{ "index": 1, "title": "One" }]
  })");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "mystery"));
}

HTMLPPT_TEST_CASE(deck_info_reports_parse_error) {
  const auto result = ParseDeckInfo(R"({ "slides": [)");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "$"));
}

HTMLPPT_TEST_CASE(deck_info_provider_reads_file) {
  htmlppt::test::TempDir dir;
  const auto info_path = dir.path() / "deck-info.json";
  {
    std::ofstream out(info_path, std::ios::binary);
    out << R"({ "slides": [{ "index": 1, "title": "One" }] })";
  }
  htmlppt::JsonDeckInfoProvider provider(info_path);
  const auto result = provider.Load(dir.path() / "deck.pptx");
  HTMLPPT_CHECK(result.info.has_value());
  HTMLPPT_CHECK_EQ(result.info->slides.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.info->slides[0].title, std::string{"One"});
}

HTMLPPT_TEST_CASE(deck_info_provider_reports_missing_file) {
  htmlppt::test::TempDir dir;
  htmlppt::JsonDeckInfoProvider provider(dir.path() / "missing.json");
  const auto result = provider.Load(dir.path() / "deck.pptx");
  HTMLPPT_CHECK(!result.info.has_value());
  HTMLPPT_CHECK(HasIssueAt(result, "$"));
}

HTMLPPT_TEST_CASE(deck_fingerprint_is_stable_and_content_sensitive) {
  htmlppt::test::TempDir dir;
  const auto first = dir.path() / "deck.pptx";
  const auto second = dir.path() / "other.pptx";
  {
    std::ofstream out(first, std::ios::binary);
    out << "PK-one";
  }
  {
    std::ofstream out(second, std::ios::binary);
    out << "PK-two-longer";
  }
  const auto first_hash = htmlppt::ComputeDeckFingerprint(first);
  HTMLPPT_CHECK_EQ(first_hash.size(), std::size_t{16});
  HTMLPPT_CHECK_EQ(first_hash, htmlppt::ComputeDeckFingerprint(first));
  HTMLPPT_CHECK(first_hash != htmlppt::ComputeDeckFingerprint(second));
  HTMLPPT_CHECK(htmlppt::ComputeDeckFingerprint(dir.path() / "missing.pptx").empty());
}
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
```

Expected: compilation fails because `core/deck_info.h` does not exist.

- [ ] **Step 3: Add the shared issue header and refactor config.cpp**

Create `native/src/core/config_issue.h`:

```cpp
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/config.h"

namespace htmlppt {

inline void AddIssue(std::vector<ConfigIssue>& issues, IssueSeverity severity,
                     std::string path, std::string message) {
  issues.push_back({severity, std::move(path), std::move(message)});
}

inline void AddError(std::vector<ConfigIssue>& issues, std::string path,
                     std::string message) {
  AddIssue(issues, IssueSeverity::Error, std::move(path), std::move(message));
}

inline void AddWarning(std::vector<ConfigIssue>& issues, std::string path,
                       std::string message) {
  AddIssue(issues, IssueSeverity::Warning, std::move(path), std::move(message));
}

inline bool HasErrorIssue(const std::vector<ConfigIssue>& issues) {
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

} // namespace htmlppt
```

In `native/src/core/config.cpp`, add the include next to `core/config.h`:

```cpp
#include "core/config_issue.h"
```

Then delete these four local definitions from the anonymous namespace:

```cpp
void AddIssue(std::vector<ConfigIssue>& issues, IssueSeverity severity,
              std::string path, std::string message) {
  issues.push_back({severity, std::move(path), std::move(message)});
}

void AddError(std::vector<ConfigIssue>& issues, std::string path, std::string message) {
  AddIssue(issues, IssueSeverity::Error, std::move(path), std::move(message));
}

void AddWarning(std::vector<ConfigIssue>& issues, std::string path,
                std::string message) {
  AddIssue(issues, IssueSeverity::Warning, std::move(path), std::move(message));
}

bool HasErrorIssue(const std::vector<ConfigIssue>& issues) {
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}
```

No other changes are needed: unqualified calls now resolve to the inline helpers in `htmlppt`.

- [ ] **Step 4: Add the deck-info header and implementation**

Create `native/src/core/deck_info.h`:

```cpp
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
```

Create `native/src/core/deck_info.cpp`:

```cpp
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
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 68/68 unit cases pass, CTest 3/3, no warnings.

- [ ] **Step 6: Commit**

```bash
git add native/src/core/config_issue.h native/src/core/config.cpp \
  native/src/core/deck_info.h native/src/core/deck_info.cpp \
  native/tests/unit/deck_info_test.cpp
git commit -m "feat: add deck info provider and fingerprint"
```

---

### Task 2: Target Resolution

**Files:**
- Create: `native/src/core/resolve.h`
- Create: `native/src/core/resolve.cpp`
- Create: `native/tests/unit/resolve_test.cpp`

**Interfaces:**
- Consumes: `Config`, `TargetConfig`, `MatchConfig`, `AssetsConfig`, `ConfigIssue`; `DeckInfo`; `ComputeDeckFingerprint`.
- Produces:
  - `std::filesystem::path ConfigBaseDir(const Config&)`
  - `std::filesystem::path ResolveDeckPath(const Config&)`
  - `struct ResolveOptions { bool allow_outside_folder = false; std::string deck_fingerprint; }`
  - `enum class ResolveState { Resolved, Disabled }`
  - `struct ResolvedTarget { int ordinal; ResolveState state; int slide; std::string label; std::string html_path; std::string query; std::string fragment; std::string stage_name; std::string edge_profile; }`
  - `struct ResolveResult { std::vector<ResolvedTarget> targets; std::vector<ConfigIssue> issues; }`
  - `ResolveResult ResolveTargets(const Config&, const DeckInfo*, const ResolveOptions& = {})`
  - `std::string ResolveTableToText(const ResolveResult&)`

- [ ] **Step 1: Write the failing resolution tests**

Create `native/tests/unit/resolve_test.cpp`:

```cpp
#include "test_main.h"
#include "test_utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "core/config.h"
#include "core/deck_info.h"
#include "core/resolve.h"

using htmlppt::Config;
using htmlppt::IssueSeverity;
using htmlppt::ResolveResult;
using htmlppt::ResolveState;

namespace {

struct Fixture {
  htmlppt::test::TempDir dir;

  Fixture() {
    Write("deck.pptx", "PK");
    Write("page1.html", "<html>1</html>");
    Write("page2.html", "<html>2</html>");
  }

  void Write(const std::filesystem::path& relative,
             const std::string& content) const {
    const auto path = dir.path() / relative;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
  }

  std::filesystem::path ConfigPath() const { return dir.path() / "watcher.json"; }
};

Config ParseConfig(const Fixture& fixture, std::string_view text) {
  const auto result = htmlppt::LoadConfigFromString(text, fixture.ConfigPath(), {});
  HTMLPPT_CHECK(result.config.has_value());
  return *result.config;
}

htmlppt::DeckInfo MakeDeck() {
  htmlppt::DeckInfo deck;
  deck.path = "deck.pptx";
  deck.slides = {
      {1, "First", "Slide1"},
      {2, "Second", "Slide2"},
      {3, "", "NamedSlide"},
  };
  return deck;
}

htmlppt::ResolveOptions DefaultOptions() {
  htmlppt::ResolveOptions options;
  options.deck_fingerprint = "0123456789abcdef";
  return options;
}

bool HasError(const ResolveResult& result) {
  for (const auto& issue : result.issues) {
    if (issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasErrorAt(const ResolveResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path && issue.severity == IssueSeverity::Error) return true;
  }
  return false;
}

bool HasWarningAt(const ResolveResult& result, std::string_view path) {
  for (const auto& issue : result.issues) {
    if (issue.path == path && issue.severity == IssueSeverity::Warning) return true;
  }
  return false;
}

} // namespace

HTMLPPT_TEST_CASE(resolve_slide_target_with_deck_info) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 2, "html": "page1.html", "label": "Second" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
  HTMLPPT_CHECK_EQ(result.targets[0].label, std::string{"Second"});
  HTMLPPT_CHECK_EQ(result.targets[0].stage_name,
                   std::string{"target-01-page1.html"});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile,
                   std::string{"htmlppt-0123456789abcdef-t01"});
  HTMLPPT_CHECK_EQ(result.targets[0].html_path,
                   std::filesystem::weakly_canonical(
                       fixture.dir.path() / "page1.html").string());
  HTMLPPT_CHECK(result.targets[0].state == ResolveState::Resolved);
}

HTMLPPT_TEST_CASE(resolve_slide_out_of_range) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 9, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].slide"));
}

HTMLPPT_TEST_CASE(resolve_slide_without_deck_info_warns) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 2, "html": "page1.html" }]
  })");
  const auto result = htmlppt::ResolveTargets(config, nullptr, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK(HasWarningAt(result, "targets[0].slide"));
}

HTMLPPT_TEST_CASE(resolve_title_exact) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Second", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
  HTMLPPT_CHECK_EQ(result.targets[0].label, std::string{"Slide 2"});
}

HTMLPPT_TEST_CASE(resolve_title_prefix) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Sec", "mode": "prefix" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
}

HTMLPPT_TEST_CASE(resolve_title_regex) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "^Sec.nd$", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 2);
}

HTMLPPT_TEST_CASE(resolve_title_requires_deck_info) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Second", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto result = htmlppt::ResolveTargets(config, nullptr, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match"));
}

HTMLPPT_TEST_CASE(resolve_title_no_match) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "Missing", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.slideTitle"));
}

HTMLPPT_TEST_CASE(resolve_title_multiple_matches) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": ".*", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.slideTitle"));
}

HTMLPPT_TEST_CASE(resolve_slide_name_exact) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideName": "NamedSlide", "mode": "exact" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].slide, 3);
}

HTMLPPT_TEST_CASE(resolve_invalid_regex) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "match": { "slideTitle": "[", "mode": "regex" },
      "html": "page2.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].match.mode"));
}

HTMLPPT_TEST_CASE(resolve_missing_html) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "missing.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].html"));
}

HTMLPPT_TEST_CASE(resolve_outside_folder_rejected) {
  Fixture fixture;
  fixture.Write("config/deck.pptx", "PK");
  fixture.Write("outside.html", "<html>outside</html>");
  const auto parsed = htmlppt::LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "../outside.html" }]
  })", fixture.dir.path() / "config" / "watcher.json", {});
  HTMLPPT_CHECK(parsed.config.has_value());
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(*parsed.config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].html"));
}

HTMLPPT_TEST_CASE(resolve_outside_folder_allowed) {
  Fixture fixture;
  fixture.Write("config/deck.pptx", "PK");
  fixture.Write("outside.html", "<html>outside</html>");
  const auto parsed = htmlppt::LoadConfigFromString(R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "../outside.html" }]
  })", fixture.dir.path() / "config" / "watcher.json", {});
  HTMLPPT_CHECK(parsed.config.has_value());
  auto options = DefaultOptions();
  options.allow_outside_folder = true;
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(*parsed.config, &deck, options);
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(resolve_absolute_html_inside_folder) {
  Fixture fixture;
  const std::string html =
      (fixture.dir.path() / "page1.html").generic_string();
  const std::string text =
      R"({"version":1,"deck":"deck.pptx","targets":[{"slide":1,"html":")" +
      html + R"("}]})";
  const Config config = ParseConfig(fixture, text);
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
}

HTMLPPT_TEST_CASE(resolve_html_query_merges) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      {
        "slide": 1,
        "html": "page1.html?a=1",
        "htmlQuery": "?b=2#frag"
      },
      {
        "slide": 2,
        "html": "page2.html",
        "htmlQuery": "c=3"
      }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.targets[0].query, std::string{"a=1&b=2"});
  HTMLPPT_CHECK_EQ(result.targets[0].fragment, std::string{"frag"});
  HTMLPPT_CHECK_EQ(result.targets[1].query, std::string{"c=3"});
  HTMLPPT_CHECK_EQ(result.targets[1].fragment, std::string{""});
}

HTMLPPT_TEST_CASE(resolve_html_query_duplicate_keys_allowed) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      {
        "slide": 1,
        "html": "page1.html?a=1&a=2",
        "htmlQuery": "?b=1&b=2"
      },
      {
        "slide": 2,
        "html": "page2.html",
        "htmlQuery": "?c=1&c=2"
      }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(result.targets[0].query, std::string{"a=1&a=2&b=1&b=2"});
  HTMLPPT_CHECK_EQ(result.targets[1].query, std::string{"c=1&c=2"});
}

HTMLPPT_TEST_CASE(resolve_html_query_conflict) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html?a=1",
      "htmlQuery": "?a=2"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].htmlQuery"));
}

HTMLPPT_TEST_CASE(resolve_fragment_conflict) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html#one",
      "htmlQuery": "#two"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].htmlQuery"));
}

HTMLPPT_TEST_CASE(resolve_explicit_stage_name) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "stageName": "custom.html",
      "edgeProfile": "custom-profile"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets[0].stage_name, std::string{"custom.html"});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile,
                   std::string{"custom-profile"});
}

HTMLPPT_TEST_CASE(resolve_rejects_stage_name_with_separator) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "stageName": "a/b.html"
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].stageName"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_slide) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "one.html",
        "edgeProfile": "profile-one" },
      { "slide": 1, "html": "page2.html", "stageName": "two.html",
        "edgeProfile": "profile-two" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].slide"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_stage_name) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "same.html",
        "edgeProfile": "profile-one" },
      { "slide": 2, "html": "page2.html", "stageName": "same.html",
        "edgeProfile": "profile-two" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].stageName"));
}

HTMLPPT_TEST_CASE(resolve_duplicate_edge_profile) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html", "stageName": "one.html",
        "edgeProfile": "dup" },
      { "slide": 2, "html": "page2.html", "stageName": "two.html",
        "edgeProfile": "dup" }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[1].edgeProfile"));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{1});
  HTMLPPT_CHECK_EQ(result.targets[0].edge_profile, std::string{"dup"});
}

HTMLPPT_TEST_CASE(resolve_disabled_target_is_listed) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [
      { "slide": 1, "html": "page1.html" },
      { "slide": 2, "html": "page2.html", "enabled": false }
    ]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK_EQ(result.targets.size(), std::size_t{2});
  HTMLPPT_CHECK(result.targets[1].state == ResolveState::Disabled);
  HTMLPPT_CHECK_EQ(result.targets[1].slide, 0);
  HTMLPPT_CHECK(result.targets[1].html_path.empty());
}

HTMLPPT_TEST_CASE(resolve_missing_assets_root) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{
      "slide": 1,
      "html": "page1.html",
      "assets": { "mode": "tree", "root": "assets" }
    }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "targets[0].assets.root"));
}

HTMLPPT_TEST_CASE(resolve_zero_targets_warns) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": []
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(!HasError(result));
  HTMLPPT_CHECK(HasWarningAt(result, "targets"));
}

HTMLPPT_TEST_CASE(resolve_missing_deck_errors) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "missing.pptx",
    "targets": [{ "slide": 1, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  HTMLPPT_CHECK(HasErrorAt(result, "deck"));
}

HTMLPPT_TEST_CASE(resolve_table_contains_rows) {
  Fixture fixture;
  const Config config = ParseConfig(fixture, R"({
    "version": 1,
    "deck": "deck.pptx",
    "targets": [{ "slide": 1, "html": "page1.html" }]
  })");
  const auto deck = MakeDeck();
  const auto result = htmlppt::ResolveTargets(config, &deck, DefaultOptions());
  const std::string table = htmlppt::ResolveTableToText(result);
  HTMLPPT_CHECK(table.find("SLIDE") != std::string::npos);
  HTMLPPT_CHECK(table.find("page1.html") != std::string::npos);
  HTMLPPT_CHECK(table.find("resolved") != std::string::npos);
}
```

- [ ] **Step 2: Run the tests and verify they fail**

```bash
cmake --build native/build --parallel
```

Expected: compilation fails because `core/resolve.h` does not exist.

- [ ] **Step 3: Add the resolution header and implementation**

Create `native/src/core/resolve.h`:

```cpp
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/deck_info.h"

namespace htmlppt {

struct ResolveOptions {
  bool allow_outside_folder = false;
  // When empty, ResolveTargets computes the fingerprint of the deck path.
  std::string deck_fingerprint;
};

enum class ResolveState {
  Resolved,
  Disabled,
};

struct ResolvedTarget {
  int ordinal = 0;  // 1-based position in Config::active.targets
  ResolveState state = ResolveState::Resolved;
  int slide = 0;  // 0 when the target is disabled
  std::string label;
  std::string html_path;  // absolute normalized filesystem path, no query/hash
  std::string query;      // merged query without the leading '?'
  std::string fragment;   // merged fragment without the leading '#'
  std::string stage_name;
  std::string edge_profile;
};

struct ResolveResult {
  std::vector<ResolvedTarget> targets;  // includes disabled targets
  std::vector<ConfigIssue> issues;
};

std::filesystem::path ConfigBaseDir(const Config& config);
std::filesystem::path ResolveDeckPath(const Config& config);
ResolveResult ResolveTargets(const Config& config, const DeckInfo* deck,
                             const ResolveOptions& options = {});
std::string ResolveTableToText(const ResolveResult& result);

} // namespace htmlppt
```

Create `native/src/core/resolve.cpp`:

```cpp
#include "core/resolve.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "core/config_issue.h"

namespace htmlppt {
namespace {

std::filesystem::path NormalizePath(const std::filesystem::path& path) {
  std::error_code error;
  auto normalized = std::filesystem::weakly_canonical(path, error);
  if (!error) return normalized;
  error.clear();
  auto absolute = std::filesystem::absolute(path, error);
  if (!error) return absolute.lexically_normal();
  return path;
}

bool IsWithin(const std::filesystem::path& base,
              const std::filesystem::path& candidate) {
  auto base_it = base.begin();
  auto candidate_it = candidate.begin();
  for (; base_it != base.end(); ++base_it, ++candidate_it) {
    if (candidate_it == candidate.end()) return false;
    if (*base_it != *candidate_it) return false;
  }
  return true;
}

struct HtmlReference {
  std::string path;
  std::string query;
  std::string fragment;
};

HtmlReference SplitHtmlReference(const std::string& html) {
  HtmlReference result;
  std::string remainder = html;
  if (const auto hash = remainder.find('#'); hash != std::string::npos) {
    result.fragment = remainder.substr(hash + 1);
    remainder.erase(hash);
  }
  if (const auto question = remainder.find('?'); question != std::string::npos) {
    result.query = remainder.substr(question + 1);
    remainder.erase(question);
  }
  result.path = remainder;
  return result;
}

struct QueryFragment {
  std::string query;
  std::string fragment;
};

QueryFragment ParseExtraReference(const std::string& text) {
  QueryFragment result;
  std::string remainder = text;
  if (!remainder.empty() && remainder.front() == '?') remainder.erase(0, 1);
  if (const auto hash = remainder.find('#'); hash != std::string::npos) {
    result.fragment = remainder.substr(hash + 1);
    remainder.erase(hash);
  }
  result.query = remainder;
  return result;
}

std::vector<std::string> SplitQuery(const std::string& query) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto end = query.find('&', start);
    const auto part =
        query.substr(start, end == std::string::npos ? std::string::npos
                                                     : end - start);
    if (!part.empty()) parts.push_back(part);
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return parts;
}

std::string QueryKey(const std::string& part) {
  const auto equals = part.find('=');
  return equals == std::string::npos ? part : part.substr(0, equals);
}

bool MergeQuery(const std::string& first, const std::string& second,
                std::string* merged, std::string* conflict) {
  std::vector<std::string> parts = SplitQuery(first);
  std::map<std::string, std::set<std::string>> first_values;
  for (const auto& part : parts) {
    first_values[QueryKey(part)].insert(part);
  }
  std::set<std::string> seen(parts.begin(), parts.end());
  for (const auto& part : SplitQuery(second)) {
    const auto key = QueryKey(part);
    const auto found = first_values.find(key);
    if (found != first_values.end() && found->second.count(part) == 0) {
      *conflict = key;
      return false;
    }
    if (seen.insert(part).second) parts.push_back(part);
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out << '&';
    out << parts[i];
  }
  *merged = out.str();
  return true;
}

std::string AsciiSlug(const std::string& text) {
  std::string slug;
  for (unsigned char character : text) {
    if (character >= 'A' && character <= 'Z') {
      slug.push_back(static_cast<char>(character - 'A' + 'a'));
    } else if ((character >= 'a' && character <= 'z') ||
               (character >= '0' && character <= '9')) {
      slug.push_back(static_cast<char>(character));
    } else if (!slug.empty() && slug.back() != '-') {
      slug.push_back('-');
    }
  }
  while (!slug.empty() && slug.back() == '-') slug.pop_back();
  return slug.empty() ? "page" : slug;
}

std::string AutoStageName(int ordinal, const std::string& html_path) {
  const std::string stem = std::filesystem::path(html_path).stem().string();
  std::ostringstream out;
  out << "target-" << std::setw(2) << std::setfill('0') << ordinal << "-"
      << AsciiSlug(stem) << ".html";
  return out.str();
}

std::string AutoEdgeProfile(const std::string& fingerprint, int ordinal) {
  std::ostringstream out;
  out << "htmlppt-" << (fingerprint.empty() ? "nodeck" : fingerprint) << "-t"
      << std::setw(2) << std::setfill('0') << ordinal;
  return out.str();
}

bool ValidateStageName(const std::string& value, const std::string& target_path,
                       std::vector<ConfigIssue>& issues) {
  if (value.empty()) return true;
  if (value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos) {
    AddError(issues, target_path + ".stageName",
             "must not contain path separators");
    return false;
  }
  if (value == "." || value == "..") {
    AddError(issues, target_path + ".stageName", "must be a file name");
    return false;
  }
  for (unsigned char character : value) {
    if (character > 0x7F) {
      AddError(issues, target_path + ".stageName", "must be ASCII");
      return false;
    }
  }
  return true;
}

bool ValidateEdgeProfile(const std::string& value,
                         const std::string& target_path,
                         std::vector<ConfigIssue>& issues) {
  if (value.empty()) {
    AddError(issues, target_path + ".edgeProfile", "must not be empty");
    return false;
  }
  if (value.find('/') != std::string::npos ||
      value.find('\\') != std::string::npos) {
    AddError(issues, target_path + ".edgeProfile",
             "must not contain path separators");
    return false;
  }
  return true;
}

bool MatchesPattern(const std::string& value, const std::string& pattern,
                    const std::string& mode) {
  if (mode == "exact") return value == pattern;
  if (mode == "prefix") return value.rfind(pattern, 0) == 0;
  if (mode == "regex") {
    try {
      return std::regex_search(value, std::regex(pattern));
    } catch (const std::regex_error&) {
      return false;
    }
  }
  return false;
}

std::optional<int> ResolveSlideNumber(const TargetConfig& target,
                                      const DeckInfo* deck,
                                      const std::string& target_path,
                                      std::vector<ConfigIssue>& issues) {
  if (target.slide.has_value()) {
    if (deck != nullptr) {
      const int count = static_cast<int>(deck->slides.size());
      if (*target.slide > count) {
        AddError(issues, target_path + ".slide",
                 "slide " + std::to_string(*target.slide) +
                     " is out of range (deck has " + std::to_string(count) +
                     " slides)");
        return std::nullopt;
      }
    } else {
      AddWarning(issues, target_path + ".slide",
                 "deck info is unavailable; slide range was not validated");
    }
    return *target.slide;
  }
  if (!target.match.has_value()) {
    AddError(issues, target_path, "exactly one of slide or match is required");
    return std::nullopt;
  }
  if (deck == nullptr) {
    AddError(issues, target_path + ".match",
             "deck info is required to resolve a slide title or name");
    return std::nullopt;
  }
  const MatchConfig& match = *target.match;
  const bool use_title = !match.slide_title.empty();
  const std::string& pattern =
      use_title ? match.slide_title : match.slide_name;
  const std::string field = use_title ? "slideTitle" : "slideName";
  if (match.mode == "regex") {
    try {
      std::regex compiled(pattern);
      (void)compiled;
    } catch (const std::regex_error& error) {
      AddError(issues, target_path + ".match.mode",
               std::string("invalid regex: ") + error.what());
      return std::nullopt;
    }
  }
  std::vector<int> matches;
  for (const auto& slide : deck->slides) {
    const std::string& value = use_title ? slide.title : slide.name;
    if (MatchesPattern(value, pattern, match.mode)) {
      matches.push_back(slide.index);
    }
  }
  if (matches.empty()) {
    AddError(issues, target_path + ".match." + field,
             "no slide matches " + match.mode + " pattern '" + pattern + "'");
    return std::nullopt;
  }
  if (matches.size() > 1) {
    std::ostringstream list;
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (i != 0) list << ", ";
      list << matches[i];
    }
    AddError(issues, target_path + ".match." + field,
             "pattern matches multiple slides: " + list.str());
    return std::nullopt;
  }
  return matches.front();
}

bool ResolveHtml(const TargetConfig& target, const std::filesystem::path& base_dir,
                 bool allow_outside_folder, std::size_t index,
                 ResolvedTarget* resolved, std::vector<ConfigIssue>& issues) {
  const std::string target_path = "targets[" + std::to_string(index) + "]";
  const HtmlReference html = SplitHtmlReference(target.html);
  if (html.path.empty()) {
    AddError(issues, target_path + ".html", "HTML path must not be empty");
    return false;
  }
  std::filesystem::path html_path(html.path);
  if (html_path.is_relative()) html_path = base_dir / html_path;
  html_path = NormalizePath(html_path);
  if (!allow_outside_folder && !IsWithin(base_dir, html_path)) {
    AddError(issues, target_path + ".html",
             "HTML path is outside the config folder; use "
             "--allow-outside-folder to override");
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(html_path, error)) {
    AddError(issues, target_path + ".html",
             "HTML file does not exist: " + html_path.string());
    return false;
  }
  const QueryFragment extra = ParseExtraReference(target.html_query);
  std::string query;
  std::string conflict;
  if (!MergeQuery(html.query, extra.query, &query, &conflict)) {
    AddError(issues, target_path + ".htmlQuery",
             "query parameter conflicts with html: " + conflict);
    return false;
  }
  if (!html.fragment.empty() && !extra.fragment.empty() &&
      html.fragment != extra.fragment) {
    AddError(issues, target_path + ".htmlQuery",
             "fragment conflicts with html: " + extra.fragment);
    return false;
  }
  resolved->html_path = html_path.string();
  resolved->query = query;
  resolved->fragment =
      !html.fragment.empty() ? html.fragment : extra.fragment;
  return true;
}

bool ResolveAssets(const TargetConfig& target,
                   const std::filesystem::path& base_dir,
                   const std::filesystem::path& html_path, std::size_t index,
                   bool allow_outside_folder,
                   std::vector<ConfigIssue>& issues) {
  if (target.assets.mode != "tree") return true;
  const std::string target_path = "targets[" + std::to_string(index) + "]";
  std::filesystem::path root = target.assets.root.empty()
                                   ? html_path.parent_path()
                                   : std::filesystem::path(target.assets.root);
  if (root.is_relative()) root = base_dir / root;
  root = NormalizePath(root);
  if (!allow_outside_folder && !IsWithin(base_dir, root)) {
    AddError(issues, target_path + ".assets.root",
             "asset root is outside the config folder; use "
             "--allow-outside-folder to override");
    return false;
  }
  std::error_code error;
  if (!std::filesystem::is_directory(root, error)) {
    AddError(issues, target_path + ".assets.root",
             "asset root does not exist: " + root.string());
    return false;
  }
  return true;
}

ResolvedTarget DisabledTarget(int ordinal, const TargetConfig& target) {
  ResolvedTarget resolved;
  resolved.ordinal = ordinal;
  resolved.state = ResolveState::Disabled;
  resolved.label = target.label;
  return resolved;
}

} // namespace

std::filesystem::path ConfigBaseDir(const Config& config) {
  std::error_code error;
  if (config.source_path.empty()) {
    return std::filesystem::current_path(error).lexically_normal();
  }
  const auto parent = config.source_path.parent_path();
  if (parent.empty()) {
    return std::filesystem::current_path(error).lexically_normal();
  }
  return NormalizePath(parent);
}

std::filesystem::path ResolveDeckPath(const Config& config) {
  if (config.active.deck.empty()) return {};
  std::filesystem::path deck = config.active.deck;
  if (deck.is_relative()) deck = ConfigBaseDir(config) / deck;
  return NormalizePath(deck);
}

ResolveResult ResolveTargets(const Config& config, const DeckInfo* deck,
                             const ResolveOptions& options) {
  ResolveResult result;
  const auto base_dir = ConfigBaseDir(config);
  const auto deck_path = ResolveDeckPath(config);
  if (config.active.deck.empty()) {
    AddError(result.issues, "deck", "must not be empty");
  } else {
    std::error_code error;
    if (!std::filesystem::is_regular_file(deck_path, error)) {
      AddError(result.issues, "deck",
               "deck file does not exist: " + deck_path.string());
    }
  }
  if (config.active.targets.empty()) {
    AddWarning(result.issues, "targets", "no targets are configured");
  }

  std::string fingerprint = options.deck_fingerprint;
  if (fingerprint.empty() && !deck_path.empty()) {
    fingerprint = ComputeDeckFingerprint(deck_path);
  }
  if (fingerprint.empty()) {
    for (const auto& target : config.active.targets) {
      if (target.enabled && target.edge_profile.empty()) {
        AddWarning(result.issues, "deck",
                   "deck fingerprint is unavailable; auto Edge profiles are "
                   "not deck-specific");
        break;
      }
    }
  }

  std::map<int, int> slide_owner;
  std::map<std::string, int> stage_owner;
  std::map<std::string, int> profile_owner;

  for (std::size_t i = 0; i < config.active.targets.size(); ++i) {
    const TargetConfig& target = config.active.targets[i];
    const int ordinal = static_cast<int>(i) + 1;
    const std::string target_path = "targets[" + std::to_string(i) + "]";
    if (!target.enabled) {
      result.targets.push_back(DisabledTarget(ordinal, target));
      continue;
    }

    const auto slide =
        ResolveSlideNumber(target, deck, target_path, result.issues);
    if (!slide.has_value()) continue;

    ResolvedTarget resolved;
    resolved.ordinal = ordinal;
    resolved.slide = *slide;
    resolved.label = target.label.empty() ? "Slide " + std::to_string(*slide)
                                          : target.label;
    if (!ResolveHtml(target, base_dir, options.allow_outside_folder, i,
                     &resolved, result.issues)) {
      continue;
    }
    if (!ResolveAssets(target, base_dir, resolved.html_path, i,
                       options.allow_outside_folder, result.issues)) {
      continue;
    }

    const std::string stage = target.stage_name.empty()
                                  ? AutoStageName(ordinal, resolved.html_path)
                                  : target.stage_name;
    if (!ValidateStageName(stage, target_path, result.issues)) continue;
    const std::string profile =
        target.edge_profile.empty() ? AutoEdgeProfile(fingerprint, ordinal)
                                    : target.edge_profile;
    if (!ValidateEdgeProfile(profile, target_path, result.issues)) continue;

    if (const auto owner = slide_owner.find(*slide); owner != slide_owner.end()) {
      AddError(result.issues,
               target_path + (target.slide.has_value() ? ".slide" : ".match"),
               "slide " + std::to_string(*slide) +
                   " is already mapped by target " +
                   std::to_string(owner->second));
      continue;
    }
    if (const auto owner = stage_owner.find(stage); owner != stage_owner.end()) {
      AddError(result.issues, target_path + ".stageName",
               "duplicate stage name '" + stage + "' (target " +
                   std::to_string(owner->second) + ")");
      continue;
    }
    if (const auto owner = profile_owner.find(profile);
        owner != profile_owner.end()) {
      AddError(result.issues, target_path + ".edgeProfile",
               "duplicate Edge profile '" + profile + "' (target " +
                   std::to_string(owner->second) + ")");
      continue;
    }

    slide_owner.emplace(*slide, ordinal);
    stage_owner.emplace(stage, ordinal);
    profile_owner.emplace(profile, ordinal);
    resolved.stage_name = stage;
    resolved.edge_profile = profile;
    resolved.state = ResolveState::Resolved;
    result.targets.push_back(std::move(resolved));
  }
  return result;
}

std::string ResolveTableToText(const ResolveResult& result) {
  struct Row {
    std::string ordinal;
    std::string slide;
    std::string label;
    std::string html;
    std::string stage;
    std::string profile;
    std::string state;
  };
  std::vector<Row> rows;
  rows.push_back({"#", "SLIDE", "LABEL", "HTML", "STAGE", "EDGE-PROFILE",
                  "STATE"});
  for (const auto& target : result.targets) {
    rows.push_back(
        {std::to_string(target.ordinal),
         target.state == ResolveState::Disabled ? "-"
                                                : std::to_string(target.slide),
         target.label.empty() ? "-" : target.label,
         target.html_path.empty() ? "-" : target.html_path,
         target.stage_name.empty() ? "-" : target.stage_name,
         target.edge_profile.empty() ? "-" : target.edge_profile,
         target.state == ResolveState::Disabled ? "disabled" : "resolved"});
  }
  std::vector<std::size_t> widths(7, 0);
  for (const auto& row : rows) {
    const std::array<std::string, 7> cells = {
        row.ordinal, row.slide, row.label, row.html,
        row.stage,   row.profile, row.state};
    for (std::size_t i = 0; i < cells.size(); ++i) {
      widths[i] = std::max(widths[i], cells[i].size());
    }
  }
  std::ostringstream out;
  for (const auto& row : rows) {
    const std::array<std::string, 7> cells = {
        row.ordinal, row.slide, row.label, row.html,
        row.stage,   row.profile, row.state};
    for (std::size_t i = 0; i < cells.size(); ++i) {
      out << std::left << std::setw(static_cast<int>(widths[i])) << cells[i];
      if (i + 1 != cells.size()) out << "  ";
    }
    out << '\n';
  }
  return out.str();
}

} // namespace htmlppt
```

- [ ] **Step 4: Build and run the tests**

```bash
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
```

Expected: 97/97 unit cases pass, CTest 3/3, no warnings.

- [ ] **Step 5: Commit**

```bash
git add native/src/core/resolve.h native/src/core/resolve.cpp \
  native/tests/unit/resolve_test.cpp
git commit -m "feat: resolve slide targets and HTML references"
```

---

### Task 3: Diagnostic CLI and CTest Coverage

**Files:**
- Create: `native/src/app/cli.h`
- Create: `native/src/app/cli.cpp`
- Create: `native/tests/fixtures/watcher.json`
- Create: `native/tests/fixtures/watcher-missing-html.json`
- Create: `native/tests/fixtures/deck-info.json`
- Create: `native/tests/fixtures/deck.pptx`
- Create: `native/tests/fixtures/page1.html`
- Create: `native/tests/fixtures/page2.html`
- Create: `native/tests/cmake/expect_validation_failure.cmake`
- Create: `native/tests/cmake/cli_contract.cmake`
- Modify: `native/src/app/main.cpp`
- Modify: `native/CMakeLists.txt`

**Interfaces:**
- Consumes: `LoadConfigFile`, `ConfigToJson`, `ConfigSchema`, `JsonDeckInfoProvider`, `ResolveTargets`, `ResolveTableToText`, `ConfigBaseDir`, `ResolveDeckPath`, `HasErrorIssue`.
- Produces:
  - `int RunCli(int argc, char** argv)`
  - Commands: `--version`, `--help`, `--validate`, `--resolve`, `--print-config`, `--print-schema`
  - Options: `--config`, `--profile`, `--deck-info`, `--stage-mode`, `--state-dir`, `--scheduling`, `--map`, `--allow-outside-folder`

- [ ] **Step 1: Add the CLI fixtures**

Create `native/tests/fixtures/watcher.json`:

```json
{
  "version": 1,
  "deck": "deck.pptx",
  "targets": [
    { "slide": 1, "html": "page1.html", "label": "First" },
    {
      "match": { "slideTitle": "Second", "mode": "exact" },
      "html": "page2.html"
    }
  ]
}
```

Create `native/tests/fixtures/watcher-missing-html.json`:

```json
{
  "version": 1,
  "deck": "deck.pptx",
  "targets": [
    { "slide": 1, "html": "missing.html" }
  ]
}
```

Create `native/tests/fixtures/deck-info.json`:

```json
{
  "slides": [
    { "index": 1, "title": "First", "name": "Slide1" },
    { "index": 2, "title": "Second", "name": "Slide2" }
  ]
}
```

Create `native/tests/fixtures/deck.pptx`:

```text
PK
```

Create `native/tests/fixtures/page1.html`:

```html
<!doctype html>
<html><body>page1</body></html>
```

Create `native/tests/fixtures/page2.html`:

```html
<!doctype html>
<html><body>page2</body></html>
```

- [ ] **Step 2: Add the failing CTest cases**

Create `native/tests/cmake/expect_validation_failure.cmake`:

```cmake
if(NOT DEFINED EXE OR NOT DEFINED CONFIG OR NOT DEFINED DECK_INFO OR
   NOT DEFINED EXPECT)
  message(FATAL_ERROR "EXE, CONFIG, DECK_INFO, and EXPECT are required")
endif()

execute_process(
  COMMAND "${EXE}" --validate --config "${CONFIG}" --deck-info "${DECK_INFO}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error)

if(NOT result EQUAL 1)
  message(FATAL_ERROR "expected exit code 1, got '${result}'\n${output}${error}")
endif()

string(FIND "${output}${error}" "${EXPECT}" found)
if(found EQUAL -1)
  message(FATAL_ERROR "expected output to contain '${EXPECT}'\n${output}${error}")
endif()
```

This wrapper asserts both the exact exit code and the diagnostic text. CTest's
`WILL_FAIL` inverts `PASS_REGULAR_EXPRESSION` on CMake 4.x, so the wrapper is
used instead of combining those two properties.

Create `native/tests/cmake/cli_contract.cmake`:

```cmake
if(NOT DEFINED EXE OR NOT DEFINED FIXTURES)
  message(FATAL_ERROR "EXE and FIXTURES are required")
endif()

function(expect_result NAME EXPECTED_EXIT EXPECTED_TEXT)
  execute_process(
    COMMAND "${EXE}" ${ARGN}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)
  if(NOT result EQUAL EXPECTED_EXIT)
    message(FATAL_ERROR
      "${NAME}: expected exit ${EXPECTED_EXIT}, got '${result}'\n${output}${error}")
  endif()
  string(FIND "${output}${error}" "${EXPECTED_TEXT}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR
      "${NAME}: expected output to contain '${EXPECTED_TEXT}'\n${output}${error}")
  endif()
endfunction()

expect_result(print_config 0 "\"version\": 1"
  --print-config --config "${FIXTURES}/watcher.json")
expect_result(map_append 0 "\"slide\": 3"
  --print-config --config "${FIXTURES}/watcher.json" --map 3=page2.html)
expect_result(unknown_option 2 "unknown option" --bogus)
expect_result(invalid_stage_mode 2 "must be file, tree, or direct"
  --validate --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --stage-mode bogus)
expect_result(invalid_scheduling 2 "must be adaptive, aggressive, balanced, or safe"
  --validate --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --scheduling turbo)
expect_result(resolve_ok 0 "page2.html"
  --resolve --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json")
expect_result(validate_missing_html 1 "HTML file does not exist"
  --validate --config "${FIXTURES}/watcher-missing-html.json"
  --deck-info "${FIXTURES}/deck-info.json")
expect_result(map_duplicate 1 "already mapped by target"
  --resolve --config "${FIXTURES}/watcher.json"
  --deck-info "${FIXTURES}/deck-info.json" --map 1=page2.html)
```

In `native/CMakeLists.txt`, change the executable target to include `cli.cpp`:

```cmake
add_executable(htmlppt-watchdog src/app/main.cpp src/app/cli.cpp)
```

Inside the `if (HTMLPPT_BUILD_TESTS)` block, after the existing `cli_help` test, add:

```cmake
  set(HTMLPPT_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  add_test(NAME cli_print_schema COMMAND htmlppt-watchdog --print-schema)
  set_tests_properties(cli_print_schema PROPERTIES
    PASS_REGULAR_EXPRESSION "htmlppt watchdog config")

  add_test(NAME cli_validate_ok COMMAND htmlppt-watchdog --validate
    --config "${HTMLPPT_FIXTURES}/watcher.json"
    --deck-info "${HTMLPPT_FIXTURES}/deck-info.json")
  set_tests_properties(cli_validate_ok PROPERTIES
    PASS_REGULAR_EXPRESSION "OK: configuration is valid")

  add_test(NAME cli_resolve_ok COMMAND htmlppt-watchdog --resolve
    --config "${HTMLPPT_FIXTURES}/watcher.json"
    --deck-info "${HTMLPPT_FIXTURES}/deck-info.json")
  set_tests_properties(cli_resolve_ok PROPERTIES
    PASS_REGULAR_EXPRESSION "page2.html")

  add_test(NAME cli_validate_missing_html
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:htmlppt-watchdog>"
      "-DCONFIG=${HTMLPPT_FIXTURES}/watcher-missing-html.json"
      "-DDECK_INFO=${HTMLPPT_FIXTURES}/deck-info.json"
      "-DEXPECT=HTML file does not exist"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/expect_validation_failure.cmake")

  add_test(NAME cli_contract
    COMMAND ${CMAKE_COMMAND}
      "-DEXE=$<TARGET_FILE:htmlppt-watchdog>"
      "-DFIXTURES=${HTMLPPT_FIXTURES}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/cli_contract.cmake")
```

- [ ] **Step 3: Run the tests and verify they fail**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: the build fails because `src/app/cli.cpp` and `app/cli.h` do not exist.

- [ ] **Step 4: Add the CLI header and implementation**

Create `native/src/app/cli.h`:

```cpp
#pragma once

namespace htmlppt {

int RunCli(int argc, char** argv);

} // namespace htmlppt
```

Create `native/src/app/cli.cpp`:

```cpp
#include "app/cli.h"

#include <charconv>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/config_issue.h"
#include "core/config_schema.h"
#include "core/deck_info.h"
#include "core/resolve.h"
#include "core/version.h"

namespace htmlppt {
namespace {

enum class Command {
  Version,
  Help,
  Validate,
  Resolve,
  PrintConfig,
  PrintSchema,
};

struct CliOptions {
  Command command = Command::Version;
  bool command_set = false;
  std::string config_path;
  std::string deck_info_path;
  CliConfigOverrides overrides;
};

struct ParseResult {
  bool ok = true;
  std::string error;
  CliOptions options;
};

void PrintUsage(std::ostream& out) {
  out << "usage: htmlppt-watchdog [options]\n"
      << "\n"
      << "commands:\n"
      << "  --version              print version (default with no arguments)\n"
      << "  --help, -h             print this help\n"
      << "  --validate             validate the config and resolved targets\n"
      << "  --resolve              print the resolved slide-to-HTML mapping\n"
      << "  --print-config         print the effective config as JSON\n"
      << "  --print-schema         print the embedded config JSON Schema\n"
      << "\n"
      << "options:\n"
      << "  --config PATH          watcher JSON config file\n"
      << "  --profile NAME         select a config profile\n"
      << "  --deck-info PATH       diagnostic JSON deck info (slides/titles)\n"
      << "  --stage-mode MODE      file, tree, or direct\n"
      << "  --state-dir PATH       override the state directory\n"
      << "  --scheduling MODE      adaptive, aggressive, balanced, or safe\n"
      << "  --map SLIDE=HTML       add a target (repeatable; override is deferred)\n"
      << "  --allow-outside-folder allow HTML paths outside the config folder\n";
}

bool TakeValue(int argc, char** argv, int* index, std::string_view option,
               std::string* value, std::string* error) {
  if (*index + 1 >= argc) {
    *error = "missing value for " + std::string(option);
    return false;
  }
  ++*index;
  *value = argv[*index];
  return true;
}

bool IsOneOf(std::string_view value,
             std::initializer_list<std::string_view> allowed) {
  for (const auto candidate : allowed) {
    if (value == candidate) return true;
  }
  return false;
}

bool SetCommand(CliOptions* options, Command command, std::string* error) {
  if (options->command_set && options->command != command) {
    *error = "only one command may be specified";
    return false;
  }
  options->command = command;
  options->command_set = true;
  return true;
}

bool ParseMapValue(const std::string& value, CliOptions* options,
                   std::string* error) {
  const auto equals = value.find('=');
  if (equals == std::string::npos) {
    *error = "--map expects SLIDE=HTML";
    return false;
  }
  const std::string slide_text = value.substr(0, equals);
  const std::string html = value.substr(equals + 1);
  int slide = 0;
  const auto parsed =
      std::from_chars(slide_text.data(), slide_text.data() + slide_text.size(),
                      slide);
  if (slide_text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != slide_text.data() + slide_text.size()) {
    *error = "--map slide must be an integer: " + slide_text;
    return false;
  }
  if (slide < 1) {
    *error = "--map slide must be >= 1";
    return false;
  }
  if (html.empty()) {
    *error = "--map HTML must not be empty";
    return false;
  }
  if (!options->overrides.map_overrides.emplace(slide, html).second) {
    *error = "--map slide is specified more than once: " + slide_text;
    return false;
  }
  return true;
}

ParseResult ParseCli(int argc, char** argv) {
  ParseResult result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;
    if (argument == "--version") {
      if (!SetCommand(&result.options, Command::Version, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--help" || argument == "-h") {
      if (!SetCommand(&result.options, Command::Help, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--validate") {
      if (!SetCommand(&result.options, Command::Validate, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--resolve") {
      if (!SetCommand(&result.options, Command::Resolve, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--print-config") {
      if (!SetCommand(&result.options, Command::PrintConfig, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--print-schema") {
      if (!SetCommand(&result.options, Command::PrintSchema, &result.error)) {
        result.ok = false;
        return result;
      }
    } else if (argument == "--allow-outside-folder") {
      result.options.overrides.allow_outside_folder = true;
    } else if (argument == "--config") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.config_path = value;
    } else if (argument == "--profile") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.profile = value;
    } else if (argument == "--deck-info") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.deck_info_path = value;
    } else if (argument == "--stage-mode") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      if (!IsOneOf(value, {"file", "tree", "direct"})) {
        result.ok = false;
        result.error = "--stage-mode must be file, tree, or direct";
        return result;
      }
      result.options.overrides.stage_mode = value;
    } else if (argument == "--state-dir") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      result.options.overrides.state_dir = value;
    } else if (argument == "--scheduling") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      if (!IsOneOf(value, {"adaptive", "aggressive", "balanced", "safe"})) {
        result.ok = false;
        result.error =
            "--scheduling must be adaptive, aggressive, balanced, or safe";
        return result;
      }
      result.options.overrides.scheduling = value;
    } else if (argument == "--map") {
      if (!TakeValue(argc, argv, &i, argument, &value, &result.error)) {
        result.ok = false;
        return result;
      }
      if (!ParseMapValue(value, &result.options, &result.error)) {
        result.ok = false;
        return result;
      }
    } else {
      result.ok = false;
      result.error = "unknown option: " + std::string(argument);
      return result;
    }
  }

  if (!result.options.command_set) {
    if (argc == 1) {
      result.options.command = Command::Version;
      result.options.command_set = true;
      return result;
    }
    result.ok = false;
    result.error =
        "no command specified (use --validate, --resolve, --print-config, or "
        "--print-schema)";
    return result;
  }

  const Command command = result.options.command;
  const bool needs_config = command == Command::Validate ||
                            command == Command::Resolve ||
                            command == Command::PrintConfig;
  if (needs_config && result.options.config_path.empty()) {
    result.ok = false;
    result.error = "--config is required for this command";
    return result;
  }
  if (!result.options.deck_info_path.empty() && command != Command::Validate &&
      command != Command::Resolve) {
    result.ok = false;
    result.error = "--deck-info is only valid with --validate or --resolve";
  }
  return result;
}

void PrintIssues(const std::vector<ConfigIssue>& issues, std::ostream& out) {
  for (const auto& issue : issues) {
    out << (issue.severity == IssueSeverity::Error ? "error: " : "warning: ")
        << issue.path << ": " << issue.message << "\n";
  }
}

std::size_t CountWarnings(const std::vector<ConfigIssue>& issues) {
  std::size_t count = 0;
  for (const auto& issue : issues) {
    if (issue.severity == IssueSeverity::Warning) ++count;
  }
  return count;
}

struct LoadedConfig {
  std::optional<Config> config;
  std::vector<ConfigIssue> issues;
};

LoadedConfig LoadConfigForCommand(const CliOptions& options) {
  auto loaded = LoadConfigFile(options.config_path, options.overrides);
  LoadedConfig result;
  result.config = std::move(loaded.config);
  result.issues = std::move(loaded.issues);
  return result;
}

struct DeckInfoLoad {
  std::vector<ConfigIssue> issues;
  std::optional<DeckInfo> info;
};

DeckInfoLoad LoadDeckInfoForCommand(const CliOptions& options,
                                    const Config& config) {
  DeckInfoLoad result;
  if (options.deck_info_path.empty()) return result;
  JsonDeckInfoProvider provider(options.deck_info_path);
  auto loaded = provider.Load(ResolveDeckPath(config));
  result.issues = std::move(loaded.issues);
  result.info = std::move(loaded.info);
  return result;
}

int RunValidate(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value()) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  Config& config = *loaded.config;
  std::vector<ConfigIssue> issues = std::move(loaded.issues);

  auto deck_info = LoadDeckInfoForCommand(options, config);
  for (auto& issue : deck_info.issues) issues.push_back(std::move(issue));

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder =
      options.overrides.allow_outside_folder;
  const auto resolved = ResolveTargets(
      config, deck_info.info ? &*deck_info.info : nullptr, resolve_options);
  for (const auto& issue : resolved.issues) issues.push_back(issue);

  PrintIssues(issues, std::cerr);
  if (HasErrorIssue(issues)) {
    std::cerr << "FAILED: configuration is invalid\n";
    return 1;
  }
  std::cout << "OK: configuration is valid (" << resolved.targets.size()
            << " targets, " << CountWarnings(issues) << " warnings)\n";
  return 0;
}

int RunResolve(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value()) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  Config& config = *loaded.config;
  std::vector<ConfigIssue> issues = std::move(loaded.issues);

  auto deck_info = LoadDeckInfoForCommand(options, config);
  for (auto& issue : deck_info.issues) issues.push_back(std::move(issue));

  ResolveOptions resolve_options;
  resolve_options.allow_outside_folder =
      options.overrides.allow_outside_folder;
  const auto resolved = ResolveTargets(
      config, deck_info.info ? &*deck_info.info : nullptr, resolve_options);

  std::cout << ResolveTableToText(resolved);
  for (const auto& issue : resolved.issues) issues.push_back(issue);
  PrintIssues(issues, std::cerr);
  return HasErrorIssue(issues) ? 1 : 0;
}

int RunPrintConfig(const CliOptions& options) {
  LoadedConfig loaded = LoadConfigForCommand(options);
  if (!loaded.config.has_value() || HasErrorIssue(loaded.issues)) {
    PrintIssues(loaded.issues, std::cerr);
    return 1;
  }
  std::cout << ConfigToJson(*loaded.config) << '\n';
  return 0;
}

} // namespace

int RunCli(int argc, char** argv) {
  ParseResult parsed = ParseCli(argc, argv);
  if (!parsed.ok) {
    std::cerr << "error: " << parsed.error << "\n";
    PrintUsage(std::cerr);
    return 2;
  }
  switch (parsed.options.command) {
    case Command::Version:
      std::cout << "htmlppt-watchdog " << version() << "\n";
      return 0;
    case Command::Help:
      PrintUsage(std::cout);
      return 0;
    case Command::Validate:
      return RunValidate(parsed.options);
    case Command::Resolve:
      return RunResolve(parsed.options);
    case Command::PrintConfig:
      return RunPrintConfig(parsed.options);
    case Command::PrintSchema:
      std::cout << ConfigSchema() << "\n";
      return 0;
  }
  return 2;
}

} // namespace htmlppt
```

Replace `native/src/app/main.cpp` with:

```cpp
#include "app/cli.h"

int main(int argc, char** argv) { return htmlppt::RunCli(argc, argv); }
```

- [ ] **Step 5: Build and run all tests**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

Expected: CTest 8/8 passes, no warnings.

- [ ] **Step 6: Exercise the commands manually**

```bash
./native/build/htmlppt-watchdog --help
./native/build/htmlppt-watchdog --print-schema | head -3
./native/build/htmlppt-watchdog --validate \
  --config native/tests/fixtures/watcher.json \
  --deck-info native/tests/fixtures/deck-info.json
./native/build/htmlppt-watchdog --resolve \
  --config native/tests/fixtures/watcher.json \
  --deck-info native/tests/fixtures/deck-info.json
./native/build/htmlppt-watchdog --print-config \
  --config native/tests/fixtures/watcher.json
```

Expected: help lists the six commands; schema starts with `{`; validate prints `OK: configuration is valid (2 targets, 0 warnings)`; resolve prints a two-row table; print-config prints the effective JSON snapshot.

- [ ] **Step 7: Commit**

```bash
git add native/src/app/cli.h native/src/app/cli.cpp native/src/app/main.cpp \
  native/CMakeLists.txt native/tests/fixtures native/tests/cmake
git commit -m "feat: add validate, resolve, and print CLI commands"
```

---

### Task 4: Documentation and Final Gate

**Files:**
- Modify: `native/README.md`
- Modify: `docs/superpowers/plans/2026-11-14-cpp-watchdog-resolve-cli.md` (as-built amendments)

**Interfaces:**
- Consumes: the CLI from Task 3.
- Produces: user-facing CLI documentation and the recorded final gate results.

- [ ] **Step 1: Document the CLI in native/README.md**

Append this section to `native/README.md`:

````markdown
## CLI diagnostics

```bash
./native/build/htmlppt-watchdog --help
./native/build/htmlppt-watchdog --print-schema
./native/build/htmlppt-watchdog --validate --config watcher.json
./native/build/htmlppt-watchdog --resolve --config watcher.json --deck-info deck-info.json
./native/build/htmlppt-watchdog --print-config --config watcher.json
```

`--validate` checks config structure, deck existence, HTML paths, duplicate
slide mappings, duplicate stage names, and duplicate Edge profiles. A warning
only result exits 0; any error exits 1.

`--resolve` prints the final mapping table and exits 1 on resolution errors.
`--deck-info` supplies the slide count and titles on Linux/WSL and in CI, where
the WPS COM host is not available:

```json
{
  "slides": [
    { "index": 1, "title": "Cover", "name": "Slide1" },
    { "index": 2, "title": "Contract", "name": "Slide2" }
  ]
}
```

Slide indices must be contiguous starting at 1. Without `--deck-info`, numeric
slide targets resolve with a warning and title/name matches fail with an error.
The WPS COM implementation of `DeckInfoProvider` is a later plan.

Auto names are deterministic: `stageName` becomes
`target-<NN>-<ascii-slug>.html`; `edgeProfile` becomes
`htmlppt-<deck-fingerprint>-t<NN>`. Explicit values are preserved.
````

Update the `Layout` section to list the new files:

```markdown
- `src/core/deck_info.*` is the deck-info provider interface, JSON provider, and fingerprint.
- `src/core/resolve.*` resolves slide/title targets, HTML paths, and auto names.
- `src/app/cli.*` implements the diagnostic CLI.
```

- [ ] **Step 2: Run the final gate**

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
./native/build/htmlppt_core_tests
ctest --test-dir native/build --output-on-failure
npm test
```

Expected:

- Native unit tests: 97/97 pass.
- CTest: 8/8 pass (`core_tests`, `cli_version`, `cli_help`, `cli_print_schema`, `cli_validate_ok`, `cli_resolve_ok`, `cli_validate_missing_html`, `cli_contract`).
- `-Werror` build with no warnings.
- `npm test`: unchanged by this plan. With Chrome installed it stays 3/3. If Chrome is absent, record the single environmental `Chrome did not start` failure; do not modify the root test.

- [ ] **Step 3: Record as-built amendments**

At the end of this plan file, add an `As-Built Amendments (2026-11-14)` section with:

- The final unit case count and CTest count actually observed.
- Any deviation from the code blocks above, with the reason.
- Any deferred item, explicitly assigned to a later plan.

Do not leave placeholders in the plan.

- [ ] **Step 4: Commit**

```bash
git add native/README.md docs/superpowers/plans/2026-11-14-cpp-watchdog-resolve-cli.md
git commit -m "docs: record Plan 2B as-built amendments"
```

---

## Self-Review

### Plan 2B spec coverage

| Spec requirement | Plan 2B coverage |
|---|---|
| `DeckInfoProvider` and slide title/name lookup | Task 1 interface, Task 2 matching |
| Slide index resolution and range validation | Task 2 `ResolveSlideNumber` |
| Title exact/prefix/regex, duplicate and no-match errors | Task 2 |
| HTML path resolution, existence, outside-folder guard | Task 2 `ResolveHtml` |
| `htmlQuery` merge and conflict detection | Task 2 `MergeQuery` |
| Auto ASCII `stageName` and Edge profile from deck hash | Task 1 fingerprint, Task 2 auto names |
| Duplicate slide/stage/profile conflict detection | Task 2 owner maps |
| `--validate` with all errors and warnings | Task 3 `RunValidate` |
| `--resolve` final mapping table | Task 3 `RunResolve` |
| `--print-config` effective snapshot | Task 3 `RunPrintConfig` |
| `--print-schema` embedded schema | Task 3 |
| CLI overrides `--config`, `--profile`, `--stage-mode`, `--state-dir`, `--scheduling`, `--map`, `--allow-outside-folder` | Task 3 parser |
| Platform-neutral, no COM/Win32 | Tasks 1-3; only `<filesystem>`, STL, nlohmann/json |
| Existing tests stay green | Every task runs the full suite; Task 4 final gate |

Deferred to later plans: WPS COM `DeckInfoProvider`, staging copy execution, `--watch-config` hot reload, `--init-config`, the config UI, and source-priority-based conflict overrides. `--allow-override` is intentionally not implemented because the spec defers conflict overrides to the config layer.

### Placeholder scan

- No `TBD`, `TODO`, "implement later", or empty steps.
- Every code step contains the code to write.
- Every run step names the exact command and expected result.

### Type consistency

- `DeckInfoResult::info` is `std::optional<DeckInfo>` everywhere.
- `ResolveOptions::deck_fingerprint` is empty by default; `ResolveTargets` computes it from `ResolveDeckPath(config)`.
- `ResolvedTarget::slide` is 0 only for disabled targets.
- `ResolveTableToText` consumes `ResolveResult` directly.
- `ConfigIssue` and `IssueSeverity` come from `core/config.h`; `AddError`/`AddWarning`/`HasErrorIssue` come from `core/config_issue.h`.
- CLI options map directly to `CliConfigOverrides`, which Plan 2A already consumes.
- `RunCli` is declared in `app/cli.h` and defined in `app/cli.cpp`, both in namespace `htmlppt`.

---

## Execution Handoff

Plan 2B is ready. Execute Tasks 1-4 in order. Each task ends with a green full suite and a commit. Task 4 records the final gate and any as-built deviations.

---

## As-Built Amendments (2026-11-15)

Plan 2B was executed and the final gate was re-run on 2026-11-15. This section
records the observed results, the deviations approved during execution, and the
items explicitly deferred to later plans.

### Final gate results

- `cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON`:
  configure and generate succeeded.
- `cmake --build native/build --parallel --clean-first`: exit 0 with `-Werror`
  and `-Wall -Wextra -Wpedantic`; no warnings were emitted. The rebuild used
  `--clean-first`, which is a stronger gate than the brief
  `cmake --build native/build --parallel` command.
- `./native/build/htmlppt_core_tests`: 97/97 cases passed, ending with
  `ALL TESTS PASSED`.
- `ctest --test-dir native/build --output-on-failure`: 8/8 tests passed
  (`core_tests`, `cli_version`, `cli_help`, `cli_print_schema`,
  `cli_validate_ok`, `cli_resolve_ok`, `cli_validate_missing_html`,
  `cli_contract`).
- `npm test`: 2 of 3 tests passed. `stepwise demo satisfies the HTML contract`
  fails with `Chrome did not start on port 9334` because Chrome is not
  installed in this WSL environment. This failure predates Plan 2B and is an
  environment limitation, not a regression; the root test was left unchanged.
  With Chrome installed the suite remains 3/3.

### Approved deviations

1. Unknown root-level deck-info fields force `DeckInfoResult::info` to
   `nullopt`. The `has_error` flag is declared before the root key scan and set
   for unknown root keys, so `info` is only populated for a fully valid
   document. This matches the slide-level unknown-field behavior and is covered
   by `deck_info_rejects_unknown_root_field`.
2. The FNV-1a offset basis is the standard `14695981039346656037ULL`; the plan
   draft had `1469598103934665603ULL`, one digit short. The prime is unchanged
   at `1099511628211ULL`.
3. Query conflicts are cross-source only: a key repeated within one source
   keeps distinct values, while exact duplicate parts are deduplicated; a key
   whose value differs between the HTML query and `htmlQuery` is an error.
   Covered by `resolve_html_query_duplicate_keys_allowed` and
   `resolve_html_query_conflict`.
4. `cli_validate_missing_html` uses the `cmake -P` wrapper
   `native/tests/cmake/expect_validation_failure.cmake` instead of `WILL_FAIL`
   plus `PASS_REGULAR_EXPRESSION`, because CTest on CMake 4.x inverts the
   regular-expression check under `WILL_FAIL`. The wrapper asserts both the
   exact exit code 1 and the expected diagnostic text.
5. The fixture `native/tests/fixtures/deck.pptx` is intentionally tracked
   despite the repository-wide `*.pptx` rule; the `.gitignore` negation
   `!native/tests/fixtures/deck.pptx` keeps it in the index. The fixture is a
   dummy `PK` fixture used only for existence and fingerprint checks.

### Deferred items

Explicitly deferred to later plans:

- The WPS COM `DeckInfoProvider` for Windows/WPS hosts; Plan 2B ships only the
  JSON provider and `--deck-info`.
- Staging copy execution: Plan 2B resolves and reports targets, but performs no
  staging copy.
- `--watch-config` hot reload and `--init-config`.
- The config UI, including a `ConfigToJson` representation that preserves
  profile structure.
- Source-priority-based target conflict overrides; `--allow-override` is not
  implemented.
- Windows reserved-name, path-case, and UTF-8 hardening of generated names.
- Owner-table error ordering when several conflicts are reported at once.
- CLI include hygiene and deduplication of the CLI pipeline helpers.
- Exit-code assertions for the remaining CTest success cases (`cli_version`,
  `cli_help`, `cli_print_schema`, `cli_validate_ok`, `cli_resolve_ok`), which
  currently rely on `PASS_REGULAR_EXPRESSION` alone.

### Final Review Fixes (2026-11-15)

The final whole-branch review produced a single fix wave, recorded here:

- The `--map` help text now says "add a target (repeatable; override is
  deferred)" instead of promising an override that is not implemented.
- The `cli_contract` CTest case covers exit codes 0/1/2 and the option paths:
  `--print-config`, `--map` append, unknown options, invalid `--stage-mode`
  and `--scheduling`, resolve success, missing HTML, and duplicate slide
  mapping.
- Invalid `--stage-mode` and `--scheduling` values now fail during option
  parsing with exit code 2 instead of surfacing later as config errors with
  exit code 1.
- README documents that relative `--map` HTML paths resolve against the
  config file directory and that auto Edge profiles use the `nodeck` segment
  when the deck fingerprint is unavailable.
- Include hygiene: `native/src/app/cli.cpp` no longer includes unused
  `<filesystem>` and `<map>`, and now includes `<initializer_list>` and
  `<utility>`.
