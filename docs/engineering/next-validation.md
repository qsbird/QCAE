# NEXT-01—03 入口一致性验证

本轮接续 C2，完成实体查询、typed 操作契约和静态生产装配三个限定切片。它们对应[开发计划](../baseline/development-plan.md#c2之后的接续计划)，不替代完整 SK、C3/C4 或 P0 验收。改动前基线为 `7c8f2eb`，在该版本完成 36/36 Qt/VTK/SQLite/IPC Release 回归，再开始实现。

## 行为变化

| 切片 | 实现与验证边界 |
|---|---|
| NEXT-01 | `entity.query` 的枚举、kind、ID、名称、分页及原有组织视图统一读取 `RecordSnapshot`；`entity.references` 使用同一记录引用图。GeometryLine 的三个不一致请求成为真实 CLI 回归；Node、GeometryLine、Mesh 与测试关系记录的字段/引用一致。未知 kind、未支持参数明确拒绝。组织关系按具体记录类型解释，不能把显示用的 kind 标签当成 C++ 类型。 |
| NEXT-02 | typed 请求贯通正整数 `requested_version` 与严格的 `expected_profile` 三字段；省略版本时使用已安装版本以兼容旧客户端。能力按已注册处理器发布，目标相关处理器在应用准备阶段校验 profile。测试证明错误请求不改变修订/历史、原成功事实可在 profile 定义变化后重放；移除处理器后仍可通过 `operations.get` 查询调用者的原回执。schema 新增/修改/删除都触发增量生成，无参数输入编译并校验空对象。 |
| NEXT-03 | `EngineContribution` 在启动阶段收集记录、规则和操作；记录注册表冻结后注入唯一应用。默认功能注册从 JSON 宿主移到 `engine_contributions.cpp`。生产入口与测试入口链接同一个实际服务器实现。仅测试链接关系贡献，真实 IPC 覆盖创建、规则/引用拒绝、查询、undo/redo、保存/正常打开、SQLite 崩溃恢复；默认生产入口不发布测试操作。重复贡献、重复操作及覆盖宿主保留名称均拒绝。 |

Profile 验证使用测试专用处理器证明合同链路，不新增正式分析操作或求解器。通用注册表检查字段存在与类型，目标适用性由具体 handler 在 `RecordPrepare` 中判断；不能把 `requires_expected_profile` 宣称为对任意自定义处理器的自动语义验证。未安装的操作版本不能直接执行或重放，请用保留事实查询；本轮不实现运行中动态替换贡献。

## 验证记录

| 检查 | 结果 | 原始证据 |
|---|---|---|
| 改动前完整 C2 Release | 36/36 | [基线元数据](evidence/next/baseline.json)、[逐项输出](evidence/next/baseline-desktop.log) |
| 最终完整 Release（IPC/SQLite/Qt/VTK） | 40/40，44.45 秒 | [汇总](evidence/next/release-tests.log)、[逐项输出](evidence/next/release-detailed.log) |
| 独立纯核心 Release（关闭 IPC/SQLite/桌面） | 26/26 | [汇总](evidence/next/core-tests.log)、[逐项输出](evidence/next/core-detailed.log) |
| 外部公共头消费者 | 44/44 编译链接 | [数量](evidence/next/public-consumer-count.txt)、[完整及最终增量构建](evidence/next/public-build.log.gz) |
| 真实测试贡献/默认生产入口 | 2/2；83 + 3 次请求 | [结构化摘要](evidence/next/contributions/engine-contributions-summary.json)、[贡献轨迹](evidence/next/contributions/contribution-transcript.json.gz) |
| 真实记录查询 | GeometryLine 三请求、组织视图、1003 条入向引用均通过 | [几何轨迹](evidence/next/queries/geometry-transcript.json.gz)、[组织轨迹](evidence/next/queries/organization-transcript.json.gz)、最终 Release 逐项输出 |
| 受影响 ASan/UBSan（Debug，IPC/SQLite，关闭桌面） | 12/12；测试夹具修正后存储单项再查 1/1，无 sanitizer 诊断 | [原始 12 项](evidence/next/sanitizer-tests.log)、[详细输出](evidence/next/sanitizer-detailed.log)、[单项复查](evidence/next/sanitizer-record-store-recheck.log) |
| 修正后存储子进程夹具 | 连续 20/20，60 次精确退出及对应恢复/WAL 断言 | [重复运行](evidence/next/record-store-repeat-20.log) |
| 架构、格式、设计、空白检查 | 41 生产编译单元、27 目标边界；112 个 C++ 文件格式通过；全部门禁通过 | [架构报告](evidence/next/architecture.json)、[静态检查](evidence/next/final-static-checks.log) |

macOS 本机已有 AppleClang 21、Qt 6.11.1、VTK 9.7 与 SQLite 工具链；本轮构建记录见[Release](evidence/next/release-build.log.gz)、[纯核心](evidence/next/core-build.log.gz)、[Sanitizer](evidence/next/sanitizer-build.log.gz)。这里不声明 LeakSanitizer、跨平台或 sanitizer 图形验证。12 项 sanitizer 与夹具修正后的单项复查分别留存，不合并成一次全新 13 项运行。

[源摘要](evidence/next/source-manifest.json)覆盖当前 191 个生产代码、schema、测试和构建/检查文件；普通文档不参与。最终验证开始后仅修改了已诊断的测试子进程夹具，[变更摘要](evidence/next/validation-source-changes.json)保留前后哈希。最终全套和存储 sanitizer 复查使用修正版本。归档不包含运行数据库或工程快照，[证据清单](evidence/next/evidence-manifest.json)提供各文件 SHA-256；C2 历史证据保持原样。

## QG-01—03 审查

链路为 `apps/engine/main.cpp` 选择贡献 → `qcae_run_engine` 装配冻结注册表、可选 SQLite 和同一 `MemoryApplication/RecordApplication` → `ipc_api` 选择入口 → query 读取不可变快照或 typed registry 解码 → feature 准备候选 → 原应用统一提交。`engine_host.cpp` 是生产和测试共用的完整服务器，`qcae_engine_host` 有实际源代码及消费者，不是空架构层。具体测试类型只存在于 `tests/`，提交、历史和存储算法没有增加该类型的分支。

Astra 极高负责查询，Sol 极高负责契约，Luna 极高负责真实贡献工作流；主代理集成装配并运行总回归。Sol 对查询、Astra 对契约/装配进行交叉审查。修复的审查问题包括：组织视图按 kind 字符串误认具体类型、贡献覆盖宿主名称导致能力声明与分派不一致、处理器停用后原回执被能力门禁遮挡。Luna 复核实际服务器/贡献链路；Sol 另行确认存储测试进程修正保留全部退出与恢复/WAL 断言。共享任务返回值转换放在适配器私有头中，避免注册逻辑移出后复制同一响应规则。

记录 ID、文档/epoch/revision、原子提交和持久历史继续由已有应用管理。读取测试检查整模型桥接计数不增长；组织视图保留 M1 中间节点/拥有者排除语义。旧交换、编辑兼容与渲染边界仍存在，不能将查询局部性证据扩大为全应用性能保证。贡献回调属于可信编译期代码，没有不受信代码加载器或动态插件 ABI。

## 测试故障与处理

一次后续完整 Release 回归得到 39/40：`record_store` 的故障注入子进程未按预期返回 17。已保留[原始输出](evidence/next/release-failure-tests.log)及[详细日志](evidence/next/release-failure-detailed.log)。[脱敏诊断及崩溃栈](evidence/next/record-store-diagnosis.txt)表明子进程在 `fork` 后、`exec` 前的系统 SQLite 打开路径中，崩溃于 `libsystem_trace` 日志初始化；唯一临时目录排除了本轮最初怀疑的目录碰撞。

仅修正测试夹具：由 `posix_spawnp` 启动新进程映像，再在同一个 `before_db_commit` / `after_db_commit` 注入点 `_exit(17)`；保留提交代次、记录数量和值的断言。新 WAL 夹具仍在提交后 `_exit(23)`，不运行 SQLite 关闭/检查点；加入退出码和信号诊断。没有放宽断言或修改生产存储代码。历史 C1 锁测试的另一次未定位失败仍按[C2报告](c2-validation.md)保留，不能由这次诊断推定为同一原因或宣称一并修复。

## 复现与后续

使用当前仓库的 CMake 配置构建：

```sh
cmake -S . -B build-next-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=OFF -DQCAE_BUILD_STORAGE=OFF -DQCAE_BUILD_DESKTOP=OFF
cmake --build build-next-core -j2
ctest --test-dir build-next-core --output-on-failure -j2

cmake -S . -B build-next-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-next-desktop -j2
ctest --test-dir build-next-desktop --output-on-failure -j2

python3 tools/check_design.py
python3 tools/check_cpp_format.py
python3 tools/check_architecture.py --build-dir build-next-desktop
```

Qt/VTK 路径是本机已有依赖位置，未引入依赖。真实 IPC 需要系统允许本地域套接字；Unix 测试使用规范化的 `/tmp` 短路径。此次没有 Linux/Windows、容量分档、真实求解器或外部 AI 验收。

下一步为 C3 的第一个可见切片：让现有 GeometryLine 显示并拾取到稳定实体 ID，再接建线/网格工具及预览交互。事件、分块资源、局部更新按原 SK 合同继续推进；不会因为本轮通过而提前宣称 C3/C4 完成。
