# C2 接续说明

C2 提供一条由本地 `qcae-engine` 执行的几何、真实后台网格和模型编辑流程。引擎以 SQLite 工作库保存任务与模型状态；CLI 和薄 IPC 客户端共用同一应用提交路径。当前只记录 C2 的运行方式和边界，不表示完整 P0 已验收。

## 构建、启动与测试

以下是 macOS/Homebrew Qt 环境的本地引擎构建命令；按实际安装位置调整 `CMAKE_PREFIX_PATH`。启用 IPC 和 SQLite 才会构建 C2 IPC 工作流测试。

```sh
cmake -S . -B build-local -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt
cmake --build build-local -j4
ctest --test-dir build-local --output-on-failure
```

聚焦 C2 任务、持久化和 CLI/脚本流程的测试：

```sh
ctest --test-dir build-local --output-on-failure \
  -R '^(typed_host|geometry_mesh|geometry_mesh_sqlite|runtime|record_store|legacy_migration|c2_workflow)$'
```

`c2_workflow` 在 Unix 上通过真实 SQLite 工作库运行 3 次 CLI 流程和 3 次薄 Python IPC 客户端流程，并比较最终工程语义。工作库使用临时目录，调用记录和汇总报告保存在构建目录的 `c2-evidence/workflow/`；这段测试客户端不是通用脚本运行时。

手动启动一个持久引擎（保持该终端运行）：

```sh
mkdir -p /private/tmp/qcae-c2
./build-local/qcae-engine \
  --socket /private/tmp/qcae-c2/engine.sock \
  --workspace /private/tmp/qcae-c2/work.sqlite
```

另一个终端可用 CLI 查询能力：

```sh
printf '%s\n' '{"api_version":"1.1","request_id":"caps-1","operation":"capabilities.list","parameters":{}}' \
  | ./build-local/qcae-cli --socket /private/tmp/qcae-c2/engine.sock --no-start
```

若工作库已有可恢复状态，先显式调用 `project.open`，参数为 `{"mode":"recover"}`；空工作库则先调用 `project.create`。CLI 的 JSON 行协议及字段定义见[engine API](../../adapters/engine_api/README.md)。

## C2 新操作与输入

七项 typed 模型操作如下。数组坐标单位为 mm；截面尺寸字段名带有对应的 mm²/mm⁴ 单位。

| 操作 | 输入要点 |
| --- | --- |
| `geometry.create_line` | `start_mm`、`end_mm`：各含三个数的数组；C2 流程使用 `[0,0,0]` 到 `[1000,0,0]`。 |
| `mesh.generate_line` | `geometry_id`、正整数 `segments`（上限 100000）；10 段生成 11 个节点和 10 个 Line2 梁。 |
| `material.create` | `name`、`young_modulus: {value, unit}`，可选 `poisson_ratio`。 |
| `material.set_young_modulus` | `entity_id`、`young_modulus: {value, unit}`。 |
| `section.create` | `name`、`material_id`、`area_mm2`、`i1_mm4`、`i2_mm4`、`torsion_mm4`。 |
| `beam.assign_section` | `beam_ids` 字符串数组、`section_id`。网格生成后梁默认未分配截面。 |
| `node.move` | `entity_id`、`position_mm` 三数数组。 |

模型写请求都带 API 版本 `1.1`、活动文档的 `document_id` / `document_epoch`、十进制字符串 `expected_revision`、`idempotency_key` 和对象 `parameters`。任务状态、取消、协调及 `entity.fields` 读取请求带文档身份和 epoch，不带预期修订。

`mesh.generate_line` 返回 `task_id` 后以 `task.status` 读取事件和状态，直到终态。`task.cancel` 请求取消；提交前接受的取消不会发布网格，已经提交的结果不会被回滚。`task.reconcile` 不接收参数，仅在 worker 和队列空闲时供本地宿主处理确定的任务状态写入失败：它可把未完成记录标为中断，不重跑任务、不改模型修订。若应用提交结果不确定，必须先显式恢复工程，再协调任务。完整请求字段与能力边界见[engine API](../../adapters/engine_api/README.md)。

## 迁移旧工作库

迁移从静止的旧 SQLite 工作库只读导入，并仅发布到一个全新的目标工作库。目标文件必须不存在；启动后仍要通过 `project.open` 的 `mode: recover` 显式恢复目标状态。

```sh
mkdir -p /private/tmp/qcae-c2
./build-local/qcae-engine \
  --socket /private/tmp/qcae-c2/migrate.sock \
  --migrate-from /absolute/path/source.sqlite \
  --workspace /private/tmp/qcae-c2/new-workspace.sqlite
```

具体兼容格式受迁移器支持范围约束；该命令不会修改源库。[引擎说明](../../apps/engine/README.md)记录了参数限制。

## GUI 与后续检查点

当前桌面端保留先前功能，但 C2 新增的几何创建和线网格任务尚未接入 GUI；这些操作从 `qcae-cli` 或薄 IPC 客户端调用。已有桌面能力与限制见[M2/M3 运行说明](m2-m3.md)。

工作库故障恢复保留原文档 ID，可以查询原任务结果。正常打开工程会创建新的文档 ID；当前 `task.status` 只允许查询属于活动文档的任务，旧文档任务的历史查询入口留待后续实现。

C3/C4 仍是后续检查项：C3 包括 GUI 工具生命周期、资源通道、增量显示和结果接入；C4 为扩展实验。本轮交付止于 C2，不据此承诺这些能力或完整 P0 验收。阶段边界见[C1/C2 检查点](../engineering/checkpoints-c1-c2.md)。

## 验证记录

本轮在 macOS 26.6.2 arm64、AppleClang 21.0.0、Qt 6.11.1、VTK 9.7.0、SQLite 3.51.0 环境完成验证；未验证 Linux/Windows。CMake Release 全组件构建回归 36/36；关闭 Qt/VTK/SQLite 的纯核心回归 25/25；独立公共头消费者编译 42/42。

CLI 三次与脚本三次流程共 6/6，六份工程语义摘要相同；SQLite 任务成功、取消、过期各十次，共 30/30；一次修改与 100 轮 undo/redo 的修订增量精确为 201。

可中断版本为 `checkpoint/c1`（`92b1151`）和 `checkpoint/c2`。完整命令、原始日志、源文件摘要、审查结论及历史失败记录见 [C2 验证报告](../engineering/c2-validation.md)。完整 SK-01—14 与 P0 尚未验收。
