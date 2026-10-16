# 开发计划

需求见[requirements.md](requirements.md)，完成标准见[acceptance.md](acceptance.md)，骨架量化合同见[skeleton-acceptance.md](skeleton-acceptance.md)。本文只记录当前状态、已确认决策和接续顺序；阶段报告与原始证据不再放在工作树中（见[归档](#归档)）。

## 当前状态

截至2026-10-12（`claude` 分支）。“已实现”指代码与适用回归存在，不等于对应 TST/SK 验收通过。

| 里程碑 | 已实现 | 未完成或未验收 |
|---|---|---|
| M0 核心与本地入口 | C++20纯核心（不依赖Qt/VTK/SQLite/MCP）、本地engine/CLI、握手与共享实例 | — |
| M1 文档与格式 | 实体/组织/引用、受控BDF资源包导入与导出、ImportReport/ExportReport、编号映射 | 其他方言与卡片按需扩展 |
| M2 事务与恢复 | 统一 `RecordApplication`/`EditSession` 提交链、document/epoch/revision、幂等、有限持久历史、SQLite工作库、保存/另存/恢复/放弃 | — |
| M3 查询与桌面 | 条件查询/选择句柄、Qt/VTK工作区、分块资源、事件缺口重同步、局部显示、布局恢复、建线/网格/材料/分析工具 | 完整SK-10/12；三档性能；原累计图形压力失败未关闭 |
| M4 任务与分析 | 有界后台任务、分析检查、冻结来源的BDF文件发布、fixture结果读取、显式MYSTRAN 19.0.0后端：真实悬臂梁三次运行396/396分量通过（macOS；2026-10-13在Linux用源码构建的MYSTRAN 19.0.0复现，396/396） | 在途取消/故障窗口、全部悬臂梁变体、三入口场景；其他Nastran方言未配置 |
| M5 外部AI | MCP桥按 `capabilities.list` 动态生成工具；双MCP连接共享同一engine | 真实AI会话仅有部分诊断（TST-I01部分）；TST-I01—I10未通过 |
| M6 集成与发布 | macOS本机包（`f2d2e7f`，265项回归及重定位包SQLite/双MCP/桌面验证） | 包未随新源码刷新；Linux/GCC移植进行中；性能证据；发布 |

骨架与接口：

- SK-01—14正式通过0/14。R1已提交，R0部分完成，R2—R4完成C1/C2限定范围，R5推迟。
- C4三项独立扩展已实现：EXT-01 Material可选说明（6个产品文件）、EXT-02批量节点平移（4个）、EXT-03 Tri3拓扑（10个），保护区触碰均为0。EXT-04/05与共同最终Release集成未完成。
- 52个typed操作由 `schemas/operations/` 生成；`changes.preview`、`model.export_preview`、`selection.*`、`view.*`、`resources.*` 等8个兼容接口仍为手写解码。
- 复制账本完整覆盖0/20；完整SDK覆盖按2026-10-01决定推迟为性能验收项。
- Linux GCC 14（2026-10-12）：纯核心CTest 43/43；Qt 6.11.1 IPC+SQLite构建CTest 91/91；clang-format 21可用。未建桌面（无VTK），无MYSTRAN、无GPU，ASan/UBSan矩阵未跑。
- 弱模型扩展轨道P1—P8已结束（见下文），不等于任何SK或TST验收通过。

## 已确认的用户决策

| 日期 | 决策 |
|---|---|
| 2026-09-30 | 依次推进C3收敛、剩余骨架机制、C4独立扩展、完整SK验收和M4/M5真实环境接续 |
| 2026-10-01 | 完整SDK内部复制量覆盖移出C4启动前置条件，保留为后续性能验收；已测预算（节点589824、材料69632、事务元数据65536字节）不变 |
| 2026-10-02 | 优先完成核心框架质量，暂停非必要性能优化；重复未通过项须单独报告 |
| 2026-10-03 | 新增显式MYSTRAN 19.0.0本机后端，复用共享Nastran/BDF语义、唯一engine、TaskService与SQLite结果链（[ADR-09](../architecture/mystran-backend.md)） |
| 2026-10-10 | 启动“弱模型低成本横向扩展”工作轨道；仓库降噪，阶段证据和早期设计移出工作树并备份 |
| 2026-10-12 | 操作schema按功能拆分并复测T4；旧IPC兼容操作暂不迁入typed registry（会改变wire响应）；保留 `QCAE_C3_*` 选项；工作推送到 `claude` 分支，不开PR；启动产品验收轨道A1—A8，先闭合本机可测项 |
| 2026-10-13 | 为18个无生成契约的可用操作补类型契约；旧工作库迁移改经提交协调器；推进MYSTRAN、参考源码与真实AI客户端（暂不推进VTK桌面、GPU环境、macOS）；按完整C4流程用Haiku做一次独立扩展 |

## 当前工作轨道：产品验收

目标：以[骨架量化合同](skeleton-acceptance.md)SK-01—14和[验收基线](acceptance.md)的P0用例为准，在同一个冻结release commit上产出能被 `tools/check_skeleton_acceptance.py` 校验的原始证据与报告。先闭合本机（Linux、Qt、SQLite、无VTK/GPU/真实求解器/真实AI）可测项；环境受限项登记为阻塞，不以替身、fixture或部分运行冒充；不降低任何阈值。

| 阶段 | 目标 | 检查点（退出条件） | 状态 |
|---|---|---|---|
| A1 差距表与构建矩阵 | 97项指标逐项标明：已有测试可测、需新增测试、需外部环境 | 差距表入本计划；本机跑通核心全OFF、SQLite/headless、核心+SQLite的ASan+UBSan三个矩阵并记录结果 | 完成（2026-10-12）：核心43/43、SQLite/headless 52/52、Qt IPC 91/91、关闭Nastran包、ASan+UBSan 91/91且无消毒器报告（修复 `solver_local` 符号链接用例的退出时序假设后） |
| A2 证据管线 | 对指定commit一次运行全部本机矩阵，收集原始证据、SHA-256、环境与保护区manifest，生成合同格式报告 | 校验器能读报告并逐项列出缺失/未达标指标；缺测指标判失败，不填默认值 | 完成（2026-10-12）：`tools/collect_acceptance.py` 从 `git archive` 导出构建5个矩阵、运行全部套件与证据探针，按合同格式出报告，未测指标写入 `gaps.json` 并注明原因 |
| A3 本机可闭合门禁 | SK-02、03、05、07、08、09、11，及SK-04、06、12中不依赖GUI/GPU的部分 | 上述指标在同一commit实测达标，或缺陷已修复并复测 | 进行中：`b602010`（run 6）实测97项中78项，72项达标，类型契约比例达标（D1完成，69/69）；新增缺口是SK-03 AST审计无法解析生成头（Clang对不完整元素类型的vector成员报错），已在 `c1f3812` 修复（审计0错误）。SK-03、05、06、07、08、09、11此前全部达标。修复求解器临时文件被误判超预算的间歇失败（`b602010`）。run 7在 `52bcf16` 上取证中。SK-02桌面公开API消费者需VTK |
| A4 SK-01覆盖 | 从归档恢复R0参考库存与AP-01—20模块合同并重新评估 | 20/20合同、8个必填字段、映射率1、未归类0 | 已评估（2026-10-12）：从归档标签读取冻结R0，20份合同8字段齐全、映射率1、未归类0；level-2按本次通过的BP计（本机最多16，BP-13/16/17/18需桌面）。用户决定不对照参考源码复核冻结库存（2026-10-13），评估保留此注记 |
| A5 SK-13扩展 | EXT-04（内存/SQLite同一合同）、EXT-05（三档局部负载重跑）、按C4流程冻结的独立扩展 | 5/5通过且保护区触碰与文件数满足阈值 | 部分：EXT-01—03取自归档的独立C4执行（基线 `b740b37`，保护区触碰0/0/0，手写产品文件6/4/10）；EXT-04内存/SQLite同合同对比本机复跑通过；EXT-05需VTK端到端测量。按用户决定以部分冻结（C4全部要素，桌面矩阵除外）用Haiku执行一次独立扩展EXT-01R（基线 `c55e6af`）：未通过——产物不能编译（引用不存在的 `Status::invalid_input`），隐藏测试因此无法构建；静态项满足（保护区触碰0、手写产品文件4、初始上下文8文件42,521字节、作者补丁0）。执行期间与run 5构建争用且读取日志不完整，记录在仓库外；不计为正式SK-13通过，是否重跑待用户决定 |
| A6 外部环境项 | 需用户决定：MYSTRAN（M4、TST-F07）；VTK桌面（SK-10、BP-13/16/17/18、GUI模式、SK-12端到端局部负载、EXT-05）；锁定GPU环境（SK-12帧时间/延迟）；参考源码（复核SK-01冻结库存）；真实AI客户端（TST-I01—I10）；macOS包（M6） | 每项环境到位后按合同实测；未到位前保持阻塞 | 进行中：MYSTRAN 19.0.0本机源码构建，真实集成连续10/10；参考源码按用户决定不复核；真实AI客户端已接入（Claude Haiku经claude CLI与MCP桥，预登记、每场景3次全过才算通过，记录在仓库外）：I01澄清3/3，I03不支持路径3/3，I04、I05、I06b、I07、I08（正确基准与损坏结果）各3/3，I06a在单进程延迟包装修正后3/3；I01单位流程、I02、I03预览各0/3（实体种类猜错、陈旧修订号、未读能力），I09 0/3（恢复后在结果未知时另起运行），I10需桌面未运行。由此修正：未知实体种类报出已注册种类（`04c6c87`），分析任务响应带 `run_id`（`19a6d51`）。VTK、GPU、macOS暂不推进 |
| A7 Release候选与SK-14 | 冻结一个release commit，全矩阵、BP 20/20、主工作流9/9 | 校验器对报告判定SK-01—14全部通过 | 未开始 |
| A8 P0产品验收 | TST-F/A/I/P逐项取证 | 全部适用用例通过，REQ全部有证据 | 未开始 |

A1差距表（2026-10-12初评；“候选测试”指意图相符的现有测试，A2取证映射时逐项确认，不等于已覆盖）：

| 门禁 | 指标 | 本机可测 | 候选测试/工具 | 主要缺口 |
|---|---|---|---|---|
| SK-01 | 6 | 6 | 归档的 `reference-inventory.json`、`module-contracts/AP-*.json`、`inventory_reference.py` | 恢复并重新评估（A4） |
| SK-02 | 7 | 6 | `check_architecture.py`（核心构建0错误）、`architecture_gate_unit`（8个变异）、`tests/public_api_consumers` | 桌面配置的公开API消费者需VTK |
| SK-03 | 5 | 5 | `check_protected_entity_ast.py`、`entity_generation`、`schema_rejection_sqlite`、`legacy_migration` | 注册顺序10种子与8项schema反例需逐项对上 |
| SK-04 | 5 | 3 | `operation_handler_probe.py`（69个可用操作均到达handler）、生成契约、绕过协调器审计 | 类型契约比例1（`ea7b5c8` 起69/69）、绕过写入口0（`d8b38ba`）待run 6确认；GUI入口需桌面 |
| SK-05 | 5 | 5 | `skeleton_representatives`（BP-03/04/05）、`geometry_mesh`、`line_mesh_regeneration` | 映射到5项计数 |
| SK-06 | 10 | 10 | `skeleton_fault_*`+`skeleton_fault_report.py`（220条与F17消毒）、BP-12、6份旧格式夹具 | F17需ASan/UBSan构建；报告尚未在冻结commit生成 |
| SK-07 | 6 | 6 | `runtime`、`typed_host`、BP-20 | 10成功/10提交前取消/10过期的30次运行器 |
| SK-08 | 5 | 5 | `parameters`、`analysis_checks`、BP-09/14 | 映射到计数 |
| SK-09 | 5 | 5 | `result_fixture`、`static_validation`、BP-19、F22 | 映射到计数 |
| SK-10 | 6 | 0 | `c3_tool_lifecycle`、`desktop_selection`（均需桌面） | VTK桌面与实际图形后端 |
| SK-11 | 5 | 5 | `engine_assembly`、`package_contributions_ipc`、`platform_only_engine_ipc`、`generated_profile`、`profile_identity` | 增加关闭Nastran包的构建矩阵 |
| SK-12 | 11 | 3 | `resources`（4MiB/256KiB）、`c3_sync_ipc`+F16（事件缺口） | 4项局部负载需含socket与派生显示的端到端测量（进程内bench未测socket，端到端bench需VTK）；4项帧时间/延迟需锁定GPU环境 |
| SK-13 | 11 | 11 | 归档的C4 common40 manifest与EXT-01—03记录 | EXT-04/05与按C4流程的独立扩展（A5） |
| SK-14 | 10 | 7 | 上述全部、ASan/UBSan、警告、格式 | BP-16/17/18与主工作流GUI模式需桌面；依赖SK-01—13 |

合计97项：本机可测77项，依赖VTK桌面/图形后端或锁定GPU环境20项（A6）。

## 已完成工作轨道：弱模型可低成本横向扩展（2026-10-10—12）

目标：新增操作、字段或实体类型时，较弱的模型只需少量固定上下文、遵循一个范例即可正确完成；遗漏必须在构建或测试中暴露，而不是静默产生错误行为。每个阶段独立提交，先保护既有行为，再按AGENTS.md的检查项验证。

| 阶段 | 目标 | 检查点（退出条件） | 状态 |
|---|---|---|---|
| P1 仓库降噪 | 工作树只含产品源码、测试、生成输入和现行文档 | 跟踪文件≤500、`docs/`≤1MiB；无证据或旧代码副本进入代码索引；`check_design.py`通过；核心构建/测试不回退；备份可校验 | 完成（2026-10-10） |
| P2 未知实体默认拒绝 | 分析输入/检查、Nastran导出、导出预览等消费者声明支持的记录类型，未声明类型自动 `unsupported_capability` | 新增测试实体不改任何消费者即被正确拒绝；Tri3的4处特例分支删除；“实体类型×消费者”矩阵测试 | 完成（2026-10-10）；IPC导出预览改动已在Qt 6.11.1 IPC构建中编译，IPC CTest 91/91（2026-10-11） |
| P3 测量与业务分离 | 复制账本由共享 `qcae_feature_support` 与按schema生成的输入计量承担 | `features/` 中ledger引用为0；材料操作计量与改造前逐项一致；移除无生成脚本的 `QCAE_C3_*` 测试选项；核心/IPC回归不变 | 核心完成（2026-10-10）；`QCAE_C3_*` 选项保留：它们是2026-10-01决定保留的SDK复制量后续性能验收的测量入口（`json_ledger.hpp` 亦受其控制），删除需另行决定 |
| P4 共享类型收敛 | 材料、网格编辑、分析三个功能统一使用 `qcae_feature_support`；`success`/`forward_failure` 移入foundation的 `Result` | `OperationPlan` 定义1处；本地 `success<T>` 副本0；功能内无自写execute/prepared辅助；材料计量不变 | 完成（2026-10-10）；`normalized_value` 是 `changes.preview` 旧wire字段及已持久提交签名的一部分，保留并在声明处注明；`resource_store.cpp` 的删除已在Qt 6.11.1 IPC构建中编译通过（2026-10-11） |
| P5 单一调度路径 | 8个兼容接口迁入typed registry，删除手写分支；拆分超长注册函数 | `ipc_api.cpp` 无 `op == "` 分支；产品代码单函数≤150行 | 部分完成（2026-10-11）：手写分支改为命名handler与路由表，`dispatch` 475→97行；求解注册函数703→51行；能力描述与 `requested_version` 白名单改为表。重构后IPC CTest 91/91（c1fef8e）。未完成：8个兼容接口迁入typed registry（会改变wire响应，需单独评审）；共享handler内仍有 `op ==` 判断；求解 `start`/`reconcile_result` 已降至113/144行。≤150行目标限于调度与注册路径（[§8](acceptance.md#8-代码可读性门禁)不以固定行数代替职责判断）；路径外仍有10个超长函数，如 `RenderService::dispatch`（330行，含复制计量）与桌面 `desktop.cpp`（无VTK无法编译），登记为存量债务 |
| P6 脚手架与配方 | `tools/new_operation.py` 与一页[新增操作/字段/实体配方](../engineering/adding-operations.md)，以 `features/materials` 为范例 | 新增操作只改功能目录、schema和已有测试文件，根 `CMakeLists.txt` 0改动；骨架登记为不可用、桩函数拒绝且不提交 | 完成（2026-10-10）；P8发现 `basic.json` 混放材料、网格编辑与几何操作，T4两次因范例 `mesh.create_tri3` 在其中而写错文件；2026-10-12起每个功能的操作写在 `schemas/operations/<功能>.json`（`basic.json` 已拆为 `materials`、`mesh_editing`、`geometry`、`mesh_generation`，生成头文件逐字节不变），`new_operation.py` 默认写入该文件；实体字段/类型仍按配方手工改schema（`records.hpp` 重新生成需clang-format 21） |
| P7 工具链与文档 | clang-format 21在Linux/macOS可固定获取；Python格式检查；文档只保留现行说明 | Linux上 `cpp_format`、`generated_entities` 实跑通过 | clang-format部分完成（2026-10-10，Debian `clang-format-21` 21.1.8，242文件通过，`records.hpp` 可逐字节重新生成）；Python格式检查未加（需新增工具依赖） |
| P8 弱模型实测 | 用小模型从冻结基线重做EXT类任务 | 运行前登记成功率与成本阈值；记录真实token、失败原因与修正次数 | 完成（2026-10-12）：Claude Haiku 4.5在独立工作树按同一提示执行，以隐藏验收测试、核心CTest（含 `cpp_format`、`generated_entities`）、改动范围与复制检查判定。T1（已有字段操作）、T2（批量操作）、T3（可选实体字段+操作）各3/3通过（登记阈值≥2/3）；T4（新实体类型，扩展项）0/3：1次自身测试与格式失败，2次功能完整（隐藏验收13/13）但把操作写入 `basic.json` 而非指定的 `mesh_editing.json`。未中断运行约57k—106k token、17—55分钟，失败构建/测试0—1次。7次中途停止（会话重启、限流）的运行未计分，均以同一提示重跑。按功能拆分schema后复测T4（T4b，2026-10-12，提示与隐藏测试不变）：2/3符合登记标准，三次都把操作写入 `mesh_editing.json`（拆分前2/3写错文件）；剩余失败模式是带着失败的自写测试提交（1次）和未按要求写自测（1次，登记标准未覆盖，已标注）。登记文件、隐藏测试与逐次记录不入库，备份于仓库外 |

P2—P5改变生产代码，须先做GitNexus影响分析并保留原回归；P8是验证本轨道目标的唯一方式，不能以强模型结果替代。

## 里程碑定义

M0的目标是一次CLI调用连接唯一engine并取得能力，核心独立于第三方框架。

| 任务 | 范围 | 完成证据 |
|---|---|---|
| M0-01 | CMake配置、types/contracts、核心测试入口 | C++20最小目标构建；核心无需Qt/VTK/SQLite/MCP |
| M0-02 | OperationDescriptor及契约校验 | 能力、参数、单位、错误及副作用单一来源；ProfileRef/TargetBinding与目标上下文；非法输入结构化返回 |
| M0-03 | application最小文档会话、内存适配 | 显式DocumentId/Epoch/Revision，无GUI查询与原子修改/撤销 |
| M0-04 | 本地IPC、engine、CLI | 启动握手；两个客户端共享同一实例；并发启动不产生双核心 |
| M0-05 | 故障与连接测试、版本清单 | 断开重连读取同一状态；协议不兼容明确拒绝；记录实际依赖版本 |

| 阶段 | 交付 | 主要依赖/退出条件 |
|---|---|---|
| M1 文档与格式 | 实体、部件/装配/集合/INCLUDE、引用、最小Nastran IO | 多视角查询与支持子集语义往返；一个Nastran profile、来源/目标编号映射及报告 |
| M2 事务与恢复 | 单提交者、差量历史、工作库、保存/另存/放弃、幂等 | 事务各崩溃窗口、旧Epoch、有限跨重启历史通过 |
| M3 查询和GUI | 条件查询/批选、隐藏/遮挡/穿透/反选、RenderPacket、Qt工作区 | 真实实体映射、人机共用状态、基础交互测量 |
| M4 任务与分析 | 背景任务、资源配额、悬臂梁场景、求解/结果/产物适配 | 真实求解与位移/反力核验、取消及陈旧结果规则 |
| M5 外部AI | MCP桥、一个真实客户端、缺参澄清、受控场景引导 | 完整无窗口分析及GUI交替操作；AI异常集通过 |
| M6 集成与发布准备 | 目标OS打包、恢复/数据迁移回归、文档及性能证据 | 所有P0验收通过，明确支持矩阵和已知限制 |

阶段之间是依赖关系，不要求串行等待；共同写入模块仍需单一责任人。每个切片声明完成前须通过[代码可读性门禁](acceptance.md#8-代码可读性门禁)。

基线1.1的接口落点：M0确定目标上下文及ProfileProvider契约；M1注册单一Nastran能力包并验证编号空间；M2验证专有扩展走事务/恢复；M4验证输入/目标/映射/解析版本与结果语义；M5验证GUI/AI使用同一能力。不单列多求解器阶段。

## 变更控制

- 调整独立engine、持久化、核心依赖或AI入口时更新ADR，注明收益、风险和测试影响。
- 环境缺项时推进不依赖它的任务；相关真实验收保持未完成，不用mock代替。
- 新增卡片、选择方法或库必须有具体用户场景和支持/错误契约，不因参考软件具备它就扩入P0。
- 性能瓶颈先记录剖析证据，不直接引入共享内存、分布式或通用工作流框架。
- 状态变化只更新上面的状态表；不在本文追加带日期的长段落。

## 归档

2026-10-10起，阶段验证报告、原始日志/证据目录、旧代码副本、早期方案（`docs/*-v0.*.md`、骨架提案、Archer对照）、研究资料、界面概念图及一次性工具已移出工作树，完整保存在标签 `archive/pre-cleanup-2026-10-10`。按路径查看：

```sh
git show archive/pre-cleanup-2026-10-10:docs/engineering/core-framework-validation-2026-10-02.md
git ls-tree -r --name-only archive/pre-cleanup-2026-10-10 docs/engineering
```

归档内容只用于追溯，不作为与本基线冲突时的依据。需要时从标签恢复：

- 正式评估SK-01时，R0参考库存 `docs/engineering/reference-inventory.json`、`docs/engineering/module-contracts/` 与 `tools/inventory_reference.py`。
- 恢复完整SDK复制量测量时，`tests/prepare_c3_*.py`、`tests/qt_freetype_*_probe.cpp` 等测量脚本；对应的测试专用CMake选项在P3处理。
- 重新生成旧格式夹具时，`tools/capture_legacy_fixtures.py`（`tests/fixtures/legacy/` 本身仍在仓库中）。
