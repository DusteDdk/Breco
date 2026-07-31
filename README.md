# Breco

Breco is a Qt desktop app for scanning binary data for byte-pattern matches and inspecting them in synchronized text and bitmap previews.

![Breco screenshot](https://raw.githubusercontent.com/DusteDdk/Breco/refs/heads/master/screenshot.png)

## License
WTFPL

## Build and run

### Ubuntu dependencies

```bash
./scripts/install_deps_ubuntu.sh
```

### Windows cross-build (from Ubuntu)

See [docs/windows-build.md](docs/windows-build.md) for MXE toolchain setup, packaging, and platform limits.

### Configure and build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

### Start

```bash
./build/breco
```

## Test and benchmark

```bash
ctest --test-dir build --output-on-failure
./build/breco_text_analysis_benchmark
./build/breco_scan_primitives_benchmark
```

Benchmark policy:
- benchmark results must never justify feature removal or correctness compromises.
- use benchmarks only to choose among approaches that are all correct, feature-complete, and similarly maintainable (or skip benchmark-driven change entirely).

## Quick start

1. Select a source with `Open file/device` (readable regular file) or `Open directory` (recursive).
2. Enter `Search term`.
3. Set scan parameters (`Ignore case`, `Shift`, `Block size`, `Workers`, `PrefillOnMerge`).
4. Run `Scan`.
5. Select a result row to load text and bitmap previews.
6. Hover text/bitmap bytes to inspect values in the current-byte panel.

## Scan controls

- `Search term`: scanned as UTF-8 bytes.
- `Ignore case`: ASCII byte-folding; `UTF-16` matching stays exact-byte.
- `Scan`: toggles to `Stop` while a scan is running.
- `Shift`:
- `Bytes`: range `-7..7`
- `Bits`: range `-127..127`
- `Block size`: `B`, `KiB`, `MiB`.
- `Workers`: number of worker threads.
- `PrefillOnMerge`: include transformed windows while merging result buffers.
- `Selected`: shows currently selected file path or directory path.

Info area shows:
- file count
- selected-source search space bytes
- scanned bytes
- progress bar (`0..1000` scale)

## Results table

Columns:
1. Thread
2. Filename
3. Offset
4. Search time

## Text preview

Controls:
- mode: `ASCII`, `UTF-8`, `UTF-16`
- display: `StringMode` / `ByteMode`
- `Wrap` (StringMode)
- `Collapse` (StringMode)
- `breathe` (StringMode)
- newline mode selector (StringMode)
- `Monospace` (StringMode)
- bytes-per-line selector (ByteMode)

Behavior:
- gutter uses GhostWhite (`#F8F8FF`).
- text is selectable; `Ctrl+C` copies selection.
- hover syncs with bitmap and current-byte panel.

## Bitmap preview

Modes:
- `RGB24`
- `Grey8`
- `Grey24`
- `RGBi256`
- `Binary`
- `Text`

Common controls:
- `Result` overlay toggle
- zoom (`1x..32x`) via buttons or mouse wheel
- pan via left-drag when zoom > 1

### Text bitmap mode

Text mode classifies bytes using the selected text interpretation mode and highlights valid sequences.

Sequence rule:
- contiguous valid bytes with length >= 5
- or length >= 2 when immediately followed by `0x00`

Color rules:
- valid sequence bytes: fixed class colors (printable/newline/CR/whitespace variants)
- valid non-sequence bytes: DarkKhaki (`#BDB76B`)
- invalid non-sequence bytes: Grey8 behavior (byte-value grayscale)
- when `Result` overlay is enabled:
- search term bytes: DodgerBlue (`#1E90FF`)
- search term context: ForestGreen (`#228B22`)

Hover behavior:
- hovering a valid sequence paints the whole sequence pink and shows tooltip:
- `<N> bytes at offset: <n>`
- `---`
- `<sequence-text>`
- bitmap hover also updates current-byte panel values.

## Current byte panel

Hovering text or bitmap data updates:
- ASCII, UTF-8, UTF-16 hints
- signed/unsigned integer interpretations for 8/16/32/64-bit widths
- little-endian and big-endian value columns (where available)
- large character display, with selectable big-endian/little-endian char interpretation mode
- caption highlighting by available byte width (8/16/32/64)

## View Data Struct mode

Struct mode parses BrecoScript declarations and previews a selected entry at
the active file offset. Its tree shows `Name`, `Type`, `Value`, `Bytes`, and
`Valid`; the name column sizes itself to its content. Passing `/cond` rows are
light green, while failed `/cond` and truncated rows are light red. Odd rows
use darker variants of those colors; unqualified complete rows retain the
normal alternating backgrounds. `Valid` reports `true`, `false`, the
missing-byte count, or both when applicable. Condition diagnostics appear
only on the failing member; containing struct rows keep their normal value.

Clicking a decoded tree item centers the hex view on that item's first byte
in natural file order and highlights its byte range. Clicking a row in the
saved `Views` table centers on that view's starting offset and highlights the
decoded view range.

Loading a declaration file remembers its path. On the next launch Breco
reloads that file when it still exists; when both the declaration and the
remembered single-file source are valid, it creates the struct preview
automatically.

## View Data Image mode

`View Data` includes an `Image` mode for finding embedded images in the active preview source.

Controls:
- format checkboxes for `TGA`, `TIFF`, `PNG`, `JPEG`, `BMP`, `ICO`, `GIF`, `XBM`, `XPM`, and `SVG`
- scope: `From start of file`, `From Here`, or `Only visible buffer`
- `Jobs`, defaulting to the main scan worker count
- `maxPixels` in decimal kilopixels, default `4096 K`
- `maxresults`, default `5`; `0` means unlimited until EOF or Stop
- `Scan`, which toggles to `Stop` while active
- file progress and, when `maxresults` is positive, result progress

Behavior:
- PNG, TIFF/BigTIFF, JPEG, BMP, ICO, GIF, XPM, and SVG are searched by signature/text opener.
- TGA and XBM are attempted only at the selected byte, falling back to the first visible byte.
- Scans use the same shifted logical byte stream as the hex view.
- The image scanner uses one reader/coordinator and shared immutable chunks scanned by worker jobs, so workers do not compete for source I/O.
- Decoded images are shown live as they are accepted; Stop keeps partial results.
- Candidates with implausible headers or dimensions above `maxPixels` are rejected before decode where possible.
- Results show image format, dimensions, and file offset. GIF results also show frame count and play continuously, with encoded delays below `16 ms` clamped to `16 ms`.
- Hovering a preview highlights its result card. Left-clicking reloads and centers the hex view at the image's first byte; right-clicking opens a format-aware Save File dialog that writes the original encoded image, including every GIF frame.

## Status line

Status bar is used for lifecycle and cache messages, for example:
- `Scanning...`
- `Merged results: <N>`
- `Scan finished`
- `Current buffer: ... -- All buffers: ...`

## Persisted settings (`QSettings`)

- last file dialog path
- last directory dialog path
- remembered single-file source path
- text byte/string display mode
- text `Wrap`
- text `Collapse`
- text `breathe`
- text newline mode
- text `Monospace`
- text bytes-per-line mode
- prefill-on-merge
- scan block size value and unit
- main splitter sizes
- text gutter format and gutter width
- View Data mode, endian/text/bitmap options, and image scan options including Jobs
- Struct declaration text, last loaded declaration file, entry, and repeat count

## Current limits and caveats

- source filtering accepts readable regular files and readable block devices.
- result table ordering follows controller batch merge order, not global byte-order sort.
- ignore-case matching is ASCII-byte folding, not full Unicode case-folding.
- TGA, TIFF, and SVG decoding depend on Qt image-format/SVG runtime plugins being installed.
