#include "test_main.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "app/deck_info_loader.h"
#include "core/config.h"

namespace {

std::filesystem::path MakeLoaderDir() {
  const auto dir =
      std::filesystem::temp_directory_path() / "htmlppt-deck-loader-test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  std::filesystem::create_directories(dir, error);
  return dir;
}

} // namespace

HTMLPPT_TEST_CASE(deck_info_loader_reads_the_json_provider) {
  const auto dir = MakeLoaderDir();
  {
    std::ofstream info(dir / "deck-info.json", std::ios::binary);
    info << R"({"slides":[)"
            R"({"index":1,"title":"Cover","name":"Slide1"},)"
            R"({"index":2,"title":"Agenda","name":"Slide2"}]})";
  }
  htmlppt::Config config;
  config.source_path = dir / "watcher.json";
  config.active.deck = "deck.pptx";

  const auto loaded = htmlppt::LoadDeckInfo(dir / "deck-info.json", config);
  HTMLPPT_CHECK(loaded.info.has_value());
  HTMLPPT_CHECK(loaded.issues.empty());
  HTMLPPT_CHECK_EQ(loaded.info->slides.size(), std::size_t{2});
  HTMLPPT_CHECK_EQ(loaded.info->slides[0].title, std::string{"Cover"});
  HTMLPPT_CHECK_EQ(loaded.info->slides[1].name, std::string{"Slide2"});
  // The provider resolves the deck from the config, not from the info file.
  HTMLPPT_CHECK_EQ(loaded.info->path.filename(),
                   std::filesystem::path{"deck.pptx"});

  // A bad file returns no info and the provider's issue, so the editor's save
  // gate can see that deck info is unavailable.
  const auto missing = htmlppt::LoadDeckInfo(dir / "missing.json", config);
  HTMLPPT_CHECK(!missing.info.has_value());
  HTMLPPT_CHECK(!missing.issues.empty());
  std::error_code error;
  std::filesystem::remove_all(dir, error);
}
