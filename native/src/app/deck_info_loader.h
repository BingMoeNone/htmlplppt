#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "core/config.h"
#include "core/deck_info.h"

namespace htmlppt {

// One deck-info load: the provider's result plus its issues. A JSON path uses
// JsonDeckInfoProvider; on Windows an empty path falls back to WpsHost, and on
// other platforms it returns an empty result. The deck path always comes from
// ResolveDeckPath(config), never from the deck-info file.
struct DeckInfoLoad {
  std::optional<DeckInfo> info;
  std::vector<ConfigIssue> issues;
};

DeckInfoLoad LoadDeckInfo(const std::filesystem::path& deck_info_path,
                          const Config& config);

} // namespace htmlppt
