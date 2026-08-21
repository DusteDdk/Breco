# BrecoLang references and relocatable export

Status: design only; no implementation is part of this document.

This is a new language/runtime feature built on the shipped lazy-decode and
forward-continuation foundation. It is not an extension of the continuation
token protocol itself. The existing `SequenceContinuation` remains the way a
sequence scan resumes; references add a graph above structural decoding, and
export consumes that graph.

## Executive decision

BrecoLang should gain a first-class, lazy `ref` value and an explicit compact
export profile. A reference declaration describes four separate things:

1. how to calculate a target address and bounded region;
2. how to identify the logical target so aliases and cycles can be recognized;
3. whether export follows the edge; and
4. for relocatable export only, how the stored reference carrier is re-encoded
   after the target is moved.

Target decoding is lazy. Evaluating a reference creates a small target handle,
not a `DecodedNode` subtree. The UI dereferences that handle on expansion.
Export uses a reference-discovery cursor which runs the interpreter with a
non-materializing sink, follows exportable edges, and spills large visited/edge
sets to bounded-memory backing stores.

The three export modes then have deliberately different contracts:

- **Null-pad** preserves every covered byte at its original coordinate relative
  to a declared address-space origin and writes zeros in all holes. It does not
  rewrite fields.
- **Embrace** emits the source hull from the first covered byte through the last
  and preserves every source byte in the holes. It does not rewrite fields and
  is rejected if trimming the prefix would change a surviving absolute address.
- **Compact + rewrite** is available only when the schema supplies an export
  profile. It assigns new locations/keys, rewrites reference carriers and stored
  derived fields, and can generate structures such as compact FAT copies.

All modes are asynchronous and stream directly from worker-owned sources to a
worker-owned destination file. Their RAM usage is bounded. Their cost is based
on discovered references and bytes read/written, not on unrelated bytes in the
containing blob. This cannot mean identical wall-clock throughput for every
layout: a 200 GiB null-padded or embraced output still performs 200 GiB of I/O,
and millions of fragmented reads are slower than long sequential reads. The
achievable promise is no full-blob buffering, no eager node graph, no work per
unreferenced source byte except bytes that the chosen output mode must emit, and
roughly constant MiB/s for comparable sequential layouts.

Reference support plus the two byte-preserving modes should ship before compact
rewrite. Compact rewrite is a substantially larger write-back and transformation
system, not merely another option on the existing span export.

## Grounding in the current implementation

The following are properties of the checked-out implementation, not assumptions
from the earlier lazy-decode design:

- `DecodeDocument` owns a program, thread-affine `ByteSource` instances, a
  `ResolvedShapeSnapshot`, materialized pages, and locator caches. Document work
  is serialized on one `BrecoDecodeWorker` thread.
- `InstanceLocator` identifies a structural instance by document generation,
  template path, and sequence indexes. It does not identify an arbitrary target
  by input/address/type, so it is insufficient as the canonical identity of a
  reference target.
- `ResolvedSequenceShape` supports arithmetic indexes and shipped forward-only
  `SequenceContinuation` tokens. Reference discovery through variable-length
  sequences should use those continuations rather than introduce another
  checkpoint/index mechanism.
- `DecodedTree` values and layouts refer to source bytes with `ByteSpanValue`;
  source data is not normally copied. This is useful for a materialized page,
  but constructing a node/value/layout for every referenced item would undo the
  lazy work.
- `CompositeSpanAccumulator` builds composite layouts incrementally during
  interpretation. A reference target must not be flattened into its owner's
  local composite layout; it is a graph edge whose target spans are included by
  reachability policy.
- Existing field syntax accepts `from <input> at <expression>` and `within
  bytes(...)`. That eagerly decodes an addressed child today. It is useful
  address syntax to reuse, but it has no reference identity, edge strength,
  alias/cycle handling, or rewrite semantics.
- A `computed` field is `StorageLayoutKind::Computed` and has no physical bytes.
  It can help calculate decode values, but it cannot be the target of a patch.
- The current `ExportSpanRequest` returns an in-memory `QVector<ByteSpanValue>`
  for a stored or arithmetic layout. It is a useful seed API, not yet a streaming
  cursor or a transitive reference export.
- Current document output is rendered into a `QTemporaryFile`, after which
  `renderOutputBlocking()` copies the temporary file to the caller's `QIODevice`
  on the GUI thread. The copy is a raw synchronous `while` loop which repeatedly
  reads and rewrites 1 MiB chunks without returning to or pumping the GUI event
  loop. Thus the current path writes every byte twice and freezes painting/input
  during the entire second copy. It is unsuitable for a 200 GiB export and must
  not be reused for this feature.
- `LimitSet::maxTransformOutput` currently defaults to 256 MiB. A 200 GiB export
  needs separate limits for in-memory transform buffers, logical output size,
  temporary planning storage, and destination free space. Raising that one limit
  would conflate unrelated protections.

## Terminology and invariants

The design uses these terms consistently:

- **Structural instance**: an instance already addressable by `InstanceLocator`,
  such as a record, field, or sequence item reached through normal nesting.
- **Reference edge**: a typed link from a structural/reference target instance
  to a target entity. Creating the edge does not materialize the target.
- **Target entity**: a canonical logical object addressed by a reference. Several
  edges may alias one entity, and entities may form cycles.
- **Target region**: the bounded byte region in which target decoding is allowed.
  A region bound is a safety boundary; it does not necessarily mean every byte
  in the region is covered for export.
- **Covered span**: source storage explicitly owned/preserved by a reachable
  entity. Computed values have no covered span.
- **Reachability closure**: the selected root plus target entities reached by
  export-following references, with aliases deduplicated and cycles terminated.
- **Address-space origin**: the source coordinate which becomes output offset
  zero for byte-preserving export.
- **Allocation unit**: the indivisible region placed by compact export, such as
  a boot sector, directory region, FAT table, or data cluster. Fields inside it
  are patch sites, not separately packed byte objects.

Core invariants are:

1. Decoding a reference never advances the containing structure's cursor.
2. Merely displaying a reference never implies that export follows it.
3. A target is decoded only when a semantic expression, UI request, graph scan,
   or export consumer explicitly dereferences it.
4. A graph edge records source provenance and a bounded target region even when
   the target has not been materialized.
5. Byte-preserving modes never change non-gap source bytes and never patch a
   reference. If coordinates would become invalid, they fail preflight.
6. Compact mode never guesses how a format encodes relocation. An export profile
   must declare placement and rewrite behavior.
7. All arithmetic on source addresses, output addresses, lengths, counts, and
   encoded values is checked for overflow and representation width.

## Address spaces and origins

A bare integer is not enough to describe a pointer. BrecoLang must preserve the
base against which an address was calculated. The initial address bases are:

```text
input_offset(x)   x bytes from logical offset zero of the named input
root_offset(x)    x bytes from the selected entry instance's start
self_offset(x)    x bytes from the containing target entity's start
```

The current `from data at expression` remains source-compatible and continues to
mean `input_offset(expression)`. New reference syntax should require one of the
explicit forms in new code; the compiler can warn when a relocatable/exported
reference uses the ambiguous legacy spelling.

The compiler lowers an address to:

```cpp
enum class AddressBaseKind { Input, EntryRoot, ContainingEntity };

struct ReferenceAddressDesc {
    InputId input;
    AddressBaseKind base;
    ExpressionId displacement;
    ExpressionId regionLength;
};
```

At runtime it becomes a checked logical input offset plus a region length. Source
offsets stored in existing `ByteSpanValue` are currently absoluteized through
`ByteSource::absoluteOffset()`. Reference identity and planning should use
`{InputId, logicalOffset}` internally, applying `absoluteOffset()` only at source
I/O/display boundaries. This avoids confusing a borrowed window's absolute blob
coordinate with the coordinate expected by the language.

This distinction corrects an ambiguity in the proposed tiers:

- If a format stores addresses from the containing input's offset zero,
  null-padding begins at input logical zero.
- If a FAT volume starts at blob offset `B` and its sector/cluster values are
  volume-relative, `root_offset(...)` makes `B` the export origin. The exported
  volume begins at output zero; it does not need `B` bytes of leading zeros.

The selected root records its export origin per input. A single-file export can
have only one output coordinate space. A strong edge into another input/address
space is therefore an error unless a future bundle or explicit input-mapping
profile defines where that input belongs.

## Language-level references

### Proposed declaration

The concrete surface spelling should be finalized with parser tests, but the
following is the intended language contract:

```breco
contents: ref Cluster(first_cluster)
    from data at root_offset(
        data_region_offset
        + (first_cluster - 2) * bytes_per_cluster)
    within bytes(bytes_per_cluster)
    key first_cluster
    follow
    rewrite {
        cluster_high = (target.@relocated_key >> 16) & 0xffff;
        cluster_low  = target.@relocated_key & 0xffff;
    };
```

`ref T(args)` produces a `Reference<T>`, not a `T`. Its clauses mean:

- `from ... at ...` and `within bytes(...)` calculate the bounded target.
- `key ...` supplies the format's logical identity, such as a cluster number.
  It is optional; the default key is the normalized input, address, region,
  target type, and target arguments.
- `follow` marks a strong export edge. `weak` creates a navigable link excluded
  from the default reachability closure. The initial design should require one
  of these words rather than choose an easy-to-miss default.
- `when condition` uses the existing conditional-field semantics for null and
  sentinel references. A false condition produces a null reference, not a
  dangling edge.
- `rewrite` is metadata for a named compact export profile. It maps physical
  source-backed carrier fields to export-time expressions. It is ignored by the
  two byte-preserving modes.

The `ref` statement itself has zero parent extent. The carrier fields (for
example `cluster_high` and `cluster_low`) remain ordinary decoded fields in the
record and retain their source layouts. The reference is a named semantic value
derived from them. This avoids hiding raw on-disk values and permits split or
packed encodings.

For the common single-carrier case, a shorthand may be added later:

```breco
next_cluster: u32le as ref Cluster
    at root_offset(cluster_offset(next_cluster))
    within bytes(bytes_per_cluster)
    follow
    rewrite target.@relocated_key;
```

The first implementation should use the explicit form; the shorthand is syntax
sugar and should not complicate the IR.

### Dereferencing and dependency behavior

A reference exposes metadata without reading the target:

```text
contents.@key
contents.@address
contents.@length
contents.@is_null
```

An explicit operation such as `deref(contents)` yields the target value and may
run its decoder. No normal member access should silently dereference: hidden I/O
would make scan cost and cycle behavior difficult to reason about. The compiler
tracks explicit dereferences as semantic dependencies. If a later decode
expression genuinely needs a target value, execution resolves it with the
normal budgets, cancellation, visited-stack cycle diagnostics, and document
thread affinity.

UI expansion does not evaluate a BrecoLang expression. It sends a target-page
request containing the reference handle and receives the target's structural
outline/window just as it does for a sequence locator.

### What counts as target coverage

`within bytes(n)` is a decode bound, not an assertion that all `n` bytes are part
of the export. By default, a reachable entity covers the union of its own
source-backed fields, `raw`/`preserve` statements, and non-reference structural
children. It does not automatically cover gaps or the storage of a referenced
target.

A schema can deliberately cover the full target region:

```breco
cluster: ref Cluster(...)
    ...
    cover region
    follow;
```

`cover decoded` is the default. `cover region` is useful for fixed allocation
units whose uninterpreted tail must survive, such as a data cluster. An
equivalent explicit `preserve remaining` inside `Cluster` is preferable when the
bytes have a meaningful field in the decoded model. This choice matters:
null-pad zeros uncovered bytes, embrace copies gap bytes, and compact packs
allocation units chosen by the export profile.

### IR additions

References should be represented explicitly rather than encoded as magic calls:

```cpp
enum class TypeKind {
    // existing kinds...
    Reference,
};

enum class StatementKind {
    // existing kinds...
    Reference,
};

enum class ReferenceStrength { Follow, Weak };
enum class ReferenceCoverage { DecodedStorage, WholeRegion };

struct ReferenceDesc {
    TypeId targetType;
    IdRange targetArguments;
    ReferenceAddressDesc address;
    IdRange keyExpressions;
    ReferenceStrength strength;
    ReferenceCoverage coverage;
    IdRange rewriteRules;
};

struct ReferenceRewriteDesc {
    // A compiler-resolved, source-backed field/bit-field in the owner.
    ValuePathId patchTarget;
    ExpressionId exportedValue;
};
```

`Statement` refers to a `ReferenceDesc` by ID. The compiler validates target
arguments, address/length types, key comparability, condition typing, and patch
targets. It records the expression slots that must remain live while creating a
reference. This reuses the existing extent/dependency analysis, but a reference
itself has `ParentAdvance::None`; target reads are random-access effects rather
than parent cursor advancement.

The compiler should also record whether an address/rewrite expression is
relocation-safe and whether a reference can be enumerated arithmetically in a
fixed-stride sequence. Exact extent information can still skip item parsing only
when the item has no data-dependent reference edges. A million fixed-size items
containing pointer fields must be read if those pointer values are needed; the
exact-byte fast path cannot invent their targets.

### Runtime handles and canonical identity

Add a small immutable handle carried in a decoded value and shape response:

```cpp
using ReferenceTemplateId = quint32;

struct ReferenceTargetKey {
    quint64 documentGeneration;
    InputId input;
    quint64 logicalOffset;
    quint64 regionLength;
    TypeId targetType;
    StableKey targetArguments;
    StableKey explicitKey;
};

struct ReferenceHandle {
    ReferenceTemplateId referenceTemplate;
    InstanceLocator owner;
    ReferenceTargetKey target;
    ReferenceStrength strength;
    bool isNull;
};

struct ReferenceTargetLocator {
    quint64 documentGeneration;
    ReferenceTargetKey target;
    QVector<StatementId> templatePathWithinTarget;
    QVector<quint64> sequenceIndexesWithinTarget;
};

using MaterializationLocator =
    std::variant<InstanceLocator, ReferenceTargetLocator>;
```

The actual representation may intern large argument/key payloads in the
`DecodeDocument`; the semantic identity must remain immutable. `InstanceLocator`
continues to locate normal structural instances. A separate
`ReferenceTargetLocator` (or a tagged `MaterializationLocator`) holds a canonical
`ReferenceTargetKey` and a path beneath that target. Overloading
`InstanceLocator::sequenceIndexes` with addresses would make aliases unstable
and is explicitly rejected.

An explicit key permits two addresses to denote one format entity, but it creates
an obligation: if the same key is later observed with inconsistent input,
address, type, arguments, or region, graph discovery reports a schema/data
conflict instead of picking one. Without `key`, full normalized target identity
is used, so same-address aliases naturally deduplicate.

`DecodedValueKind::Reference` stores an interned handle when a page actually
shows the reference. Shape and export sinks use the handle directly and do not
create a decoded value.

`DisplayPageRequest::root` and `MaterializedPageDelta::root` should evolve to the
tagged `MaterializationLocator`; the existing `InstanceLocator` alternative
remains unchanged for all shipped callers. Page results add compact resolved
reference rows/handles. The `DecodeDocument` validates and opens a target locator
by reconstructing a bounded cursor from its concrete target arguments and
normalized region; it does not search the materialized-node cache to discover
the address. The cache may then key target pages by canonical target locator so
aliases can reuse immutable data.

## Lazy target resolution and reachability

### Interpreter sinks

Reference execution should be separated from the object graph by a sink contract:

```cpp
class DecodeSink {
public:
    virtual void sourceSpan(const SourceSpanEvent&) = 0;
    virtual void reference(const ReferenceEvent&) = 0;
    virtual void scalar(const ScalarEvent&) = 0; // only when requested
};
```

Concrete sinks are:

- the existing page materializer, which creates `DecodedNode`, `DecodedValue`,
  and `StorageLayout` records for the requested page only;
- the shape sink, which records structural summaries and reference templates;
- the reachability sink, which emits compact entity/span/edge records and drops
  display values; and
- the streaming JSON/outform consumers, as they are migrated.

This is an internal factoring, not a public promise that every current
interpreter path is immediately rewritten. The important semantic requirement
is that reference discovery can execute without the page materializer.

When executing a `Reference` statement, the interpreter:

1. evaluates the condition, target arguments, address, region, and key with
   checked arithmetic;
2. validates that the bounded region is within the selected input;
3. emits a `ReferenceEvent` containing the immutable handle and patch metadata;
4. does not call `decodeType()` for the target unless the consumer explicitly
   asks to dereference it; and
5. does not add target spans to the current `CompositeSpanAccumulator`.

The owner record's carrier fields remain in its normal layout. If the page sink
is active, the reference itself appears as a small child row with its raw key,
resolved address, strength, and expansion state.

### Graph discovery cursor

Export begins with a `ReachabilityCursor` rooted at either an
`InstanceLocator` or `ReferenceTargetLocator`:

```cpp
struct ReachabilityRequest {
    DecodeDocumentHandle document;
    MaterializationLocator root;
    ReferenceSelection selection; // followed strengths/profile filters
    ReachabilityBudget budget;
    CancellationToken cancellation;
};

struct ReachabilityProgress {
    quint64 entitiesVisited;
    quint64 edgesVisited;
    quint64 coveredBytes;
    bool complete;
};
```

For each entity, it runs the interpreter with the reachability sink, emits local
covered spans and outgoing edges, and queues previously unseen followed targets.
Aliases are emitted as edges but decoded only once. A visited/in-progress state
terminates cycles; cycles are valid unless the target decoder itself performs a
semantic recursive dereference without an allowed base case.

Large sequences use the shipped mechanisms:

- arithmetic sequences can derive local spans/locators without decoded items
  when their bodies have no data-dependent reference effects;
- variable-length or data-dependent sequences resume with the returned
  `SequenceContinuation` and scan forward in bounded chunks; and
- cancellation and work budgets are checked between items, target entities, and
  source-read chunks.

There is no new periodic sequence index. A paused graph scan owns its frontier
and immutable sequence successor tokens; resuming the export job continues those
tokens on the document worker.

### Bounded graph storage

The logical graph can itself be very large. A 200 GiB volume with 4 KiB clusters
has about 52 million possible clusters; a C++ object and hash entry per cluster
is not a bounded-memory solution.

Introduce an internal `ReachabilityStore` abstraction with two implementations:

```text
InMemoryReachabilityStore    small jobs under a configurable RAM threshold
SpilledReachabilityStore     append-only records + external sort/merge
```

The spilled store maintains compact records for target keys, edges, local spans,
and patch sites. It periodically sorts bounded runs in a worker-owned temporary
directory, then merge-deduplicates them. It must expose sequential cursors over
targets, source-ordered spans, source-ordered patches, and destination-ordered
placement records. A custom fixed record/run format avoids making QtSql a new
runtime requirement; using an embedded database is an alternative worth
benchmarking, not a semantic dependency.

Append-only output alone is not sufficient for a cyclic graph: discovery needs
to know whether a target has already been queued or visited. The general spilled
implementation should use one of two equivalent external algorithms:

- an LSM-like set of immutable sorted key runs, each with a Bloom filter and
  sparse seek index, plus a bounded in-memory recent set; or
- breadth/depth work in bounded frontier batches, externally sort/deduplicate
  each candidate batch, merge-subtract the sorted visited set, then decode only
  the new keys.

The second algorithm has less random I/O and is the recommended starting point.
It may visit targets in a different order than the in-memory queue, which is why
relocation order must be independently declared and deterministic. Both methods
terminate aliases/cycles without holding all keys in a `QHash`.

Dense integer domains such as FAT cluster numbers get a specialized
`DenseIntegerSet`: a chunked bitset records reachability, and rank/popcount
support maps an old key to its packed ordinal. Fifty-two million membership bits
are roughly 6.5 MiB before indexing, dramatically smaller than 52 million map
entries. Sparse/general keys use the external sorted store.

RAM, temporary-disk, edge-count, source-read, elapsed-work, and nesting limits
are explicit request budgets. A budget stop returns `Paused` with job-owned
continuation state when safe to resume; insufficient destination/temp disk or a
hard schema limit returns an error. Tokens are opaque and valid only for the
same document generation and source identities.

## Common streaming export pipeline

The current `ExportSpanRequest` is retained for the existing direct-layout
operation. Reachable export adds a higher-level request:

```cpp
enum class ReachableExportMode { NullPadded, Embraced, CompactRewrite };
enum class DanglingTargetPolicy { FailExport, SkipEdgeAndDiagnose };

struct ReachableExportRequest {
    DecodeDocumentHandle document;
    MaterializationLocator root;
    ReachableExportMode mode;
    DanglingTargetPolicy danglingTargets =
        DanglingTargetPolicy::FailExport;
    std::optional<ExportProfileId> profile;
    QString destinationPath;
    ExportBudget budget;
    CancellationToken cancellation;
};

struct ExportProgress {
    ExportPhase phase;
    quint64 entitiesDiscovered;
    quint64 referencesDiscovered;
    quint64 sourceBytesRead;
    quint64 outputBytesWritten;
    std::optional<quint64> estimatedOutputBytes;
};
```

The worker opens a `QSaveFile` at `destinationPath`, reads the worker-owned
`ByteSource` in fixed-size chunks, writes directly to the destination, and calls
`commit()` only after successful completion. Cancellation abandons the
`QSaveFile`, leaving the previous destination intact. `QIODevice` and `QFile`
objects never cross threads.

The public controller API is asynchronous and reports preflight, progress,
completion, and diagnostics through queued signals. It does not call
`renderOutputBlocking()`, does not create a full-output `QTemporaryFile`, and
does not copy the result on the GUI thread.

For `NullPadded` and `Embraced`, `SkipEdgeAndDiagnose` is an explicit recovery
mode for damaged data. If a followed strong edge has a data-level resolution
failure—such as an out-of-input target, a violated target-region bound, or a
target decoder which fails/no-matches—the discovery transaction for that target
is discarded, the target is not enqueued, and the rest of the graph continues.
The unresolved edge, its carrier location, target address/key, reason, and graph
path are retained as a warning diagnostic and included in preflight counts.
Already reachable owner bytes, including the original dangling carrier value,
remain byte-for-byte covered under the selected tier; the exporter neither
clips a partially valid target nor silently repairs the reference.

This recovery does not swallow source I/O failures, source invalidation,
compiler/schema errors, internal errors, cancellation, or budget exhaustion.
`FailExport` remains the default. Compact mode rejects request-level
`SkipEdgeAndDiagnose`: omitting a target while preserving its carrier would make
relocation unsound. Compact recovery must be an explicit profile rule which
also defines the rewritten carrier value or replacement target.

Internally all modes produce a cursor of bounded output operations:

```cpp
using OutputOp = std::variant<CopyRun, ZeroRun, GeneratedRun, PatchScalar>;
```

`CopyRun` names an input/source offset, destination offset, and length. `ZeroRun`
names an output range. `GeneratedRun` streams schema-generated content.
`PatchScalar` is ordered with the destination bytes it modifies. Operations are
coalesced where possible and consumed with a fixed buffer (for example 1–8 MiB);
they are not accumulated in a `QVector` proportional to the export.

For the byte-preserving modes, graph discovery writes covered intervals to the
reachability store. An external source-order sort coalesces overlapping and
adjacent intervals, then feeds the output cursor. Work is proportional to the
number of reference/span records and bytes selected by the mode, not the source
blob's declared total length.

## Tier 1: null-padded reachable image

Let `C` be the coalesced union of covered spans from the selected root and every
transitively followed reference target in one export address space. Let `A` be
that address space's declared origin, and let `E = max(end(span))`.

The output represents `[A, E)`:

```text
destination offset = source logical offset - A
covered interval    = CopyRun
uncovered interval  = ZeroRun
```

The selected root's own source-backed layout is included. Reference declarations
add target coverage only when their edges are followed. Weak edges are not
followed unless the request/profile explicitly opts in. Computed values add no
bytes. `cover region` contributes the entire declared target region; otherwise
only decoded/preserved local spans contribute.

This mode preserves the numeric meaning of references based at `A` without
rewriting them. It also excludes unrelated source contents from holes. It may
produce a large file when the last reachable extent is far from the origin.

Zeros are emitted from a reusable zero buffer. A platform-specific sparse-file
optimization may seek/punch holes, but it is optional because sparse allocation
semantics vary. The logical file contents and length must be identical whether
the destination supports sparse files or not.

The owner's reading "export `[0, structureEnd)`" is therefore conditionally
correct: zero is the reference address-space origin, not necessarily the blob's
physical offset zero. For an input-absolute schema `A = 0`; for a root-relative
embedded FAT volume `A = root.startOffset`.

## Tier 2: embraced reachable image

Let `S = min(start(span))` and `E = max(end(span))` over `C`. Embrace emits the
source hull `[S, E)` as one byte-preserving stream:

```text
destination offset 0 = source logical offset S
every byte in [S,E)   = copied verbatim, including uncovered gaps
```

The graph still determines `S` and `E`; unlike null-pad it does not use coverage
to redact gaps. This is intentionally capable of preserving padding, unknown
metadata, and layout bytes between known structures. It may also preserve
deleted, stale, private, or otherwise unrelated data in those gaps, so the UI
must explain that consequence before export.

Because embrace does no patching, it is legal only if translating `[S,E)` to
output zero leaves all surviving reference meanings unchanged. Preflight proves
one of:

- the schema's address origin is exactly `S` and all followed references are
  relative to that movable origin; or
- every relevant address is otherwise invariant under the translation.

If an input-absolute reference survives and `S != 0`, or a reachable span lies
before the declared movable root, embrace fails with a diagnostic recommending
null-pad or a compact profile. It must not emit a plausibly shaped but invalid
file.

For a single source, emission can be one large sequential `CopyRun`; interval
records are needed to compute/validate the hull but not to copy each region
separately. Thus region fragmentation does not reduce emission throughput in
embrace mode, although reference discovery still pays for the edges it must
understand.

## Performance contract for tiers 1 and 2

The practical complexity targets are:

```text
Discovery: O(reachable entities + followed edges + data required to decode them)
Planning:  O(interval records log interval records) via external merge sort
Memory:    bounded by configured caches/run buffers
Emission:  O(logical output bytes), sequential where the source layout permits
```

No stage allocates a buffer the size of the source, the output, or the eager
decoded graph. Paged sources already bound source caching; the exporter adds
fixed read/write buffers and a bounded run-sort buffer.

The phrase "no performance difference compared with a 1 MiB system" should be
accepted as a per-MiB scalability requirement, not zero fixed overhead and not
identical throughput under arbitrary fragmentation. A 200 GiB embraced image
must read and write 200 GiB. A null-padded image ending at 200 GiB must write 200
GiB of logical zeros/copies unless sparse output is available. Conversely, a
compact export retaining only 1 GiB from a 200 GiB sparse blob should not scan or
buffer the unrelated 199 GiB merely because it exists.

Instrumentation should report discovery records/s, random source reads, bytes
read, bytes written, spill bytes, peak RAM, and phase times so this contract can
be tested rather than inferred from elapsed time alone.

## Tier 3: compact and rewrite

### Why a profile is required

The generic runtime can relocate source spans and encode a scalar, but it cannot
infer format invariants. FAT32, for example, has reserved sectors, one or more
FAT copies, cluster numbering starting at two, directory references split across
fields, end-of-chain sentinels, boot-sector capacity fields, and potentially
checksums or mirrored metadata. Packing every covered span consecutively and
adding a delta to every integer would not produce a filesystem.

Compact export is therefore enabled only by a named, compiler-checked export
profile associated with the selected type. The profile declares logical domains,
allocation units, placement, reference rewrites, generated regions, and stored
derived-field rewrites.

An illustrative—not yet final—surface form is:

```breco
export compact fat32_reachable(root: Fat32) {
    domain clusters from reachable(root, Cluster) {
        old_key = item.@key;
        new_key = ordinal + 2;
        order by old_key;
    }

    let kept_clusters = count(clusters);
    let fat_bytes = fat_storage_bytes(kept_clusters, root.bytes_per_sector);

    place root.boot at 0;
    generate root.fat_copies at root.reserved_bytes
        using fat32_fat(clusters, root.fat_count, fat_bytes);
    place clusters at root.data_region_offset
        + (item.@new_key - 2) * root.bytes_per_cluster
        size root.bytes_per_cluster;

    rewrite root.boot.total_sectors =
        ceil_div(export.@size, root.bytes_per_sector);
    rewrite root.boot.sectors_per_fat =
        fat_bytes / root.bytes_per_sector;
}
```

The language feature is declarative. It does not expose arbitrary destination
seeks or mutation during decode. Compiler lowering produces a deterministic
relocation plan description.

### Export-profile IR

The core IR concepts are:

```cpp
struct ExportProfileDesc {
    TypeId rootType;
    IdRange domains;
    IdRange placementRules;
    IdRange generationRules;
    IdRange derivedRewriteRules;
};

struct RelocationDomainDesc {
    ReferenceTargetSelector selector;
    ExpressionId oldKey;
    ExpressionId newKey;
    OrderingKind order;
};

struct PlacementRuleDesc {
    EntitySelector selector;
    ExpressionId destinationOffset;
    ExpressionId allocationSize;
    PlacementOverlapPolicy overlapPolicy;
};

struct StoredRewriteDesc {
    EntitySelector owner;
    ValuePathId patchTarget;
    ExpressionId exportedValue;
};
```

Export expressions can read immutable namespaces:

```text
source.*                  original decoded metadata
target.@old_key           referenced entity's original key
target.@relocated_key     referenced entity's assigned key
target.@output_offset     referenced entity's assigned byte offset
domain.<name>.count       retained entity count
domain.<name>.extent      planned output extent
export.@size              final planned logical size
```

They need checked integer and bitwise operations, shifts, masks, `ceil_div`,
alignment, and bounded reducers. Current expression support and outform encoder
helpers should be reused where appropriate, but this is not the existing
`computed` field feature: `computed` has no storage, while `rewrite` names an
existing source-backed destination field.

### Patch targets and encoding

A rewrite target must initially be a fixed-width `SourceSlice` or `BitSlice`
whose type, endianness, width, and containing allocation unit are known. The
compiler resolves the path and records that layout, so emission can use a common
checked scalar encoder extracted from the outform machinery.

Reference-local rewrites use the target's relocated identity. This supports FAT
directory entries whose cluster number occupies high and low 16-bit fields:

```breco
rewrite {
    cluster_high = (target.@relocated_key >> 16) & 0xffff;
    cluster_low  = target.@relocated_key & 0xffff;
}
```

Stored derived fields use profile-level `rewrite` rules, for example total
sectors from planned output size. The planner computes all plan-derived values
before emission. Content-derived checksums require a streaming reducer and
possibly a reserved patch followed by a final seek/write; those belong to the
advanced transformation phase unless the first target format requires them.

Variable-width rewrites are not permitted in the initial compact implementation.
They create a placement/re-encoding feedback loop and need a separately designed
fixed-point or size-pass protocol.

### Allocation and relocation

The planner operates on allocation units, not every field span. A profile may:

- keep a source unit at a fixed destination;
- pack reachable units sequentially with alignment;
- assign logical keys independently of byte offsets;
- reserve or generate table regions; and
- emit multiple derived copies of a generated region.

For FAT32, a useful deterministic policy is to sort retained cluster numbers,
assign new cluster numbers `2..N+1`, place their data at the profile's computed
data-region base, and generate each FAT copy from the relocated chain edges.
Directory reference fields are patched to the new starting cluster. Boot-sector
capacity fields are derived from the final layout. This preserves logical chain
order even if physical clusters are reordered by original key.

Traversal order should not determine placement: graph queue order may change
with caching or parallelism. Profiles must select a stable order such as source
offset, original logical key, or an explicit schema key.

### Bounded-memory multipass algorithm

Compact export needs at least two logical passes and normally three phases:

1. **Discover** the full followed reference closure. Emit entity, edge, local
   span, allocation-unit, patch-site, and generator-input records to the bounded
   reachability store.
2. **Plan** retained domains and destination layout. External-sort records by
   domain/key, assign relocated keys/offsets, compute counts/extents, validate
   conflicts, and emit a destination-ordered output plan. This is the first time
   `export.@size` and plan-derived stored fields are final.
3. **Emit** the plan sequentially. Copy source units through a fixed buffer,
   overlay destination-ordered scalar patches, stream generated units, and
   commit the destination atomically.

The relocation map is also bounded:

- a dense integer domain uses the reachability bitset plus rank/popcount to map
  `old cluster -> packed ordinal` without an object per cluster; and
- general keys use sorted `(oldKey, newKey, outputOffset)` runs with a bounded
  merge/index cache.

General reference-patch records are first sorted by `(targetDomain, oldKey)` and
merge-joined with those relocation runs. The joined records are then externally
sorted by destination. Emission therefore does not perform a random disk lookup
for every patch.

Patch records are externally sorted by destination allocation unit and relative
offset. During emission the writer loads one fixed-size block/unit, copies its
source bytes, applies all patches for that block, and writes it. It never loads
the complete filesystem, FAT, directory graph, or relocation plan into RAM.

Some generated structures, such as a FAT, are naturally streamed from the
relocated edge records in logical-key order. A generator cursor emits one output
chunk at a time and can emit multiple identical logical copies without retaining
the whole table.

Temporary disk use is proportional to graph/plan records, not total blob size,
but can still be substantial for tens of millions of edges. Preflight estimates
both destination and temporary space, and the request has hard limits. A future
resumable-across-process export would need stable plan files and source identity
validation; it is not required for the first implementation.

### Aliases, overlaps, cycles, and dangling references

The initial safety policies should be conservative:

- Multiple references to the same canonical target are aliases and allocate it
  once. All carrier fields are patched to the same relocated key.
- Reference cycles are legal and terminate through the visited set. They are
  patched after every member has an assigned relocation.
- Structural subfields within one allocation unit are expected overlaps; the
  unit is copied once and its fields become patch sites.
- Two independently placed allocation units that overlap in source or
  destination are an error unless the profile explicitly declares containment
  or a generation/overlay rule.
- Overlapping patches are an error in the first implementation, even if their
  computed bytes happen to match. Bit-slice patches can share a carrier only
  when the compiler proves their bit ranges are disjoint.
- A null/sentinel edge declared with `when` is not dangling.
- A missing or invalid strong target fails by default. Tiers 1/2 may instead use
  the request's explicit `SkipEdgeAndDiagnose` policy, with the transactional
  semantics defined above. Compact mode requires a profile recovery rule which
  also defines how the carrier is rewritten.
- A weak/external reference that survives compact output is an error by default.
  A profile may explicitly preserve, null, or map it; silent preservation is
  unsafe.
- An eager legacy `from ... at ...` child can contribute spans to tiers 1/2, but
  compact mode rejects relocation through it because there is no carrier/edge
  rewrite contract. Schemas intended for compact export must use `ref`.

These policies make it impossible for the graph to guarantee away every dangling
reference "by construction": the graph guarantees closure only over successfully
resolved followed edges. Invalid data, excluded weak edges, multi-input targets,
and profile filters still require explicit diagnostics/policy.

## Outform, JSON, and direct span consumers

This proposal does not make the UI's materialized tree the source of export.
Reachable binary export always runs the non-materializing graph/export cursors.
The saved pages are caches for display only.

The existing direct `ExportSpanRequest` remains useful for "save exactly this
materialized value's storage" and for fixed arithmetic items. It should grow a
streaming span-cursor implementation before it is used for layouts with very
many discontiguous spans; the returned `QVector` cannot represent millions of
spans with bounded RAM.

JSON already has a streaming re-execution path. It may later expose references
as metadata, inline followed targets with cycle/alias markers, or use a caller
chosen graph policy. Those are serialization-policy questions and do not block
binary export.

Phase B changes only the new reachable-binary-export path. Existing
`exportJson()` and `renderOutform()` still pass through
`renderOutputBlocking()` and retain its temporary-file plus synchronous GUI-copy
behavior until separately migrated.

Outform remains the largest migration risk from the lazy-decode design because
its current renderer assumes a fully materialized graph. Relocatable export must
not be implemented as a hidden outform that forces legacy materialization.
Reusable scalar encoders and expression evaluation can be extracted, but the
export planner/generators consume graph and relocation cursors directly. A
future outform provider can expose reference cursors and sequence continuations;
that is adjacent work, not a prerequisite for tier 1/2.

## UI and asynchronous worker integration

### User actions

The decoded tree view should add a context submenu on an eligible top-level
record/reference target:

```text
Save binary as
  Null-padded reachable image…
  Embraced reachable image…
  Compact image: fat32_reachable…
```

The existing toolbar "Save Binary" can remain the direct-layout export for
backward compatibility; its label/help should distinguish it from reachable
export. Compact entries appear only when the selected type has a compatible
profile. A mode-picker dialog is less discoverable than explicit actions, but a
shared preflight/confirmation dialog should show:

- export origin and expected output range/size;
- referenced and covered byte counts;
- null/gap byte count for null-pad;
- embraced gap byte count and the data-disclosure warning;
- followed, weak, dangling/skipped, and cross-input reference counts, plus the
  selected dangling-target policy;
- estimated temporary-plan space for compact mode; and
- the compact profile and relocation order.

For null-pad/embrace, the dialog exposes an advanced "Invalid referenced target"
choice: "Stop export" (the default) or "Skip target, keep the broken reference,
and diagnose." If default preflight encounters a dangling edge, the error UI may
offer to rerun with the skip policy; it must not silently downgrade the request.
Compact mode does not show this request-level choice.

Exact estimates may require the discovery phase. The file picker may precede
discovery, but no destination data is committed until preflight succeeds and the
user accepts warnings. The controller may keep a completed preflight plan under
a short-lived, document-generation-bound handle so confirmation does not repeat
the scan.

Reference navigation uses the existing lazy-model idiom. An unresolved reference
row reports that it can fetch a target, and expansion issues a reference-target
page request. The returned outline becomes the row's child subtree; sequences in
that target retain today's 64-item window and successor-token behavior. If the
target is already on the expansion ancestry, the model inserts a non-expandable
cycle/back-reference row. If another branch has materialized the same target,
the document may reuse its immutable page data while each model location keeps
its own parent/index bookkeeping.

### Controller/worker request

Add an asynchronous `BrecoDecodeController::requestReachableExport(...)`. It
queues the request to the same document-owning worker used by decode operations.
Signals return preflight, periodic progress, paused/budget status, completion,
and errors. Request generation and cancellation follow the shipped stale-result
rules.

The worker serialization is load-bearing for current `ByteSource` thread safety.
Immutable graph/token records make future parallel planning possible, but they do
not make the current sources safe to access concurrently. If export is later
moved to a dedicated worker so display requests can continue during a multi-hour
job, that worker must open independent `ByteSource`/file handles from a frozen
document descriptor; it must not share the existing `QFile`, page cache, hash
tables, or sequential/spooling source across threads.

A single serialized worker keeps the GUI event loop responsive, but a long
monolithic worker call would queue all further tree expansions. Each discovery
and planning phase should therefore process bounded chunks and requeue its next
step, or use a dedicated export worker with independent sources once that extra
concurrency is justified. Cancellation is checked at item, entity, external-sort
merge, generator, and I/O chunk boundaries.

Progress rates and byte totals use 64-bit values. All Qt file APIs and every
conversion to `qsizetype` are chunk-bounded; no 200 GiB length is narrowed to an
in-memory size.

## Source consistency and failure behavior

An export is based on one immutable logical document generation. Before every
phase and before commit, the worker verifies `ByteSourceIdentity` values. If a
source changes, the request is invalidated and the partial `QSaveFile` is
discarded. For operating systems where path identity cannot guarantee stable
contents while an open handle is mutated, document creation/export should hold
the stable read handle or document the snapshot limitation.

Diagnostics include a reference path from the selected root, the source carrier
location, target address/key, and schema span. This is essential for explaining
why a graph edge is dangling or why an embrace/relocation validation failed
without materializing the entire tree.

There are distinct outcomes:

- `Paused`: a resumable work budget expired; the job retains opaque worker-owned
  continuation/plan state.
- `Cancelled`: the caller requested cancellation; partial destination and job
  state are discarded.
- `Invalidated`: source identity/document generation changed.
- `Error`: schema/data conflict, a dangling strong edge under `FailExport`,
  overflow, plan collision, disk failure, or unsupported rewrite.

## Delivery recommendation

This should not be one delivery phase. Relative to the shipped lazy-decode and
continuation foundation, first-class references are a substantial language and
runtime feature, while compact rewrite is a new compiler-checked write-back and
external-planning subsystem.

### Phase A — references and lazy navigation

- Add explicit address-base, `Reference<T>`, `ref`, strength, key, condition, and
  coverage semantics to syntax and IR.
- Compile reference dependency/effect metadata and enforce bounded target
  regions.
- Add canonical target handles/locators, alias/cycle handling, reference events,
  and lazy reference expansion in `DecodedTreeModel`.
- Exercise reference discovery through arithmetic and forward-replay sequences
  without eager target materialization.

This phase can display a FAT/directory graph lazily but does not yet promise a
transitive saved image.

### Phase B — bounded reachability plus null-pad/embrace

- Implement the reachability sink/cursor and in-memory/spilled stores.
- Add externally sorted/coalesced span cursors and the two byte-preserving output
  policies.
- Add request-level `FailExport`/`SkipEdgeAndDiagnose` handling for dangling or
  invalid strong edges, transactional target discovery, diagnostic retention,
  and preflight counts for damaged inputs.
- Replace the blocking temporary-file path for these actions with a direct,
  asynchronous `QSaveFile` worker request, progress, cancellation, and preflight.
- Add security/coordinate validation for embrace and cross-input failures.
- Wire `BrecoLangPanel` and its decoded tree view to the explicit "Save binary
  as" context submenu, file selection, asynchronous preflight/confirmation,
  progress/cancellation UI, and completion/error reporting.

This is the recommended first user-visible export milestone. It provides useful,
safe FAT volume extraction without defining mutation semantics.

### Phase C — relocatable export foundation

- Add compact-profile IR, relocation domains, allocation units, stable placement,
  fixed-width reference patching, and plan-derived stored-field rewrites.
- Add dense-key bitset/rank mapping and general external relocation maps.
- Add bounded discovery/plan/emission passes, direct copied units, and conflict
  validation.

This supports formats whose retained structures can be copied and patched with
fixed-width scalar references.

### Phase D — format-grade transformations

- Add streaming generated regions/tables and mirrored copies (needed for a truly
  compact FAT, not just relocated clusters).
- For FAT32, cover FSInfo free-cluster/next-free fields and Long File Name entry
  chains/checksums in the schema/profile and its validation/generation rules.
- Add content reducers/checksums and controlled final patches.
- Add any variable-sized encoding/fixed-point planning only when a real format
  requires it.
- Consider multi-input bundles/mappings and resumable plan files.
- Migrate outform to provider/cursor semantics separately; never route compact
  export through eager outform rendering.

A complete FAT32 compact profile likely spans Phases C and D because rebuilding
FAT copies is generation, not merely reference patching. Calling all four phases
"soon" is plausible only as a staged program of work, not as a low-risk single
change.

## Acceptance criteria

### Reference semantics

- The same target reached through multiple fields has one canonical entity and
  multiple edges; cycles terminate without recursion overflow.
- Creating/displaying a reference handle does not decode its target or allocate
  target nodes. Expanding it materializes only the requested outline/windows.
- Explicit input/root/self bases resolve correctly for an entry at a nonzero blob
  offset, including `BorrowedWindowSource` absolute offsets.
- Invalid, overflowing, out-of-region, sentinel, strong, weak, and cross-input
  cases produce deterministic diagnostics.
- Variable-length reference discovery resumed through `SequenceContinuation`
  has boundary and semantic value equivalence with one uninterrupted scan.

### Byte-preserving export

- Given a model graph with aliases, cycles, weak edges, disjoint spans, and gaps,
  null-pad produces exactly `[origin, lastCoveredEnd)`, copies every covered byte
  at its origin-relative coordinate, and zeros every other byte.
- Embrace produces exactly `[firstCovered, lastCoveredEnd)` byte-for-byte,
  including gaps, when origin translation is safe; unsafe absolute references
  fail before destination commit.
- With `FailExport`, one invalid followed strong edge aborts preflight. With
  `SkipEdgeAndDiagnose`, the same edge is omitted transactionally, its owner and
  carrier bytes remain unchanged, other reachable targets are exported, and the
  warning identifies the carrier, target, reason, and graph path. Non-reference
  I/O/schema/internal failures still abort.
- A reference target outside the root's local composite layout is included only
  through a followed graph edge. A weak edge is excluded by default.
- Large span/edge sets spill under a small configured RAM limit and produce the
  same output as the in-memory store.
- Cancellation, source mutation, disk-full, and temporary-space exhaustion leave
  no committed partial replacement.
- The GUI paints and remains interactive while exporting; no full-output temp
  copy or destination write occurs on the GUI thread.

### Compact rewrite

- Aliased and cyclic targets receive one deterministic placement and all carrier
  fields encode the assigned key/offset.
- Split/bit-field, endian, minimum/maximum-width, overflow, and overlapping-patch
  cases are covered by encoder tests.
- Dense and spilled relocation maps produce identical plans and bytes.
- Stored derived counts/extents match the final planned output, not the source.
- Generated FAT/table copies are byte-equivalent to the declared relocated graph.
- Reopening the emitted file with the same BrecoLang schema/profile yields value
  equivalence for retained semantic fields, reference-edge equivalence under the
  old-to-new mapping, no dangling strong edges, and expected capacity/extent
  values. Matching only byte boundaries is not an adequate oracle.

### Scale and throughput

- Run the same schema at 1 MiB, 1 GiB, and a sparse/logically 200 GiB scale with
  comparable sequential layout. Peak RAM remains within the configured budget,
  decoded-node count remains proportional to requested UI pages, and steady-state
  emission MiB/s does not degrade with total size beyond a documented tolerance.
- Separately benchmark fragmentation/reference density so random I/O and
  per-edge costs are visible rather than misattributed to total blob size.
- Assert metrics for bytes actually read/written, graph/plan records, spill I/O,
  materialized nodes, and maximum buffer size. A compact export that retains a
  small subset must not read unreferenced payload merely to advance through the
  containing blob.
- Test outputs and offsets above 4 GiB and well above `qsizetype`-sized buffers;
  all buffers remain fixed-size and all file/address arithmetic remains 64-bit.

## Open questions and decisions still needed

1. **Final reference syntax.** The semantics above are required, but `ref`,
   `follow`/`weak`, `cover region`, and rewrite block spelling need grammar and
   usability review against real FAT32 source.
2. **Address-base ownership.** Is `root_offset` always the selected entry start,
   or may a nested type declare a named movable base such as `volume`? Named
   address spaces may be clearer for partition-table -> filesystem nesting.
3. **Canonical key.** Should explicit keys replace or supplement physical target
   identity? The recommendation is to supplement it and diagnose inconsistent
   observations, but formats with intentional address aliases need examples.
4. **Follow defaults.** This design requires an explicit strength initially.
   After experience, should `follow` become the default for `ref`, or is explicit
   export policy worth the verbosity?
5. **Coverage default.** Is `cover decoded` plus explicit `preserve` sufficient,
   or will fixed allocation formats use `cover region` often enough to make it a
   type-level declaration?
6. **Embrace with backward references.** If the first covered span precedes the
   selected root or movable address base, should embrace always fail, or may a
   schema declare the earlier span part of the same movable address space?
7. **Multiple inputs.** The initial recommendation is to reject a single-file
   export whose followed closure crosses inputs. A bundle or explicit address
   mapping needs its own output contract.
8. **Weak/external compact references.** The safe default is error. Profiles may
   eventually choose null, preserve, or externally map them; the allowed policy
   set needs real formats.
9. **Allocation overlap.** The proposed default permits structural containment
   within one declared unit and rejects independent overlaps. Overlay formats may
   need explicit precedence or copy-on-write regions.
10. **Placement order.** Source offset/original key is deterministic; graph order
    is not. FAT32 should choose whether preserving original cluster order or file
    traversal order is preferable for locality.
11. **FAT32 consistency policy.** How should cross-linked clusters, orphaned
    allocated chains, bad-cluster markers, multiple FAT copies that disagree, and
    directory loops affect reachability and compact output? FSInfo's
    free-cluster/next-free hints and Long File Name pseudo-entry chains/checksums
    also need explicit validate/preserve/rebuild policy. These are format policy,
    not generic graph mechanics.
12. **Capacity/minimum geometry.** A valid FAT32 result has specification-level
    constraints beyond `kept cluster count`; the FAT32 profile must define
    rounding, reserved entries/sectors, minimum cluster-count classification, and
    alignment. The generic runtime should expose checked arithmetic, not encode
    FAT-specific rules.
13. **Generated data scope.** Decide whether Phase C must include a minimal table
    generator to claim FAT32 support, or whether Phase C is deliberately generic
    infrastructure and the first complete FAT32 profile lands in Phase D.
14. **Checksums and final seek patches.** Plan-derived fields are straightforward.
    Content-derived checksums need reducers and a controlled post-write patch;
    their first required format should set the exact design.
15. **Sparse null runs.** Should the UI expose "sparse when supported," and which
    platforms/filesystems can guarantee reads return zeros with correct logical
    size? It must remain an optimization, never a semantic difference.
16. **Large-job scheduling.** Is serialized chunk/requeue sufficient, or should
    large export open independent source handles on a dedicated thread so page
    expansion stays fast? Sharing the current sources across threads is not safe.
17. **Temporary-plan format.** A custom external-sort run format is simple and
    dependency-free; an embedded database offers indexes but needs performance,
    recovery, and deployment evaluation.
18. **Source snapshot strength.** Define which mutations can be detected reliably
    on each platform and whether an open file handle is treated as the immutable
    export snapshot.
19. **UI eligibility.** Should reachable export be offered only on entry/root
    types, or on any referenced entity with a declared address origin? The latter
    is useful but makes null-pad/embrace origin choice more visible.

## Recommended immediate next step

Before implementing parser changes, write one small but realistic FAT32 schema
slice covering a boot record, a FAT entry, a split directory starting-cluster
field, and a data cluster. Use it to settle address-base, key, coverage, and
rewrite syntax and to define golden graphs for aliases, cycles, end-of-chain, and
dangling references. Then implement Phase A without any compact writer. This
keeps language semantics driven by the motivating format while preserving the
phase boundary between lazy graph discovery and write-back.
