# M4 本地求解进程适配器

对应 REQ-11/13/15/19/20、TST-A08/09/11、TST-P01/P06/P07。本文描述当前实现与不变量；真实求解器接入见[MYSTRAN本地接入](mystran-local.md)，状态见[开发计划](../baseline/development-plan.md#当前状态)。开发过程中的分片计划与证据日志已归档。

## 分层与权威

执行、解析、数值核验是三个独立阶段，分别持久化：零退出码或输出文件存在不会让解析或数值阶段成功；解析成功也不等于工程数值通过。

| 位置 | 职责 |
|---|---|
| `modules/contracts/include/qcae/solver_runner.hpp` | 纯标准库 `ISolverRunner` 契约、运行请求与观察 DTO |
| `adapters/solver_local/` | `LocalSolverRunner`：POSIX fork/exec、启动身份、取消、输出原始观察 |
| `adapters/engine_api/src/solver_contribution.cpp` | 可信配置、六个typed操作、任务发布与跨行验证（`SolverCoordinator`） |
| `adapters/engine_api/src/solver_run_record.cpp` | `qcae.solver.run` owned行 v1/v2/v3 编解码 |
| `adapters/engine_api/src/solver_version_probe.cpp` | 可信版本探测与可执行文件身份 |
| `adapters/engine_api/src/solver_result_store.cpp` | 输出readback、F06解析结果与发布意图 |
| `adapters/engine_api/src/solver_validation_store.cpp` | 数值核验报告与有界索引 |

权威状态仍是同一 `RecordApplication`/SQLite 中的 task 与 owned 行；进程表只持有本实例的子进程句柄和已持久化事实，不是第二个模型。

## 操作

`schemas/operations/solver.json` 定义：

| 操作 | 效果 | 说明 |
|---|---|---|
| `analysis.start(analysis_id, artifact_id, run_config_id)` | 后台任务 | 不接收 executable、argv、intent 或 caller；配置来自engine的可信 `--solver-config` |
| `analysis.get_run` | 只读 | 原始运行状态、来源、当前性及缺少解析/验证的明确事实 |
| `analysis.get_result` | 只读 | 位移/反力六分量、原solver编号、单位、reader/工况/坐标基、`input_current`、`raw_resources_verified` |
| `analysis.reconcile_result` | 辅助写 | 重新核验已发布文件并完成未确认的发布；不启动进程、不写文件 |
| `analysis.validate_result` | 辅助写 | 用已登记悬臂梁参考比较132分量，保存不可变报告 |
| `solver.configuration` | 只读 | 当前可信配置与探测摘要 |

## 启动链

`analysis.start` → `verified_artifact`（principal、已发布状态、任务回执、完整跨行与 `LocalArtifactStore::verify`）→ 用原冻结编号映射调用 `freeze_analysis_input` 并复用Nastran导出校验/语义读回 → 排队task与冻结run快照一次CAS → TaskWork 调用 runner。

runner 内：持久化 startup intent → 复制已核验输入到新 run 目录（原冻结目录不作工作目录）→ fork，子进程在闸门等待 → 持久化 pid 与 OS 启动身份 → 打开闸门 execve。身份持久化失败（包括已提交但回包失败）时关闭闸门并回收未执行的子进程。子进程只得到固定 `LANG/LC_ALL=C`、空 stdin、私有目录，不继承环境或凭证；argv 逐项给出，只展开规定的输入路径占位符，不使用 shell。

## 查询、取消与恢复

- query 先用 waitpid 观察真实退出；只对启动身份匹配、本实例持有的进程组发信号。TaskWork 每10ms有界轮询，执行墙钟和聚合输出预算；单文件用 `RLIMIT_FSIZE`。
- 取消先持久化意图；`cancellation_requested` 只表示请求，终态以真实退出为准。取消持久化与执行观察的连续失败各自计数，20次后保留 `outcome_unknown`。
- 重启后没有子进程句柄的非终态记录一律 `outcome_unknown`/interrupted：不按旧PID发信号、不自动重跑；旧epoch重试被拒绝，未知外部事实阻止 close，需显式核实。
- 运行中修改物理模型不改变原运行输入；结果标记 `input_current=false`，不写入新激活文档。

## 配置与版本探测

可信本机配置 `qcae.local-solver-config.v1`（14个键）或 v2（另含精确5键 `result_reader`：`qcae.nastran.static-f06.v1`、F06相对路径、subcase `"1"`、`mm-N-MPa`、`basic`）。默认未配置时 start 不可用；test_only 与真实程序分开标记，配置不能自报 `version_validated`。

非 test_only 程序在启动前执行固定 `[help]` 探测（默认2000ms、输出65536B、可执行readback 128MiB），记录waitpid/启动身份、输出SHA256以及可执行文件 dev/inode/size/mtime/ctime/SHA256。探测前后、start admission 与 runner 都复核可执行身份。`msc.help.v1` 只接受行首 MSC Nastran 2022.1/2024.1，`mystran.version.v1` 只接受 19.0.0（见[MYSTRAN本地接入](mystran-local.md)）。探测不是厂商签名鉴定；核验至 execve 之间仍有 TOCTOU 窗口。

## 结果发布

只接受正常退出且完整收集的 expected_outputs + stdout + stderr。每个文件以 `O_NOFOLLOW` 打开，核对读取前后 fd/path 身份、长度、EOF 和 SHA256；拒绝 symlink、重复、遗漏或超限输出。解析在已核验字节上调用 `read_nastran_static_f06`，携带原冻结GRID映射：完整位移GRID集、非空反力子集、唯一ID、有限值，分量单位 mm/rad 与 N/N·mm。

解析成功后先对 run 与 result 未发布行做两行CAS，再在 run 内 `.qcae-result` 分阶段写入全部原输出与 `parsed-result.qcr`，manifest 最后发布，复制后重解析并比较 canonical qcr，最终三行CAS设置 task succeeded、run/result published。数据库确认未知时返回 `STORAGE_UNCERTAIN` 并保留意图，由 `analysis.reconcile_result` 核实；不猜测成功、不删文件、不重跑。

## 数值核验

`analysis.validate_result` 要求当前 document/epoch/revision、幂等键和 `expected_profile`，只接受完整已发布且重新核验文件的结果，调用唯一的 `compare_preregistered_cantilever`。报告保存132个分量的实际值、期望值、误差、容差与来源摘要；每个run最多16份报告，读取有界。test_process 来源即使全部匹配也只记 `synthetic_comparison`，只有真实 external_solver 来源可记录 passed/failed；陈旧结果的结论只针对原输入。

## 容量与已知边界

- 单run行64KiB、qcr 1MiB、owned结果2MiB、最终产物聚合16MiB、报告行64KiB、索引8KiB；超限保留执行观察并拒绝发布。
- 只有POSIX实现（macOS/Linux分支）；无Windows支持、进程树隔离或内核磁盘配额。
- 摘要与跨行校验证明一致性，不是归档事实的密码学认证。
- 在途取消/故障窗口的完整验收、全部悬臂梁变体与三入口场景仍未完成。

## 测试

`tests/solver_local_tests.cpp`（runner）、`tests/solver_version_probe_tests.cpp`、`tests/solver_result_store_tests.cpp`、`tests/solver_numerical_validation_tests.cpp` 及真实 SQLite/IPC 的 `tests/solver_run_ipc_tests.py`；后者全部使用明确 test-only 子进程，不能作为真实求解验收。
