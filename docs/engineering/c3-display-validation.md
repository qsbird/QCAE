# C3 首个显示交互切片

从 `f464dde` 的完整 Release 40/40 基线开始，按[切片计划](c3-display-plan.md)实现 GeometryLine 显示与建线/网格桌面工具。此次没有新增依赖。完整 C3、SK 和 P0 仍未完成。

## 实现范围

- `RecordSnapshot → query → view.render_packet` 直接产生带真实稳定 ID 的几何线；端点只作为显示坐标，不制造节点。几何参与已有隐藏、空间查询和图形候选选择，受相同文档/epoch/revision、视图版本与数量配额约束。
- VTK 增加几何线显示、高亮、单击、穿透框选与可见框选。紫色线段预览独立于工程对象，无实体 ID、不能被拾取；取消不会增加修订或历史。顺便修复已有“包含式框选”模式影响普通单击的问题。
- 桌面 Modeling 面板输入 mm 坐标，先预览，再通过 `geometry.create_line` 应用；选中几何线后调用已有 `mesh.generate_line`，显示任务进度/状态并调用 `task.cancel`。网格仍使用引擎内置均匀一维离散器，没有重复实现网格算法或候选网格预览。
- GUI、CLI 使用同一引擎事务与历史；真实窗口流程覆盖双击防重、撤销/重做及外部客户端保存/关闭/正常打开后的界面同步。保存/打开通过独立真实客户端触发，测试不模拟原生文件对话框。

## 生命周期与审查

入口为 `DesktopWindow → ModelingTools → DesktopClient → typed handler/TaskService → RecordApplication`；显示走不可变记录快照到公共 DTO，再由 VTK 维护可重建缓存。`ModelingTools` 只持有输入、预览和单个已知任务的交互状态，权威实体、任务事实、事务和持久历史仍由引擎拥有。公共核心 DTO 不包含 Qt/VTK 类型。

Astra 极高负责记录查询/IPC和工具状态回归，Sol 极高负责 VTK 与窗口生命周期，Luna 极高负责真实 IPC 流程；主代理负责工具入口、集成与总体验证。交叉审查修复了以下问题，并加入受控时序回归：

1. 超时和瞬时断线保留原写入意图，包括原参数、版本、期望修订和幂等键。断线禁用控件，重连同一文档/epoch 后仅由用户显式重试原请求；新请求 ID 不改变逻辑意图。已知网格任务恢复查询而不是再次提交。文档或 epoch 更换则清除旧工具状态，不跨上下文自动重放。
2. 旧 `task.status` 回包不能覆盖较新的取消结果；失败查询提供只读 Refresh task，不重新生成网格。提交期间与取消请求期间分别追踪，断线时不会混淆两者。
3. 视口拾取要求当前安装的包与文档、模型及视图版本一致；选择请求具有递增代号，较早的 evaluate/get 回包不能替换用户较新的选择。同一模型内视图刷新期间保留用户最后一次尚未完成的选择，待新显示包就绪后再验证；模型上下文更换则清除该意图。回调还受对象生命周期和会话代号保护。

自动格式、架构依赖与同一套改动前/后回归用于 QG-01—03。真实 VTK 测试使用图形窗口；工具/窗口协议测试使用受控本地域协议端点；真实引擎流程另行执行，不能用协议替身代替业务正确性证据。

## 验证与证据

| 检查 | 结果 | 证据 |
|---|---|---|
| 改动前完整 Release | 40/40，21.65 秒；`f464dde` | [基线汇总](evidence/c3-display/baseline-tests.log)、[详细输出](evidence/c3-display/baseline-detailed.log) |
| 最终完整 Release：IPC/SQLite/Qt/VTK | 44/44，52.41 秒 | [汇总](evidence/c3-display/release-tests.log)、[逐项输出](evidence/c3-display/release-detailed.log) |
| 纯核心 Release：关闭 IPC/存储/桌面 | 26/26，12.79 秒 | [汇总](evidence/c3-display/core-tests.log)、[逐项输出](evidence/c3-display/core-detailed.log) |
| 外部公共头消费者 | 44/44 编译链接 | [数量](evidence/c3-display/public-consumer-count.txt)、[构建](evidence/c3-display/public-build.log.gz) |
| 新增真实引擎显示流程 | 60 次请求；几何/混合包、空间/候选/隐藏选择、旧版本拒绝、网格、稳定 ID、保存重开 | [摘要](evidence/c3-display/display-ipc/c3-display-summary.json)、[完整轨迹](evidence/c3-display/display-ipc/c3-display-transcript.json.gz) |
| Qt/VTK 桌面与时序 | 完整套件含真实窗口工作流、10 个工具状态用例、3 个窗口选择时序用例及原有 VTK 图形测试 | 最终 Release 逐项输出；[选择定向日志](evidence/c3-display/vtk/selection-ctest.log) |
| 自动门禁 | clang-format 21：117 文件；42 生产编译单元、27 CMake 目标边界；设计与空白检查通过 | [静态检查](evidence/c3-display/final-static-checks.log)、[架构报告](evidence/c3-display/architecture.json) |

真实窗口截图已经目视检查：[未提交预览](evidence/c3-display/desktop/line-preview.png)、[网格完成](evidence/c3-display/desktop/line-mesh.png)、[保存后正常重开](evidence/c3-display/desktop/saved-reopened.png)。VTK 定向图像另外保留[几何高亮](evidence/c3-display/vtk/geometry-highlight.png)、[独立预览](evidence/c3-display/vtk/geometry-preview.png)与[清除预览](evidence/c3-display/vtk/geometry-preview-cleared.png)。不把截图或小模型流程时长作为性能门槛达标证据。

[源摘要](evidence/c3-display/source-manifest.json)覆盖 197 个代码、测试、schema 与构建/检查文件；最终全套运行后哈希未变化。[集成修正前后摘要](evidence/c3-display/validation-source-changes.json)仅包含桌面选择调度和对应时序测试的变化；纯核心不受该修正影响。最终构建记录见[Release](evidence/c3-display/release-build.log.gz)、[纯核心](evidence/c3-display/core-build.log.gz)。[证据清单](evidence/c3-display/evidence-manifest.json)列出归档文件 SHA-256，不含运行数据库或工程快照。

## 集成期间发现的问题

一次中间完整回归为 43/44：新建几何后，树已更新而显示包尚在刷新时，树选择的回包被新增版本保护丢弃，导致网格按钮一直不可用。修复保留同模型内最后一次尚未完成的选择意图，显示包就绪后再验证；新增受控延迟回包测试覆盖该顺序。保留[失败汇总](evidence/c3-display/release-intermediate-tests.log)及[详细输出](evidence/c3-display/release-intermediate-detailed.log)，没有增加固定等待或削弱原工作流断言。

一次中间纯核心检查为 25/26，唯一失败是新增文档链接先于目标文件写入；补齐实施文档后相同核心套件 26/26 通过。[原始日志](evidence/c3-display/core-intermediate-tests.log)保留该构建期间状态。

## 剩余边界

本切片只覆盖 TST-F01/F03/F06、TST-A07/A12 和 SK-04/10/12 的相关子集。当前显示仍使用完整 JSON 包和轮询；版本化分块资源、事件缺口重同步、局部显示更新、通用拓扑/显示贡献和容量性能门槛继续按开发计划实施。没有 GUI 重启后的工具/任务浏览器恢复，也不将同文档瞬时重连扩大解释为跨 epoch 的工具恢复。

外部几何/网格后端仍留待独立适配切片明确输入、输出和验收场景后接入；本轮没有 CAD 内核、外部网格库、三角形扩展、真实求解器或真实 AI 客户端。完整 C3 及外部能力接入不能由本次小模型窗口测试推定为已完成。

## 复现

```sh
cmake -S . -B build-c3-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-c3-desktop -j2
ctest --test-dir build-c3-desktop --output-on-failure -j2

cmake -S . -B build-next-core -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=OFF -DQCAE_BUILD_STORAGE=OFF -DQCAE_BUILD_DESKTOP=OFF
cmake --build build-next-core -j2
ctest --test-dir build-next-core --output-on-failure -j2

cmake -S tests/public_api_consumers -B build-next-public -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON \
  -DQCAE_BUILD_DESKTOP=ON -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-next-public -j2

python3 tools/check_design.py
python3 tools/check_cpp_format.py
python3 tools/check_architecture.py --build-dir build-c3-desktop
```

工具链沿用 macOS arm64、AppleClang 21、Qt 6.11.1、VTK 9.7 和 SQLite 的已有本地安装。真实本地域套接字及窗口测试需要相应系统执行权限；没有以无窗口协议替身冒充可见性选择证据。没有 Linux/Windows 或本轮 sanitizer 图形验证。
