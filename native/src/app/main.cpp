#include "core/version.h"

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc <= 1) {
    std::cout << "htmlppt-watchdog " << htmlppt::version() << "\n";
    return 0;
  }

  const std::string_view argument = argv[1];
  if (argument == "--version") {
    std::cout << "htmlppt-watchdog " << htmlppt::version() << "\n";
    return 0;
  }
  if (argument == "--help" || argument == "-h") {
    std::cout << "usage: htmlppt-watchdog [--version] [--help]\n";
    return 0;
  }

  std::cerr << "unknown option: " << argument << "\n";
  return 2;
}
