# 开发计划

需求见[requirements.md](requirements.md)，完成标准见[acceptance.md](acceptance.md)，骨架量化合同见[skeleton-acceptance.md](skeleton-acceptance.md)。本文只记录当前状态、已确认决策和接续顺序；阶段报告与原始证据不再放在工作树中（见[归档](#归档)）。

## 当前状态

截至2026-10-10（`codex/c3-resource-sync` 的 `896ab71` 之后）。“已实现”指代码与适用回归存在，不等于对应 TST/SK 验收通过。

| 里程碑 | 已实现 | 未完成或未验收 |
|---|---|---|
| M0 核心与本地入口 | C++20纯核心（不依赖Qt/VTK/SQLite/MCP）、本地engine/CLI、握手与共享实例 | — |
| M1 文档与格式 | 实体/组织/引用、受控BDF资源包导入与导出、ImportReport/ExportReport、编号映射 | 其他方言与卡片按需扩展 |
| M2 事务与恢复 | 统一 `RecordApplication`/`EditSession` 提交链、document/epoch/revision、幂等、有限持久历史、SQLite工作库、保存/另存/恢复/放弃 | — |
| M3 查询与桌面 | 条件查询/选择句柄、Qt/VTK工作区、分块资源、事件缺口重同步、局部显示、布局恢复、建线/网格/材料/分析工具 | 完整SK-10/12；三档性能；原累计图形压力失败未关闭 |
| M4 任务与分析 | 有界后台任务、分析检查、冻结来源的BDF文件发布、fixture结果读取、显式MYSTRAN 19.0.0后端：真实悬臂梁三次运行396/396分量通过 | 在途取消/故障窗口、全部悬臂梁变体、三入口场景；其他Nastran方言未配置 |
| M5 外部AI | MCP桥按 `capabilities.list` 动态生成工具；双MCP连接共享同一engine | 真实AI会话仅有部分诊断（TST-I01部分）；TST-I01—I10未通过 |
| M6 集成与发布 | macOS本机包（`f2d2e7f`，265项回归及重定位包SQLite/双MCP/桌面验证） | 包未随新源码刷新；Linux/GCC移植进行中；性能证据；发布 |

骨架与接口：

- SK-01—14正式通过0/14。R1已提交，R0部分完成，R2—R4完成C1/C2限定范围，R5推迟。
- C4三项独立扩展已实现：EXT-01 Material可选说明（6个产品文件）、EXT-02批量节点平移（4个）、EXT-03 Tri3拓扑（10个），保护区触碰均为0。EXT-04/05与共同最终Release集成未完成。
- 52个typed操作由 `schemas/operations/` 生成；`changes.preview`、`model.export_preview`、`selection.*`、`view.*`、`resources.*` 等8个兼容接口仍为手写解码。
- 复制账本完整覆盖0/20；完整SDK覆盖按2026-10-01决定推迟为性能验收项。
- 2026-10-10 Linux GCC纯核心构建：39项通过，`cpp_format`、`generated_entities`因缺少clang-format 21跳过。

## 已确认的用户决策

| 日期 | 决策 |
|---|---|
| 2026-09-30 | 依次推进C3收敛、剩余骨架机制、C4独立扩展、完整SK验收和M4/M5真实环境接续 |
| 2026-10-01 | 完整SDK内部复制量覆盖移出C4启动前置条件，保留为后续性能验收；已测预算（节点589824、材料69632、事务元数据65536字节）不变 |
| 2026-10-02 | 优先完成核心框架质量，暂停非必要性能优化；重复未通过项须单独报告 |
| 2026-10-03 | 新增显式MYSTRAN 19.0.0本机后端，复用共享Nastran/BDF语义、唯一engine、TaskService与SQLite结果链（[ADR-09](../architecture/mystran-backend.md)） |
| 2026-10-10 | 启动“弱模型低成本横向扩展”工作轨道；仓库降噪，阶段证据和早期设计移出工作树并备份 |

## 当前工作轨道：弱模型可低成本横向扩展

目标：新增操作、字段或实体类型时，较弱的模型只需少量固定上下文、遵循一个范例即可正确完成；遗漏必须在构建或测试中暴露，而不是静默产生错误行为。每个阶段独立提交，先保护既有行为，再按AGENTS.md的检查项验证。

| 阶段 | 目标 | 检查点（退出条件） | 状态 |
|---|---|---|---|
| P1 仓库降噪 | 工作树只含产品源码、测试、生成输入和现行文档 | 跟踪文件≤500、`docs/`≤1MiB；无证据或旧代码副本进入代码索引；`check_design.py`通过；核心构建/测试不回退；备份可校验 | 完成（2026-10-10） |
| P2 未知实体默认拒绝 | 分析输入/检查、Nastran导出、导出预览等消费者声明支持的记录类型，未声明类型自动 `unsupported_capability` | 新增测试实体不改任何消费者即被正确拒绝；Tri3的4处特例分支删除；“实体类型×消费者”矩阵测试 | 完成（2026-10-10）；IPC导出预览改动已在Qt 6.11.1 IPC构建中编译，IPC CTest 91/91（2026-10-11） |
| P3 测量与业务分离 | 复制账本由共享 `qcae_feature_support` 与按schema生成的输入计量承担 | `features/` 中ledger引用为0；材料操作计量与改造前逐项一致；移除无生成脚本的 `QCAE_C3_*` 测试选项；核心/IPC回归不变 | 核心完成（2026-10-10）；`QCAE_C3_*` 选项保留：它们是2026-10-01决定保留的SDK复制量后续性能验收的测量入口（`json_ledger.hpp` 亦受其控制），删除需另行决定 |
| P4 共享类型收敛 | 材料、网格编辑、分析三个功能统一使用 `qcae_feature_support`；`success`/`forward_failure` 移入foundation的 `Result` | `OperationPlan` 定义1处；本地 `success<T>` 副本0；功能内无自写execute/prepared辅助；材料计量不变 | 完成（2026-10-10）；`normalized_value` 是 `changes.preview` 旧wire字段及已持久提交签名的一部分，保留并在声明处注明；`resource_store.cpp` 的删除已在Qt 6.11.1 IPC构建中编译通过（2026-10-11） |
| P5 单一调度路径 | 8个兼容接口迁入typed registry，删除手写分支；拆分超长注册函数 | `ipc_api.cpp` 无 `op == "` 分支；产品代码单函数≤150行 | 部分完成（2026-10-11）：手写分支改为命名handler与路由表，`dispatch` 475→97行；求解注册函数703→51行；能力描述与 `requested_version` 白名单改为表。未完成：8个兼容接口迁入typed registry（会改变wire响应，需单独评审）；共享handler内仍有 `op ==` 判断；求解 `start`/`reconcile_result` 已降至113/144行。≤150行目标限于调度与注册路径（[§8](acceptance.md#8-代码可读性门禁)不以固定行数代替职责判断）；路径外仍有10个超长函数，如 `RenderService::dispatch`（330行，含复制计量）与桌面 `desktop.cpp`（无VTK无法编译），登记为存量债务 |
| P6 脚手架与配方 | `tools/new_operation.py` 与一页[新增操作/字段/实体配方](../engineering/adding-operations.md)，以 `features/materials` 为范例 | 新增操作只改功能目录、schema和已有测试文件，根 `CMakeLists.txt` 0改动；骨架登记为不可用、桩函数拒绝且不提交 | 完成（2026-10-10）；实体字段/类型仍按配方手工改schema（`records.hpp` 重新生成需clang-format 21） |
| P7 工具链与文档 | clang-format 21在Linux/macOS可固定获取；Python格式检查；文档只保留现行说明 | Linux上 `cpp_format`、`generated_entities` 实跑通过 | clang-format部分完成（2026-10-10，Debian `clang-format-21` 21.1.8，242文件通过，`records.hpp` 可逐字节重新生成）；Python格式检查未加（需新增工具依赖） |
| P8 弱模型实测 | 用小模型从冻结基线重做EXT类任务 | 运行前登记成功率与成本阈值；记录真实token、失败原因与修正次数 | 进行中（2026-10-11）：Haiku执行、隐藏验收测试，阈值为T1—T3各≥2/3成功 |

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
