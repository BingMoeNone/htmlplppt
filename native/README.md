# htmlppt-watchdog native

## Build on Linux/WSL

```bash
cmake -S native -B native/build -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --parallel
ctest --test-dir native/build --output-on-failure
```

## Build on Windows with MSVC

```powershell
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64 -DHTMLPPT_BUILD_TESTS=ON -DHTMLPPT_WERROR=ON
cmake --build native/build --config Release --parallel
ctest --test-dir native/build -C Release --output-on-failure
```

## Layout

- `src/core/` is platform-neutral and must build on Linux.
- `src/core/deck_info.*` is the deck-info provider interface, JSON provider, and fingerprint.
- `src/core/resolve.*` resolves slide/title targets, HTML paths, and auto names.
- `src/app/` contains the executable entry point.
- `src/app/cli.*` implements the diagnostic CLI.
- `tests/unit/` contains test cases registered with `HTMLPPT_TEST_CASE`.
- `third_party/` contains vendored single-header dependencies.

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
