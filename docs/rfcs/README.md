# RFCs

Use this directory for API/ABI/metrics changes that affect external users.

## When an RFC is Required

1. Public C API signature changes
2. Public C++/Rust interface removals or behavior-breaking changes
3. Prometheus metric name/label/schema changes
4. Compatibility policy changes

## RFC Template

Create a new file: `docs/rfcs/YYYY-MM-DD-short-title.md`

Recommended sections:

1. Problem statement
2. Proposed change
3. Compatibility impact (C ABI, C++ API, Rust API)
4. Observability impact
5. Rollout plan
6. Alternatives considered
