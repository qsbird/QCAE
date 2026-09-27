# qcae_query

Immutable record queries, disposable selection sessions and the existing complete render packet.

Public API: `qcae/query.hpp`

Direct dependencies: `qcae_document`, `qcae_contracts`

Boundary: Native queries read `DocumentView` directly. The `ModelSnapshot` compatibility
overloads are isolated in `src/query_legacy.cpp`; they use the instrumented full import
bridge and are excluded from locality evidence. No query depends on an application
facade or incremental display cache.

Readability review: `execute_query` validates document, epoch and model revision,
checks the predicate and candidate identities, then evaluates immutable records.
Relationship closures use stable entity IDs. A beam without a section remains a
queryable beam with two node references; material/section closures skip its absent
assignment. `SelectionService` owns only disposable view/handle state and checks
caller ownership and model/view revisions before returning a page. Reads publish
no engineering changes and perform no storage writes.

Selection pages use sorted unique stable IDs. Entity/reference pages carry their
immutable `RecordVersion` and enumerate live records, copying only the requested
slice. Their order belongs to that snapshot; an offset is not an entity identity
or a cursor that can be reused across model revisions. Limits above 10000 are
rejected, and offsets past the end return an empty page without arithmetic
overflow. Legacy wrappers hide synthetic geometry/mesh records introduced by
import so the original query universe stays unchanged.

Minimal consumer: `#include "qcae/query.hpp"`; link `qcae_query` in CMake.

Validation from the repository root after configuring/building the applicable options:

```sh
cmake --build build-core --target qcae_query_tests qcae_record_query_tests
ctest --test-dir build-core --output-on-failure -R "^(query|record_query)$"
```

The native suite checks relationship/spatial predicates, caller/revision/epoch
boundaries, bounded page copies and zero whole-model bridges. It also covers
unassigned beams, stable IDs over individual pages and deletion/insertion,
large offsets and the legacy universe. The original query suite continues to
cover hidden membership, set operations, view invalidation and complete render
packet compatibility. These focused tests do not establish graphical selection
or large-model performance acceptance.

Actual ownership and links: [target manifest](../../modules/targets.json). Generated build directories are not committed.
