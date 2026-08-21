# BrecoLang 0.1

BrecoLang describes binary data for both the Breco desktop application and
`brecodump`. A schema is compiled once into an immutable program and interpreted
against one or more named byte inputs.

## Minimal schema

```breco
language breco 0.1

inputs { input data "Packet file" { default } }

record Packet {
    identify { marker: u16be match marker == 0xCAFE else "not a packet" }
    commit
    flags: bitfield u8 { urgent: bit 7 kind: bits 3..0 }
    payload_length: u16le
    payload: region bytes(payload_length) { preserve remaining as bytes }
}

entry PacketAtOffset from data { packet: Packet }
default entry PacketAtOffset
```

The GUI loads `.breco` files from the BrecoLang tab. Bind each declared input,
select an entry and byte offset, then choose Decode. Editing recompiles after a
short debounce; successful edits re-decode the live view. Pin View preserves an
independent result while the live view changes.

From the command line:

```sh
brecodump --schema examples/png.breco \
  --input data=image.png --entry PNG
```

Use `--offset`, `--output`, and `--outform` as needed. JSON is written
incrementally. File destinations are atomically replaced only after decoding
and rendering succeed.

## Declarations

- `inputs` declares named sources. Exactly one may be marked `default`.
- `limits` sets `max_parse_depth`, `max_loop_iterations`, `max_nodes`,
  `max_probe_bytes`, and `max_transform_output`.
- `const NAME: TYPE = expression` declares a typed constant.
- `enum Name: integer_type` declares named integer values.
- `record Name(parameters)` declares a reusable decoder.
- `entry Name from input` declares a callable top-level decoder.
- `default entry Name` selects the default entry.
- `outform Name(root: Entry) text|binary` declares an export renderer.

Integer types are `u8`, `i8`, `u16le`, `u16be`, `i16le`, `i16be`, and the
corresponding 32-bit and 64-bit forms. Floating-point types are `f32le`,
`f32be`, `f64le`, and `f64be`.

## Record statements

Fields use `name: Type`. A field may read another source or location:

```breco
item: IndexItem from index at iteration * INDEX_BYTES
payload: Payload from data at item.offset within bytes(item.length)
```

Important statements are:

- `identify { ... match condition else "message" }` reads a tentative prefix.
- `commit` makes later failure fatal to the current alternative.
- `require condition else "message"` fails decoding immediately.
- `check condition else "message"` records validation without changing layout.
- `computed name: Type = expression` adds a value with no source bytes.
- `name: bitfield u32le { flag: bit 7 code: bits 3..0 }` adds virtual children
  sharing the containing word's source span.
- `name: region bytes(length) { ... }` bounds a nested cursor.
- `preserve remaining as name` and `raw remaining as name` expose undecoded
  bytes without copying them.
- `repeat count`, `repeat { ... } until condition`, `while condition`, and
  `many Type` decode sequences. `iteration`, `remaining`, and `at_end` are
  available where applicable.
- `select expression { value => Type default => ... }` dispatches by value;
  `select { when condition => ... else => ... }` dispatches by condition.
- `one_of { as First as Second }` tries alternatives transactionally.
- `recover Type { sync one_of { bytes [...] } step N byte max_probe SIZE gaps
  as Name }` emits valid items and explicit corruption gaps while resynchronizing.
- `continue when condition` and `break when condition` control loops.

Record calls may be recursive. Runtime depth and loop limits are always
enforced.

## Transactions and probes

`identify`, `one_of`, `many`, loops, and recovery use cursor/value checkpoints.
An uncommitted failed alternative rolls back completely. Streaming JSON first
runs transactional scopes without output, restores state, then replays the
successful path once. Probe mode executes the same semantics without allocating
tree nodes and is used by schema scanning.

## Outforms

Text and real binary outforms share a small statement language:

```breco
outform Csv(root: Rows) text {
    emit "timestamp,value,input,offset\n"
    for row, index in root.rows {
        if row.@valid {
            emit "${row.timestamp},${row.value},${csv(row.@input)},${row.@offset}\n"
        } else {
            emit "${index},invalid,,\n"
        }
    }
}

outform Index(root: Rows) binary {
    for row in root.rows {
        emit u64le(row.@offset)
        emit u32le(row.value)
    }
}
```

Statements are `emit`, `let`, `if`/`else`, and `for`. Interpolation uses
`${expression}`. Metadata includes `@name`, `@type`, `@value`, `@offset`,
`@length`, `@bytes`, `@path`, `@input`, and `@valid` where meaningful.

Text helpers include `csv`, `hex`, `hex_bytes`, `enum_name`, `string`, and
`int`. Binary integer encoders include `u8`, the signed/unsigned 16/32/64-bit
little- and big-endian forms, plus floating-point encoders. Integer encoders
reject values outside their declared ranges rather than truncating them.

## Shipped examples

- `examples/png.breco` decodes PNG chunks and includes a text outform.
- `examples/elf64.breco` follows section-table offsets with random access.
- `examples/portable_executable.breco` follows a relocated header.
- `examples/structure-library/` demonstrates scanning, recovery, multiple
  inputs, bitfields, and text/binary outforms.

All shipped examples are compiled by the test suite.
