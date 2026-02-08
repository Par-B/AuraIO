# Compatibility Policy

This policy defines compatibility expectations for AuraIO as it transitions from internal to open source.

## Current Stage: `0.x`

AuraIO is currently in pre-1.0 mode.

1. Breaking changes may occur between minor releases.
2. API/ABI drift is still tracked and reviewed in CI.
3. Breaking changes require an RFC and updated API snapshots.

## C Library (C API + ABI)

### Pre-1.0 (`0.x`)

1. Source and ABI breaks are allowed with explicit documentation.
2. ABI checks run against the most recent tag to make breaks visible.

### Post-1.0 (`1.x`)

1. No ABI-breaking changes in `1.x` without a major version bump.
2. Public C symbols and struct layout changes require a major release.
3. Compatibility checks are blocking in CI.

## C++ Bindings

### Pre-1.0

1. Public interface changes are allowed.
2. Potential removals are flagged by C++ surface diff checks.

### Post-1.0

1. Follow semantic versioning.
2. No source-breaking removal in minor/patch releases.

## Rust Bindings

### Pre-1.0

1. Public interface changes are allowed.
2. Potential removals are flagged by Rust surface diff checks.

### Post-1.0

1. Follow Rust semver conventions for public items.
2. Breaking API changes require a major crate version bump.

## API Freeze Workflow

1. Public surface snapshots live in `api/snapshots/`.
2. CI enforces snapshot consistency.
3. If snapshots change, an RFC must be included in `docs/rfcs/`.
