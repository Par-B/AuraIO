# Observability Contract

This document defines how AuraIO telemetry is versioned and stabilized.

## Goals

1. Keep hot-path overhead low and predictable.
2. Provide stable metric names and labels for external users.
3. Make schema changes explicit and reviewable.

## Data Consistency Model

Observability reads are intentionally lock-free best-effort snapshots.

1. Values may be updated while being read.
2. Minor cross-field skew is expected.
3. Metrics are intended for trends/alerting, not strict accounting.

## Prometheus Schema Versioning

The Prometheus exporter schema is versioned.

1. A schema info metric is emitted:
   - `auraio_metrics_schema_info{schema="v0",stability="experimental"} 1`
2. Before `1.0`, new metrics may be added and labels may change.
3. Experimental metrics must be clearly marked as experimental.
4. At `1.0`, schema transitions to stable and breaking metric changes require major-version process.

## Naming and Label Rules

1. Metric names use `auraio_` prefix.
2. Avoid high-cardinality labels (`req_id`, raw fd/offset, user_data).
3. Keep labels bounded and operationally meaningful.

## Change Process

Any change to exporter metric names/labels requires:

1. Updated metric snapshot (`api/snapshots/prometheus_metrics.txt`)
2. RFC under `docs/rfcs/`
3. Documentation update in observability docs
