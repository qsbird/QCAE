# EXT-02 批量节点平移

扩展独立从 `b740b374c934f3b4624a21247b9a5deeb2ca6bf4` 开始，执行者为 Codex native subagent `/root/c4_ext02_frozen41`。本记录只覆盖该扩展；共同 Release 集成、完整 C4/SK/P0 验收仍由主执行者完成。

## 公开契约与权威路径

新增 `NodeTranslateBatchInput`、`prepare_translate_nodes` 和生产 typed 操作 `node.translate_batch`。参数为非空、不重复的 `node_ids` 集合以及三个显式长度 Quantity `x`、`y`、`z`，允许 `mm`、`m`，表示平移向量而非目标位置。所有写入仍使用 document、epoch、expected_revision 和 idempotency_key。

生产装配原有 mesh_editing contribution 自动注册新增操作。registry 的生成 typed decoder → mesh_editing 单位归一化/排序 → `RecordApplication.execute` → 一个 `EditSession` → 原有原子提交/历史/存储；没有第二份可写模型。节点身份集合排序、三轴归一到 mm 后生成签名，使重排集合及等价单位重试返回原事务。错误类型、未知节点、重复节点、无单位、非有限值、坐标溢出和旧上下文均拒绝整批修改。

`node.translate_batch_preview` 使用同一准备函数和既有 `RecordApplication.preview`，返回可用于 `changes.commit` 的 preview_id。取消沿用既有 CLI 意图处置语义：调用者放弃 preview_id，不发送 changes.commit；不声称新增服务端取消 API。预览和放弃均不产生模型修订或历史。

示例参数（请求上下文另按既有契约携带）：

```json
{"node_ids":["node4","node5"],"x":{"value":0,"unit":"mm"},"y":{"value":1,"unit":"mm"},"z":{"value":0,"unit":"mm"}}
```

## 验证与可读性审查

`tests/model_operation_tests.cpp` 经真实 feature registry 验证 NaN/Infinity、后一个未知节点与后一个坐标溢出无部分发布；正常操作只有一次持久提交，重排/等价单位重试、undo 后重试和 redo 维持原事务语义。

`tests/batch_translation_ipc_tests.py` 已登记为本地 SQLite/IPC CTest。它复用生产几何建线和后台离散任务构造完整 M：11 Node、10 Line2 及原有材料、截面、组织、载荷和 Analysis。真实 CLI 和 Unix socket 脚本各执行一次；完整 entity.query 记录全等比较只允许节点 4、5 的 y 增加 1 mm，单次 undo 恢复完整 M，redo 恢复平移结果。每条路径还验证预览/取消、10 个错误分支、同键异参拒绝和已有 node.move。

独立公共消费者新增 `consumer_batch_translation`，通过目标公开 usage requirements 编译 typed input 和准备函数并运行，无私有 include 路径。自动格式为冻结 clang-format 21，生成器不改，产物经生成器 --check 验证。手写产品文件为 mesh_editing 的 CMake、公开头、实现及独立操作 schema 共 4 个；保护文件触碰 0。

执行者可读性自查：单位/集合校验在准备入口完成；回调只更新目标 Node 的 position，保留 mesh、连接和其他记录；错误在私有 EditSession 内退出，副作用只由唯一应用协调者提交。预览与直接写入共用该准备函数。旧 node.move、beam.assign_section 处理器保持原实现；新增注册逐项传播失败。此自查不替代共同集成的独立审查。

实际命令、失败诊断、M 原始/平移记录及 IPC transcript 保留在执行者的独立证据目录，由交付 manifest 绑定 SHA256。初始八文件为 42078 内容字节、42601 含 heading/换行交付字节；重复读取、后续文件、搜索和构建诊断另计，真实 token 统计为 null。首个外层工具显示发生截断，修改前已完整重新交付；不能把初始包长度当作总上下文成本。

验证状态以独立交付 manifest 和原始日志为准。完整核心回归初次为 38/39，既有 c4_freeze_contract 在默认 90 秒门槛超时；提升执行权限后仍超时，阈值和保护工具未改。停止本扩展编译负载后，同一 CTest 在 50.65 秒通过；最终完整核心 Release 回归 39/39 通过，该测试耗时 35.97 秒。两个超时与通过原始日志均保留。受影响的本地核心/SQLite/IPC 回归 13/13 通过，公共消费者 48/48 编译通过且新增消费者运行成功；最终新增 IPC CTest 包含实际预览应用/撤销验证并通过。产品/测试实现曾有一次测试专用 EntityId 显式构造编译错误，修复后核心和桌面 Release 构建通过；错误与修复日志均保留。

获得独占 native GUI 执行槽后，冻结环境 `QT_QPA_PLATFORM=cocoa` 的 `desktop`、`desktop_selection`、`modeling_tools`、`desktop_smoke`、`c3_tool_lifecycle` 原定五项回归全部通过，实际总耗时 130.02 秒。使用 `ctest --test-dir build-ext02-desktop -R '^(desktop|desktop_selection|modeling_tools|c3_tool_lifecycle|desktop_smoke)$' -j 1 --verbose --output-on-failure`，完整日志保留每项真实可执行文件 argv、超时门槛及退出结果；未更改阈值。GUI 槽已释放给主执行者。
