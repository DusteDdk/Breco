# BrecoScript Language Reference

BrecoScript is Breco's Struct Declaration language. It describes binary data
as named fields and structures so that the **Struct** editor and
`brecodump` can decode the bytes into a tree.

BrecoScript resembles a small subset of C declarations, but it is a
sequential binary-decoding language rather than C. Fields are read in
declaration order with no implicit padding or alignment. The language has no
pointers, seeking, unions, or enums.

Declaration files are UTF-8 text. The preferred extension is `.struct`;
Breco's file dialog also accepts `.txt`.

## Quick start

The following declaration decodes an eight-byte packet header:

```text
struct PacketHeader {
    /cond(=0x42524543) uint32<be> magic;
    uint8 version;
    uint8 flags;
    uint16<le> payloadLength;
}
```

The decoder starts at the selected byte offset and reads:

1. a four-byte big-endian magic value, which must equal `0x42524543`;
2. two one-byte fields; and
3. a two-byte little-endian payload length.

In Breco, paste the declaration into the Struct editor, select
`PacketHeader`, choose the file offset and entry count, and leave `Enable`
checked to preview it. Use `Add previewed` to save that preview in `Views`.
The editor reparses the declaration as it changes.

With the command-line utility:

```sh
build/brecodump -s packet.struct -i packet.bin -e PacketHeader -ofs 0 -r 1
```

`brecodump` writes a JSON representation of the selected visualizable
top-level entry. Run `build/brecodump --help` for the current options.

## How decoding works

A decode has four inputs:

- a declaration;
- a top-level entry name;
- a byte offset at which that entry starts; and
- an entry count.

Breco decodes every member sequentially from a byte cursor. A nested struct
continues at the current cursor position. An entry count greater than one
decodes consecutive instances, with each instance starting where the previous
one ended.

There is no implicit padding. For example, this struct is three bytes, not
four:

```text
struct PackedValue {
    uint8 kind;
    uint16<le> value;
}
```

The start offset and default byte order belong to the view or command-line
invocation, not to the declaration. BrecoScript cannot move the cursor to an
absolute or relative offset from inside a struct.

## Lexical rules

### Whitespace and comments

Whitespace may appear between tokens. Both C++-style line comments and
C-style block comments are accepted:

```text
// One-line comment
struct Example {
    uint8 first;       /* block comment */
    uint8 second;
}
```

Block comments do not nest. In the current implementation, an unterminated
block comment consumes the rest of the declaration instead of producing a
dedicated comment error.

### Identifiers

Identifiers start with a letter or underscore and continue with letters,
digits, or underscores. Letter and digit recognition follows Qt's Unicode
character classification:

```text
Header
item_count
_reserved2
```

Names are case-sensitive. Built-in type names cannot be redefined.

### Integer literals

Integer literals are decimal or hexadecimal:

```text
17
0x11
0XFFFF
-4
```

Negative values are valid comparison operands, but lengths and repeat counts
cannot be negative. Binary, octal, and integer suffix notation are not
supported. Non-negative literals range through `0xFFFFFFFFFFFFFFFF` and are
stored as unsigned values; negative literals range down to
`-0x8000000000000000`.

### String literals

String literals use double quotes. The recognized escapes are:

- `\n`, `\r`, and `\t`;
- `\0`;
- `\"`; and
- `\\`.

For any other escaped character, the backslash is discarded and that
character is used literally.

String values are mainly useful as `<until>` sentinels and `/cond` operands:

```text
byte<until:="\r\n"> lineBytes;
```

### Variable references

A variable reference starts with `$`. Dots address variables exported by
nested structs:

```text
$length
$header.itemCount
$outer.header.payloadLength
```

Only fields explicitly bound with `/var` become variables.

### Comparison operators

BrecoScript has three single-character comparison operators:

- `=` for equality;
- `<` for less than; and
- `>` for greater than.

Use `=`, not `==`. Strings and byte sequences support equality only. Ordering
either with `<` or `>` is a decode-time error.

## Declarations and entries

A non-empty file contains one or more top-level declarations. Declarations
are processed in source order, so a type must be declared before it is used.

### Default preview entry

An optional file-level `/default` directive selects the entry initially shown
in the Struct editor's entry dropdown:

```text
/default PacketHeader

struct PacketHeader {
    uint32<be> magic;
}
```

`/default` may appear once anywhere at top level, and its trailing semicolon is
optional. Its name must resolve to a visualizable top-level entry in the same
file. The target may be declared later because Breco validates the default
after parsing all declarations. The directive controls the GUI's initial
preview selection; `brecodump` continues to use its explicit `-e` option.

### Named structs

```text
struct Point {
    int16<le> x;
    int16<le> y;
};
```

The semicolon after a named struct is optional. A struct must have at least
one member, and member names must be unique within that struct.

`Point` becomes a visualizable entry.

### Standalone fields

A field can be declared at top level:

```text
uint32<be> sequenceNumber;
```

The field name becomes a visualizable entry.

### Simple typedefs

```text
typedef uint32<be> be32;
typedef int16<le> sample16;

struct Sample {
    sample16 i;
    sample16 q;
}
```

The base type must already exist. A typedef may alias a built-in, a previous
typedef, or a previously declared struct. Endianness can be applied before or
after the base type:

```text
typedef <be>uint32 be32;
typedef uint32<be> alsoBe32;
```

If both prefix and postfix endianness are present, the postfix decoration
wins.

### Typedef structs

Anonymous and tagged typedef structs are supported:

```text
typedef struct {
    uint16<le> width;
    uint16<le> height;
} Dimensions;

typedef struct TaggedRecord {
    uint8 type;
    uint8 value;
} Record;
```

An anonymous struct is stored under its alias. A tagged form defines the tag
and, when different, an alias to it. The final semicolon is required.

An anonymous top-level `struct { ... }` without `typedef` is invalid.

### Entry behavior

Standalone fields, named structs, and resolvable typedefs are visualizable
entries. The Struct editor lets the user choose one entry. `brecodump -e`
does the same; without `-e`, it chooses the last visualizable declaration in
source order. Defining helper types before the intended root type therefore
gives a useful command-line default. All top-level struct, typedef, and
standalone-field names share one namespace and must be unique.

## Built-in types

### Integers

BrecoScript provides signed and unsigned integers of 8, 16, 32, and 64 bits.
Each type has three equivalent spellings:

| Width | Unsigned | Signed |
| --- | --- | --- |
| 8 | `uint8_t`, `uint_8`, `uint8` | `int8_t`, `int_8`, `int8` |
| 16 | `uint16_t`, `uint_16`, `uint16` | `int16_t`, `int_16`, `int16` |
| 32 | `uint32_t`, `uint_32`, `uint32` | `int32_t`, `int_32`, `int32` |
| 64 | `uint64_t`, `uint_64`, `uint64` | `int64_t`, `int_64`, `int64` |

Multi-byte integers use their declared byte order or, if none is declared,
the view's default byte order.

### Raw bytes

`byte` is one uninterpreted byte:

```text
byte flags;
byte<len:16> identifier;
```

A scalar `byte` may be bound with `/var`. Arrays of bytes are displayed as
raw data rather than as integer arrays.

### Strings

The string types are:

- `asciistr`: Latin-1 bytes;
- `utf8str`: UTF-8 bytes; and
- `utf16str`: UTF-16 code units.

Without a length modifier, a string is NUL-terminated. `asciistr` and
`utf8str` consume one terminating zero byte; `utf16str` consumes a zero
two-byte code unit.

```text
struct Labels {
    asciistr shortName;
    utf8str displayName;
    utf16str<le> windowsName;
}
```

Only `utf16str` accepts `<le>` or `<be>`. ASCII and UTF-8 have no byte-order
setting.

## Endianness

Use `<le>` or `<be>` immediately before or after an integer or UTF-16 type:

```text
<be>uint32 first;
uint32<le> second;
utf16str<be> title;
```

An undecorated type has `native` endianness in the decoded tree. Here,
`native` means "use the decode's default byte order," not necessarily the
machine CPU's native byte order.

In the Struct editor, the data view's little-/big-endian selection supplies
the default. Little endian is the normal default. `brecodump` also uses
little endian for undecorated types. Explicit decorations are preferable for
portable declarations.

Endianness applies to the type, while field modifiers follow it:

```text
uint16<be><len:4> values;
utf16str<le><len:12> label;
```

## Field syntax

The order of a field declaration is:

```text
directives type-and-endianness length-modifier name bitfields?;
```

For example:

```text
/var(n) uint16<le> count;
/repeat($n) uint32<be> values;
byte<len:32> payload;
```

Directives come before the type. A field may have at most one length
modifier. The member name is normally required; it may be omitted for a
`/repeat` field, in which case the type's display name is used.

Scalar integer fields may include display-only bitfield children:

```text
uint32<le> status {
    bits 31:16 highHalf;
    bit 0 enabled;
}
```

`bit 0` is the least significant bit. `bits HI:LO` is inclusive and requires
`HI >= LO`. Bitfields do not consume additional bytes, and overlapping or
out-of-width ranges are parse errors.

## Length modifiers

Length modifiers apply to integer, byte, and string fields. They do not apply
to struct-typed fields.

### Fixed length: `<len:...>`

`<len:N>` requests exactly `N` units:

```text
byte<len:16> digest;          // 16 bytes
uint32<le><len:8> values;     // 8 uint32 elements, 32 bytes
utf8str<len:12> label;        // exactly 12 bytes
```

The unit depends on the field:

- `byte`: bytes;
- strings, including UTF-16: bytes; and
- integer types: elements.

For strings with fixed length, NUL bytes are data. No terminator is searched
for or consumed. A fixed UTF-16 byte length that is odd produces an
incomplete-code-unit error.

If the input is shorter than requested, the decoded node reports missing
bytes.

### Maximum length: `<max:...>`

`<max:N>` imposes an upper bound:

```text
asciistr<max:64> name;
uint16<le><max:20> samples;
byte<max:4096> availablePayload;
```

For strings, Breco searches for a NUL terminator within the byte limit and
consumes it when found. If no terminator occurs within the limit, the whole
limited region is the string and the field remains valid.

For integer arrays, only complete elements are consumed. An incomplete
trailing element is left for the next field. A maximum byte field consumes up
to the available number of bytes.

### Sentinel length: `<until:...>`

`<until:OP VALUE>` reads until the comparison succeeds:

```text
uint16<le><until:=0xFFFF> values;
byte<until:="END"> prefix;
```

The matching element or sentinel is not consumed. A following field can
decode it:

```text
struct SentinelRecord {
    uint16<le><until:=0xFFFF> values;
    /cond(=0xFFFF) uint16<le> terminator;
}
```

For a `byte` field, a string sentinel is encoded as UTF-8 and searched as a
byte sequence. Numeric byte sentinels compare one unsigned byte at a time.
String fields search in their own encoding. An empty string sentinel is
invalid.

If the condition is never found, the field consumes the scanned data and is
marked invalid.

## Directives

Field directives are written before a field as a slash, directive name, and
parenthesized argument. As elsewhere in the language, whitespace and comments
may separate these tokens. The file-level `/default` directive is described
under [Declarations and entries](#default-preview-entry).

### Bind a variable: `/var`

`/var(NAME)` binds the decoded value of a scalar integer or scalar `byte`:

```text
struct Blob {
    /var(payloadSize) uint32<le> size;
    byte<len:$payloadSize> payload;
}
```

The bound field cannot have a length modifier or `/repeat`. Strings and
structs cannot be bound. Variable names must be unique within one struct.

An unknown variable is accepted by the parser but causes a decode-time error
when evaluated.

### Repeat a field: `/repeat`

`/repeat(COUNT)` decodes the field repeatedly:

```text
struct Sample {
    int16<le> i;
    int16<le> q;
}

struct SampleBlock {
    /var(sampleCount) uint16<le> count;
    /repeat($sampleCount) Sample samples;
}
```

The count may be a non-negative integer expression. Repeating zero times is
valid and consumes no data. A truncated repetition marks its parent invalid
and reports missing bytes when the repeated item's static size is known.

`/repeat` is distinct from `<len>`:

- `/repeat(3) Sample samples;` repeats the entire `Sample` field;
- `uint16<len:3> samples;` makes one integer-array field with three elements.

A repeat may wrap a dynamically sized field, but each iteration must make
progress.

### Validate a field: `/cond`

`/cond(OP VALUE)` decodes a field and validates its result:

```text
struct Chunk {
    /cond(=0x43484E4B) uint32<be> magic;
    uint32<le> size;
}
```

If the comparison fails, the field and its immediately containing struct are
marked invalid, and no later members of that struct are decoded. Conditions
can validate fixed, maximum, and sentinel-length strings. A string operand can
also validate the UTF-8 bytes returned by a length-modified `byte` field:

```text
/cond(="PNG") asciistr<len:3> identifier;
/cond(="END") byte<max:3> marker;
```

String and byte-sequence conditions support equality only. Length-modified
integer fields produce arrays and cannot be compared because BrecoScript has
no array literals. `/cond` cannot be combined with `/repeat`.

`/cond(true)` and `/cond(false)` are Boolean forms. On an integer or scalar
`byte`, `true` means nonzero and `false` means zero:

```text
/cond(true) uint8 hasPayload;
/cond(false) uint16<le> reserved;
```

On a struct member, the Boolean form tests whether the nested struct decoded
validly. Nested struct invalidity is contained by default: the nested node
remains visibly invalid, but it does not automatically invalidate every
containing struct. Add `/cond(true)` to carry invalidity into the immediately
containing struct. `/cond(false)` instead requires the nested struct to be
invalid:

```text
struct Container {
    /cond(true) Header header;
    byte<len:16> payload;
}
```

This propagation is controlled one level at a time. If `Container` is itself
nested, its invalidity propagates farther only when that member also has
`/cond(true)`.

Different directives may be combined where their constraints permit:

```text
/var(version) /cond(=3) uint8 versionByte;
```

The same directive cannot appear twice on one field.

### Conditionally decode a field: `/when`

`/when(EXPR OP EXPR)` decides whether the next field is present:

```text
struct Header {
    /var(headerWords) uint32<le> headerWords;
    /when($headerWords = 19) uint64<le> extendedTimestamp;
}
```

If the expression is false, the field is omitted from the decoded tree and
consumes zero bytes. If it is true, the field decodes normally. `/when`
requires variables and expressions that have already been decoded, and it
cannot be combined with `/repeat` or a length modifier on the same field.

### Validate a struct after decoding: `/assert`

`/assert(EXPR OP EXPR);` is a struct-body item, not a field:

```text
struct Frame {
    /var(lengthWords) uint32<le> lengthWords;
    /var(itemCount) uint32<le> itemCount;
    /repeat($itemCount) uint32<le> items;

    /assert($lengthWords = 1 + $itemCount);
}
```

Assertions consume no bytes. They run after preceding members have decoded,
append a condition-like node to the tree, and invalidate the struct if the
comparison fails or cannot be evaluated. Already decoded members remain
visible.

## Expressions, variables, and scope

Length and repeat counts accept integer expressions made from literals,
variables, parentheses, and `+`, `-`, `*`, and `/`. Numeric `/cond`,
`/when`, `/assert`, and `<until>` operands use the same expression rules.
Conditions and sentinels also accept string literals. The `true` and `false`
literals are accepted only as the complete argument to `/cond`.

Runtime counts are limited to 1,000,000 elements. Counts must evaluate to a
non-negative integer. Division by zero and arithmetic overflow are decode-time
errors.

Each struct has its own variable scope. When a nested struct is decoded, its
bound variables are exported under the nested member's name:

```text
struct Header {
    /var(itemCount) uint16<le> count;
}

struct Item {
    uint32<le> id;
}

struct FileRecord {
    Header header;
    /repeat($header.itemCount) Item items;
}
```

The field named `count` is not referenced as `$header.count`; its binding is
named `itemCount`, so the path is `$header.itemCount`.

Lookup starts in the innermost scope and proceeds outward. Deeper nested paths
work the same way, for example `$container.header.itemCount`.

## Strings in detail

The four string modes differ in termination and cursor movement:

| Declaration | Behavior |
| --- | --- |
| `utf8str value;` | Requires and consumes a NUL terminator |
| `utf8str<len:N> value;` | Reads exactly N bytes; NUL is ordinary data |
| `utf8str<max:N> value;` | Reads at most N bytes; consumes NUL if found |
| `utf8str<until:="X"> value;` | Reads before `X`; leaves `X` unconsumed |

The same rules apply to `asciistr`. For `utf16str`, terminators and string
sentinels are UTF-16 code units in the field's effective byte order.

A default, unbounded string with no terminator is invalid. A `<max>` string
with no terminator is valid.

## Nesting and declaration order

Structs can contain previously declared structs:

```text
struct Coordinates {
    int32<le> latitude;
    int32<le> longitude;
}

struct LocationRecord {
    uint64<le> timestamp;
    Coordinates position;
}
```

Forward references are not supported. Reversing these declarations would
make `Coordinates` an unknown type when `LocationRecord` is parsed.

Static sizes can be determined only when every member has a fixed static
size. Strings, length modifiers, repeats, `/when`, and overflowing aggregate
sizes make the static size unknown. Dynamic structs still decode normally.

## Worked example: a length-prefixed message

This example combines validation, variables, fixed byte lengths, and nested
repeats:

```text
struct Attribute {
    uint8 type;
    /var(valueLength) uint8 length;
    byte<len:$valueLength> value;
}

struct Message {
    /cond(=0x4D534731) uint32<be> magic;
    /var(attributeCount) uint16<le> count;
    /repeat($attributeCount) Attribute attributes;
}
```

`Attribute` must be declared first. Each attribute has its own scope, so its
`valueLength` controls only its own value. `Message.attributeCount` controls
the number of complete nested attributes.

## Worked example: classic TIFF

TIFF demonstrates both BrecoScript's strengths and its offset limitation.
A classic TIFF file starts with:

- a two-byte byte-order marker (`II` or `MM`);
- the value 42 in that byte order; and
- a four-byte offset to the first image file directory (IFD).

Because BrecoScript has no branches that select a type layout, use the
declaration matching the file's marker.

### Little-endian TIFF header

```text
struct TiffHeaderLE {
    /cond(=0x4949) uint16<be> byteOrderMarker;
    /cond(=42) uint16<le> version;
    uint32<le> firstIfdOffset;
}
```

The marker is read as big endian only to make the literal match the visible
bytes `49 49`; its bytes are symmetric, so either byte order gives the same
value.

### Big-endian TIFF header

```text
struct TiffHeaderBE {
    /cond(=0x4D4D) uint16<be> byteOrderMarker;
    /cond(=42) uint16<be> version;
    uint32<be> firstIfdOffset;
}
```

Decode the appropriate header at offset 0 and note `firstIfdOffset`.

### Little-endian classic IFD

Each classic IFD entry is 12 bytes. To decode the first IFD in a
little-endian TIFF, use:

```text
struct TiffIfdEntryLE {
    uint16<le> tag;
    uint16<le> fieldType;
    uint32<le> valueCount;
    byte<len:4> valueOrOffset;
}

struct TiffIfdLE {
    /var(entryCount) uint16<le> count;
    /repeat($entryCount) TiffIfdEntryLE entries;
    uint32<le> nextIfdOffset;
}
```

Then start `TiffIfdLE` at the byte offset reported by the header. For example,
if `firstIfdOffset` is 8:

```sh
build/brecodump -s tiff-ifd-le.struct -i image.tif -e TiffIfdLE -ofs 8 -r 1
```

For big-endian TIFF, change every IFD integer decoration to `<be>`.

`valueOrOffset` is deliberately raw. TIFF stores a value inline when it fits
in four bytes and otherwise stores an offset to the value. BrecoScript cannot
branch on `fieldType` or seek to that offset, so inspect the raw four bytes or
decode the pointed-to data in a separate view at the calculated external
offset. The same approach is required for `nextIfdOffset`. BigTIFF uses
different field widths and needs a separate declaration.

## Worked example: PNG

PNG files are a signature followed by a sequence of self-describing chunks.
Each chunk has a four-byte big-endian length, a four-byte ASCII type, a
payload, and a four-byte CRC. The first chunk is always `IHDR`, which carries
the image dimensions and pixel format.

`examples/pnghead.brecostruct` provides several entry points:

| Entry | Extracts |
| --- | --- |
| `PNGHeader` | Signature, width, height, bit depth, color type, compression/filter/interlace methods |
| `PNGChunk` | One arbitrary chunk at a chosen offset (length, type, raw payload, CRC) |
| `PNGsRGBChunk`, `PNGgAMAChunk`, `PNGpHYsChunk` | Fixed-layout color and physical-scale ancillary chunks |
| `PNGtEXtChunk` | A `tEXt` comment chunk (`keyword\0text` in the raw payload) |
| `PNGtIMEChunk` | Last-modified timestamp when a `tIME` chunk is present |
| `PNGIENDChunk` | The zero-length end marker |
| `PNGWithColorMetadata` | `PNGHeader` plus `sRGB`, `gAMA`, and `pHYs` when they immediately follow `IHDR` |

Decode the header at offset 0:

```sh
build/brecodump -s examples/pnghead.brecostruct -i image.png -e PNGHeader -ofs 0 -r 1
```

`width` and `height` are the image size in pixels. `bitDepth` and `colorType`
describe the stored samples. The on-disk file size is not part of the PNG
layout; `brecodump` reports it in the JSON `metadata.byteCount` field.

To read a text metadata chunk such as `tEXt`, `zTXt`, or `iTXt`, locate the
chunk's starting byte offset externally and decode `PNGChunk` there. For
`breco-icon.png`, the `zTXt` chunk starts at offset 33:

```sh
build/brecodump -s examples/pnghead.brecostruct -i res/breco-icon.png \
    -e PNGChunk -ofs 33 -r 1
```

Inspect the `data` field as ASCII or UTF-8 to see the keyword and text bytes.
`PNGtEXtChunk` applies the same raw-payload layout when the file contains an
uncompressed `tEXt` chunk at the chosen offset.

When `sRGB`, `gAMA`, and `pHYs` appear in that order right after `IHDR`,
`PNGWithColorMetadata` decodes them in one pass:

```sh
build/brecodump -s examples/pnghead.brecostruct -i image.png \
    -e PNGWithColorMetadata -ofs 0 -r 1
```

`PNGpHYsChunk` adds a physical scale: `pixelsPerUnitX` and `pixelsPerUnitY`
with `unit` set to `1` for dots per meter. `PNGgAMAChunk.gammaTimes100000`
stores gamma multiplied by 100000.

PNG's main limitation in BrecoScript is the same as TIFF's: chunk order is not
fixed, there is no in-declaration dispatch on chunk type, and there is no seek
past large `IDAT` image-data chunks. Declarations must match the bytes at the
chosen offset. Use multiple views or `brecodump -ofs` for each chunk you need,
and reserve `PNGWithColorMetadata` for files whose ancillary chunks appear in
that exact order immediately after `IHDR`.

## Partial input and errors

Errors fall into two categories:

- Parse errors reject the declaration, for example an unknown type, duplicate
  name, malformed directive, or length modifier on a struct.
- Decode errors depend on the bytes, for example an unresolved variable,
  failed condition or assertion, missing string terminator, missing sentinel,
  excessive dynamic count, division by zero, or incomplete UTF-16 code unit.

Short fixed integer and byte reads can produce partial nodes with a
`bytesMissing` value. Failed `/cond`, unresolved runtime expressions, failed
`<until>`, and truncated repeated fields invalidate their current struct and
stop its later members as appropriate. Failed `/assert` invalidates its struct
after decoded members remain visible. A false `/when` omits its field and
consumes no bytes. Invalid nested structs are contained unless their member
has a Boolean `/cond`.

The decoded tree reports names, types, decorations, byte order, raw bytes,
values, missing-byte counts, child nodes, and validity/error information.

## Command-line use

The current command shape is:

```text
brecodump [-s STRUCT_DECLARATION_FILE] -i BINARY_FILE_NAME
          [-e ENTRY_NAME=last]
          [-ofs BYTE_OFFSET=0] [-bs BITSHIFT=0]
          [-r REPEATNUM=1] [-o OUTPUT_FILE=stdout] [-h|--help]
```

With `-s`, the declaration is read from the named UTF-8 file. Without `-s`,
it is read from standard input:

```sh
printf 'uint32<be> magic;\n' |
    build/brecodump -i input.bin -e magic -ofs 0
```

`-e` selects a visualizable struct, typedef, or standalone field by name. If
it is omitted, the last visualizable declaration in source order is used;
an unknown name is an error that lists the available entries. `-ofs` chooses
the byte offset before decoding, `-bs` applies the requested bit shift to the
selected input window, and `-r` chooses the consecutive entry count. JSON is
written to standard output unless `-o` is supplied. Its `metadata.entrypoint`
property records the effective entry name.

Within each decoded object, fields are serialized in declaration order, which
is also their order in the binary stream. A scalar field object starts with
`value`; a struct's `value` is an object containing its members, and a
repeated or numeric-array value is an array. Immediately after `value`,
`valid` is included for `/cond` fields and `/assert` nodes. It is followed by
`rawBytesHex` and the remaining metadata. Explicit `<le>` and `<be>`
decorations produce `endianness`; undecorated, byte, and structural fields
omit it.

## Struct view copy and export

In the GUI's Struct data tree, `Ctrl+C` copies the default value for every
selected scalar row, one row per line. Integer fields copy as decimal values,
string fields copy as decoded text, and `byte` fields copy as space-separated
unsigned decimal byte values. Right-clicking a scalar row also offers direct
value copies as default, uppercase spaced hex bytes, decimal bytes, or a
prefixed form:

```text
/path/to/file:offsetInBytesDecimal:GivenName:DataType > value
```

The prefixed action can format the value as hex bytes, decimal bytes, ASCII,
UTF-8, or UTF-16. UTF-16 uses the field's effective byte order.

The same context menu can copy or save JSON for `This item`, `Selected items`,
or `All Items`. `This item` writes the exact brecodump node object. `Selected
items` and `All Items` write a JSON array of those same node objects in tree
order, without the CLI metadata envelope.

Binary exports never add container framing. `Save as binary struct (source
endianness)` writes the captured bytes unchanged. `Save as binary (declared
endianness)` walks the selected tree in declaration order and reverses each
complete integer or UTF-16 unit only when an explicit `<le>` or `<be>` differs
from the data view's source byte-order setting. Undecorated fields, `byte`
fields, ASCII/UTF-8 strings, and incomplete trailing units are written
unchanged.

Breco remembers the last normally opened single source file and active byte
offset. On startup it repopulates the Open field and reloads that file if it is
still readable, clamping the saved offset to the file size. Missing files stay
visible in the Open field with missing-path feedback. Sources opened through
privileged access are not remembered or reopened automatically.

## Grammar sketch

This sketch summarizes the accepted source shape. It is descriptive rather
than a parser-generator grammar:

```text
file              := file-item+
file-item         := default-directive | declaration
default-directive := "/default" IDENT ";"?
declaration       := struct-declaration
                   | typedef-declaration
                   | standalone-field

struct-declaration := "struct" IDENT "{" struct-item+ "}" ";"?
struct-item       := field | assert-directive

typedef-declaration
                  := "typedef" type-spec IDENT ";"
                   | "typedef" "struct" IDENT? "{" struct-item+ "}" IDENT ";"

standalone-field  := field

field             := directive* type-spec modifier? IDENT? (";" | bitfield-block)
directive         := "/var(" IDENT ")"
                   | "/repeat(" count-expression ")"
                   | "/cond(" (comparison | BOOLEAN) ")"
                   | "/when(" two-sided-comparison ")"
assert-directive  := "/assert(" two-sided-comparison ")" ";"

type-spec         := endian? IDENT endian?
endian            := "<le>" | "<be>"

modifier          := "<len:" count-expression ">"
                   | "<max:" count-expression ">"
                   | "<until:" comparison ">"

count-expression  := INTEGER | variable
                   | count-expression ("+" | "-" | "*" | "/") count-expression
                   | "(" count-expression ")"
comparison        := ("=" | "<" | ">") value
two-sided-comparison := count-expression ("=" | "<" | ">") count-expression
value             := count-expression | STRING
variable          := "$" IDENT ("." IDENT)*
BOOLEAN           := "true" | "false"
bitfield-block    := "{" bitfield+ "}" ";"?
bitfield          := "bit" INTEGER IDENT ";"
                   | "bits" INTEGER ":" INTEGER IDENT ";"
```

An omitted field name is valid only for `/repeat`, where the type display
name becomes the field name.

## Language limits

BrecoScript intentionally does not implement:

- floating-point types;
- enums or named constants;
- unions or variant selection;
- pointers;
- includes or modules;
- absolute or relative seek operations;
- alignment or automatic padding;
- general Boolean expressions;
- `!=`, `==`, `<=`, or `>=`; or
- forward type references.

Use explicit byte fields for reserved/padding data. Use the view or
`brecodump -ofs` to start at externally calculated offsets. Formats whose
layout depends on indirect offsets, broad variant dispatch, or large tagged
unions may require multiple declarations and decode passes.
