#include "app/cli.h"

#ifdef _WIN32
#include <string>
#include <vector>

#include "platform/win32/args_win32.h"
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  // The Windows CRT builds argv through the ANSI code page, so a non-ASCII
  // --config/--state-dir path arrives mangled before the CLI ever sees it.
  // Parse the UTF-16 command line and hand the CLI UTF-8 arguments instead;
  // argv stays the fallback for a host that offers no command line at all.
  if (const auto utf8_arguments = htmlppt::CommandLineUtf8Arguments();
      utf8_arguments.has_value() && !utf8_arguments->empty()) {
    std::vector<std::string> arguments = *utf8_arguments;
    std::vector<char*> pointers;
    pointers.reserve(arguments.size());
    for (auto& argument : arguments) {
      pointers.push_back(argument.data());
    }
    return htmlppt::RunCli(static_cast<int>(pointers.size()), pointers.data());
  }
#endif
  return htmlppt::RunCli(argc, argv);
}
