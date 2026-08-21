# Behavioral Invariants and Edge Cases (Current Implementation)

This file records behavior that is currently enforced by code and/or tests.

## Matching and Text Interpretation

- `MatchUtils::indexOf(...)` applies ignore-case folding only for non-UTF-16 modes.
  - For UTF-16 mode, it delegates to exact `QByteArray::indexOf`.
- Ignore-case folding is ASCII-only (`A-Z` -> `a-z`) and byte-based.
- Ignore-case path rejects empty needles (`-1`).

Evidence:
- `src/scan/MatchUtils.cpp`
- `tests/unit_tests.cpp` (`testMatchUtilsIndexOf`)

## Text Sequence Detection Rules

`TextSequenceAnalyzer::finalizeSequences(...)` marks a sequence when either:

- contiguous non-invalid run length >= 5
- contiguous non-invalid run length >= 2 and immediately followed by `0x00`

This applies after mode-specific classification (ASCII/UTF-8/UTF-16).

Evidence:
- `src/text/TextSequenceAnalyzer.cpp`
- `tests/unit_tests.cpp` (`testTextSequenceAnalyzer`)

```mermaid
flowchart TD
startRun["Contiguous non-invalid byte run"] --> runLen{"Run length >= 5?"}
runLen -->|Yes| qualifiesLong["Qualifies as sequence"]
runLen -->|No| minLen{"Run length >= 2?"}
minLen -->|No| notQualifyShort["Does not qualify"]
minLen -->|Yes| followedByNull{"Immediate next byte == 0x00?"}
followedByNull -->|Yes| qualifiesNullTerm["Qualifies as sequence"]
followedByNull -->|No| notQualifyNoNull["Does not qualify"]
```

## String Mode Null Visibility

`StringModeRules` behavior:

- NUL rendering is denied when there is no predecessor byte.
- NUL rendering is denied when predecessor is `0x00`.
- NUL rendering is allowed only if predecessor is:
  - printable ASCII (`0x20..0x7E`)
  - `\r` or `\n`
- For first byte in a viewport, predecessor can come from backing byte before viewport start.

Evidence:
- `src/text/StringModeRules.cpp`
- `tests/unit_tests.cpp` (`testStringModeNullVisibilityRule`)

```mermaid
flowchart TD
byteIsNull{"Current byte == 0x00?"} -->|No| visibleTrue["Visible"]
byteIsNull -->|Yes| hasPrev{"Previous byte available?"}
hasPrev -->|No| visibleFalseNoPrev["Hidden"]
hasPrev -->|Yes| prevIsNull{"Previous byte == 0x00?"}
prevIsNull -->|Yes| visibleFalsePrevNull["Hidden"]
prevIsNull -->|No| prevPrinted{"Prev is printable ASCII or CR/LF?"}
prevPrinted -->|Yes| visibleTrueNull["Visible"]
prevPrinted -->|No| visibleFalseNonPrinted["Hidden"]
```

## Scan Lifecycle Guardrails

- A scan cannot start if:
  - another scan is running
  - search term is empty
  - filtered target list is empty
- Worker count is always at least `1`.
- Block size is always at least `1`.
- Stop requests are cooperative (atomic flag + CV wake); no forced thread termination.
- Stop cancels never-started queued jobs, drains active jobs, publishes their partial matches,
  and always completes through the normal finalization signal path.
- Result rows are published in scan-job order while scanning; every published row has a
  placeholder or resident buffer mapping before the UI receives it.

Evidence:
- `src/scan/ScanController.cpp`
- `src/app/MainWindow.cpp`

## Partition and Merge Guarantees

- Reader creates job segments with explicit overlap to prevent missing boundary matches.
- Partition validity is checked and warnings logged on invalid splits.
- Final merge guarantees ordered output by:
  - fast k-way merge when per-worker streams are sorted
  - fallback global sort when stream order contract is broken

Evidence:
- `src/scan/ScanController.cpp` (`readerLoop`, `buildFinalResults`)

## Result Buffer and Cache Invariants

- Every result row has an index in `m_matchBufferIndices`.
- Evicted buffers become placeholders with stable `scanTargetIdx` and `fileOffset`.
- On-demand load restores bytes for a row and re-enforces cache budget.
- Cache eviction policy prefers largest resident buffer; tie-breaker least referenced.
- Protected buffer indices are never evicted in that enforcement pass.

Evidence:
- `src/app/MainWindow.cpp` (`evictOneBufferLargestFirstLeastUsed`, `ensureRowBufferLoaded`)

## Preview Synchronization Invariants

- Shared center offset is always clamped to current backing buffer bounds.
- Center requests are coalesced through queued updates to avoid re-entrancy storms.
- Text/bitmap updates run under `m_previewSyncInProgress` guard.
- Hover decode uses only the currently rendered hover buffers (text or bitmap side).
- Image-result jumps that land outside the resident buffer reload the active backing window before centering.

Evidence:
- `src/app/MainWindow.cpp` (`scheduleSharedPreviewUpdate`, `updateSharedPreviewNow`)

## Embedded Image Scan Rules

- PNG, TIFF/BigTIFF, JPEG, BMP, ICO, GIF, XPM, and SVG are scanned from the chosen scope start to scope end.
- TGA and XBM are attempted only once at the chosen start offset.
- `From Here` is inclusive and can return an image already starting at the selected byte.
- Candidates are rejected when cheap header validation finds zero, implausible, or over-limit dimensions.
- Decoded results must fit `maxPixelsK * 1000` pixels.
- File-wide scans use one coordinator as the only `source.read(...)` caller; worker jobs scan the same shared immutable chunks and never perform source I/O.
- Per-chunk candidates are merged by offset/format before serial decode, keeping one-job and multi-job result order equivalent.
- Accepted images are streamed to the UI as live result cards.
- Accepted results retain encoded payload bytes so saving does not re-encode or flatten them.
- GIF results retain all decoded frames and show their frame count; encoded frame delays below `16 ms` are clamped to `16 ms` for playback.
- The scanner stops when positive `maxresults` accepted images have been found.
- `maxresults == 0` is unlimited until EOF or cooperative Stop.
- User Stop preserves displayed partial results; source/target/shift invalidation clears them as stale.

Evidence:
- `src/image/EmbeddedImageScanner.cpp`
- `src/app/MainWindow.cpp` (`startImageScan`, `buildImageScanRequest`)

## Result Model Contract

- Table currently has exactly 4 columns:
  1. `Thread`
  2. `Filename`
  3. `Offset`
  4. `Search time`
- Offset display is rounded humanized units (`B`, `KiB`, `MiB`, ...).
- Search time display is `elapsedNs / 1_000_000` in milliseconds.

Evidence:
- `src/model/ResultModel.cpp`
- `tests/unit_tests.cpp` (`testResultModelColumnOrder`)

## Persistence Invariants

`AppSettings` persists and rehydrates:

- last file dialog path
- last directory dialog path
- text byte/string mode
- text wrap mode
- text monospace mode
- newline mode combo index
- byte line mode combo index
- prefill-on-merge toggle
- Image tab format mask, scope, max pixels K, max results, and Jobs
- last BrecoLang schema path and schema-library directory

When the remembered schema path still exists, startup recompiles its current
contents. A live decode is restored only when compilation succeeds and the
remembered single-file source also opens successfully.

Settings are saved immediately at control-change call sites (no delayed batch commit).

Evidence:
- `src/settings/AppSettings.cpp`
- `src/app/MainWindow.cpp` constructor + control handlers

## BrecoLang Runtime Invariants

- Source-order `require`, `check`, and `match` evaluation is preserved.
- Regions and `within` cursors cannot advance beyond their declared bounds.
- Uncommitted alternatives restore cursor, values, nodes, diagnostics, and
  probe anchors. Streaming transactional scopes trial silently and replay once.
- Repeat and while failures do not emit partial sequence JSON.
- Parse depth, loop iteration, node, recovery probe, and transform-output limits
  are enforced at runtime.
- Tree nodes reference input spans; they do not retain per-node raw-byte copies.
- Bitfield members are virtual children sharing the containing word's span.
- GUI and CLI file exports use staged output and leave the destination unchanged
  when decoding or rendering fails.
- Binary outform integer encoders reject values outside the target type range.

Evidence:
- `src/brecolang/runtime/Interpreter.cpp`
- `src/brecolang/render/OutformRenderer.cpp`
- `src/cli/brecodump.cpp`
- `tests/brecolang_runtime_tests.cpp`
- `tests/brecodump_cli_tests.cpp`

## Error Handling Invariants

- User-facing scan errors are signaled and surfaced via `QMessageBox::warning`.
- Many operational faults are non-fatal and logged (stderr/stdout), including:
  - chunk read failure
  - invalid partition warning
  - invalid worker callback id
  - stream-order fallback warning
- On-demand preview load failure does not crash; preview update just returns.

Evidence:
- `src/scan/ScanController.cpp`
- `src/app/MainWindow.cpp`

## Notable Doc/Implementation Mismatches

Compared with `README.md`, current code indicates:

- Results table columns are currently 4, not the larger column set described in README.
- The README caveat says scan auto-stops when merged results exceed `4000`, but there is no corresponding stop-limit logic in current `ScanController` or `MainWindow` runtime path.

These are documentation drift points and should be treated as current-behavior discrepancies.
