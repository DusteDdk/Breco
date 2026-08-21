# Phase A implementation task: references and lazy navigation

Status: task specification for implementation. Read alongside `future-idea.md`
(repo root), which is the full design this task implements the first phase of.
This document exists because an independent readiness assessment found the
substrate is narrower than the design's own "Grounding" section implies, and
found two concrete bugs in the design's reference-identity model. This task
exists to close those gaps as part of a clean Phase A implementation, not to
build Phase A on top of an idealized substrate that doesn't actually exist yet.

## Intention

BrecoLang (this project's custom binary-structure decoding language,
`src/brecolang/`) should be able to express formats like FAT32, where fields
are references/pointers to other regions of data (a FAT chain entry points to
the next cluster by number; a directory entry points to a file's starting
cluster; a boot sector points to FAT table locations). A user should be able
to open a FAT32 filesystem at an arbitrary offset inside a blob, browse its
FAT entries, directories, files, and content lazily (without eagerly
decoding/materializing everything), and eventually (later phases, not this
task) export the reachable data as a binary file at multi-hundred-GiB scale
with throughput independent of blob size.

This task is Phase A only: first-class reference/pointer language support and
lazy navigation of the resulting graph in the UI. It does **not** include
reachability-graph export, null-pad/embrace/compact binary export, or any of
Phase B/C/D from `future-idea.md`. Phase A's own success criterion (per
`future-idea.md`): a FAT/directory graph can be displayed and navigated
lazily. It does not yet promise a transitive saved image.

## Current problem

An independent assessment (agent with no prior involvement in designing this
feature) reviewed `future-idea.md` against the actual current implementation
(the already-shipped lazy-decode/continuation-token foundation on this
branch) and found:

### The design's "Grounding" section is accurate but optimistic

Every type/function it names is real, but several make the substrate sound
more general than it is:

- `DisplayPageRequest::root` and `maxDepth` are declared fields that are
  currently **ignored**. `requestDisplayPage()` always re-executes from the
  document's original entry root and only varies sequence windows
  (`src/brecolang/runtime/DecodeDocument.cpp`). There is no existing "decode
  an arbitrary target at this address/type" entry point — `decodeType()` is
  private, entry-oriented interpreter machinery
  (`src/brecolang/runtime/Interpreter.cpp`).
- There is no general `DecodeSink` interface. `ShapeSink` and
  `MaterializationSink` are narrow, purpose-built helpers, not a pluggable
  architecture a reference-discovery sink could simply plug into.
- Continuation tokens (`SequenceContinuation`) are process-local objects that
  copy the entire live frame/value arena, not compact serializable
  checkpoints — fine for the shipped sequence-paging use case, but not a
  reference-target "resume decoding here" primitive as-is.
- Runtime unsigned arithmetic (add/subtract/multiply) currently wraps rather
  than being checked. The design's checked-address invariant requires real
  evaluator changes, not just new reference-specific code paths.
- `DecodedTreeModel` globally deduplicates by locator to one model node with
  one parent (`src/brecolang/gui/DecodedTreeModel.h`/`.cpp`). It cannot
  currently represent the same reference target reachable through two
  different edges (aliasing), which references need by design.
- `ShapeScanOptions::maxShapeNodes` is accepted but not enforced anywhere.

### Two concrete bugs in the design's reference-identity model

1. `ReferenceTargetKey` (future-idea.md) bundles physical address/region
   *and* an optional `explicitKey`, but the design text simultaneously says
   (a) an explicit key can unify two *different* addresses into one logical
   entity, and (b) the same key observed with an inconsistent address is an
   error. Rules (a) and (b) cannot both define equality as written — pick
   one and specify it precisely (recommended: explicit key is the primary
   identity when present, full physical/type/argument tuple is identity
   otherwise; inconsistent-address-under-same-key is only an error if that's
   what's actually meant by "inconsistent," which needs a precise
   definition of what varying is allowed vs. not).
2. `ReferenceHandle::owner` is typed as `InstanceLocator`, which can only
   express "reached via normal structural nesting from the entry root." A
   reference declared *inside* a reference's target (e.g. a directory entry
   inside a cluster that was itself reached via a FAT chain reference) needs
   an owner locator that can itself be a target-relative path, not a
   root-relative structural one. Fix the type to actually support nested
   reference ownership.

### The most consequential risk: reference fields defeat the arithmetic fast path

The compiler's existing `LoopScanPlan` is binary: `BatchAdvance` or
`ExecuteItems` (`src/brecolang/compiler/Compiler.cpp`,
`src/brecolang/runtime/Interpreter.cpp`). Any `ref` field inside an
otherwise fixed-stride sequence (e.g. a directory full of fixed-size entries,
each containing a cluster-number reference — exactly the FAT32 case that
motivates this whole feature) would currently force the *entire sequence*
onto `ExecuteItems`, meaning shape resolution executes every item just to
discover references that were never asked for. This defeats the lazy
arithmetic fast path for precisely the case Phase A needs to handle well,
and risks silently hitting the existing million-iteration/five-million-node
limits on a large fixed-size directory. This needs a genuine fix (see task
below), not a workaround.

## The task: implement Phase A fully and cleanly

"Fully and cleanly" means: do not implement reference syntax as a thin layer
that assumes the gaps above don't matter. Building the missing substrate
pieces is part of Phase A, not deferred technical debt. Concretely, this
task includes:

1. **A genuine arbitrary-target decode entry point.** Extend the
   interpreter/document layer so a bounded region can be decoded as a
   specific type with specific arguments, independent of the document's
   original entry root — this is what `DisplayPageRequest::root` was always
   supposed to mean and currently doesn't.
2. **Separate structural-shape scanning from reference-effect scanning.**
   Fix the `LoopScanPlan` conflation: a sequence with reference fields but
   otherwise fixed-stride, data-independent structure should still get
   arithmetic/batch shape resolution for its *structural* shape; reference
   discovery is a distinct, separately-triggered concern that does not by
   itself force full item execution during ordinary shape resolution.
3. **Transient vs. durable value separation**, at least to the extent Phase
   A's reference discovery needs it: a reference-aware sink must not simply
   discard every scalar value (later expressions and reference address/key
   computation depend on them), but it also should not force full durable
   node/value/layout construction the way today's page materializer does.
4. **Corrected reference identity model**, fixing both bugs above:
   precisely specify explicit-key vs. physical identity semantics, and give
   `ReferenceHandle`/owner-tracking a representation that supports
   references nested inside reference targets.
5. **Checked address arithmetic.** Reference address/region/key computation
   must use checked (overflow-detecting) arithmetic, per the design's own
   invariant #7. This may require fixing or wrapping the underlying
   unsigned evaluator behavior for this code path if a full evaluator-wide
   fix is out of scope — use judgment, but do not silently allow wraparound
   in address computation specifically.
6. **Alias-capable `DecodedTreeModel`.** The UI model must support the same
   canonical target being reachable through more than one reference edge
   (shown as separate rows, sharing underlying target data), plus
   non-expandable cycle/back-reference rows when a target is already on the
   current expansion ancestry.
7. **The actual language feature**, per `future-idea.md`'s Phase A section:
   `ref T(args) from ... at input_offset|root_offset|self_offset(...) within
   bytes(...) key ... follow|weak [when condition] [cover region]` syntax,
   IR (`ReferenceDesc`, `ReferenceTargetKey`, `ReferenceHandle` — corrected
   per item 4), compiler validation, and `contents.@key`/`@address`/
   `@length`/`@is_null` metadata access plus an explicit `deref(...)`
   operation. The `rewrite {...}` clause should parse and be validated but
   can be inert (compact-export-only, not consumed) in this phase.
8. **Lazy UI navigation**: an unresolved reference row in the tree view can
   be expanded, issuing a target-page request through the async
   controller/worker (never blocking the GUI thread), showing the target's
   structural outline/window exactly as sequence expansion does today.

Explicitly out of scope for this task (do not implement): reachability graph
export, null-pad/embrace/compact binary export, `ReachabilityCursor`/
`ReachabilityStore`, `ExportProfileDesc` and compact-export planning, and any
FAT32-specific schema or profile content. Those are Phase B/C/D.

## Suggested approach

The independent assessment recommended a small spike before broad
implementation, proving four things work together on a minimal realistic
slice (e.g. a toy format with one fixed-size record type containing a
reference field, plus one variable-length sequence containing references):
arbitrary bounded target decode, transient semantic storage with reference
events, separated structural/effect scan plans, and the corrected identity
model. Do that first if it de-risks the rest — use judgment on whether to
formalize it as a separate step or fold it into normal incremental
development, but do not build the full language surface on assumptions that
haven't been exercised end-to-end at least once.

## Acceptance criteria

Adapt the relevant subset of `future-idea.md`'s "Reference semantics"
acceptance criteria (the ones that don't depend on export/reachability):

- The same target reached through multiple fields has one canonical entity
  and multiple edges (aliases render as separate rows sharing target data);
  cycles terminate without recursion overflow, shown as non-expandable
  back-reference rows.
- Creating/displaying a reference handle does not decode its target or
  allocate target nodes. Expanding it materializes only the requested
  outline/window.
- Explicit `input_offset`/`root_offset`/`self_offset` bases resolve
  correctly for an entry at a nonzero blob offset, including
  `BorrowedWindowSource` absolute offsets.
- Invalid, overflowing, out-of-region, and sentinel (`when` producing null)
  cases produce deterministic diagnostics, not crashes or silent wraparound.
- A fixed-stride sequence containing reference fields still resolves its
  own structural shape (count/stride) arithmetically; only actual reference
  dereference/discovery triggers item-level execution.
- Full existing test suite continues to pass; add new coverage for the
  above under `tests/brecolang_runtime_tests.cpp`,
  `tests/brecolang_compiler_tests.cpp`, and
  `tests/mainwindow_integration_tests.cpp` following existing patterns in
  those files.

## Reporting

This is a large task. Work incrementally. If something in `future-idea.md`'s
Phase A description turns out to be ambiguous or unworkable given what you
find in the real code, make the most conservative choice consistent with the
stated invariants and flag it clearly in your report rather than silently
improvising scope — same standard as prior implementation work on this
project. Report back with: what was implemented, what (if anything) had to
be simplified or deferred and why, how the two identity-model bugs were
resolved, how the `LoopScanPlan` fix works, test results, and honest
confirmation of what does and doesn't yet work end-to-end.
