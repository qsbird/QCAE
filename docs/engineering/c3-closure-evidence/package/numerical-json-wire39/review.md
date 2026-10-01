# Linked numerical summary JSON byte regression

Source: tests/solver_numerical_validation_tests.cpp
SHA256: 296e4d12d76853535c12ae24e3a99ad43a80dc408b58a601ece37440d4783927

Only the owned C++ test changed; production/CMake were not edited. The test
builds 32 complete reports through prepare_solver_validation, canonical row
encoding/decoding and validate_solver_validation_link against the original
production-codec frozen input/qcr/artifact. Each report retains 132 components,
test_only=true and numerical_stage=not_run. Sources and publication state are
explicit synthetic representations; no real OS solver or filesystem publication
is asserted by this unit test.

The test-only summary projection has the same fifteen fields as production
validation_summary. Actual JSON uses typed_json_array and Qt Compact output;
typed_json_array_bytes must equal the observed Compact length. The ordinary
100-byte task ID group has sixteen reports, JSON 15288 B, canonical 15728 B.
The legal 100-byte U+0001 task ID group has sixteen reports, JSON 39288 B,
canonical 15728 B. Task ID round-trip is exact. Ordinary JSON is below 32768 B;
control JSON is above it although canonical bytes remain below it.

Strict independent Release compile/link and 197 checks pass in unit.log. Static
archives are copied read-only from the completed common headless build and
bound by dependencies.json; commands.json lists the independent compiler/link
argv. This is not a new common CTest run. The initial compile.log records the
test author accidentally calling .size() on Result<string>; the test now uses
the existing good() result checker. No actual test failure was concealed.

The public get_result overflow/recovery guard is outside this codec/byte test;
the separate carried-project IPC regression must supply that evidence.
Owned test clang-format 21 and git diff --check pass.
