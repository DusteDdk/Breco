# Core Runtime Codemap

## Entrypoints

- `src/main.cpp` starts the Qt desktop application and `MainWindow`.
- `src/cli/brecodump.cpp` is the BrecoLang-only command-line decoder.
- `CMakeLists.txt` defines compiler/runtime libraries, application targets,
  tests, and benchmarks.

## BrecoLang

### Compiler

- `src/brecolang/compiler/Lexer.*` tokenizes source and retains source spans.
- `src/brecolang/compiler/Parser.*` produces a complete syntax tree with
  recoverable, batched diagnostics.
- `src/brecolang/compiler/Compiler.*` resolves names and types into immutable,
  contiguous `BrecoProgram` tables and calculates extent/effect summaries.
- `src/brecolang/ir/BrecoProgram.h` defines the resolved program layout.

### Runtime and rendering

- `src/brecolang/runtime/ByteSource.*` supplies borrowed-window, paged-file,
  sequential, and spooling sources with checked ranges.
- `src/brecolang/runtime/Interpreter.*` executes tree, probe, and streaming
  modes with transactional alternation, recovery, cursor regions, limits,
  multi-input reads, and source-span storage layouts.
- `src/brecolang/runtime/DecodedData.*` stores flat decoded trees and values in
  contiguous arrays.
- `src/brecolang/runtime/JsonWriter.*` writes JSON incrementally to a
  `QIODevice`.
- `src/brecolang/render/RenderStore.*` exposes decoded values, raw source spans,
  and metadata including the declared input role.
- `src/brecolang/render/OutformRenderer.*` executes text and binary outforms.

### Desktop surface

- `src/brecolang/gui/BrecoLangPanel.*` owns the live editor, schema/input/entry
  controls, independent pinned views, JSON/binary/outform actions, source
  navigation, and scan requests.
- `src/brecolang/gui/DecodedTreeModel.*` adapts the flat decoded tree to Qt.
- `src/brecolang/gui/BrecoLangLibrary.*` indexes `.breco` schemas and reports
  older files requiring manual migration without modifying them.

## Application orchestration

- `src/app/MainWindow.*` coordinates sources, scans, results, raw previews,
  image scanning, BrecoLang navigation, and persisted settings.
- `src/panel/MainTabsPanel.*` owns `Scan`, `Raw`, `BrecoLang`, and `Image` tabs,
  including detach/reattach behavior.
- `src/settings/AppSettings.*` persists general UI state plus the last schema
  and schema-library directory.

## Scanning

- `src/scan/ScanController.*` owns scan lifecycle, reader coordination, worker
  scheduling, progress, merging, and result buffers.
- `src/scan/ScanWorker.*` performs either text matching or BrecoLang probe-mode
  decoding. Each schema-scan worker owns paged input sources; probe mode does
  not construct decoded nodes.
- `src/scan/MatchUtils.*` implements byte/text matching.
- `src/scan/ShiftTransform.*` maps shifted logical byte streams.
- `src/scan/ScanTypes.h` defines shared scan jobs and buffers.

## Other runtime modules

- `src/io/` contains file enumeration, protected opening, pooled reads, and
  shifted-window loading.
- `src/image/EmbeddedImageScanner.*` validates and decodes embedded image
  candidates asynchronously.
- `src/model/ResultModel.*` presents scan matches.
- `src/view/` contains text and bitmap byte views.
- `src/panel/` contains the remaining raw, scan, result, image, byte-info, and
  tab wrappers.
- `src/text/` contains text sequence classification and display rules.

## Tests

- `tests/brecolang_compiler_tests.cpp` covers the full grammar, recovery,
  resolution, extent analysis, and every shipped `.breco` example.
- `tests/brecolang_runtime_tests.cpp` covers tree/probe/streaming execution,
  transactions, recovery, limits, byte sources, render metadata, and outforms.
- `tests/brecodump_cli_tests.cpp` covers the sole CLI interface, stdin,
  streaming JSON, binary/text outforms, diagnostics, and atomic output.
- `tests/mainwindow_integration_tests.cpp` covers the desktop pipeline,
  multi-view/export behavior, schema scanning, library migration notice, and
  the raw/image workflows.
- `tests/unit_tests.cpp` covers non-language runtime utilities.
