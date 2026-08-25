# BrecoLang lazy decode and windowed materialization design

Status: Phases 1–2 are implemented; this document describes the remaining design. No implementation is included here.

## Executive summary

BrecoLang should stop treating “decode” as synonymous with “construct every `DecodedNode`, `DecodedValue`, `StorageLayout`, and child link.” The proposed result has two distinct planes:

1. A compact, immutable **resolved shape** says what the interpreter found: structural paths, runtime counts, types, nesting, aggregate byte extents, observed alternatives, and how item boundaries can be recovered.
2. A bounded, incrementally growable **materialized view** contains actual nodes, scalar values, and layouts only for requested depth and sequence windows.

The owner of both planes is a first-class `DecodeDocument` returned by the runtime. It owns the compiled program, replayable input sources, shape, sequence-start continuations, and a bounded materialization cache. In the GUI, the document is confined to a dedicated BrecoLang worker thread; the UI asks for windows through an asynchronous controller and receives immutable snapshots/deltas. Full-output consumers use streaming or pull cursors over the document; they do not grow a UI tree until it contains everything.

The key execution distinction is:

- **Fixed-stride, scan-safe counted sequences** bind one compiler-generated item template to `{start, count, stride}` and advance the cursor with one checked multiplication and one bounds check. They do not execute or store one result record per item during shape resolution.
- **Variable or semantically active sequences** execute a lightweight scan that finds real boundaries and counts, but keeps only aggregate shape data and an immutable continuation at the sequence start. Each bounded read returns a successor continuation containing the next offset and compact live interpreter state. Normal UI and outform access pass that token forward; a cold request without a usable token replays from the sequence start while discarding preceding items.

`ExtentSummary::exactBytes` is necessary but not sufficient for the fixed-stride fast path. The compiler must also prove that skipping item execution does not suppress checks, data-dependent control flow, required semantic values, external-input effects, or diagnostics.

```text
DecodeRequest
    |
    v
shape scan/resolve -----> DecodeDocument
                          | program + stable sources
                          | resolved shape
                          | arithmetic access/start continuations
                          | bounded materialization cache
                          |
             +------------+----------------+
             |                             |
             v                             v
DisplayPageRequest                   streaming/pull consumer
depth + list windows                 JSON, outform, binary
             |                             |
             v                             v
DecodedTreeModel delta               QIODevice / export result
```

## Goals and non-goals

### Goals

- Resolve runtime shape without eagerly creating the current full decoded object graph.
- Make a million fixed-size items effectively constant-size during shape resolution.
- Keep variable-size resolution linear in bytes/items while retaining state proportional to active sequence cursors rather than item count.
- Allow independent requests for depth and sequence ranges.
- Make follow-up requests true interpreter continuations/replays against the existing result, not fresh full decodes followed by truncation.
- Preserve exact decoding, validation, recovery, multi-input, and byte-layout semantics.
- Give the Qt model stable, bounded pages with `O(1)` row counts and child lookup.
- Let full-output consumers process all items with bounded memory.
- Keep BrecoLang execution independent of QWidget and model types.

### Non-goals

- This design does not change BrecoLang syntax.
- It does not make malformed or adversarial input unlimited; existing loop, depth, node, and probe limits remain, with new shape/continuation/materialization limits.
- It does not promise constant-time shape resolution for data-dependent or variable-length formats. Those must be scanned to determine their real shape.
- It does not introduce a general application-wide job framework. This design does mandate a concrete, dedicated BrecoLang worker thread and queued controller described below.
- It does not require immediate removal of `DecodedTree`. Small legacy tests and callers may temporarily request full materialization explicitly.

## Current constraints that drive the remaining design

- Phase 1–2 added `ResolveShape`, `MaterializePage`, `DecodeDocument`, and the worker-owned request path. Variable-length sequences still select `LegacyEager`, however, so their scan builds the general `DecodedValue`/node graph.
- [`execLoop()`](../src/brecolang/runtime/Interpreter.cpp) now batch-advances compiler-proven fixed-stride counted loops. On the non-fast path it still creates a sequence-item node, object/field values, and final sequence `valueRefs` for every iteration.
- [`setCompositeLayout()`](../src/brecolang/runtime/Interpreter.cpp) no longer has the shipped quadratic rescan, but eager paths still create per-item span/layout records and require a complete child store.
- [`ExtentSummary`](../src/brecolang/ir/BrecoProgram.h), `Statement.itemExtent`, and `LoopScanPlan` now drive the arithmetic fast path. The remaining problem is resumable semantic execution where a boundary cannot be calculated from `{start, count, stride}`.
- [`DecodedTreeModel`](../src/brecolang/gui/DecodedTreeModel.cpp) now merges bounded page trees and exposes the synthetic continuation row. Its page state is still `{firstItem, shown}` with no variable-sequence successor token, and `DecodeDocument::requestDisplayPage()` only selects arithmetic sequences lazily.
- [`OutformRenderer`](../src/brecolang/render/OutformRenderer.cpp) traverses `DecodedValue.fields`, `DecodedValue.elements`, node children, and stored layouts. It currently requires the full graph.
- JSON export already performs a separate streaming decode. That is closer to the desired model for a consumer that needs every item but does not need every item resident.
- `PagedFileSource` is naturally compatible with lazy reads, but source lifetime and mutation must be part of the new document contract. Sequential sources must be retained or spooled if earlier windows can be requested later.

## Terminology and invariants

### Program shape template

A compiler-produced structural template describes schema-level fields, records, sequences, alternatives, and relative layouts. It is independent of a particular input. A sequence item template exists once even if there are one million instances.

### Resolved shape

A resolved shape binds templates to a particular decode and records runtime facts such as counts, aggregate extents, observed branch statistics, and access strategy. It is not an enumerated tree of every item.

### Instance locator

An instance locator identifies a concrete runtime object without requiring a materialized node. Conceptually it is:

```text
template path + enclosing sequence indexes + decode-document generation
```

For example, `root.rows[123].payload.samples` is a locator even when row 123 has never been materialized. Locators, not `DecodedNodeId`, are the stable identity exposed across materialization requests.

### Materialized page

A materialized page contains actual display nodes and values for a bounded locator/depth/window request. Pages may overlap and are merged by locator. They never imply that the entire resolved shape has been instantiated.

The following invariants are required:

- A completed resolved shape reports an exact top-level status, end offset, and exact count for every concrete sequence instance represented directly in the shape.
- Fixed-stride indexes calculate item boundaries arithmetically.
- Variable access reproduces item boundaries by advancing a runtime-minted continuation. Cold replay from the sequence start and resumed replay from every returned continuation must land on the boundaries found by the original scan and compute identical semantic values.
- Materialization may add cache entries but may not change resolved counts or extents. A mismatch means the source changed or the continuation is invalid, and invalidates the document.
- Names such as `[123]` are presentation derived from the locator. They are never interned per item in the resolved shape.
- No UI object appears in the interpreter or decode-document API.

## Public runtime API

The declarations below are illustrative API shape, not implementation code.

### Initial request and result

Phase 1 made `ResolveShape` the normal GUI intent and retained `Tree`/legacy materialization for compatibility. `StreamingJson` may remain while streaming is generalized into a consumer API.

```cpp
enum class DecodeIntent {
    ResolveShape,
    StreamingJson,
    LegacyMaterializeAll,
};

enum class DecodeStatus {
    Success,
    Paused,
    NoMatch,
    Error,
    Invalidated,
};

struct ShapeScanOptions {
    quint64 maxShapeNodes = 100000;
    quint64 maxContinuationBytes = 4 * 1024 * 1024;
    WorkBudget budget;
};

struct DecodeRequest {
    std::shared_ptr<const BrecoProgram> program;
    QString entryName;
    QVector<std::shared_ptr<ByteSource>> inputs;
    quint64 startOffset = 0;
    DecodeIntent intent = DecodeIntent::ResolveShape;
    ShapeScanOptions shapeOptions;
    CancellationToken cancellation;
};

struct DecodeResult {
    DecodeStatus status;
    DecodeDocumentHandle document;
    std::shared_ptr<const ResolvedShapeSnapshot> shape;
    QVector<RuntimeDiagnostic> diagnostics;
    DecodeMetrics metrics;
};
```

`DecodeDocument` owns what `BrecoLangPanel::ViewState` currently keeps separately:

- compiled program;
- entry and starting offset;
- input sources plus source identities/generations;
- resolved shape and shape version;
- sequence-start and paused-scan continuations;
- bounded materialized pages;
- diagnostics and validity state.

The handle is opaque: callers cannot dereference mutable document or source state. The execution service that created the document resolves the handle and enforces its thread affinity. The initial result contains no `rootValue` that implies a fully materialized object. It contains a root `InstanceLocator`. Compatibility code may obtain a legacy root value only from an explicit full-materialization request.

### Follow-up materialization

```cpp
// The payload is an immutable, runtime-minted value blob. It contains no
// QObject, ByteSource, DecodedNodeId, or output-sink state.
struct SequenceContinuation {
    quint64 documentGeneration;
    quint64 shapeVersion;
    InstanceLocator sequence;
    quint64 nextItem;
    std::shared_ptr<const OpaqueContinuationState> state;
};

struct SequenceWindow {
    InstanceLocator sequence;
    quint64 firstItem = 0;
    quint64 itemCount = 64;
    // The only additive field needed by the variable-length path. On a
    // request, this is the successor returned by earlier work and is the
    // boundary from which to resume. In a returned delta it is the successor
    // after the actual returned range. Arithmetic windows leave it empty.
    std::optional<SequenceContinuation> successor;
};

struct DisplayPageRequest {
    DecodeDocumentHandle document;
    InstanceLocator root;
    quint32 maxDepth = 1;
    quint32 defaultSequenceItems = 64;
    QVector<SequenceWindow> sequenceWindows;
    quint64 maxNewNodes;
    quint64 maxNewBytes;
    WorkBudget budget;
    CancellationToken cancellation;
};

struct MaterializedPageDelta {
    InstanceLocator root;
    QVector<SequenceWindow> windows;
    std::shared_ptr<const DecodedTree> tree;
    bool legacyFullTree;
};

struct DisplayPageResult {
    DecodeStatus status;
    quint64 documentGeneration;
    QVector<MaterializedPageDelta> deltas;
    QVector<RuntimeDiagnostic> diagnostics;
    MaterializationMetrics metrics;
};

struct ExportSpanRequest {
    DecodeDocumentHandle document;
    InstanceLocator target;
    WorkBudget budget;
    CancellationToken cancellation;
};

struct ExportSpanResult {
    DecodeStatus status;
    quint64 documentGeneration;
    ResolvedSpanPlan spans;
    QVector<RuntimeDiagnostic> diagnostics;
};
```

These are the two concrete request shapes needed now:

- `DisplayPageRequest` always returns display nodes, scalar display values, and direct source spans for navigation. It does not construct full composite layouts.
- `ExportSpanRequest` resolves the exact direct/composite/bit-slice span plan for one target. A large dynamic plan may be a worker-owned streaming cursor rather than an in-memory vector.

The execution service accepts an existing document handle and returns generation-tagged snapshots or deltas:

```text
requestDisplayPage(request) -> DisplayPageResult
requestExportSpans(request) -> ExportSpanResult
```

Requests may combine a depth request and explicit sequence windows. `maxDepth` alone means “materialize composites down to this depth; whenever a sequence is encountered, use the default first page rather than expanding every item.” A node/byte budget remains authoritative, preventing a wide tree from exploding merely because its depth is small.

This is an additive evolution of the shipped API, not a replacement with `SequencePageSpec` or another parallel page contract. `SequenceWindow` gains the one optional `successor` member shown above; `DisplayPageResult` already returns the applied windows in `MaterializedPageDelta`, so each returned window can carry the token for its end boundary. The model stores it and copies it into the next outgoing window. The arithmetic fast path neither needs nor populates the field and continues to use `{sequence, firstItem, itemCount}` exactly as shipped.

Each returned variable sequence window carries the successor continuation for the first item after that window. The continuation is immutable: advancing it produces a new token and never consumes or mutates the input token. This permits safe retry/fork semantics and is required for future parallel materialization, but current cross-request isolation is enforced primarily by the single worker thread that serializes all `DecodeDocument` and `ByteSource` access. The token is validated against the document generation, sequence locator, expected item boundary, shape version, and source identities before use.

The public continuation is opaque rather than a caller-constructed `{type, offset, variables}` record. That phrase accurately describes its semantics, but an offset and a few named variables are insufficient for regions, multiple inputs, `while`/`many` termination, alternatives, and recovery. The runtime-minted payload includes all required execution state and cannot be used to resume an unrelated type or sequence.

The display request is not permission to omit semantic prerequisites. The compiler/runtime expands it to the transitive dependency closure needed to decode and compute the requested values correctly; prerequisite values can remain transient and need not appear in the returned page. General projection bitflags can be introduced later if a third real consumer combination justifies them.

### Shape continuation and cancellation

Variable-length shape scans can be long. A budget exhaustion should produce `Paused`, not a partially successful decode masquerading as complete. The document retains the current opaque scan continuation, and a follow-up `continueResolve(document, budget)` advances it. The UI can normally let a worker run to completion, but this contract allows cancellation, progress, and responsive shutdown without restarting completed scan work.

Shape snapshots state whether they are `Partial`, `Complete`, or `Failed`. Exact total counts are exposed only when the containing sequence is complete; partial shapes expose a discovered lower bound and current offset.

### Replay work-budget policy

The per-request cold/resumed replay limit counts variable-sequence items advanced, whether sent to `DiscardSink` or `MaterializationSink`. Arithmetic address calculation is not replay and is unaffected. When the next item would exceed that limit, the interpreter stops at the last committed item boundary and returns `DecodeStatus::Paused`—never `Success` with a silently truncated page and never a permanent error.

The paused result carries the successor token for that committed boundary. If budget was spent only seeking toward a cold `firstItem`, the result contains no display nodes; the caller resubmits the same target with that token and receives another bounded slice of work. If complete requested items were materialized before the pause, the result may contain that exact prefix with its actual `itemCount`; the model may merge it but must keep the logical request pending and request the remainder from the returned successor. No partial item is published. Cancellation discards unpublished item work, while the immutable input token remains reusable.

This replay-item limit does not claim to bound the cost of one pathological item. Statement/read work budgets, interpreter limits, and atomic cancellation checks remain responsible for intra-item responsiveness. The default replay limit should be at least the largest normal UI page so ordinary hot “show next” requests finish in one turn.

## Shape data model

### Shape nodes

A `ResolvedShape` is an append-only graph of structural templates and runtime bindings, not one node per runtime item. A shape node needs approximately:

```text
ShapeNodeId
kind
StatementId / SourceSpanId
SymbolId name policy
TypeId
parent template and child-template range
presence/cardinality summary
aggregate source extent/layout summary
optional SequenceShapeId
validation/diagnostic summary
```

Names normally refer to compiler symbols. Sequence-item names use a policy such as `IndexedItemName` and are formatted from the locator only when `data()` asks for them.

### Source extents and layouts

The shape should distinguish:

- `ContiguousExtent {input, start, length}`;
- `RelativeExtent {relativeOffset, length}` in a fixed item template;
- `CompositeExtent` for exceptional external or multi-input spans;
- `Computed` for nodes with no source bytes;
- `BitSlice` recipes for bit members.

A sequence records its aggregate primary extent and its item access recipe. It does not store one identical span and layout per fixed-size item. Adjacent exceptional spans should be coalesced into runs where possible.

### Sequence shape and access recipes

Each sequence has:

```text
item type and item-template ID
resolved item count or partial lower bound
total primary extent
observed item extent min/max
observed alternative/presence summaries
access strategy
scan completeness
```

Access strategies are:

1. `ArithmeticIndex {start, count, stride}` for fixed contiguous items.
2. `ForwardReplay {count, startContinuation}` for variable or semantically active items.

An arithmetic index is sufficient to locate every item and can bind one static item template to all indexes.

A forward-replay recipe stores one immutable continuation for item zero, not a boundary table. Reading a page from a supplied continuation costs `O(requestedItems)` semantic execution and returns the next continuation. Opening a cold cursor at item `i` restores the start continuation and executes `i` items into a discard sink before producing the page, so cold access is `O(i + requestedItems)` time and bounded memory.

There is deliberately no automatic periodic or dense index in the baseline design. The current UI grows prefixes in order, and full-output consumers traverse sequentially, so retaining every 1024th boundary and providing `O(log n)` checkpoint lookup would optimize an access pattern neither currently requires. If future telemetry or a real random-access feature shows repeated cold replay to be material, a bounded sparse continuation cache can be added inside `DecodeDocument` without changing the token or page API. Such a cache is an optional performance optimization, not part of correctness or the shape contract.

### Heterogeneous and nested shapes

Arbitrary data can contain a million different item lengths, branch selections, or nested counts. No lossless description of every individual choice can be both exact and sublinear in the worst case. The shape contract therefore separates aggregate facts from per-instance facts:

- The enclosing concrete sequence has an exact count and total extent after scan.
- Its item template lists possible structural children.
- The shape records observed alternative counts, presence counts, item min/max extent, and optionally run-length-compressed shape signatures.
- An exact branch, nested count, or extent for item `i` is obtained through its instance locator and replay/materialization.

For a nested sequence that occurs once, its exact count belongs directly in the shape. For a nested sequence occurring independently inside every outer item, the template stores aggregate family statistics; exact per-parent counts are lazy. If the project owner requires all per-parent nested counts in the initial shape, that requirement necessarily introduces `O(parentCount)` retained descriptors. This is an explicit product decision, not something a continuation policy can hide.

When a materialized outer item contains a nested sequence, that page returns a start continuation for the concrete nested sequence. The view or consumer retains it only while that nested cursor is active. If it is later discarded, the runtime replays the enclosing sequence from its start to re-derive the parent item and nested start. This avoids both one boundary table per outer sequence and one permanently retained token per nested sequence instance.

## Compiler and IR foundation

### Preserved item/body extents

`Statement.extent` is the aggregate statement extent. For a runtime repeat count it loses exactness even if the body is one byte. Phase 1 added the separate `Statement.itemExtent` and `staticItemTemplate` references used by the arithmetic path. Variable continuation execution should consume those same item/template descriptors rather than introducing a second type description.

### Concrete two-way loop scan plan

`exactBytes` must not be treated as permission to skip execution. The shipped compiler persists only the decision the interpreter needs, rather than a speculative multi-flag effect model:

```text
LoopScanPlan::BatchAdvance {itemExtent, staticItemTemplate}
LoopScanPlan::ExecuteItems {itemExtent}
```

The compiler selects `BatchAdvance` only after a conservative internal proof. It selects `ExecuteItems` if item control depends on bytes or `iteration`, per-item validation/diagnostics are possible, values are needed by later decode semantics, loop control can change the count, the structure is data-dependent, or external/random-access effects are not batch-provable. The remaining dependency/liveness work for continuations must preserve this conservative boundary. Rejection reasons may be exposed as compiler debug metrics, but they do not need ten persistent booleans in the IR.

### Fixed-stride fast-path conditions

A counted `repeat` can use arithmetic shape resolution when all of the following hold:

- the count evaluates successfully and is within `maxLoopIterations`;
- there is no `until` or loop-control path affecting the actual count;
- the item body has an exact nonzero contiguous parent advance;
- the structural item template is data-independent;
- the compiler selected `LoopScanPlan::BatchAdvance`;
- `count * stride` does not overflow;
- the containing region/source has at least the total byte count;
- external and multi-input effects are absent, or a future richer affine plan proves them safe.

For known-size random-access input, the interpreter need not read item bytes at all during shape resolution. For an unknown-size sequential source it must still pull/spool enough bytes to prove availability, but it can do so in large chunks rather than one item at a time.

### Static shape templates

The shipped compiler constructs reusable item templates for scan-safe fixed layouts, including relative offsets of fields and nested fixed structures. This makes materializing item 900,000 arithmetic: bind the template at `start + index * stride`, then execute only the dependency closure required for its display page. Variable replay reuses the same template identity while deriving concrete boundaries by execution.

## Interpreter architecture

### Separate semantic execution from result storage

The current `DecodedValue` store serves two jobs:

1. temporary semantic values used by decode expressions;
2. durable values exposed to the tree and outforms.

That is why `Probe` mode still creates roughly two million values in the benchmark. Split these jobs:

- `RuntimeValueStore` holds compact scalar/string/span/object summaries needed while executing. Frame slots refer to it.
- `ShapeSink` records aggregate shape and sequence-start/current-scan continuations but no display nodes.
- `MaterializationSink` constructs bounded node/value/layout pages for requested locators.
- `StreamingSink` writes JSON or feeds another sequential consumer.

Sequence runtime values become `SequenceRef {shape/instance locator, count}` rather than an array of every element value. Compiler liveness/dependency analysis identifies which scalar/object fields must survive in an enclosing frame. Per-item values that do not escape are discarded or arena-rolled back at the next item boundary.

The interpreter remains one semantic engine. Sinks control durable output, not decoding correctness.

### First-class forward continuations

Do not retain a suspended recursive C++ call stack. A continuation is explicit serializable interpreter state at a committed item boundary:

```text
sequence instance locator
next item index
cursor/input and containing region bounds
explicit continuation frames / statement PCs
compact scope-frame values needed by later expressions
loop iteration and initializer state
commit/probe state required by semantics
source generation
optional boundary verification token
```

“Serializable” here means pointer-free/snapshot-able interpreter state, not a stable on-disk ABI. Tokens are valid only for the originating live document generation unless a future version deliberately defines source-snapshot and program-version persistence.

Frame snapshots should share immutable runtime values copy-on-write. A continuation must not reference `DecodedNodeId`, `DecodedValueId`, a `QModelIndex`, a mutable `ByteSource`, or an output writer. This makes it sink-neutral: the same boundary can next be consumed by a display materializer, an outform provider, a layout cursor, or a discard scan.

Continuations are returned only at item zero or after a successful item or committed recovery action. Speculative `many`, `oneof`, and recovery attempts use local transactional marks and publish nothing until committed.

The token is immutable and copyable. “One continuation” means one current token per active cursor chain, not one global mutable position in `DecodeDocument`. Advancing token `T` by `N` items produces a page plus successor `T'`; `T` remains valid and can be retried or forked. Caller-held tokens own or share their compact immutable payload independently of the materialized-page cache, so evicting page nodes cannot invalidate an active cursor. Discarding the last token releases that state. The runtime may reject creating an oversized token under `maxContinuationBytes`, but it must never silently evict a live token.

A token can only be compact when the sequence has compact live state. Compiler dependency/liveness analysis must capture every later-needed value. A schema that deliberately accumulates an unbounded sequence of prior item values has an `O(N)` semantic-state lower bound; it cannot be advertised as bounded-memory lazy merely by wrapping that state in a token.

### Variable-length forward-resume strategy

During shape scan:

1. Mint and retain the sequence-start continuation at item zero.
2. Execute items with `ShapeSink` and compact runtime values.
3. Update count, aggregate extent, branch/presence summaries, and diagnostics.
4. On a work-budget pause, return the current scan continuation so `continueResolve()` resumes exactly there.
5. On completion, retain the exact count/extent and the start continuation. The scan's transient current token may be released once enclosing execution no longer needs it.

The scan therefore retains `O(active scan depth + directly represented sequence starts)` continuation state, not `O(itemCount / interval)`. A nested sequence occurring once can keep a start token with its concrete shape entry. A nested sequence family occurring once per outer item does not retain a million starts; materializing an outer item produces its nested start token on demand.

For the normal requested page after a caller-held token:

1. Validate the document and source generation.
2. Validate that the token belongs to this sequence and its `nextItem` equals the request's `firstItem`.
3. Restore its explicit continuation and compact environment.
4. Execute the requested items with `MaterializationSink`, honoring the display-page depth and node/byte budgets.
5. Return the page and a successor token at the requested end.
6. Verify that known count/end information and observed boundary/shape signatures match the original scan.

For a cold requested page `[first, first + count)` with no usable token, restore the sequence-start continuation, execute `first` items with `DiscardSink`, then follow the same materialization steps. This is bounded-memory but `O(first + count)` time. It has the same semantic execution order as eagerly decoding the prefix while avoiding its durable node/value/layout allocations. Repeated backward or random requests can cost `O(sum(firstItem))`, up to `O(kN)` for `k` far requests; the current sequential UI and export flows do not have that pattern.

The API should expose cold `firstItem` for correctness and tooling, but UI “show next” must always pass the successor token from the currently shown prefix. The runtime records cold replay metrics so a future random-access feature can justify an optional bounded sparse-token cache based on evidence rather than prebuilding an index for every document.

### `execLoop()` changes

`execLoop()` should become policy-driven rather than always constructing an `items` vector.

In shape mode it should:

- evaluate initializers and count into compact runtime values;
- create one sequence shape binding and one item template reference;
- take the arithmetic fast path when the compiler scan plan permits;
- otherwise scan items, update aggregate shape, retain only the start/current continuation, and publish a successor token when a bounded operation stops;
- return a `SequenceRef` containing locator and count, not one value ID per item.

In materialization mode it should:

- know the requested pages for the current sequence instance;
- skip arithmetic ranges directly, resume variable ranges from the supplied token, or cold-replay them from the sequence start;
- construct item nodes/values only inside requested pages;
- apply the depth policy to nested composites and the default page policy to nested sequences;
- preserve transient semantic execution for nonmaterialized items when it affects requested results.

In streaming mode it should iterate values directly into the consumer. It should not build an intermediate sequence value-reference array.

`execMany()` and `execRecover()` use the same forward-continuation abstraction. Their stop/failure/recovery behavior remains semantic execution, so they normally scan rather than use the arithmetic path. A token may be published only after the `many` item or recovery action commits; its explicit program counter and commit state distinguish “before probe,” “after recovered gap,” and ordinary item boundaries. Recovery gap occurrences are counted and summarized; exact gap rows are materialized by forward or cold replay. If every byte produces a distinct semantic gap and the product requires every gap descriptor upfront, linear storage is unavoidable and should be subject to a dedicated limit.

### Composite layouts without store rescans

Replace “finish a node by searching already-created nodes” with a `CompositeSpanAccumulator` on the execution stack:

- entering a composite records its primary input/start and opens an accumulator;
- a completed child contributes its span recipe directly to its parent accumulator;
- adjacent compatible spans are coalesced immediately;
- closing the composite yields an aggregate extent/layout recipe;
- in shape mode the recipe is stored once on the shape/template;
- in materialization mode a concrete `StorageLayout` is created only for returned nodes;
- in streaming mode spans are forwarded or discarded according to consumer needs.

This removes the need for `setCompositeLayout()` to inspect the node store at all. It also avoids duplicating one source span and one layout for every fixed item. Fixed item layouts are template-relative; variable item layouts are reconstructed during requested replay.

### Transactions, errors, and diagnostics

Existing rollback semantics must cover all sinks:

- runtime-value arena mark;
- shape aggregate mark;
- sequence-shape/continuation-publication mark;
- materialized-page mark;
- diagnostics mark;
- cursor/probe/commit state.

Speculative failures leave no shape observations or materialized nodes behind.

Replay must also be observationally idempotent. Shape scanning owns document-level diagnostic counting; a later display/outform/layout replay verifies the same diagnostic decisions and may attach page-local error metadata, but it must not append duplicate global diagnostics or repeat any external/UI side effect. A mismatch invalidates the document. This is another reason the continuation and semantic engine cannot contain an output writer or UI callback.

Diagnostics can themselves defeat bounded memory if a check emits one warning per item. Add a diagnostic policy: retain the first configurable number, aggregate repeated diagnostics by code/schema location with counts and first/last input offsets, and expose a streaming diagnostic cursor if exact enumeration is required. The choice of default cap is an open product question.

## Mandatory asynchronous execution mechanism

The shipped GUI integration uses one dedicated, long-lived BrecoLang `QThread`, not ad hoc `QtConcurrent::run()` calls. A single worker gives every mutable document and source an unambiguous owner and serializes resolve, materialize, and export operations.

### Qt objects and request flow

- `BrecoDecodeController` is a `QObject` living on the GUI thread. `BrecoLangPanel` talks only to this controller; it never calls `decodeBrecoProgram()` or a `DecodeDocument` method directly.
- `BrecoDecodeWorker` is a `QObject` moved to the dedicated `QThread`. It owns a map from opaque `DecodeDocumentHandle` values to actual `DecodeDocument` instances.
- Resolve, continue, display-page, span/export, and document-close requests are delivered to worker slots with queued connections. Each worker slot invokes the synchronous core interpreter on the worker thread.
- The panel submits a `ResolveJob` containing the immutable program, entry/offset, generation, and input-binding specifications such as paths—not opened `ByteSource` objects. The worker creates and opens `PagedFileSource`, `SequentialSource`, or `SpoolingSource` instances on its own thread, then constructs the core `DecodeRequest`. The GUI must not open a `PagedFileSource` and hand its `QFile` across threads.
- Results are immutable snapshots/deltas and diagnostics registered as Qt metatypes, returned to the controller through queued signals. Only the GUI thread mutates `DecodedTreeModel`.

```text
GUI thread                                      BrecoLang worker QThread

BrecoLangPanel -> BrecoDecodeController
                   | queued ResolveJob -----------------> BrecoDecodeWorker
                   |                                      opens ByteSources
                   |                                      owns DecodeDocument
                   | <--------- immutable snapshot/delta |
                   v
             DecodedTreeModel
```

The controller is mandatory infrastructure in the minimal foundation. The core synchronous API remains useful for CLI and unit-test callers that already run on an appropriate thread, but `BrecoLangPanel` does not include a synchronous fallback.

### Cancellation that works while the worker is busy

A queued `cancel` slot is insufficient because the worker event loop cannot service it while a long interpreter call is running. Every submitted request therefore carries a shared `RequestControl` containing an atomic cancellation flag and request generation. The GUI-side controller sets that atomic directly when a view is replaced, closed, or explicitly cancelled. The interpreter checks it at statement boundaries, every loop iteration, replay intervals, and chunked source operations. Work budgets provide an additional bounded yield point for shape continuation.

The queued cancel/close message performs cleanup once the running call returns, but correctness and responsiveness do not depend on that message being processed first.

### Stale-response handling

Every job is tagged with `{viewId, documentHandle, generation, requestId}`. Starting a new schema/input/entry/offset decode increments the view generation and cancels all older controls. The controller discards a response unless all identifiers still match the active view before it forwards the delta to the model.

The worker queue is serial. It rejects or coalesces superseded display-page requests before execution and does not run materialization until the corresponding shape exists. JSON, outform, and binary exports use the same queue so they cannot race a materialization request against the same unsynchronized sources.

### Startup and shutdown wiring

The startup auto-replay path must call `BrecoDecodeController::requestResolve()`, never `decodeSelected()` followed by an inline core decode. The restored request is posted after widget construction (for example with `QTimer::singleShot(0, ...)`), and that controller call immediately queues work to the worker, allowing the first paint/event-loop turn regardless of decode cost.

On shutdown the controller atomically cancels all active controls, asks the worker to destroy documents/sources on their owning thread, then quits and joins the worker thread. Interpreter cancellation checks must make this join bounded by a small unit of decode/replay work rather than an entire million-item operation.

Debug builds should assert document/source thread affinity at every execution-service entry point. UI integration tests should also verify that a slow fake source never executes on `QApplication::instance()->thread()`.

## Source lifetime and consistency

Lazy replay is sound only if it sees the same bytes as shape resolution.

### Phase-1 thread-safety decision

`ByteSource` and `DecodeDocument` remain internally unsynchronized. For GUI documents, all mutable operations on a document and all reads/cache mutations on its sources are confined to the one BrecoLang worker thread and serialized by its queued request stream. Sources are created, used, and destroyed there. Immutable program, result snapshots, and opaque immutable continuation payloads may cross threads; mutable sources and caches may not. CLI/tests may own an execution service on their calling thread, but the same rule applies: one owning thread and serialized access for a document’s entire lifetime.

This is a deliberate decision, not an open question. Adding mutexes to `QFile`, `QHash`, page-order, and buffer operations would still leave ordering and document-cache races to solve, while providing no benefit to the initial single-panel workflow. Future parallelism can assign independent documents to independent workers, but must not concurrently operate on one document or share one mutable source.

- `DecodeDocument` owns its `ByteSource` objects.
- Random-access sources expose a stable identity/generation. A continuation checks it before replay.
- A changed source invalidates the whole document; it is not acceptable to combine old shape with new bytes.
- Sequential sources used by a lazy document must be spooled or retained. `releaseBefore()` cannot discard bytes that a future window may need.
- `PagedFileSource` currently caches pages, but an externally modified file could make old and new pages inconsistent. The implementation must choose between an immutable source snapshot, robust file identity/metadata validation, or documented invalidation on change. Size/mtime alone is inexpensive but not a perfect snapshot guarantee.

## Materialized data model

The first version can reuse much of `DecodedNode`, `DecodedValue`, and `StorageLayout` for small returned pages, but it should not expose one global dense `DecodedTree` as the authoritative result.

Each materialized node has a stable `InstanceLocator` key. The cache maps locators to node/value/layout records and stores child pages per parent as sorted, nonoverlapping index ranges. Overlapping follow-up requests merge by locator rather than creating duplicates.

Useful changes even if the record structs are initially reused:

- synthesize indexed names instead of interning them;
- use direct child arrays/ranges rather than sibling linked lists;
- make object/sequence relationships provider-backed instead of requiring all `fieldValues`/`valueRefs`;
- store direct spans once and construct full layouts only when requested;
- make topology incremental, eliminating the whole-tree `finalizeTopology()` pass.

Materialization cache eviction is optional initially because the UI only grows on user request, but the API should permit it. If implemented, visible, selected, and expanded locators must be pinned so live `QModelIndex` objects are not invalidated unexpectedly.

Continuation lifetime is separate from page-cache lifetime. A GUI sequence state retains its latest successor token even if old materialized pages are evicted; an outform cursor owns its own token. Cache eviction may discard optional copies of old tokens, but not a token still held by a caller. If all copies of a useful token are gone, a later request cold-replays from the sequence start.

## `DecodedTreeModel` and tree-view behavior

### Backing model

`DecodedTreeModel` should hold an opaque `DecodeDocumentHandle`, immutable shape/page snapshots, and per-view page state—not a mutable `DecodeDocument` or an immutable eager `DecodedTree`. Pinned views may share the worker-owned document/shape while keeping independent GUI-side expansion state and successor tokens.

Model indexes use stable materialized handles or locator IDs; they must not point into a movable `QVector`. Parent and child tables provide `O(1)` lookup.

The normal panel flow is: ask `BrecoDecodeController` to resolve shape, attach the returned handle/snapshot to the model, display the root/structural outline available from shape, and issue a small initial display-page request for root scalar values. Expanding nodes then issues the depth/window requests below. The model never waits for or receives a hidden full-tree decode.

### Sequence rows

For a sequence with resolved count `total` and currently materialized prefix `shown`:

```text
rowCount = shown + (shown < total ? 1 : 0)
```

The final row is synthetic, for example:

```text
Show next 128 items (999,936 remaining)
```

With the owner’s requested growth rule, the request amount is:

```text
min(remaining, max(initialPage, shown * 2))
```

The owner confirmed that this means adding `shown * 2` beyond the existing rows: an initial 64-item page asks for the next 128 (192 total), then the next 384 (576 total), and so on.

The synthetic row is model state, not a `DecodedNode`. Activating it issues a `DisplayPageRequest` for only the new range and passes the sequence state's current successor token. While pending it becomes a loading row; failure becomes a retry/error row without discarding the existing token or items. A successful result installs the returned successor token atomically with the new rows.

### `canFetchMore()` and `fetchMore()`

There are two valid Qt integrations:

1. Standard lazy model: `canFetchMore()` returns true while items remain and `fetchMore()` requests the next page. This may auto-fetch during scrolling and is less explicit.
2. Explicit synthetic footer: activating the footer calls an internal `requestMore()`. `canFetchMore()` can still support programmatic callers, but automatic view behavior must not silently request the full sequence.

The explicit footer better matches the product requirement. The model should funnel both paths through the same request method.

When a page arrives, use correct incremental notifications: remove or replace the old footer, insert real item rows, then insert a new footer if items remain. Avoid resetting the whole model, which loses expansion and selection.

### Expansion behavior

- Expanding a nonsequence composite requests one more depth if its children are not materialized.
- Expanding a sequence requests its first 64 items.
- Expanding an item requests its fields to the configured depth.
- Nested sequences independently receive their own first-page window.
- Offsets and values display from returned display pages; indexed names are formatted lazily.
- Double-click navigation uses the locator’s direct span rather than reaching through a globally retained source vector.

The existing “Expand All” action cannot keep its literal unbounded meaning. It should become “Expand Loaded,” or issue a bounded depth/materialization request with a clear item/node budget and confirmation before large expansion.

### Threading and stale results

`BrecoLangPanel` owns or is given the GUI-thread `BrecoDecodeController` described in “Mandatory asynchronous execution mechanism.” `decodeSelected()` becomes request assembly plus `requestResolve()`; it contains no direct interpreter call. Startup replay uses that same route. Each response reaches the panel through a queued signal, passes the controller’s generation check, and only then updates the model on the GUI thread.

Closing a view cancels its shared atomic controls and queues release of its worker-owned document handle. Shared pinned views keep a handle reference count but never acquire direct source/document access.

### Cursor ownership, reopening, and cold access

- **Normal UI growth:** each concrete sequence in a view has one current successor token. The 64-item page returns the boundary at 64; requesting 128 more returns the boundary at 192, and so on. This is strictly forward and never searches an index.
- **Independent and nested requests:** different views and nested expansions keep separate immutable token chains, which gives retry/fork value semantics and avoids accidental cursor consumption. Today, however, the load-bearing guarantee that a display request, nested request, and export cannot concurrently corrupt `DecodeDocument` or `ByteSource` state is the Phase-1 single-worker queue: their actual operations are serialized. Token immutability is defense in depth now and a prerequisite—not sufficient synchronization by itself—for any future parallel-materialization phase. A long request can still delay another, so streaming consumers should yield/resubmit bounded chunks if fair queue service is required.
- **Closing and reopening:** the current application restores the source decode offset, not a tree scroll/sequence-item position, so this is not a present access path. While a `DecodeDocument` remains alive, a future recent-view/session object may retain its logical page state and successor tokens and reopen cheaply. Releasing the document, invalidating its source, or restarting the application invalidates those runtime tokens. Persisted session state should store locators/item indexes, not opaque live interpreter payloads; restoration re-resolves the document and can cold-replay to the saved index on the worker thread. However, the current prefix-only Qt model cannot display a lone far page at row 500,000 without either rebuilding all earlier visible rows or gaining sparse/virtual gap rows. The baseline may reopen such a released view at the sequence start; exact far-scroll restoration requires retaining the live view/page state or a separate sparse-model enhancement. A periodic decode checkpoint would reduce replay CPU but would not solve that model/topology issue.
- **Cold random access:** a request for item 500,000 without a token executes/discards the preceding 500,000 variable items, then materializes the requested page. It remains bounded-memory and cancellable but can have eager-prefix-scale CPU latency. Repeated binary-search-like or backwards requests are the concrete workload that degrades without a sparse index.
- **Eviction:** page-cache eviction and continuation ownership are independent. The sequence-start token is pinned for the document lifetime, and live successor tokens are pinned by their view/cursor owners. If an optional old successor or the entire view state is discarded, correctness falls back to the sequence-start token; it never resumes from a guessed offset or partial environment.

This degradation is acceptable for the stated UI and export behavior because neither performs cold random access. If a future hex-editor interaction adds frequent jumps by sequence index, first measure cold replay. The compatible next step is a bounded LRU of returned tokens at observed access boundaries; an automatic periodic index remains unnecessary unless that real workload proves otherwise.

## Full-output consumers

“Needs all output” does not mean “needs all nodes resident.” The default answer for full consumers should be streaming or pull iteration, not progressively requesting UI windows until the entire graph happens to exist.

### JSON export

Keep the existing streaming approach. It should eventually consume the common semantic engine/shape document rather than the tree. JSON output is inherently `O(itemCount)` time and output size, but memory remains bounded by nesting depth and a small buffer.

The current streaming loop sometimes performs a suppressed trial and replay to avoid partial invalid JSON. An immutable continuation mark or a transactional output buffer can reduce duplicate work, but that is independent of lazy UI materialization.

### Binary export

- Exporting a selected node issues `ExportSpanRequest` for that locator and streams the returned exact span plan.
- Exporting an aggregate fixed contiguous sequence uses its shape extent directly.
- Dynamic multi-input/composite output uses the same forward continuation primitive with a layout/span sink, emitting spans incrementally rather than building one giant vector.

### Outform rendering

Outforms are the biggest consumer refactor. Growing UI windows is the wrong abstraction because outforms may iterate every sequence, access metadata, and revisit values.

Replace direct `DecodedTree` access in `RenderStore`/`OutformRenderer` with a provider interface conceptually offering:

```text
scalar(handle)
field(handle, SymbolId)
sequenceCount(handle)
openSequenceCursor(handle, start)
metadata(handle, member)
layout/spans(handle)
```

Handles are locators or ephemeral values. `openSequenceCursor(handle, 0)` obtains the sequence's arithmetic position or immutable start continuation. Cursor `next(N)` calls the same runtime `advanceSequence(token, N, sink)` primitive as display paging, but uses an outform value-provider sink rather than `MaterializationSink`; it installs the returned successor token as its new position. `count(sequence)` comes from complete shape in `O(1)` for directly represented sequences. `for` loops stream items. `@children` becomes a repeatable cursor/iterable rather than a newly allocated vector of every child.

Every call to `openSequenceCursor()` creates an independent cursor chain. This is enough for nested, interleaved, and repeated traversal: a second pass opens a fresh cursor at zero and replays forward again. `start > 0` is also supported, but without a supplied continuation it is a cold open that discards the prefix. Thus multi-pass outforms remain bounded-memory but may multiply semantic execution time. For a lazily resolved nested sequence whose exact count is not already in shape, `count()` may itself scan once and a later `for` may scan again. A bounded page/token cache can eliminate measured duplicate work, but it is not required for correctness.

The continuation solves cursor position and decode state; it does not by itself solve provider value lifetime. An item handle yielded by a cursor must remain valid for at least the loop body's lexical use. If the language permits a value to escape that scope, the provider must copy/pin the compact referenced value or define a bounded retained-handle policy. No yielded handle may silently alias a mutable “current item” that changes on the next cursor advance.

Because the current `OutformRenderer` eagerly copies `DecodedValue.elements` into `QVector<ValueRef>` and constructs `@children` arrays, this provider/cursor rewrite remains a major release gate. The worker may execute a long export sequentially, or split it into queued chunks using the returned token so display requests get fair service. It must not manufacture UI pages or use legacy full materialization for a large sequence.

An interim compatibility path may use legacy full materialization for small documents under a strict limit, but large-count outforms require the provider/cursor design. Silently falling back to a million-node tree would negate the feature.

### General consumer API

Longer term, JSON, outform, and binary layout traversal should be consumer implementations over the same replayable `DecodeDocument`. They differ in request shape and traversal order, not in decoding correctness.

## Limits and metrics

Retain `maxLoopIterations`, `maxParseDepth`, and `maxProbeBytes`. Do not silently change the language-visible meaning of `maxNodes`: split it into a compatibility/logical-node limit and explicit materialization limits, then decide whether a future language version deprecates the logical limit. Add explicit limits for:

- shape nodes/templates;
- logical nodes/items, if compatibility requires it;
- bytes per continuation and active continuation/cursor state;
- materialized nodes/bytes per request and per document;
- retained diagnostics;
- variable replay items per request, with `Paused` continuation semantics;
- wall-time/work budget and cancellation.

Expose metrics useful for testing and UI status:

- scanned items and bytes;
- arithmetic-skipped items and bytes;
- shape-node count;
- active/start continuation bytes and cursor count;
- materialized node/value/layout count;
- page/token cache hits, resumed items, cold-replayed items, and cold cursor opens;
- elapsed scan/materialization time.

The status bar should distinguish “resolved 1,000,000 items” from “materialized 64 items,” instead of reporting only constructed nodes.

## Minimal changes versus bigger lifts

### Shipped minimal foundation (Phases 1–2)

These changes are localized enough to land first while establishing the correct architecture:

- Add `ResolveShape`, `DecodeDocument`, shape snapshots, locator IDs, `DisplayPageRequest`, and `ExportSpanRequest`.
- Add the mandatory `BrecoDecodeController` plus dedicated `BrecoDecodeWorker` `QThread`; remove direct interpreter calls from `BrecoLangPanel` and startup replay.
- Make the worker-confined document own program and sources, with all document/source operations serialized on that worker.
- Preserve loop item/body extent in IR and add the two-way `LoopScanPlan`.
- Implement arithmetic shape/index resolution for simple counted fixed-stride repeats.
- Remove the unused periodic-checkpoint tuning fields from `ShapeScanOptions`; the shipped code never consumed them.
- Introduce `ShapeSink` and `MaterializationSink` boundaries.
- Reuse current decoded record structs inside bounded materialized pages.
- Replace model sibling traversal with direct page child arrays.
- Add the 64-item first page and synthetic “show next” row.
- Leave JSON streaming as it is except for using document-owned inputs.
- Keep explicit legacy full-tree mode for small tests/callers.

This foundation solves the reported million-`u8` case without a UI-only truncation hack.

For non-fast-path sequences—including variable-length `repeat`, `many`, `while`, and `recover`—Phases 1–2 deliberately retain today’s eager Tree path with the already-shipped `setCompositeLayout()` O(N²)-to-O(N) fix. They are therefore no worse than current behavior, but are not advertised as lazy until compact values and forward continuation replay land.

### Required larger lift for complete semantics

- Split transient runtime values from durable `DecodedValue` storage.
- Add compiler dependency/liveness analysis so skipped items retain every value required by later decode semantics.
- Make recursive execution resumable through explicit continuation frames.
- Implement immutable variable-sequence start/successor continuations, paused-scan continuation, cold replay, replay verification, nested-token derivation, and committed recovery boundaries.
- Add continuation-size and variable replay-item limits plus cold/resumed replay metrics when the continuation path lands.
- Replace composite-layout reconstruction with span accumulators and layout cursors.
- Refactor outforms to a locator/value-provider API whose sequence cursor directly wraps the same continuation primitive; specify yielded-value lifetime and multi-pass replay.
- Define robust immutable source identity/change invalidation beyond worker thread confinement.
- Add bounded/aggregated diagnostics and optional cache eviction.

These are architectural changes, but they are not optional if the feature must work soundly for arbitrary variable-length and outform-heavy schemas rather than only the benchmark.

## Suggested delivery sequence

The outform rewrite is a major compatibility and release risk, not a routine consumer cleanup. `OutformRenderer` is coupled to the full node/value graph, and leaving an eager fallback for large documents would reintroduce the original failure through an export path. The overall lazy-decode feature is not complete—and eager defaults must not be retired—until the provider/cursor gate is satisfied.

1. **SHIPPED — contracts, worker boundary, and golden equivalence tests**: document/shape/window APIs, dedicated queued worker/controller with atomic cancellation, locator, thread-affinity, and source-generation rules.
2. **SHIPPED — fixed-stride path**: compiler item extent and two-way `LoopScanPlan`, arithmetic sequence shape, bounded page materialization, and lazy UI model.
3. **Compact semantic values**: stop `Probe`/shape mode from constructing durable values; add sequence summaries and slot liveness.
4. **Variable sequence continuations**: implement immutable forward cursor tokens for repeat/while/many, then committed recovery boundaries and nested-token derivation; compare forward pages and cold arbitrary windows against legacy full-tree results.
5. **Incremental layout**: span accumulators and selected-node binary export.
6. **MAJOR RELEASE GATE — outform provider/cursors**: replace `RenderStore`’s full-graph contract, prove bounded-memory million-item outforms, and forbid silent eager fallback above the legacy limit.
7. **Retire eager defaults**: make legacy materialize-all explicit and bounded; migrate tests and CLI consumers.

Each stage should preserve the first-class API. The UI must not implement independent truncation logic while the runtime still eagerly decodes everything.

## Verification and acceptance criteria

### Runtime equivalence

- Materializing all windows of a small fixture produces values, topology, diagnostics, spans, and layouts equivalent to legacy Tree mode.
- Random windows in variable sequences match the corresponding legacy items and land on identical boundaries.
- Advancing from every returned continuation produces the same next boundary as uninterrupted execution; cold replay from the sequence start reaches the same boundary for sampled and final item indexes.
- A test-only canonical semantic trace/value digest compares uninterrupted execution with execution resumed from every returned continuation and with cold replay: live frame values, computed fields, condition/alternative decisions, diagnostics, and final root semantic values must match, not only cursor offsets. Fixtures must include state carried across iterations (for example a running checksum or a later expression depending on a prior item) so under-captured liveness cannot pass on boundary equivalence alone.
- Copying/forking one continuation into two consumers produces identical items and successor state; cancelling or rejecting one consumer does not consume the original token.
- Tokens are rejected after document/source generation change, for a different sequence locator, or when their boundary is inconsistent with the request. A normal hot token has `nextItem == firstItem`; a token returned while cold-seeking may have `nextItem < firstItem` and continues discarding, but `nextItem > firstItem` is invalid.
- Replaying a page or opening a second outform cursor does not duplicate document-level diagnostics or other observable decode effects.
- Exhausting the variable replay-item limit returns `Paused` at a committed boundary with a reusable successor; repeated bounded continuations produce the same page/value result as one unlimited request. No response reports `Success` for a truncated range.
- Speculative `many`/`oneof` failures and recovery rollbacks leave no shape or page residue.
- Nested conditions, selects, bitfields, external inputs, regions, gaps, and computed fields preserve semantics.

### Performance behavior

- The one-million-`u8` shape contains one sequence binding and one item template, not one million item records.
- Fixed-stride shape resolution performs `O(1)` cursor advancement after evaluating count and checking limits/bounds.
- Initial 64-item materialization time and memory are independent of total item count.
- Variable shape resolution is `O(items/bytes)` time and retains state proportional to active scan depth and directly represented sequence starts, not item count, unless the schema's genuinely live semantic state itself grows with item count.
- A hot variable page costs `O(requestedItems)` from its supplied continuation. A cold page at index `i` costs `O(i + requestedItems)` with bounded materialization memory, and metrics report that degradation explicitly.
- A million-item sequential UI expansion or outform pass retains only its current/start tokens and bounded page/provider values; it does not accumulate periodic boundary records.
- No model method walks all resolved sequence items merely to answer `rowCount`, `index`, or `data`.

### UI behavior

- The window paints before long shape work completes; progress and cancellation remain responsive.
- Expanding a million-item sequence initially displays 64 real items and one synthetic continuation row.
- Activating the continuation row inserts only the requested range and preserves selection/expansion.
- Changing source/schema/offset invalidates or cancels stale requests safely.
- “Expand All” cannot accidentally materialize an unbounded document.
- A slow/cancellable fake source proves that startup replay, `decodeSelected()`, materialization, and export execute on the BrecoLang worker thread; the GUI event loop paints and processes cancellation while work is active.

### Consumer behavior

- JSON export uses bounded memory.
- Binary export of an unmaterialized item requests only its locator/layout.
- Outforms over a million-item sequence use a cursor and bounded memory rather than legacy full-tree fallback.

## Open questions and tradeoffs

1. **Meaning of “complete shape” for heterogeneous nested data**: Is aggregate family information plus lazy exact per-instance facts acceptable, or must every nested count/branch be retained? The latter has an unavoidable `O(N)` lower bound.
2. **Optional random-access acceleration**: The baseline has no periodic index. If measured workloads later show repeated cold jumps, decide whether a bounded LRU of already-returned tokens is sufficient or whether an explicit sparse index is justified. This must remain an optimization behind the continuation API.
3. **Source stability**: Decide whether lazy documents require immutable snapshots, metadata-based invalidation, or application-level guarantees that an opened input does not change.
4. **Diagnostics**: Decide cap and aggregation behavior for millions of repeated warnings.
5. **Materialization cache and far-scroll restoration**: Initially monotonic prefix growth per view is simpler. Long-running arbitrary-window use may need eviction, which complicates stable Qt indexes. Exact restoration to a cold far row also requires retained page state or a sparse/virtual-row model; a faster decode index alone would not provide the missing rows.
6. **Legacy API lifetime**: Define when `DecodeMode::Tree`, `rootValue`, and direct `DecodedTree` access are deprecated versus retained for small tooling/tests.
7. **Outform repeated traversal and escaping values**: The baseline reopens at the sequence start for each pass. Measure whether real outforms need a bounded token/page cache, and define how long provider item handles remain valid if a loop value escapes its lexical body.
8. **Fast-path conservatism**: The initial `BatchAdvance` proof should reject uncertain cases. Later affine/external-input optimizations can expand it without changing the API.
9. **`maxNodes` semantics**: Decide whether it continues to cap logical potential nodes, is versioned/deprecated, or is split in schema syntax from request-level materialization limits. It must not accidentally become a no-op merely because nodes are lazy.

## Recommended decision

Adopt `DecodeDocument + ResolvedShape + DisplayPageRequest/ExportSpanRequest` as the stable architecture, with every GUI-owned document confined to the mandatory queued BrecoLang worker. For variable sequences, use immutable forward continuation tokens as the single runtime primitive for shape pauses, display pages, layout streaming, and outform cursors. Retain one sequence-start recipe and caller-owned successor tokens; cold-replay from the start when no token is available. Do not build a periodic checkpoint index unless a future measured random-access feature justifies it behind the unchanged API. Do not expose a truncated eager `DecodedTree` as “lazy decoding”; that would retain the interpreter cost and make later continuation semantics much harder to add correctly.
