# Diagrams Index

This folder contains Mermaid source files used by the core runtime docs.

## Diagram Sources

- `architecture-overview.mmd`
  - Used in: `docs/codemap.md`
  - Purpose: runtime ownership and dataflow graph.
- `runtime-lifecycle.mmd`
  - Used in: `docs/runtime-behavior.md`
  - Purpose: end-to-end lifecycle from startup to preview.
- `signal-slot-flow.mmd`
  - Used in: `docs/runtime-behavior.md`
  - Purpose: key UI/controller signal-slot wiring.
- `scan-execution-flow.mmd`
  - Used in: `docs/scan-and-io-behavior.md`
  - Purpose: reader/worker dispatch, merge, and completion path.
- `cache-eviction-reload.mmd`
  - Used in: `docs/preview-cache-and-status.md`
  - Purpose: budget enforcement, eviction, and on-demand reload.
- `preview-sync-flow.mmd`
  - Used in: `docs/preview-cache-and-status.md`
  - Purpose: shared-center synchronization and render update sequence.
- `string-null-visibility.mmd`
  - Used in: `docs/behavioral-invariants.md`
  - Purpose: String mode NUL visibility decision rules.
- `text-sequence-qualification.mmd`
  - Used in: `docs/behavioral-invariants.md`
  - Purpose: text sequence qualification rule decision tree.

## Notes

- Mermaid blocks in docs are maintained to match these source files.
- HTML rendering is done client-side via Mermaid JS in generated docs HTML.
