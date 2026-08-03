# BrecoScript next steps: `skip`, `switch`, and `union`

## Context

BrecoScript currently has a hand-written parser (`StructDeclarationParser`), an in-memory declaration graph (`StructureGraph`), and a direct tree-producing interpreter (`StructVisualizer`). There is no separate compiler or bytecode stage. The language is deliberately sequential: every decoded field advances one `ReadCursor`, and a nested struct starts at the cursor left by the preceding field. The language reference explicitly says that seeking, unions, and variant selection are not implemented.

That model is a good constraint to preserve. The three constructs below should be added as distinct concepts rather than made into special cases of ordinary fields:

- `skip` advances the cursor without interpreting bytes;
- `switch` selects exactly one layout and advances by that selected layout;
- `union` interprets multiple layouts from the same start offset and advances once, according to a defined extent rule.

Keeping those meanings separate will make declarations readable and prevent subtle cursor bugs.

## `skip`

### Current behavior

There is no `skip` keyword, directive, AST node, or interpreter operation. The parser's many `skipWhitespaceAndComments()` calls are only parser internals and are unrelated to the language.

Authors currently represent padding or ignored data as a named raw-byte field, for example `byte<len:16> reserved;`. The documentation recommends this workaround. Dynamic lengths can use an earlier `/var` binding, and `<max:N>` can consume only what remains. Because this is an ordinary field, it appears in the visualized tree and JSON export and retains all skipped bytes in `VisualizedNode::rawBytes`.

### Issues and limitations

- The workaround requires inventing names for bytes that are intentionally semantically absent. Repeated padding fields also collide with the rule that struct member names must be unique.
- Ignored regions clutter both the GUI tree and `brecodump` JSON. This is especially noticeable for alignment gaps, large payloads, and reserved areas.
- Raw skipped bytes are copied into a `QByteArray` and later serialized/displayed. For a large payload, a construct whose only purpose is cursor movement should not require an additional allocation proportional to its length.
- `<len:N>` on `byte` is close to skipping, but its unit and short-input behavior are inherited indirectly from field semantics. There is no dedicated way to say whether a short skip is an error, nor a clear skip-specific error such as "wanted 32 bytes, only 7 remain."
- There is no alignment form. Describing common binary layouts therefore requires manually calculating padding, and dynamic alignment cannot be expressed with the current arithmetic because the expression language has no remainder operator and no current-offset value.
- Omitting a name is currently permitted only for `/repeat`. Extending unnamed ordinary byte fields would blur the distinction between data that should be exported and data that should be discarded.

### Concrete suggested improvements

1. Add a struct-body statement with explicit byte units, preferably `skip(EXPR);`, rather than treating `skip` as a type or overloading `<len>`:

   ```text
   struct Record {
       uint8 kind;
       skip(3);
       /var(payloadSize) uint32<le> payloadSize;
       skip($payloadSize);
   }
   ```

   Reuse `IntExpression`, its overflow checks, and the existing 1,000,000 dynamic-element safety limit initially. Rename or generalize the diagnostic from "element count" to "byte count" for this operation.

2. Give `skip` fixed cursor semantics: a non-negative count advances by exactly that many bytes; insufficient input records `bytesMissing`, invalidates the containing struct, and prevents later members from decoding. Negative values, overflow, unknown variables, and values over the configured limit should be decode errors. `skip(0)` should be valid.

3. Represent it explicitly in the declaration graph (for example, a variant `StructItem` containing `StructMember`, `AssertItem`, and `SkipItem`) instead of manufacturing a `ByteType` member. `StructNode` currently stores fields and assertions separately, which already causes assertions to execute after all fields regardless of their source position. An ordered item list would give `skip`, future control flow, and assertions faithful source-order semantics.

4. By default, advance only the cursor and do not copy skipped bytes or emit a JSON property. In the GUI, either omit the node or show a lightweight node containing offset and length but no `rawBytes`. If visibility is useful, make it an explicit option or add a named form later; do not silently pay the memory/export cost for every skip.

5. Include constant skips in static layout calculation. A dynamic skip must make the enclosing static size unknown, just as `/when` and dynamic lengths do. Use checked `size_t`/`quint64` arithmetic when adding to the cursor so a very large count cannot wrap.

6. Add alignment as a separate follow-up operation, such as `align(4);`, with alignment required to be a positive power of two (or clearly document support for arbitrary positive alignment). It should compute padding from the entry-relative cursor, not the absolute file offset, unless an explicit absolute form is introduced. Avoid disguising alignment as a complicated `skip` expression.

7. Add parser and decode tests for constant and variable skips, zero, truncation, unknown variables, negative/overflowing expressions, size-limit enforcement, static-size inference, nested structs, entry repetition, source offsets after the skip, and confirmation that exports do not contain skipped data.

## `switch`

### Current behavior

There is no `switch` syntax or graph/runtime representation. The closest feature is `/when(EXPR OP EXPR)` on a single field. `/when` can compare integer expressions using only `=`, `<`, or `>`, and a false condition omits that field and consumes no bytes. Multiple `/when` fields can approximate a small dispatch, but each condition is independent.

The discriminator must normally have been exposed with `/var`, variables are scoped to a struct, and forward references are rejected. The TIFF and PNG documentation calls out the practical consequence: authors need separate declarations or decode passes when a layout depends on a tag or chunk type.

### Issues and limitations

- A chain of `/when` fields is not mutually exclusive. Overlapping predicates can decode two alternatives sequentially, which is not switch behavior and corrupts the cursor interpretation.
- There is no `default` arm, no exhaustiveness/error policy, no grouped case labels, and no duplicate-case detection.
- Each arm must be expressed as a field. Multi-field alternatives require a helper struct, increasing boilerplate and interacting with declaration-order/no-forward-reference restrictions.
- The comparison language is unnecessarily weak for dispatch: it lacks `!=`, ranges, Boolean composition, and named constants/enums. String or byte tag dispatch is also unavailable because `/when` only accepts two integer expressions, even though `/cond` already has string/byte comparison machinery.
- An unmatched chain silently consumes nothing. For a required variant this can make the following member decode at the wrong offset without a direct "unknown tag" error.
- The current `StructNode` model cannot preserve a control-flow construct among ordinary members without adding parallel collections or an ordered item representation. Variables exported by a selected nested struct also need a deliberate scope rule.

### Concrete suggested improvements

1. Introduce selection as a struct-body item. A compact first version could require a previously bound integer expression and field-valued arms:

   ```text
   switch ($kind) {
       case 1: HeaderV1 header;
       case 2: HeaderV2 header;
       case 3, 4: LegacyHeader header;
       default: byte<len:$payloadSize> unknown;
   }
   ```

   Exactly one arm must be selected, and only that arm may decode or advance the cursor. Case values should be constant integer expressions in the first release so duplicates can be rejected at parse time.

2. Define unmatched behavior explicitly. Prefer an error when no case matches and there is no `default`; silently consuming zero bytes is dangerous in a binary decoder. If optional dispatch is needed, support an explicit empty arm such as `default: skip(0);` rather than making it implicit.

3. Permit an arm block in a later or same release (`case 1: { ... }`) so variants do not require one-off named structs. Blocks need a lexical variable scope. Bindings created inside an arm should remain inside that arm unless an explicit export mechanism is designed; otherwise references after the switch become dependent on which case ran and hard to validate.

4. Store switches and their cases directly in an ordered AST/graph. At parse time validate duplicate/default arms and case-value compatibility. At runtime evaluate the selector once, choose via a lookup table for equality cases, decode one arm, and attach a single switch/selected-case node (or just the selected named field) to the visualization. Evaluating each case predicate in sequence is avoidable for the common tagged-union use case.

5. Make static sizing conservative but useful: if every arm has the same known static size, the switch has that size; otherwise its static size is unknown. A selected-arm truncation should use that arm's static size for `bytesMissing` and should invalidate the containing struct consistently with an ordinary field failure.

6. Initially keep `switch` to integer equality, which matches the current expression engine and permits efficient duplicate checking. Then consider string/byte selectors by generalizing evaluated scalar values and comparisons. Add `!=`, `<=`, `>=`, logical operators, enums/named constants, or range cases only as coherent expression-language work rather than switch-only syntax.

7. Decide how directives compose. A switch item should support an optional name for stable GUI/JSON output, but field directives such as `/var`, `/repeat`, and `/cond` should not be blindly accepted on the switch itself. If repetition is needed, put the switch in a named helper type or define control-item attributes explicitly.

8. Test first/middle/default selection, unmatched selection, duplicate values/defaults, grouped labels, selector evaluation failures, truncation, nested arm scopes and exports, cursor position after each arm, differing/equal static arm sizes, JSON/tree shape, and a regression showing that only one of two candidate layouts consumes bytes.

## `union`

### Current behavior

There is no union declaration, union type in `ResolvedType`, overlay cursor, or union decoding logic. The reference explicitly lists unions as unsupported. Every current struct member begins where the prior member ended, so writing several candidate fields always concatenates them rather than overlaying them.

`/when` can select optional sequential fields but cannot rewind the cursor. `/cond(true)` on nested structs can propagate validity, but trial-decoding several candidates would still consume from the shared cursor and conditions can have externally visible tree/error effects. Raw `byte<len:N>` plus separate top-level entry decoding is the current manual workaround.

### Issues and limitations

- C-style overlays and protocol payload variants cannot be represented as one entry. Users must duplicate declarations, inspect raw bytes, or invoke the decoder at the same offset multiple times.
- A union and a switch solve different problems. A switch selects one arm from a discriminator; an untagged union exposes multiple interpretations of the same bytes. Conflating them would make cursor advancement, validity, and output unpredictable.
- Dynamic-size alternatives make the union extent ambiguous. Advancing by the selected arm, the largest declared arm, the largest successfully decoded arm, or a fixed enclosing length all produce different results.
- Decoding every arm can multiply CPU, node count, and retained raw-byte memory. Nested or repeated unions could amplify this substantially, particularly with sentinel scans and large dynamic fields.
- Existing nested scopes export variables under a member name. Multiple union arms may export the same binding names with different values; merging them into the parent would be ambiguous.
- Existing validity propagation assumes one sequential decode result. In an overlay, one invalid interpretation should not necessarily invalidate the union if other arms are merely alternative views.

### Concrete suggested improvements

1. First decide whether the primary user need is tagged variants. If so, implement `switch` first; it is safer, cheaper, and covers the TIFF/PNG limitations cited in the documentation. Add a true untagged `union` only for formats that genuinely require simultaneous overlay views.

2. Use an explicit union type/declaration, not a directive on a struct. For example:

   ```text
   union WordView {
       uint32<le> value;
       byte<len:4> bytes;
       TwoHalves halves;
   }

   struct Record {
       WordView word;
       uint8 next;
   }
   ```

   Each arm must decode from a cloned cursor at the same start offset. After decoding arms, the parent cursor advances exactly once.

3. Make the initial extent rule statically safe: require every arm to have a known static size and advance by the maximum arm size, matching conventional union storage. Better still, require equal arm sizes in the first release and provide a precise parse error when they differ. This avoids data-dependent cursor movement and makes truncation accounting deterministic.

4. If dynamic arms are later supported, require an explicit union extent such as `union<len:EXPR>` (bytes) or a selected-arm policy tied to a discriminator. Do not infer extent from whichever speculative arm happened to read farthest: sentinel searches could consume the whole remaining input and unexpectedly move all following fields.

5. Keep each arm's variables in an isolated child scope and export none by default. If a union-level binding is eventually needed, it should refer to an explicitly selected arm or to a property common to all arms, not to an arbitrary decode order.

6. Define validity as metadata per arm. The union container should be structurally valid when its declared storage extent is available; an invalid arm remains visibly invalid but does not invalidate siblings or the union by default. Provide an explicit selection/validation mechanism when exactly one arm must succeed. Avoid implementing "first arm whose `/cond` passes" as implicit union behavior because arm order then changes semantics and speculative failures become control flow; that behavior belongs in a named `match`/switch facility.

7. Avoid duplicating backing bytes for each arm. Store source offset/length spans and decode values from a shared immutable input buffer where possible. At minimum, do not concatenate every arm's `rawBytes` into the union container, and consider lazy arm decoding in the GUI. Add depth, arm-count, and total-node/work limits so repeated nested unions cannot cause disproportionate decoding cost.

8. Extend `ResolvedType`, type resolution, entry listing, typedef handling, and static layout as a coherent change. Clarify whether named unions are visualizable top-level entries, whether `typedef union` exists, and whether forward references remain disallowed. Consistency with named structs suggests top-level visualization and typedef support, while retaining declaration-order resolution for the first release.

9. Define stable output: one union object containing one property per arm, with identical `sourceOffset` values and arm-specific lengths/validity. The union object's `sourceLength` should equal its storage extent, not the sum of child lengths. This is especially important because the current generic child-span calculation was designed for sequential/nested fields and must not accidentally treat overlapping children as concatenated storage.

10. Test equal and unequal static arms, truncation, signed/endianness interpretations, nested structs, union inside `/repeat`, typedefs and top-level entries, isolated variables, invalid-arm containment, source offsets/lengths, cursor advancement to the following field, JSON representation, and performance limits for many/nested arms. A key invariant test should assert that decoding more arms never advances the parent cursor more than the union's single declared extent.

## Further format-decoding gaps

The `switch` work above already covers tagged dispatch, an explicit `default`
arm, string/byte selectors, and coherent additions such as `!=` and logical
operators. Several related capabilities are still needed to describe a whole
chunked format such as PNG rather than isolated chunks.

### Post-tested repetition

`/repeat(EXPR)` requires a count known before the first iteration. `<until>`
can stop a primitive array or byte/string scan, but cannot repeat a struct and
test a field decoded by that struct. PNG consequently cannot express "decode
chunks through IEND" without scanning raw bytes for an IEND-like sequence,
which can produce a false match inside chunk data.

Add an explicitly post-tested repetition construct that decodes one complete
item before evaluating its termination condition, for example:

```text
repeat {
    PNGChunk chunk;
} until ($chunk.type = "IEND");
```

The terminating item should remain in the result and consume bytes. Define
zero-iteration behavior separately if a pre-tested `while` form is added.
Require every iteration to advance the cursor, retain the existing dynamic
element limit, and specify how truncation or an unevaluable termination
condition invalidates the loop and containing struct.

### Bindable byte sequences and FourCC values

`/var` currently binds only scalar integers and scalar `byte` fields. A
decoded string or fixed byte sequence therefore cannot act as a later loop or
switch discriminator even though `/cond` and `<until>` can compare such
values.

Generalize bindings and evaluated scalar values to include fixed strings and
byte sequences. A dedicated `fourcc` type or well-defined four-byte literal
syntax would make common chunk and box formats less dependent on string
encoding details. Specify whether bindings preserve raw bytes, decoded text,
or both, and initially allow equality only. Bound data must have a small,
explicit size limit so an arbitrary payload is not copied into variable
scope.

### Length-bounded struct decoding

A length-prefixed payload needs a child cursor limited to the declared byte
extent. Applying `<len:EXPR>` directly to a struct is currently rejected, so
a selected payload parser can neither be prevented from reading into the CRC
or following record nor verify that it consumed the entire payload.

Add a bounded struct/substream form, for example:

```text
PNGTextData<len:$length> data;
```

The nested decoder must see at most the bounded bytes. Define whether unused
bytes become an explicit remainder node, are skipped, or invalidate an
exact-length form; providing distinct exact and maximum forms may be clearer.
Truncation must report bytes missing relative to the declared extent, and the
parent cursor must advance by a deterministic documented amount even when the
nested decoder fails.

### Loop state and format invariants

Post-tested repetition alone cannot express ordering and multiplicity rules,
such as PNG requiring IHDR first, IEND last, at least one IDAT, consecutive
IDAT chunks, and PLTE only in permitted positions. Ordinary struct variables
are immutable decoded bindings and each repeated nested struct has an
isolated scope.

Design explicit loop state rather than implicitly leaking the last iteration's
bindings. Useful minimal operations are an iteration index, previous-item
access, and monotonic flags/counters updated by a loop body. State should be
lexically scoped, initialized explicitly, overflow checked, and visible to
post-loop assertions without making the decoded result depend on hidden
global mutation.

### Checksums and CRC validation

BrecoScript can display stored checksum fields but cannot calculate a value
over previously consumed bytes. PNG therefore cannot validate the CRC32 over
the chunk type and payload.

Add checked byte-span expressions and checksum functions, beginning with a
well-specified CRC32 variant rather than a PNG-only directive. A declaration
needs stable ways to name a member's raw span or the span between two labels,
for example an expression equivalent to
`crc32($chunk.type.raw + $chunk.data.raw)`. Algorithms must state polynomial,
initial value, reflection, and final-XOR parameters, and evaluation should
stream over backing data without concatenating large temporary arrays.

### Cursor and input-boundary expressions

There is no declaration-visible current offset, remaining-byte count, or
end-of-input predicate. A root structure can consume IEND yet cannot assert
that no trailing bytes remain, and dynamic alignment or size reconciliation
also requires cursor information.

Expose read-only, entry-relative cursor and bounded-input properties, such as
`offset()`, `remaining()`, and `atEnd()`. Keep absolute source offsets distinct
from entry-relative offsets, use unsigned checked arithmetic, and ensure a
bounded struct observes its child boundary rather than the entire backing
file. These values should be usable in assertions and loop conditions without
themselves moving the cursor.

### Payload transforms and filters

Structural decoding stops at compressed or transformed payloads. PNG image
data requires concatenating IDAT payloads, zlib/DEFLATE decompression, and
reversing scanline filters before samples can be interpreted.

Treat transforms as explicit bounded data sources rather than implicit field
side effects. A transform should consume a declared source span, expose an
immutable derived substream to a nested decoder, enforce input/output/work
limits, and report malformed or truncated data without changing the parent
cursor unpredictably. Start with reusable primitives such as zlib inflate;
format-specific stages such as PNG scanline unfiltering need parameters from
IHDR and should be layered on top. Multi-part inputs such as consecutive IDAT
chunks also require an explicit, bounded concatenation facility.
