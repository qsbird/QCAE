# qcae_application

`RecordApplication` owns the active immutable `DocumentView`, preview overlays,
revision checks, idempotency, history, lifecycle and commit publication. It accepts
prepared record changes through callbacks; it has no concrete entity types or
legacy `Model` codec dependency. Direct `execute` releases its private preview on
success, definite failure and exceptions, so retryable writes cannot exhaust the
public preview quota.

Public API: `qcae/record_application.hpp`. Dependencies: document record APIs,
contracts and the standard thread library. Hosts inject the record registry,
`IRecordStore`, optional project lease/publication port and feature validation.

An ordinary edit submits only touched record rows, one immutable history row,
one operation fact and document metadata. Unchanged record/history payloads remain
shared. Candidate state publishes after durable batch success. Uncertain outcomes
poison writes until explicit recovery. Full images are reserved for explicit
project publication, initial load and migration; ordinary commits do not call a
whole-model codec. `stats()` counts record work performed by the coordinator and
prepared edit sessions; legacy export/snapshot materialization is outside it.

`RecordStateImage` is a detached restoration value, not access to active mutable
state. A legacy workspace must be decoded outside this module and written to a
separate destination before activation. Project open, save/save-as, discard and
recovery retain distinct paths.

```cpp
#include "qcae/record_application.hpp"
// Supply a frozen registry and a store in RecordApplicationOptions.
// RecordApplication app(options); then app.create_document(...).
```

Run `ctest --test-dir build-core --output-on-failure -R
"^(record_application|core|persistence|allocation_atomicity)$"` after building.
The private `application_state.hpp` defines metadata and row codecs. Ownership is
listed in [the target manifest](../targets.json).
