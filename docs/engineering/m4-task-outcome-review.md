# M4 外部任务终态端口：实施与可读性审查

实施前计划：保留现有 mesh/产物工作负载的默认行为，仅让可信 TaskPayload
可返回显式的非成功终态。未知外部执行事实优先于取消标记；不能由 worker
直接返回 succeeded 绕过原提交/产物发布。终态仍由现 TaskService→publisher
CAS→同一 RecordApplication/SQLite 持久化，模型修订和历史不被终态推进。

原风险路径：TaskWork 已记录外部 outcome_unknown，但 cancel_requested 标记
使 TaskService 无条件写 cancelled。新端口仅接受 failed/cancelled/interrupted/
outcome_unknown；有显式终态就持久化该事实，无终态继续原候选校验/发布流程。
非终态/成功值转为 INVALID_INPUT，不能成为已提交事实。取消期间确认失败也可
持久化 failed，实际运行的更细执行/解析/数值阶段由 AnalysisRun 原记录保留。

验证计划：实际 worker 的取消屏障→未知/失败返回、持久任务图像 round-trip、
零模型提交、成功终态注入拒绝，以及完整既有 runtime/mesh/产物回归。实际结果
已执行：build33 的完整 `runtime` CTest 1/1 通过（0.61 秒），其中包含取消后
未知/失败事实保持、终态编码往返及成功注入拒绝。日志为
`c3-closure-evidence/package/runtime-outcome-build33.log`；该端口不证明真实
Nastran 或数值验证通过。
