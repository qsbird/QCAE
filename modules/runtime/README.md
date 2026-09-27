# Bounded local task runtime

`qcae_runtime` depends on foundation/contracts and the standard thread library. Its
public `task_service.hpp` contains no Qt, SQLite or concrete model entity types.
`TaskService` owns two workers and an eight-entry waiting queue by default; limits
cannot exceed those bounds. Worker computation occurs outside its publication
mutex. Progress, cancellation decisions and publication are serialized.

A `TaskInputContext` freezes DocId, Epoch, model revision and an optional profile.
An all-empty profile triple means a solver-independent task; a partially specified
profile is invalid. No global Nastran target is inferred. A task start key is scoped
by caller and document; identical normalized input replays its original task fact
before revision validation. Session/Epoch validation still precedes replay.

Task records retain transitions and monotonically numbered progress events. The
current payload codec is version 2 and reads version 1 with an empty receipt primary
entity. Progress events are bounded, with room reserved for cancellation/failure
and restart transitions. `wait` uses a condition variable; workers and tests do not
poll or sleep. Cancellation is cooperative during computation and checked again
before publication. Once publication owns the mutex, a concurrent cancel waits for
the committed or failed fact and cannot undo it.

`TaskPublisher` is the runtime's neutral persistence/publication port. Its callbacks
must not re-enter or destroy `TaskService`. `record_application/task_application.hpp`
implements this port through the same `RecordApplication` used by other clients.
Register `task_row_handler()` in application options **before** constructing the
application. The task service and its callbacks must be destroyed before that
application. The bridge has its own `qcae_task_application` target; application
does not depend on runtime.

The application validates registered row owners/schemas, preserves owned rows in
recovery and project snapshots, and applies side-row compare-and-swap updates.
Queued/running/progress writes leave model revision and undo history unchanged.
Task success, its receipt, the model delta, history and operation fact are persisted
in one application batch. There is no second document writer or model authority.

Unfinished rows become `interrupted` during explicit application recovery/open;
they are never automatically re-executed. Durable `succeeded` rows retain their
committed facts. An uncertain publication stops further service writes, reports
`outcome_unknown`, and requires explicit application recovery followed by
`TaskService::reconcile()`. It never overwrites possible durable success with a
failure. A definite task-state write rollback also stops workers; after they stop,
explicit `reconcile()` marks durable unfinished rows `interrupted` and resumes the
service without rerunning work or advancing the model revision. If that repair
write fails, the service remains blocked and reconciliation can be retried. Closing a document with active owned task rows is rejected; a host can
explicitly cancel and wait before closing. No retired-workspace mechanism is used.

Verification targets: `qcae_runtime_tests` and `qcae_geometry_mesh_tests`. The former
uses the real application coordinator with a fault-injecting storage port to check
queue bounds, cancellation, completion atomicity, owner/schema rejection, replay
and recovery. The latter runs thirty real background uniform-meshing jobs. These
component faults do not replace the required SQLite child-process crash tests.
