# Contributing to Breco

## Pull Request Review Checklist

Use this checklist for every PR:

- [ ] Build and tests pass locally (`cmake -S . -B build -G Ninja`, `cmake --build build`, `ctest --test-dir build --output-on-failure`).
- [ ] UI control additions/renames/removals are reflected in docs.
- [ ] Runtime behavior changes (scan lifecycle, preview sync, status reporting, decoding behavior) are reflected in docs.
- [ ] Persisted settings changes are reflected in docs.

## Doc Drift Guard

If a PR changes UI controls, panel wiring, mode options, status behavior, preview behavior, decoding behavior, or persisted settings, update docs in the same PR or explicitly justify why docs are deferred.

Review these files during PR review:

- `README.md`
- `docs/runtime-behavior.md`
- `docs/codemap.md`
