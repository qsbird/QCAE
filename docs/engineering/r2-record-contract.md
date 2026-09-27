# R2 immutable record contract

The authoritative document representation is `DocumentView`. The legacy `Model`
aggregate remains an explicit import/export value. The application coordinator is
responsible for caller authorization, DocId/Epoch/Revision checks, idempotency,
history bounds, durable batches, and publishing a prepared view. Preparing a record
edit never publishes a view or advances its revision.

## Public interface and ownership

- `qcae/record_registry.hpp`: persistent type/field identities, descriptors, typed
  immutable `Record` images, canonical encoding and work counters.
- `qcae/document_view.hpp`: immutable tables, references, stable-key changes,
  forward/reverse application and history-image encoding.
- `qcae/edit_session.hpp`: a private edit overlay and `PreparedRecordChange`.
- `qcae/records.hpp`: reproducibly generated C++ records and descriptors.
- `qcae/records_model_bridge.hpp`: explicit full legacy import/export and migration
  differences. These functions must not be called by a local edit handler.

The registry, table and edit algorithms contain no concrete entity type names.
The generated registration, explicit `records_rules.cpp` domain rules and
`record_model_bridge.cpp` compatibility conversions are schema/domain contributions,
not generic coordination or history algorithms.

```cpp
auto registry = qcae::make_record_registry();
qcae::DocumentView current(registry, version);
qcae::EditSession edit(current);
edit.put(qcae::records::Material{qcae::EntityId("steel"), "Steel", 210000, .3});
auto prepared = edit.prepare();
// Coordinator checks the version, persists its batch, then publishes:
current = prepared.candidate.with_version(next_version);

qcae::EditSession change(current);
change.update<qcae::records::Material>(qcae::EntityId("steel"), [](auto& material) {
    material.young_modulus_mpa = 205000;
});
auto modified = change.prepare();
```

Failure throws `RecordError`, carrying the existing `ErrorCode` and field location.
The compatibility/application boundary translates this into its existing `Result`.
All checks finish on private candidate data. Failed preparation, reverse application
or decoding leaves the supplied view and historical record images unchanged.

## Schema and canonical images

`schemas/entities/entities.json` is the single authority for the fourteen current
record types, their fields, units, optional semantics and references. Type IDs and
field IDs are explicitly assigned, never inferred from a name or position. Field
ID 1 is reserved for identity, which is encoded once in the record envelope.
Removed IDs must remain reserved. `tools/generate_entities.py --check
--permutation-check` verifies the checked-in generated file and ten deterministic
registration permutations. Generation uses the repository's clang-format 21 policy.

The current envelope is `QCR1`: type ID, record version, stable identity and
field-ID-tagged, typed, unit-bearing payloads. Integer byte order is little endian.
Fields are encoded by increasing persistent field ID. Finite reals use their
binary64 bits; zero is canonicalized. Duplicate/unknown fields, wrong types,
missing or incompatible units, and unknown future versions are rejected. Optional
omitted fields get their generated default. Decoding an older supported record
produces the current canonical image through its generated typed value; no future
record becomes editable merely because its known fields can be read.

Each `RecordImage` owns a `shared_ptr<const T>` and its cached canonical bytes. Its
descriptor and type token prevent a mismatched C++ cast. A view retains the
registry and all pinned record lifetimes. No mutable record or container reference
is exposed. The temporary `RecordInput` is a checked codec/UI input, not the
authoritative storage model or an arbitrary JSON property database.

The schema enforces a maximum encoded node size of 256 bytes and material size of
4096 bytes, including their envelope, identity and units. An oversized identity or
extension returns `resource_limit`. Other records have a bounded 16 MiB encoding;
history batches have a separate 256 MiB cap. Application-level resource quotas may
be stricter and must be checked before durable publication.

## Stable changes and immutable pages

`RecordKey` is `(RecordTypeId, stable identity)`. `RecordChange` stores immutable
before/after images and persistent field IDs. Missing before means insertion;
missing after means deletion. A present null image, duplicate key, mismatched
registry or mismatched expected image is rejected. Reverse application swaps
before/after. Container offsets are never history identities.

`EditSession` coalesces repeated changes to a key and removes semantic no-ops. The
whole composite candidate exists before reference validation, so mutually related
new records may be introduced in one transaction. Deleting a referenced record
requires an explicit composite repair. Validation borrows typed values and string
views; it does not materialize `Model` or encode unchanged records.

Tables use 1024-entry immutable pages of shared record references. Updating an
existing record shares the identity index, all untouched pages and all untouched
record objects/encodings. It copies one page of references and the bounded page
directory. A material update shares neighboring material values; it never copies
1024 material payloads. Add/delete operations may copy identity indexes and are
outside the fixed one-record locality workload. Deleted locations remain as
tombstones so undo can reuse them. `max_records` also bounds allocated slots;
repeated unique insertion/deletion cannot grow the document without a resource
limit. A future compactor may reclaim unreferenced tombstones without changing
stable record keys; compaction is not claimed by this slice.

`RecordStats` accumulates typed input ownership, intermediate codec copies, encoded
bytes, copied metadata, changed records and dirty pages. History encoding adds its
actual before/after payload copying and encoded batch size. Optional stats must be
threaded through the storage/display integration to measure the full request.
`record_activity_counters()` additionally exposes monotonic per-thread full-model
serialization/materialization counters, so omitted optional ledgers do not hide
calls to the explicit bridges. Copy counts include one owned input-value copy as a
conservative allowance even when the caller moves that value into `put`.

## Geometry, ownership and compatibility

`records::GeometryId`, `records::MeshId` and legacy `EntityId` are distinct C++ ID
types. Geometry lines own endpoints and a geometry revision. Mesh records identify
their origin and optional geometry provenance. Nodes and beams can reference a
mesh; a beam's referenced nodes must belong to that mesh. A changed geometry
revision cannot leave a bound mesh falsely current: the composite edit must mark
it stale or rebuild it against the new revision.

Full legacy import covers all twelve original collections, creates an explicit
imported mesh when nodes/elements exist, and preserves the original engineering
IDs, references and source numbers. Source mappings gain their own stable IDs,
derived from their existing unique `(source model, entity)` identity. Collisions
are detected and rejected. Their identity does not depend on vector position or
source number. Full legacy export produces the original twelve collections; it
does not pretend to express the additional geometry/mesh metadata. This is why a
legacy export/import roundtrip cannot be used as an editing mechanism for the new
authoritative view.

The bridge does not itself replace the existing SQLite-container-v1,
project-payload-v1, workspace-payload-v2 or model-payload-v1 readers. The migration
coordinator must read them without rewriting the source, replay old index-based
history (including its redo suffix), convert consecutive states with
`record_changes_from_models`, and preserve transaction/content-state IDs, cursor,
operation facts and pending-save verification. Unknown profile semantics still
require the application's explicit compatibility decision.

## Focused validation and remaining integration

`record_document` checks typed canonical roundtrips, ten registration permutations,
all eight schema rejection cases, all twelve legacy collections, immutable pinned
reads, stable-key history including 100 reverse/forward rounds, history encoding,
duplicate application rejection, reference-safe deletion, geometry staleness and
three chain sizes with twenty edits per size. It uses the fixed limits of 500000
records and 2000000 references for each locality size.

These sixty samples measure the document and history-image layer only. They do
not constitute SK-12 end-to-end evidence: the coordinator, SQLite batches,
transport and derived display updates must join the same measured request, and
the final release still requires its frozen environment/fixtures and raw evidence.
The R2 tests also do not replace old workspace/project migration fixtures or the
fault-injection suite.
