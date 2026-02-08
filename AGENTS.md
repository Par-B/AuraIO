# AuraIO Agent Instructions

## Environment
- Host OS is often macOS, but AuraIO is Linux/io_uring based.
- Do **not** run build/test binaries directly on macOS.
- Run build/test commands in Orb container `linux`.

## Canonical command wrapper
- Use this form for all build/test execution:
  - `orb -m linux bash -c "<command>"`

## Build and test commands
- Build library:
  - `orb -m linux bash -c "cd /Users/par/Documents/src/AuraIO && make -j4"`
- Build and run C/C++ test suites:
  - `orb -m linux bash -c "cd /Users/par/Documents/src/AuraIO/tests && make -j1 all"`
- Run Rust workspace tests (requires shared library path):
  - `orb -m linux bash -c "cd /Users/par/Documents/src/AuraIO/bindings/rust && LD_LIBRARY_PATH=/Users/par/Documents/src/AuraIO/lib:${LD_LIBRARY_PATH} cargo test --workspace"`

## Validation workflow
1. Build core library in Orb.
2. Run `tests/make all` in Orb.
3. Run Rust workspace tests in Orb with `LD_LIBRARY_PATH` set as above.

## Notes
- If Orb command fails due to VM startup, retry the same Orb command once with escalated host permissions.
- Do not rely on prebuilt binaries under `tests/`; always rebuild before running tests.
