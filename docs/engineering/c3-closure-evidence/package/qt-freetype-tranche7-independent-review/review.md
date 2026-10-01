# SDK7 FT cache observation independent review

Reviewer: `/root/lean_refresh_review`. Scope: the frozen SDK7 preparation script,
headless hash probe, original-call/source-counter contract, and existing native
64 evidence. No product/test source, SDK prefix, CMake, shared build, or GUI was
modified/run. The new boundary probes run without a GUI against the exact
SDK7 QtCore image, using layout-compatible test-only hash keys.

## Conclusion and blocking finding

The entry/state/adapter path is understandable, and the finite normal-operation
receipts and unknown classifications are consistent. **SDK7 QG-03 is not passed
for collector exceptions.** Actual isolated fault injection reproduces a bridge
failure; neither the existing native 64 GREEN nor reentry/thread GREEN closes it.
The frozen SDK7 remains unchanged. Whole SDK/process coverage remains unknown.

**B1 — collector exception poisons observation and can change original failure
behavior.** The inherited bridge at
`build-c3-qt-observed/tranche7/qt-ft-hash-sources/src/corelib/global/qglobal.cpp:403`
sets its thread-local dispatching flag before the callback (`:407–410`) without
RAII or a catch. `observer_enter` (`:413–415`) increments depth before dispatch.
An injected callback `std::bad_alloc` escapes admission: no completed scope
exists to leave, dispatching remains true, and subsequent callbacks are silently
suppressed. Reinstalling the callback does not recover.
`private-probe/exception.log` is actual RED, exit 1: caught=1, before=1, after=1.
The fixture deliberately throws bad_alloc; this does not assert a natural OOM
occurred during the earlier 64 run. Both real collectors allocate strings/map
nodes (`tests/qt_freetype_hash_probe.cpp:45`;
`tests/qt_freetype_store_probe.cpp:34–37`), so the exception class is feasible.

The new helper's Scope destructor (`tests/prepare_c3_qt_freetype_hash_tranche.py:54–58`)
also invokes the callback before restoring active; added callbacks occur inside
original Qt `Span() noexcept` (`tranche7/qt-headers/QtCore/qhash.h:267`). A callback
exception there can terminate, rather than preserve the original Qt call. This
termination consequence is source-derived; the isolated RED above tests the
admission/poisoning branch, not a process-abort branch.

Minimum follow-up: in a new independent SDK prefix, make bridge dispatch cleanup
RAII, prevent callback exceptions escaping SDK/Qt noexcept paths, roll back entry
depth when admission fails, and expose a sticky collector-failure unknown state
to the final report. Silently catching and reporting known zero is insufficient.
Use bounded/preallocated collection or catch allocation failures; require
exception/recovery/sticky-unknown regressions. Do not rewrite the old prefix or
its logs as repaired evidence. Root has accepted this boundary for SDK8.

## Entry, observation state and original-call invariants

`prepare():243` admits only a fresh SDK7 path. `official_headers():229` verifies
the pinned Qt6.11.1 archive and reads exact hash/FT headers; `prepare():250–252`
requires frozen6 qhash bytes equal the official header. `transform_hash():129`
limits changes before QMultiHash, exact anchors reject drift, and site identities
are unique. Node/Span/Data construction/copies/stores remain original statements,
with observation added afterwards. Bulk wrappers (`HELPER :104–112`) call the
original libc operation once with its original byte argument and return its
original result; they do not add per-field charges for that same relocation.
QMultiHash, unsupported types and non-insertion cache behavior are outside scope.

`prepare():265–282` wraps only the two frozen FT insertion call sites and calls
finish after original return. Scope keeps a thread-local active/previous stack
(`:41–58`), actions/finished/sticky unknown and a decrementing observer byte
budget (`:65–73`). Exactly one supported Node action plus normal return and no
gap permits a kind0 completion; missing/multiple actions, type mismatch or
budget exhaustion remain kind3. The type proof (`:79–92`) is intentionally the
controlled trivial12-byte glyph key/pointer Node and unsigned4-byte/empty-value
set Node. It does not validate arbitrary deep-owning keys or pointed-to glyph
objects. These observation fields are not application/domain authority.

Parents and completion callbacks carry zero bytes. Creation/value replacement/
detach/rehash are separate reached original destinations. sizeof-based typed
representation counts are source-level upper bounds, not hardware traffic or
allocator footprints. The existing FT bitmap/Glyph object stores are separate
from hash key/pointer/index destinations. Borrowed iteration/refcount handles
are not charged as deep payload. Native totals stay raw diagnostic facts and
are not added to formal contract_copy_bytes or compared with the 69,632-byte
material budget.

`build():356` retains frozen command flags, compiles Core+FT only, inserts a
private shadow include path for FT, copies the older prefix into a new one,
and relinks with 494 existing object hashes. Named preparation/compile/framework
steps make side effects locatable; pinned anchors are preferable to silently
adapting changed upstream headers. The unchanged older prefix/objects are
independently hashed. Original SDK C++17/Release/ABI flags remain; the headless
test uses C++20 separately. The earlier C++20-trait preparation failure remains
recorded in qt-freetype-tranche7-preparation-red.json.

## Actual finite receipts and additional bounded checks

Independent evidence-validation.json verifies all 20 report evidence hashes,
309 unique manifest sites, exact marker/raw manifest, 139 raw and 33 headless
site identifiers against the manifest, 20/20 binding availability, 494 reused
objects, and native/previous runtime image hashes. Pure in-memory header/helper
replay is byte-exact. No preparation/build entry point was called.

The actual native 64 log has 705 glyph admissions/completions and 2 missing-set
admissions/completions: 707 scopes, 1414 zero-byte entry/completion callbacks.
Recomputed FT totals exactly equal the report: 1418 copy callbacks/35,644 B;
6019 write callbacks/24,592 B; zero added FT unsupported callbacks. Four
Span-storage relocation calls copied 4608 B. Native rehash/shared detach was not
reached and is not called native coverage. The existing headless run reached
growth/rehash, exact 1000-node COW detach, 100 existing-value assignments and
one new/one duplicate empty set action. Type, 32-byte observer exhaustion,
no-child and original impossible-capacity bad_alloc cases retain kind3.

All 64 original complete case records were independently compared with frozen 6:
34 are raw equal; the remaining 30 require the report's explicit full-run
bijection. The same bijection matches all six parallel arrays, including engine
and font-table identity, glyphs, positions and source indexes, while every other
field matches exactly. No sorting-by-partial-key, omitted field or silent
normalization was used. This proves finite permutation equivalence only.

New private-probe/reentry.log is GREEN: original and nested compatible hash calls
execute, active returns to null, one outer admission/completion/Node callback,
zero unknown; nested observer-induced callbacks are suppressed as intended.
New private-probe/threads.log is GREEN: two threads with independent hashes
each insert 100 entries, preserve outputs/active stacks, and produce 200
admissions/completions and zero unknown through a thread-safe atomic test
collector. This does not upgrade the single-thread headless collector's global
map into a concurrent collector or verify arbitrary concurrent install/uninstall.
The real native collector has its existing mutex; installation must remain
outside active operations, with callback/context lifetime stable.

The private probe's first compile.log contains a test-fixture defaulted equality
operator rejected for its anonymous subobject. Explicit scalar equality repaired
that fixture; compile-final.log passes strict compilation. This was not an SDK
runtime RED. The exception.log is the separate actual runtime RED.

## Remaining boundaries

Default CoreText, FreeType2.14.3 internals, LCD/outline/path/transformed and
non-insertion cache paths, other Qt/HB stores, and font asset/model-text
classification remain open. No zero callback closes them. Counters are uint64
and are only checked against the finite recorded totals here; no unbounded
stream/overflow claim is made. No solver, model authority, numerical acceptance,
GUI rerun, public copy budget, broad QG03, SK/P0 completion or full SDK coverage
is claimed. The old normal evidence remains useful while B1 blocks the expanded
exception-path behavior claim.
