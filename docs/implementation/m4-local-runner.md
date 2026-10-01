# M4 本地进程适配器：有界实施计划与证据

本切片对应 REQ-11/13/15/19/20、TST-A08/09/11 和 QG-02。它提供真实 POSIX
进程边界，已连接生产 `analysis.start` 的冻结、任务与执行事实；后续切片已
加入可信版本探测及受控 F06 解析/发布。真实 Nastran 环境仍未配置，实际
Nastran 解析及数值验收没有证据。测试程序明确为 test-only，不能作为
TST-F07/M4 的真实求解通过证据。

## 实施计划和既有行为保护

1. 新增纯标准库 `ISolverRunner` 契约；保留独立执行、解析、数值核验状态。
   冻结文档/epoch/revision、analysis/ProfileRef、输入和映射摘要、规则/reader
   版本及 RunConfiguration。配置只接受绝对 executable、逐项 argv、明确方言
   和版本依据，不使用 shell、不从当前环境读取配置或凭证。
2. 新增 `solver_local` 适配器，直接调用现有 `LocalArtifactStore.verify` 核验
   完成清单与每个输入文件，然后复制这些已核验字节到新 run_id 目录。原冻结
   目录不作为求解器工作目录。启动前持久化 startup intent；fork 后子进程
   等待闸门，只有 pid 与 OS start identity 已持久化后才能 execve。
3. 持久化与跨行语义检查必须由可信宿主提供：解析被冻结的原 ArtifactIntent、
   检查分析/Profile/配置兼容性、加载运行事实、CAS 持久化运行事实。成功
   回调必须达到宿主声明的 SQLite 持久边界。适配器不建立第二权威模型，也
   不把进程启动与 SQLite 伪称为一个事务。
4. query/cancel 只操作本实例实际持有的子进程，并核对 OS 启动身份。重启
   后没有子进程所有权的非终态记录标记 outcome_unknown，不按旧 PID 发
   信号，不自动再次执行。取消先持久化意图；终态查询以真实 waitpid 为准。
5. 退出后仅记录原始输出完整性和 digest；零退出码、输出存在不改变 parsing
   或 numerical validation 的 not_run。结果 reader 与最终产物发布仍走外层
   原服务，不改现有 fixture 标签、TaskService、事务或历史。

本 agent 仅拥有 `modules/contracts/include/qcae/solver_runner.hpp`、
`adapters/solver_local/`、`tests/solver_local_tests.cpp` 和本文。root 后续负责
构建装配、TaskService/AnalysisRun 的持久事实接入、原输入的跨行验证，以及
真实 Nastran reader/数值核验。当前实现不修改已有共享服务或根 CMake。

## 验证计划

在独立 `/tmp` 目录以 C++20 严格警告编译；测试使用同一个明确 test-only 的
可执行文件作为子进程。覆盖完成输入/manifest 损坏、启动意图与身份持久化
失败不执行、argv 字面量、真实运行查询、取消与已退出竞态、执行失败、损坏
或缺失输出不能成为已解析/已验证，以及旧/未知启动身份不重跑、不发信号。
不运行共享构建、GUI、真实求解或收费 API。实际结果在运行后追加。

## 当前实现与 QG-02 路径

入口为 `LocalSolverRunner::start` → 可信宿主解析既有 ArtifactIntent →
`LocalArtifactStore::verify` → 可信宿主语义检查 → CAS 持久启动意图 → 新目录内
的核验副本 → fork/启动身份持久化 → 闸门 → execve。启动前的任何拒绝都
不会运行配置中的程序。身份持久化失败，包括“实际提交但回包失败”，都会
关闭闸门、回收未执行的子进程，旧持久记录保留为待核实。

权威工程状态仍在 RecordApplication/SQLite；`LocalSolverHost` 的四个回调
分别承担原 artifact 的解析、语义/配置核验、事实读取、精确 CAS 持久化。
进程表只持有本实例的 OS 子进程句柄和已持久化事实，不是第二个模型。
宿主必须将配置资源 ID 解析为可信本机配置；不能将请求携带的 executable、
argv、version_evidence 或 frozen input 原文视为授权或已验证事实。

query 先用 waitpid 观察真实退出。运行中只能对启动身份匹配的所持有进程组
发信号。macOS 退出期间可能短暂无法读取 proc_pidinfo；尚未回收的子进程
继续等待下一轮 waitpid，期间不发信号。恢复后无本实例子进程句柄的旧事实
一律 outcome_unknown，包含旧 PID 复用和 startup intent 窗口，不重新启动。
已观察的退出若持久化失败，保存在进程表中供下一次 CAS 重试，不再 wait
同一个已回收的 PID。取消先记录意图，最终零退出或非取消信号仍报告真实
退出；`cancellation_requested` 保留请求事实，不能声称取消已完成。

输出仅为未解析的原始观察：期望路径、长度、SHA256、日志和输入副本核验。
输出损坏、缺失、空文件、symlink 或副本被改写可使 output_state=incomplete，
执行的退出事实仍保留。损坏但非空的任意字节可以被收集，parsing 和 numerical
validation 仍为 not_run；真正的 reader 必须另行拒绝坏语义。主进程退出而
进程组仍有成员时不收集输出，标待核实。输出摘要不是最终结果发布清单。

## 宿主接入约束与已知范围

- `input_artifact` 必须读取既有持久 artifact/task 行，并复用原跨行验证，不
  接受低权限参数提供的 intent。`validate_start` 复核冻结分析、导出映射、
  ProfileRef、规则版本与实际本机配置/版本依据的关系。适配器的字节核验
  不能替代这一层。
- `load/persist` 需纳入同一个工作库、原上下文和明确的 run_id/task_id；CAS
  包含整个原事实，原事实缺失也是预期条件。返回成功必须已到达持久边界。
  callback 不能重入 runner，也不能仅把更新排入后台队列后返回成功。
- TaskWork 必须有界轮询 query 并映射 TaskControl 取消。墙钟和聚合输出预算
  在 query 时执行，单文件使用 RLIMIT_FSIZE；不是独立后台 watchdog、内核
  总磁盘配额、进程树隔离或 Windows 支持。run 目录私有，不自动清理未知
  运行；原 artifact 目录保持冻结。宿主必须保持 runner 到任务收敛。
- 初版只给固定 LANG/LC_ALL=C，不继承环境。真实许可程序若需要额外启动
  环境，需显式可信配置扩展及单独核验；本切片没有读取凭证或许可环境。
  当前版本依据由可信宿主核实；适配器没有自动执行版本探测或猜测方言。
- 真正的结果 reader、数值检查、最终 manifest 发布和 TaskService 成功回执
  尚需接入。runner 不修改 parsing/numerical 的 not_run，不修改模型/历史，
  不让零退出或原始输出收集自动成为 analysis 成功。

## 实际验证（2026-10-01）

独立编译命令使用 clang++、C++20、`-Wall -Wextra -Wpedantic -Werror -pthread`，
仅链接本切片源码和既有 artifacts_local.cpp。最终可执行文件与日志：
`/private/tmp/qcae-solver-local-review/solver_local_tests`、
`/private/tmp/qcae-solver-local-review/solver-local-final.log`。真实运行结果：
**PASS 48 checks，exit 0**。包含真实 fork/execve/waitpid、进程组取消与 SIGKILL
收敛、持久 hook 的文件 fsync/目录 fsync/CAS、标准输入关闭、启动/身份提交
未知不执行、取消持久失败不发信号、取消后零退出与 SIGSEGV 不改写事实、
退出观察持久失败可重试、旧 PID/start identity 不发信号/不重跑、wall time
与聚合输出预算及坏输出防线。

首轮新增取消竞态确实失败：exit-on-term 在 macOS proc_pidinfo 暂不可读的
退出窗口误成 outcome_unknown，证据保留在
`/private/var/folders/vf/j_dmv8cd4yb3dt0sqw_29w940000gn/T/qcae-solver-test-79182/`。
修正为持有子进程等待下一轮 waitpid 后，同一扩展集 PASS 48；该失败没有
隐藏为“测试替身问题”。最后增加私有目录权限、完整执行前预分配进程表和
输出 readback metadata 核验后再次独立严格编译并 PASS 48。

`python3 tools/check_design.py`、`python3 tools/check_cpp_format.py`（192 files）
与 `git diff --check` 实际通过。未跑共享 CMake/CTest、GUI、真实 Nastran，
没有数值解析/核验或 M4/P0 整体通过结论。QG-02 的入口、权威状态、副作用
与错误边界已在上述路径列明，独立审查及 root 后续装配仍需执行。

## 下一切片：同一 TaskService/工作库的生产接入计划

新贡献只拥有 solver_contribution.hpp/.cpp、solver_run_record.hpp/.cpp、
schemas/operations/solver.json 和 tests/solver_run_ipc_tests.py，继续不修改共享
服务和根构建。root 提供可信 `NastranArtifactCoordinator::verified_artifact`
accessor，复用既有 principal/published/task/manifest/file 完整验证，不复制
私有 decode 或简化跨行校验。

`analysis.start(analysis_id, artifact_id, run_config_id)` 不接收 executable、argv、
intent 或 caller 等权限事实。配置在 host 的显式 `--solver-config` 可信本机
JSON 中读取；默认未配置，start unavailable。test_only 与实际 Nastran 配置
分开标记；逐项 argv 仅展开规定的输入路径占位符，不使用 shell。配置摘要
与实际逐项参数固定进 run。

开始时从当前 snapshot 使用原 artifact 的冻结编号映射调用现有
freeze_analysis_input，并核对 target/物理签名；同时复用 validate_nastran_export
与 verify_nastran_readback 验证该冻结资源可表达当前物理语义。run 绑定当前
epoch/revision，原 export 的 document/epoch/revision/manifest/map 独立保留。
携带旧工程可重用一致的冻结产物，但不能用当前重编号推断旧文件身份。

共同 publisher 包装现 Nastran publisher（assembly 只允许一个 factory）。
初次 queued task 与冻结 run 快照在同一个 update_owned_rows CAS 中存储；
该快照明确标为“尚未持久 startup intent”，不伪称进程已启动。TaskWork 用
已分配 task_id/run_id 调用 runner；startup intent 与后续实际身份仍由同一个
RecordApplication CAS 达到 SQLite 边界。10ms query 处理取消/预算，原分析
修订变更不改旧 run 的输入，也不将原 run 结果写入新激活文档。

execution/原始输出事实、parsing、numerical validation 和最终结果发布分层。
解析器未接入时整体 analysis 不报告已验证成功；analysis.get_run 返回原始
运行状态、来源、当前性及缺少解析/验证的明确事实。最终结果 reader、数值
核验与有效结果 manifest 仍需后续真实环境闭环。

root 已提供受控 `TaskPayload::completion()` 诊断/终态端口，在普通取消处理前
把明确的外部 failed/cancelled/interrupted/outcome_unknown 持久到原任务行。
该端口拒绝 worker 自报 succeeded 或非终态绕过 publisher，原 mesh 默认行为
保持。root 报告的 runtime CTest 1/1 实际通过（0.61 秒）见
`../engineering/m4-task-outcome-review.md`；本新 contribution 的生产 IPC 仍待
装配后的实际回归，不能将通用端口通过扩成生产 solver 已通过。

## 冻结编码接线复核（2026-10-01）

接真实 `encode_frozen_analysis_input` 时发现该编码是含 NUL 的长度前缀二进制，
而初版 runner 将它误当作不含 NUL 的文本。此前 48 项用纯文本 test-only
sentinel，未证明真实 codec 能经过边界；该接线缺口已明确撤回 source stable。
现改成非空、有界 opaque bytes，executable/argv/路径等文本仍拒绝 NUL。
独立严格编译后 **PASS 49，exit 0**；原全部 48 路径改为含 NUL 的输入，新增
跨持久化原样保留断言。证据 `/private/tmp/qcae-solver-local-review/solver-local-binary-green.log`。

新的 run codec 与 task 共用 `solver_run_signature`，参数的 canonical 编码与
配置摘要须共同匹配真实任务签名；不能修改携带输入的 config digest 后仍
沿用旧 task 事实。run 的 frozen/source/map/profile 与原 artifact 的完整跨行
验证继续复用 existing accessor。v1 owned schema 直接拒绝 parsed/passed 标志，
零退出、非空坏字节和取消都不能成为已验证成功。

取消持久化与执行观察的连续错误预算独立，不能由一次成功 query 重置取消
失败。10ms 轮询分别连续 20 次失败后通过 completion 保留 outcome_unknown，
不把未确认终止写成 cancelled。新 C++ 源码独立 `-fsyntax-only` 严格警告已通过；
生产 socket/SQLite 回归在 `tests/solver_run_ipc_tests.py`，运行后再追加实际证据。

本 owned run v1 行最大 64KiB，包含原 source 与当前 frozen 编码；超限在 queued
task/run 的原子提交前拒绝，不丢弃 provenance 字段。它是当前有界切片容量，
未证明 P0 全部小/中模型覆盖。恢复中的旧 OS 身份只给 outcome_unknown，
旧 epoch 重试被拒绝，原任务/运行查询保留；未知外部事实会阻止 close，需要
后续受控 reconciliation，初版不以盲重跑、盲信号或删行清除该障碍。

replay 直接复用原 Nastran 的调用者/键/签名/document/epoch/revision/profile
完整 fence；匹配后核对原 run/task，再走 `TaskService::query`。正常打开携带
工程会创建新文档身份，旧键返回 conflict，不能使用 dummy work 让
`TaskService::start` 误进入新 admission。携带工程回归保留该旧键冲突、零
 owned facts 变化和单一子进程目录的断言。

## 生产接入实际证据（build34b，2026-10-01）

已只读核对 root 的统一日志
`../engineering/c3-closure-evidence/package/solver-and-sdk-targeted-build34.log`：
**targeted CTest 10/10 PASS，exit 0，总 20.35 秒**。其中 solver_run_ipc 5.13 秒，
solver_local 5.01 秒，原 nastran_artifact_ipc/result_fixture_ipc/runtime 均通过；
其余为 MCP 3 项、Qt widget 和 VTK ranges。这是实际共同编译产物的回归，
本代理没有另跑共享构建或 GUI。独立 runner 的详细日志仍为上述 PASS 49。

已读取 `build-c3-closure-desktop/m4-solver-run-evidence/solver-run-ipc-transcript.json`：
8 个 host 会话、225 个业务请求（不包含每次连接的 handshake）。2 个环境
负样本为“未配置”和“声明 Nastran 但无实测版本依据”，均 unavailable。
6 个实际 POSIX test-only 子进程场景为：

| 场景 | 持久执行/任务事实 | 结果边界 |
|---|---|---|
| 非工程的原始输出、零退出 | exited/failed，exit_code=0 | parsing/numerical 都 not_run |
| 非空损坏字节、零退出 | exited/failed，收集原始摘要 | 不变成已解析或已验证 |
| 缺失期望输出、零退出 | exited/failed，没有完整输出列表 | 不凭退出码或文件存在成功 |
| 运行中实际材料修改 | 原运行输入 revision 不变，input_current=false | 不覆盖当前模型/历史 |
| 忽略 SIGTERM 的真实子进程 | cancelled/cancelled，观察 SIGKILL=9 | 取消意图与完成区分 |
| 杀掉 engine 后明确恢复 | outcome_unknown/interrupted | 旧键 conflict、不重跑、close conflict |

所有成功 `analysis.get_run` 回包仍 `test_only=true`、source_kind=test_process、
results_available=false。重启场景的旧子进程是本测试的有界程序，按自己的
2 秒期限自然退出；恢复 engine 没有盲信号或再次启动它。

IPC 还执行了 4 个非法 start 请求、1 个损坏 manifest（均零新 task/run 行和
零启动目录）；8 个真实保存 `.qcae` 携带输入的单字段损坏：principal/key/
signature/test_only/manifest digest/config digest/parsing/numerical。5 个在打开
时拒绝且不部分安装，3 个结构合法但跨行不一致，在 get_run 时拒绝且 owned
facts 字节与模型 revision 不变。未改生产权限路径来构造坏行。正常携带输入
可读取原观察，但旧 doc/epoch 键明确 conflict，不制造第二个子进程。

## 本切片 QG-02 与尚未解除的门禁

生产入口在 `apps/engine/engine_host.cpp` 可信绝对 `--solver-config` 文件及
`engine_contributions.cpp` 单一 Nastran coordinator/publisher wrap。服务入口
在 solver_contribution.cpp；权威是同一个 RecordApplication/SQLite 的 task
与 qcae.solver.run 行。外部适配由 solver_local.cpp 执行。原 artifact/task/file
验证通过 verified_artifact 复用，冻结物理签名/映射与 Nastran semantic readback
复用既有函数；没有拷贝简化的 validator 或第二个可写工程模型。

已复核原子 queued task+snapshot、startup intent→实际身份→exec gate、精确
CAS、取消/退出分层、原 document/epoch 的旧回执 fence、结构损坏与跨行拒绝。
在这个受限执行事实切片中，已识别的阻塞均修复并由上述实际回归保护，可
进入下一轮独立可读性审查。该结论不覆盖真实 Nastran 或完整 M4/P0。

后续真实程序仍需实际路径/许可/受控方言版本，以及可持久复核的实际版本
探测来源和可执行身份；不得只把内存 `version_validated` 置 true，亦需防止
探测与启动间 executable 被替换。配置 JSON 不可自报这个布尔值。当前没有
这些证据，实际 solver 继续 unavailable。

root 已有 F06 parse-only helper 的单元证据，但它没有外部真实性标签，尚未
连接本 run v1。这里 result_reader_version=unconfigured，codec 仍拒绝伪造
parsed/passed 标志。下一切片需明确 reader 配置、完整输出字节 readback/冻结、
source/map/profile/version 校验、数值核验及最终 result manifest/任务原子回执。
不能将原始输出摘要、parse-only helper 或 test-only 数字提升为真实求解验收。

## 下一切片计划：可信版本探测

仅对可信启动配置的 non-test-only 程序执行固定 `help`，不接收任意探测
argv、不运行求解输入、不使用 shell。该信息命令见
[MSC Nastran 2024.1 Installation and Operations Guide](https://documentation-be.hexagon.com/bundle/MSC_Nastran_2024.1_Installation_and_Operations_Guide/raw/resource/enus/MSC_Nastran_2024.1_Installation_and_Operations_Guide.pdf)
第64页；受控旧版也见
[MSC Nastran 2022.1 Installation and Operations Guide](https://documentation-be.hexagon.com/bundle/MSC_Nastran_2022.1_Installation_and_Operations_Guide/raw/resource/enus/MSC_Nastran_2022.1_Installation_and_Operations_Guide.pdf)
第64页。帮助文本中不能严格识别 MSC/目标版本时，保持 unavailable。
配置中的 version_evidence 是声明，不能代替进程观察。

新增相邻 adapter DTO/私有 helper，固定 timeout、stdout/stderr 聚合字节上限，
关闭继承 fd、空 stdin、固定 C locale、私有 cwd；记录实际 waitpid/启动身份、
输出摘要和 executable SHA256/dev/inode/size/mtime/ctime 纳秒。探测前后检查
同一 executable，start admission 与 runner validate_start 再复核；任何替换
都不能变成已验证启动。没有配置的环境不启动程序，继续 unavailable。

v2 run 只增加实际 probe 事实及其对配置/任务签名的绑定，v1 行保持可读，
test_only 的既有执行/取消/重启/字段损坏样本不提升真实性。继续禁止 v1/v2
在 reader 未配置时接受 parsed/passed。初版复核不伪称在 macOS/Linux 消除了
所有核验至 exec 的竞态或固定了 launcher 的全部 transitive 安装资源。

测试用真实 fork/exec 的明确 synthetic child，覆盖无header/错误版本/混合
版本/非零退出/超时/超大输出/文件替换和配置自报版本拒绝；没有真实Nastran
验收。root 负责新 helper/test 的构建注册，本代理不改共享 parser/targets。

## 取消竞态测试的 sanitizer 因果修正（2026-10-01）

root 的 fresh core ASan/UBSan build35 CTest 实际为 34/36，不能作为全通过。
solver_local 在取消后自然失败分支断言 SIGSEGV=11；实际保留的
`qcae-solver-test-78536/crash-on-term.fact` 为 exited、cancel requested=true、
termination signal=6，stderr 为 ASan 对故意 SIGSEGV 的 DEADLYSIGNAL/ABORTING。
产品正确保存 waitpid 观察；测试错误地将 sanitizer 改写的信号当成产品错误。
原始日志在 `../engineering/c3-closure-evidence/core-sanitizers-ctest-build35.log`，
原始 facts 保留，不改写旧失败。

仅修改测试 child 与断言，选择明确 SIGABRT=6；保留已有 handler 安装之后的
真实 `child.started` 屏障，未增加等待时间或放宽信号判断。产品 runner 未改。
独立 fresh 严格 Release 和 ASan/UBSan 各实际 PASS 49、exit 0，日志为
`/private/tmp/qcae-solver-local-review/solver-local-release-signal-green.log` 与
`/private/tmp/qcae-solver-local-review/solver-local-sanitizer-signal-green.log`。
共同 sanitizer 构建的重新运行仍由 root 登记；此处不将 build35 旧红项改绿。
随后已只读核对 root 的
`../engineering/c3-closure-evidence/solver-sanitizers-signal-test-build35b.log`：
共同 ASan/UBSan solver_local 重验 **1/1 PASS，4.97 秒**。这是该修正 target
的实际绿证据，不代表前述 full CTest 的其他失败已经解除。

## 版本探测实现与独立证据（2026-10-01）

可信配置 loader 仅对 non-test-only 配置执行绝对 executable 的固定 argv
`[help]`。默认期限 2000ms、聚合 stdout/stderr 65536B、executable readback
128MiB；私有 0700 cwd，空 stdin，仅 LANG=C/LC_ALL=C，不继承凭证环境或
其他 fd。实际 waitpid 与创建身份、输出字节数/SHA256、可执行文件
dev/inode/size/mtime/ctime 纳秒/SHA256 保存在 SolverVersionEvidence。
回包仅返回结构化摘要，不回显原帮助文本。正常非零退出或无匹配 banner
仍可作为观察回报，不能授权启动；超时、信号退出、NUL、超限和无法确认
进程组结束均拒绝。leader 已 reaped 但 group 未确认消失时保留 scratch，
不信号可能已被复用的身份；这不是完整后代隔离或主机 sandbox。

`msc.help.v1` wrapper 契约为行首 MSC Nastran、可选 V、精确版本 token
2022.1 或 2024.1；只允许行首空格/Tab，不接受任意文本中的子串、未知后缀
或混合版本。需要不同真实 launcher 输出时，先提供其实际非敏感 help
样本并审查版本化解析契约，不使用配置自报的 version_evidence 填补它。
显式 `QCAE SYNTHETIC VERSION PROBE` 标记保持 synthetic，匹配文本也不能
成为 validated。选定路径本身是可信本机配置；本探测不是 vendor 签名鉴定，
不能证明任意未标记程序确实来自厂商，也不能证明数值正确。

新 owned run v2 在 v1 末尾增添完整 probe 事实和 declaration digest。
配置/任务签名绑定这份实际观察（包括创建身份）；probe 前后、start
admission 和 runner 的 validate_start 都重新 readback executable identity。
runner 验证位于 startup intent/private input copy 之前，之后仍有至 execve
的 TOCTOU 窗口；没有以 fd-exec 或固定整套安装资源消除它，不能宣称完整
防替换。v1 原行字节/codec 保留，test-only 路线不 probe、不提升真实性；
读回 v1/v2 都拒绝 reader 未配置时的 parsed/passed。operation inputs 仍 v1，
配置/运行回包只添加 probe 摘要字段；此片没有执行/解析/数值状态重解释。

独立严格 C++20 编译与实际 fork/exec 单元 **PASS 21，exit 0**，日志
`/private/tmp/qcae-solver-local-review/solver-version-probe-final.log`。包含真实
固定 argv/环境 canary、不匹配/混合/旧版/非零/NUL、timeout、单流及跨流
聚合输出超限、存活后代保留 scratch、自然结束后测试清理、同字节 inode
替换、内容修改、symlink/字节预算；所有程序为明确 synthetic，无真实
Nastran 或数值验收。三新/变更 glue 源严格 syntax、owned clang-format21、
Python compile、design 和 diff-check 通过；全仓格式检查仍被其他 agent
正在修改的 document_view.cpp 阻塞，未擅改其源。

生产 IPC 已保留原六 test-only 执行场景及字段损坏样本，并新增三环境场景
（未配、实际 Python help 不匹配、实际 synthetic help 匹配但拒绝就绪）。
该扩展源码通过 Python compile，实际共同 engine 运行待 root 登记；不能
借上轮 build34b 日志宣称新增样本已通过。无真实配置环境仍 unavailable。

## 结果发布实施前的接口边界（历史设计）

现 FixtureResultReader/ResultBundle 只承载一个三分量位移字段，不能改其
source_kind 让它冒充真实求解。root 的 read_nastran_static_f06 是纯解析 DTO，
提供位移/约束反力六分量、冻结 solver number→EntityId 映射及 reader version，
不授予进程真实性或 numerical passed。

最小后续接点为 owned solver 的可信 ResultReaderConfiguration（明确 subcase、
mm-N-MPa/basic 坐标基、reader version、F06 相对资源名）和
`VerifiedSolverOutputResolver(owned run, output manifest)`。后者复用已有
ArtifactStore 的 stage/publish/verify；先按实际 raw output 长度/hash 完整
readback，再冻结到新结果目录并以最后 manifest 发布，不能信任低权限
run intent/path 或仅文件存在。解析调用携带原 FrozenAnalysisInput/
ExportIdentityMap/ProfileRef/实际版本依据，校验完整字段和所有 ID；纯
解析通过与悬臂梁场景数值核验保留不同事实与版本。

TaskPublisher 的 solver 专用 payload 在同一 Core/SQLite 中对原 task/run/
新 result-owned row 作一个 CAS；已有 Nastran publisher 是外部 manifest→
task/artifact 两行发布的复用模式。文件已发布但数据库确认未知时保留
可核实 receipt，不删文件、重跑或提前 succeeded。当前模型已变化允许
登记原 run 的 stale 结果，不覆盖当前模型/历史；fixture owner/codec 继续
独立。后续 schema 应显式 v3，v1/v2 保持原 not_run 解释，未知新 reader
定义拒绝恢复，不按新配置重解释旧事实。本片未新增这些产品接口。

## 版本探测共同构建的实际验证

已只读核对 root 的
`../engineering/c3-closure-evidence/package/solver-version-integration-tests-build36.log`：
共同 Release/core/ASan 编译 exit 0 后，目标组 **11/11 PASS，32.68 秒**。
其中 solver_version_probe 为 12.93 秒、solver_run_ipc 为 5.45 秒；原
nastran_artifact_ipc、result_fixture_ipc、runtime、MCP 三组、贡献装配、
nastran_static_result 与 solver_local 均在同一组实际运行。此日志确认前述
版本探测新增样本已运行，不覆盖下文随后新增的结果发布切片。原 build35
失败日志保持不变；真实 Nastran 与 numerical acceptance 仍未通过。

## 受控 F06 解析和发布：实施计划与兼容

本片对应 REQ-13/15/19/20、TST-P01/P06/P07、TST-A08/A09/A11 和 QG-02。
目标是保留正常退出的完整原始观察，在原冻结输入/GRID map/ProfileRef 上
解析六分量位移及 SPC 反力，发布可核验文件及原子任务回执。执行、解析、
逐分量数值/悬臂梁结论继续是三个阶段；本片 numerical_validation 只能为
not_run。成功 TaskState 表示完成解析产物发布，不表示工程数值通过。

1. 可信本机配置新增 `qcae.local-solver-config.v2`。保留 v1 原 14 keys；
   v2 增加一个精确 5-key result_reader：reader_version=
   qcae.nastran.static-f06.v1、resource 为 expected_outputs 中明确相对 F06
   路径、subcase 为十进制字符串 `"1"`、unit_system=mm-N-MPa、
   coordinate_basis=basic。拒绝缺项、额外 keys、不支持定义和保留路径。
   没有 reader 的 v1 不静默选择 reader；未配置真实 executable 继续不可启动。
2. `qcae.solver.run` owned schema v3 冻结这份 reader，追加可选原版本观察、
   结果发布 intent 与已发布事实。原 v1/v2 codec 字节及 not_run/unconfigured
   解释保留；未知 reader 和无依据的 parsed/passed 行拒绝。配置升级不能
   重解释旧运行，低权限请求不能携带 executable、reader 或 artifact intent。
3. 新 SolverParsedResult 保留原完整 SolverOwnedRun 编码，复用已有完整
   run/frozen-input validator，不制作删减的第二份来源验证。新
   `qcae.solver.result` schema v1 存解析值和同一发布 intent，数据库仍是原
   RecordApplication/SQLite，无第二模型、模型修订、历史或 fixture relabel。
4. typed `analysis.get_result.v1(run_id)` 只读返回两字段、稳定 ID、原 solver
   number、六分量/分量单位、reader/case/basis、来源、原 input_revision、
   input_current、raw_resources_verified 与 manifest 摘要。已有三项 solver
   operation 输入定义不变。读旧结果不依赖当前配置，但复核原 task/run/
   result/source-artifact 链；尚未完整发布不会提供字段。

新增 public DTO 为 solver_result_reader.hpp、solver_result.hpp；私有边界为
solver_result_store.hpp/.cpp，单元为 solver_result_store_tests.cpp。可信配置/
操作/publisher 接线仍在原 solver_contribution.cpp，v3 编码在原
solver_run_record.cpp。root 仅注册构建、ownership 和 disabled operations，
本 agent 不修改共享 TaskService、Nastran parser、装配或生成器。

## 结果发布的 QG-02 路径与不变量

入口 `analysis.start` 仍先走原 verified_artifact：principal、已发布状态、
task receipt、完整跨行与 LocalArtifactStore.verify 一并校验。运行结束后，
`State::prepare_result` 再确认原 document/epoch、task/run 链与同一原 artifact；
模型当前物理状态已变化允许读取原结果并标 stale，不能用当前映射或当前
Profile 覆盖 frozen input。reopened epoch 拒绝继续写旧任务事实。

私有 `read_verified_solver_outputs` 只接受正常观察的 exit=0、collected、
完整 expected_outputs + stdout + stderr 集合。每个文件用 O_NOFOLLOW/
CLOEXEC/NONBLOCK 打开，仅 regular file；核对读取前后 fd/path 的 dev/inode/
mode/长度/mtime/ctime 纳秒、精确长度、EOF 及 SHA256，拒绝祖先 symlink。
空日志必须存在且参加 hash，重复/遗漏/超限/损坏输出不能进入 parser。
聚合字节使用原配置限额（上限 16MiB），不是仅校验被选中的 F06。

`prepare_solver_result` 在已核验字节上调用 root 的
`read_nastran_static_f06`，携带原 frozen GRID map 和明确 SUBCASE1/mm-N-MPa/
basic 契约。六分量分别为 mm/mm/mm/rad/rad/rad 与 N/N/N/N*mm/N*mm/N*mm；
完整位移 GRID 集、反力非空子集、唯一 ID、合法数字及 finite 分量必须成立。
这些是结构和语义解析检查，不能代替悬臂梁位移/平衡/每分量容差验证。

解析成功后，run 的 parsed + intent 与 result 的未发布行作一次精确两行
CAS，确认持久化之后才 stage 文件。最终目录固定为原 run 内
`.qcae-result`：包含所有原输出/两日志和 parsed-result.qcr；qcr 冻结完整
原始 pre-parse run/input/map/profile/config/probe/reader 及解析值。最终
manifest 记录每文件长度/hash 和完整来源摘要；不接受 portable input 任意
指定另一个目录或 root_resource。ArtifactStore.publish/verify 后，再对
复制的 F06 使用原 map 解析并比较整份 canonical qcr。最终一次三行 CAS
同时设置 task succeeded/artifact receipt、run/result published；未知确认
保持待核实事实，不能猜 succeeded、删掉可能已发布文件或自动重跑进程。

取消在读取前、解析后、stage 每个 checkpoint 和返回 payload 前检查；原
TaskService 在 committing fence 后完成 manifest/CAS。低权限携带行必须与
原 pre-parse snapshot 的 principal、key、signature、test_only、probe、reader、
declaration digest、完整 request、OS start identity、退出/信号/execution、
原 cancellation_requested、完整 raw manifest 及 frozen input 一致。intent 的
manifest、目录、root_resource、文件集与 published 也必须跨行一致。

查询由同一链取得数据库解析事实，原最终文件丢失或损坏时仅
raw_resources_verified=false，不用当前模型或新文件改写旧字段。原 run raw
在 publication 后被改写不影响独立冻结产物。test_only 原始程序始终标
test_process；FixtureResultReader 仍标 fixture，没有升级为 external_solver。

可维护性分工：runner 只记录 OS/输出事实；私有 result helper 负责有界文件
readback、canonical codec 和派生 intent；coordinator 负责可信 artifact
resolver、同一个应用状态/CAS 与 TaskPublisher；既有纯 parser 只解释 F06。
DTO 不带 Qt/SQLite/MCP 类型。辅助格式验证不是另一个可信权限端口。

## 结果切片独立证据与待运行验证

独立严格 C++20 编译后，真实临时文件/ArtifactStore 单元最初 **20 checks
PASS**（不执行 solver）。审查找到原 cancellation_requested 未被跨行比较，
追加一个合法 current-run 单字段变化的负例，实际 **RED exit 1**：
`/private/tmp/qcae-solver-local-review/solver-result-cancellation-red.log`，错误
`Result query trusted a changed original cancellation observation`。修正此项、
reader 及 root_resource 比较后，同一 helper/codec 重编，实际 **21 checks
PASS，exit 0**：
`/private/tmp/qcae-solver-local-review/solver-result-cancellation-green.log`。
旧失败及对应 synthetic unit 临时文件保留；此 RED 不是生产 IPC 或真实
Nastran 失败/成功的替代证据。

单元还覆盖完整/非零/未收集/missing-empty-log/duplicate/quota、同长度 SHA
损坏、symlink/FIFO、原 GRID/分量单位、canonical codec、错 subcase、未验证
memory bytes、NaN、test-only relabel 拒绝、原映射差异、manifest-last 与复制
后源文件变化不污染最终产物。全部是 synthetic 文件与格式，数值未验证。

三个产品 glue/helper 源严格 syntax exit 0，owned clang-format、Python
compile 与 diff-check exit 0。产品和 schema 已冻结给 root 统一构建；新增
solver_result_store 及生产 IPC 回归实际共同运行仍待登记，不能引用 build36
版本-only 的 11/11 作为本片通过证据。IPC 保留原三环境/六执行场景，新增
五个实际 test-only 子进程：文件 gate 下 pending 零发布、正常 parsed 发布、
错 SUBCASE、NaN、错误物理值仍 numerical not_run、物理 edit 标 stale；正常
发布再检查完整四文件 SHA/receipt、幂等、已发布恢复、raw 分离与五个
portable 单字段损坏。实际计数以运行输出及完整 transcript 为准。

## 本片未解除的边界

- 真实 Nastran executable/许可/受控版本尚未提供，无真实运行、真实解析、
  TST-F07 或 M4 通过；完整 numerical validation 仍 not_run。
- 解析 qcr 限额 1MiB、owned result 2MiB、单 run 64KiB、最终 artifact 聚合
  16MiB 是本小切片明确容量。超过限额保留执行观察并拒绝 publication，
  不据此声明全部小/中模型性能范围已经验收。
- 最终文件已发布而数据库确认未知时保留 intent/outcome_unknown；以下
  显式 reconcile 切片补齐恢复。恢复不会猜测结果，也不会重新运行。
- portable 项目内 task/run/result/hash 进行结构和一致性校验，未提供厂商
  签名或归档事实认证。单字段拒绝证据不能声称可阻止攻击者重写全部一致
  的归档行、probe 摘要和文件；hash 不证明进程来源。可信实际本机配置、
  当前进程观察及归档真实性需区分，完整来源认证不在本片已验收范围。
- 版本 identity 复核至 exec 的 TOCTOU 和 launcher transitive 资源限制保持
  前述事实；probe banner 匹配不等于 numerical validation 或 vendor 认证。

## 显式结果恢复与共同 build38

`analysis.reconcile_result.v1(run_id)` 重用原 task/run/result/source 的完整
验证链及 ArtifactStore.verify，再用原 frozen map 重解析 F06 并核对整份
canonical qcr。它不启动进程、不写文件、不改变原 pre-parse snapshot；
task/run/result 与请求幂等事实一起 CAS。旧 epoch、旧 revision、活动任务、
failed/cancelled task、坏 manifest/raw/qcr 均拒绝。历史回执区分当时已验证
与当前文件仍完整，同 key 重放不追加事件。

真实 SQLite final-CAS 注入失败曾出现文件已发布、数据库回滚却返回
STORAGE_FAILURE。旧 RED 保留于
`/private/tmp/qcae-solver-local-review/solver-publication-rollback-red.log`。
修复后返回 STORAGE_UNCERTAIN，保留原错误代码、message、field 与 intent；
实际重启恢复及显式 reconcile 不重跑 child。独立临时 IPC GREEN 保留于
`/private/tmp/qcae-solver-local-review/solver-reconcile-independent-green.log`。

共同 build38 实际 15/15 targets PASS，28.90s，solver_run_ipc 6.66s；纯 core
37/37 PASS，20.73s。日志分别为
`docs/engineering/c3-closure-evidence/package/solver-result-reconcile-common-tests38.log`
与 `core-tests-build38.log`。实际 IPC transcript 为 16 个逻辑 scope、419
次业务请求，其中 reconcile_result 19 次、get_result 20 次。全部 child
仍为 test-only；这些证据不包含真实 Nastran 或数值验收。

## 数值比较持久事实短计划与实现

新增 `analysis.validate_result.v1(run_id)`，要求当前 document/epoch/revision、
幂等 key 及明确 expected_profile。只接受完整已发布、重新核验最终文件、
原 qcr/F06/source 链的结果，调用唯一已登记悬臂梁数值 helper 比较 132
分量。原 run/result/task 和 v1/v2/v3 编码不变；独立 owned validation 行
（最多 64KiB）保存全部 actual/expected/误差/容差、原来源 digest binding、
请求与顺序。新事实 CAS 无文件副作用；明确回滚仍返回 STORAGE_FAILURE。

私有 validation store 拆出有界 codec 与可信上下文再验证。handler 只做
有界结构自洽检查；查询/写入必须通过原完整来源链，再以相同 reference
helper 重算整份 canonical report。不得在 Core 的 handler 锁内重入 app。
每 run 最多 16 份报告，有界 prefix lookup 取 limit+1 即停；overflow 明确。

`get_result` 原 numerical_validation=not_run 表示解析时原事实，保留兼容；
新增 nested numerical_checks 明确 original_run_validation_stage、最新适用
检查摘要、original_source_state（current/stale）、current_files_verified 及
有界不可变历史。test_process 即使全部匹配也仅 synthetic_comparison，
数值阶段仍 not_run；只有完整实际 external_solver 来源可记录 passed/failed。
陈旧结果结论保留针对原输入，不能声称验证当前模型。2-node 旧 fixture、
未登记 profile、错单位/方向、坏输出和 portable report 损坏须拒绝或明确
mismatch，零 partial fact。新增测试使用明示 synthetic 21-node child，不
替代真实 Nastran。以下证据限于本地合成过程和独立编译。

## 数值切片 QG-02 源审与独立证据

入口是 solver.json 中的第六个 typed operation，由
`SolverCoordinator::register_operations` 处理。先核对本地注册的
NastranCodec.definition().reference 三元组、可信 caller 与原 task/run/result
链，再用 verified_artifact resolver 核验原导出/frozen input。最后
verify_result_artifact 同时检查 manifest、所有复制输出与 canonical qcr，
并用原 GRID map 重解析 F06。没有新的模型、权限来源或 provenance validator。

私有 `solver_validation_store` 负责格式、摘要绑定和 canonical report 重算；
所有参考物理规则来自已登记的 compare_preregistered_cantilever。它把
原 qcr、pre-parse run、frozen input 和 result manifest 的 SHA256 与 run/task/
result ID 一起保存，完整原快照仍只有现有 qcr/run authority。报告包含
132 个有唯一 entity/quantity/component key 的实际值、预期值、绝对误差、
容差和 matched。raw handler 验证 finite/单位/误差/聚合自洽；真实查询还
必须重算并核对整份 canonical report，不能仅信任 handler 接受的字节。

同 owner 的 schema1 是不可变报告，schema2 是有界 index。报告 identity
108B，含 44B run prefix 与 caller/key digest；head identity 为 prefix+head，
不会与报告冲突。index 与全部 ordinal 1..N 报告双向一致，run/principal
一致；新报告与 head 一次两行 CAS。index 使两个从同一旧 head 出发的
请求不能各自领取 ordinal。root 提供同锁 WriteContext overload，在
epoch/revision/caller 围栏后做 owned CAS；旧 DocRef task publisher 不变。
写入无外部文件副作用，真实确认回滚保留 STORAGE_FAILURE；原 filesystem
publication 之后的未知边界仍用 STORAGE_UNCERTAIN。

历史读取用 indexed prefix 范围，最多取 17 个 shared_ptr（16 报告+head）
及一个 overflow 判断，不遍历/复制全部 owned rows。get_result 的摘要有
32KiB canonical 字节 guard，超限明确 history_overflow_reason，禁止返回
latest applicable。报告行 64KiB、index 8KiB；超过容量拒绝新 key，已有
key 可重放。重复请求保留全部分量并返回 replayed，不重执行、不增加任务
事件，不改变模型/history 或原执行/解析阶段。原始 numerical_validation
键保留 not_run；当前检查由 nested numerical_checks 表达，明确原输入的
current/stale 与当前文件完整性。文件损坏保留不可变历史，fresh validation
拒绝；test_process 匹配也永远是 synthetic_comparison/not_run。

独立严格 C++20 单元 **157 checks PASS**：生产 codec 导入/导出真实登记
21-node 输入，再解析明确 synthetic F06；逐一改动 132 个实际分量并保持
误差/aggregate 自洽，均被原源重算拒绝。还覆盖 test-only 改标、原源/
qcr/manifest/input 摘要、reader、expected/tolerance、重复 key/NaN/错单位、
未发布结果、错误 profile digest、16/17 index 容量以及真实 RecordApplication
两写者争抢旧 head 的 CAS：败方没有部分报告行、模型 revision 未改变。
日志 `/private/tmp/qcae-solver-numerical-review/unit-green-final.log`；初次独立
链接发现旧 archive 不含新 prefix port 的限制日志保留，不当产品 RED。

独立临时 engine 重新编译新 coordinator/helper，借用已归档 common libraries
与 root 已冻结新 Core port（每份来源/SHA 记录于该目录 dependencies.json）。
实际本地 SQLite/Unix IPC 最终 **exit 0**，日志
`/private/tmp/qcae-solver-numerical-review/ipc-bounded-final.log`，完整 transcript
在 `ipc-bounded-evidence/solver-run-ipc-transcript.json`：23 个逻辑 host scope
及两个并发客户端，共 25 个 transcript scope、634 次业务请求，含
validate_result 59、get_result 45、reconcile_result 19。原环境/执行/解析/
reconcile 16 scope 保留；新增七个显式 synthetic analytic child 覆盖匹配、
内部节点错方向、反力矩错符号、全零错误物理、inclusive 容差边界、刚超界
与源模型 stale，均逐 132 分量核对、六分量单位/方向保持。

实际 report-only SQLite rollback 触发器证明普通 STORAGE_FAILURE、零部分
report/head、原文件和任务/run/result 不变；同 key 后续成功。并发两个
实际 IPC 请求得到不同 ordinal，16/17 容量拒绝且已有 key 仍重放。实际
重启恢复保留完整历史且不启动第二 child。七个低权限 saved-project 负例
覆盖 test_only、parsed digest、ordinal、容差、自洽的改分量/aggregate、
head 缺引用及额外未索引报告；额外报告使 prefix read 明确 overflow，
不宣称最新适用检查，新写入拒绝，零额外写入。全部修改在携带输入上，
没有新增生产权限绕过路径。旧 2-node fixture 数值请求明确 UNSUPPORTED。

最终 strict source compile、Python compile、design（41 ops）、clang-format
（226 C++ files）及 diff-check 均 exit 0。产品/helper/schema/tests 已 SOURCE
FREEZE 给 root 统一 build39；独立 library snapshot 不是共同全项目构建，
本节尚无 common39 或新增 sanitizer 通过声明。root 的独立审查/共同测试
仍决定该切片是否完成。真实 Nastran 未配置，numeric helper 或合成 child
通过不能冒充 TST-F07、M4 或 full numerical acceptance。

保留的实际限制：摘要及跨行校验证明一致性，不是归档事实的密码学认证；
完全重写一致归档/文件的攻击与 trusted binary→exec TOCTOU 仍按前述边界。
历史报告验证针对原输入，stale 与文件损坏时不声称当前模型或当前文件已
验收。新检查不更改任何 profilevalidated 全局状态。

## macOS 版本探测描述符隔离修复（2026-10-01）

`e94f2b8` 的完整 ASAN IPC 回归为 79/80；版本探测间歇失败。独立实际
Result 诊断捕获过 `Version probe timeout`，本机 `_SC_OPEN_MAX=1048575`，
旧子进程在执行前逐项关闭整个区间。仅拆出不链接 Qt/engine 的小型合成
可执行文件后，一次通过、第二次仍失败；该失败记录保留，没有据此闭环。

macOS 适配器现于 fork 前分配有界描述符快照缓冲区并预调用既有 libproc；
子进程读取自身的 `PROC_PIDLISTFDS`，关闭实际继承的描述符，保留标准流
及原 admission gate。不可用、截断或格式错误的快照拒绝执行。子进程不
分配缓冲区，父进程其他线程在 fork 前打开的描述符也由子进程快照覆盖。
Linux 区间关闭路径保持原实现，本轮没有 Linux 执行证据。

测试保留原 21 项断言及 2000ms 默认/100ms 短超时、输出总额、固定环境、
身份与进程组清理规则。新增可继承的 FD4096，并将首次 macOS 探测的软
描述符上限暂降至 128、调用后恢复，验证已存在的高编号描述符仍被关闭。
同一增强测试链接保留的旧 ASAN archives 时实际 exit 1；新源码严格清洁
构建后连续三次 **23/23、exit 0**。直接向合成 helper 传入 FD4096 时，
helper 实际拒绝并报告继承泄漏。真实适配器及调用者继续 ASAN/UBSAN，
仅独立外部合成 helper 不插桩。

原始命令、退出码、输入 SHA 和日志在
`/private/tmp/qcae-version-descriptors-fix45/actual-commands.json`、
`old-implementation-counterexample/` 及 `helper-canary-negative/`；之前的
完整回归及精简 helper 失败分别留在 `qcae-final-sk43` 和
`qcae-version-fixture-fix44`。设计、232 个 C++ 文件格式及 diff-check 通过。
这些证据限于本修复的定向验证；全量同源码矩阵、真实 Nastran、数值验收
和完整 P0 尚未完成。
