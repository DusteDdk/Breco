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

For compact single-source schemas, `language breco 0.1` and the `inputs`
declaration may be omitted; BrecoLang then assumes one default input named
`data`. If the file contains exactly one parameterless `record` and no
`entry`, that record is also used as the default entry at the selected offset.
Explicit declarations keep their normal behavior.

The BrecoLang tab groups declared entries and parameterless records in its
decode selector. Selecting a record adapts it to the normal entry execution
path and starts it on the sole input, or on the declared default input when
multiple inputs exist. Records with parameters are omitted because the UI does
not collect record arguments. A declared default entry is selected when the
schema is loaded; editor recompiles preserve the current selection instead.

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

An anonymous record defines a one-off nested object inline:

```breco
header: {
    kind: u8
    size: u16le
}
```

It is equivalent to declaring a named record and using it as the field type,
but its generated type is not reusable by BrecoLang source. Its internal
`$anon_record_...` name may appear after named records in the UI selector for
direct inspection. It consumes the bytes actually decoded by its body and
supports the ordinary `when`, `from ... at ...`, and `within ...` field
modifiers.

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

### Boxed and inline selects

A named (boxed) select remains one variant-valued field. It accepts either
direct type arms or braced arms and creates the named `Select` node and JSON
member:

```breco
payload: select kind {
    1 => HeaderV1
    default => { raw remaining as unknown }
}
```

A bare (inline) select yields the chosen arm's named statements directly into
the enclosing object. It creates no select node, key, or wrapper:

```breco
select kind {
    1 => { samples: repeat count { sample: Sample16 } }
    2 => { samples: repeat count { sample: Sample32 } }
    default => raw remaining as unknown_payload
}
computed has_samples: bool = present(samples)
```

Conditional `select { when ... => ... else => ... }` supports the same two
forms. A bare arm must use named statements; a direct type arm such as
`1 => HeaderV1` is rejected because it has no yielded field name. Fields not
produced by every arm are optional and are omitted when absent. Same-named
fields in different arms are one variant field.

Separate bare selects in the same object may contribute the same field. Such a
field is an ordered aggregate: one contribution retains its scalar, object, or
sequence JSON shape; a second contribution promotes it to an array. Sequence
contributions append their elements without a nested array. `present`,
`count`, and outform `for` work on the resulting flattened stream. Because
promotion is statically possible, the decoded tree always shows one sequence
container for an emitted aggregate, including a single scalar contribution.
Ordinary fields and named-select fields cannot be reopened this way and
therefore conflict with a bare-select yield of the same name.

Streaming JSON remains one-pass for ordinary fields. Aggregate members are
held until their enclosing object finishes so non-contiguous contributors can
be combined in source order; memory for each such field is proportional to its
encoded value, and its key is emitted at object finalization.

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
