# Migration notes

## Release identity

Use `QUANTUMEDITOR_VERSION` for the engine/editor version and
`QUANTUM_RELEASE_CHANNEL` for `stable`, `beta` or `nightly`. Desktop packages
expose `release-manifest.json` with the resolved platform and architecture.

## Android

The Gradle workspace accepts `QUANTUM_VERSION` and `QUANTUM_VERSION_CODE`. The
Old legacy property names are no longer part of the public configuration surface.

## Runtime validation

Release CI invokes the headless engine with `--help`; it never opens an editor
window or mutates project scenes. Use `scripts/release-smoke.py` against a local
build or installed tree before distributing an artifact.
