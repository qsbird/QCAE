# C2 接口与后台网格检查点

本轮完成 [C1/C2 阶段合同](checkpoints-c1-c2.md)，停止在 C2。C1 独立提交为 `92b115124e26454fe1204ec410bee83f6a5774ac`，标签 `checkpoint/c1`；C2 使用标签 `checkpoint/c2`。本报告只验收这个阶段，不将完整 SK-01—14 的 97 项指标标为通过。

## 功能与量化结果

| 项目 | 实测结果 | 证据 |
| --- | --- | --- |
| 完整 Release 回归，IPC/SQLite/Qt/VTK 开启 | 36/36，失败 0 | [汇总](evidence/c2/release-tests.log)、[逐项原始输出](evidence/c2/release-detailed.log) |
| 独立纯核心 Release，IPC/SQLite/桌面关闭 | 25/25，失败 0 | [核心日志](evidence/c2/core-tests.log) |
| 外部公共头消费者，只使用目标公开依赖 | 42/42 编译链接完成 | [数量](evidence/c2/public-consumer-count.txt)、[最终增量构建](evidence/c2/public-build.log.gz) |
| CMake 实际依赖、生产源归属检查 | 39 个生产编译单元；26 个目标；错误 0 | [依赖报告](evidence/c2/architecture.json) |
| 空模型→直线→后台网格→材料/截面→编辑→历史→保存/打开/故障恢复 | CLI 3/3，薄 Python IPC 客户端 3/3；每次 11 节点、10 个 Line2 梁；语义差异 0 | [工作流报告](evidence/c2/workflow/workflow-report.json) |
| SQLite 真实任务成功、提交前取消、输入过期 | 各 10 次，共 30/30；取消/过期错误模型提交 0 | release-detailed.log 中 `backend: sqlite` 的 30 条 JSON |
| 一次修改后执行 100 轮 undo/redo | 语义差异 0；修订增量 201 | release-detailed.log 中 geometry_mesh_sqlite |
| 2 个 worker / 排队上限 8；任务和模型原子发布 | runtime、geometry_mesh_sqlite、typed_host 全部通过 | release-detailed.log |
| 旧格式夹具只读迁移 | 6/6；源 SHA-256 与冻结清单相同；旁路文件 0 | [夹具报告](evidence/c2/fixtures.json)、legacy_migration 测试 |
| schema 反例与 ID 稳定性 | 8 个反例拒绝；10 种注册顺序结果一致 | record_document、generated_entities 测试 |

六次完整工作流均包含真实进程中止后恢复、历史重放、操作事实查询及正常重新打开，比较工程实体语义而不比较随机生成的 ID。六份语义 SHA-256 均为 `12c8eae72acfbda466fedc33daaa99583a1dd067329e6d0232d1709abcc870e0`。每次调用的原始请求/响应以 `workflow/*-transcript.json.gz` 保存，可用 `gzip -dc` 查看；不提交运行工作库或临时工程。

## 可复现命令与环境

本次环境：macOS 26.6.2（25G83）、arm64、AppleClang 21.0.0（clang-2100.1.1.101）、Qt 6.11.1、VTK 9.7.0、SQLite 3.51.0。构建为 C++20、Ninja、Release；测试使用 Release 中仍执行的检查。VTK 路径是本机已有依赖安装，不是仓库内提交的依赖二进制。没有跨平台或大模型性能结论。

```sh
cmake -S . -B build-c2-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-c2-desktop -j2
ctest --test-dir build-c2-desktop --output-on-failure -j2

cmake -S . -B build-c2-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=OFF -DQCAE_BUILD_STORAGE=OFF -DQCAE_BUILD_DESKTOP=OFF
cmake --build build-c2-core -j2
ctest --test-dir build-c2-core --output-on-failure -j2

cmake -S tests/public_api_consumers -B build-c2-public -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON \
  -DQCAE_BUILD_DESKTOP=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-c2-public -j2
python3 tools/check_architecture.py --build-dir build-c2-desktop \
  --report build-c2-desktop/architecture-report.json
python3 tools/check_design.py
python3 tools/check_cpp_format.py
git diff --check
```

本机沙箱禁止绑定本地域套接字；实际 IPC 与桌面测试在允许这类本地操作的执行环境完成。[沙箱启动失败日志](evidence/c2/sandbox-socket-denied.log)保留 `QLocalServer::listen: Unknown error 1`，随后相同测试及全量回归通过。Unix 测试临时目录使用规范化的 `/tmp`，避免 macOS 专有路径和过长套接字路径；Linux/Windows 未执行。

## QG-01—03 与代码审查

入口链路为 `apps/engine/main.cpp` 装配唯一 `RecordApplication` → `adapters/engine_api/src/ipc_api.cpp` 分派 → `typed_host.cpp` 参数转换/操作发现 → `modules/operations` 注册处理器与 `features/*` 准备变更 → `modules/application/src/core.cpp` 唯一提交 → `adapters/storage_sqlite` 持久化批次。GUI 原兼容入口使用同一应用协调器；CLI 和薄 Python 客户端没有第二份业务状态。

权威模型是 `DocumentView` 的不可变记录；`EditSession` 只产生候选。稳定实体 ID、字段 ID、求解器编号和位置分离。写入检查文档/epoch/revision、幂等键与配额。后台网格持有输入快照，提交时复核版本；取消在提交前生效。模型、历史、创建回执与任务成功事实在同一 SQLite 批次发布。恢复保留文档 ID、更新 epoch；正常打开创建新文档 ID，两个路径分别测试。

GPT 6 Sol 极高完成跨层审查，修正旧材料预览与 typed 输入在极小模量换算上的重复规则，统一到 `modules/parameters`；`quantity_entry_consistency` 和原模型测试保护两个入口。旧生成目录数组更名为 `operation_catalog`，由公共消费者验证能与新 `operations` 命名空间同处一个编译单元。GPT 6 Sol 中复核交付命令与边界，发现并修正测试临时路径的可移植性问题。没有为 C3 增加空模块。

原有核心、IPC、持久化和桌面回归全部纳入最终 36 项。格式、设计与依赖门禁通过，生成契约/实体/输入校验通过。字段读取和新增查询从记录层读取；旧格式桥保留在兼容边界。记录局部性测试不能证明全应用的大模型或帧率性能。

## Sanitizer 与保留问题

独立 Debug 构建开启 ASan/UBSan，IPC/SQLite 开启、桌面关闭。首次非套接字测试 29 项中 28 项通过；唯一失败为临时只读夹具错误假定 SQLite 关闭时必定清理空 WAL/SHM。确认最新数据已在主库、WAL 为 0 字节后，只修改临时夹具准备逻辑，相关 2 项复查通过。共享单位规则修正后，6 项受影响测试再次通过。未出现 ASan/UBSan 诊断；这不是一次未经中断的 29/29 日志，也不声称 LeakSanitizer、图形或真实 IPC 的 sanitizer 验收。[原始日志和复查记录](evidence/c2/sanitizers/summary.md)均保留。

C1 曾出现一次 SQLite 跨进程锁子进程退出断言失败；192 次针对性复测未复现，C1 34/34 与 C2 36/36 最终回归通过。原断言保留并增强 case/退出状态诊断，原因仍未定位，不能称为已修复。详见 [C1 历史失败](c1-validation.md)。

## 停止边界

新几何/网格功能通过 CLI/IPC 可用，尚无对应新 GUI 工具。正常打开后查询前一个文档任务的历史入口未实现；同一文档故障恢复后的任务查询已验证。当前仍仅为线几何和均匀 Line2 网格，没有接入 CAD/第三方自动网格器。真实 Nastran 执行、外部 AI、完整 P0、亿级性能、C3/C4 均未验收。

交接入口为 [C2 运行说明](../implementation/c2-handoff.md)。[源文件摘要](evidence/c2/source-manifest.json)记录通过最终测试的生产代码、测试、schema、构建及检查工具；文档不参与摘要，避免证据自引用。[证据清单](evidence/c2/evidence-manifest.json)记录归档文件的 SHA-256。Git 标签固定完整交付版本。
