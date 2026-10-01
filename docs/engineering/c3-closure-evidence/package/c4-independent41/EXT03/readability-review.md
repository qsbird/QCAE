# EXT-03 可读性与行为审查

执行者：`/root/c4_ext03_frozen41`。基线：`b740b374c934f3b4624a21247b9a5deeb2ca6bf4`。审查是执行者自审，未冒称独立外部审查。

随后根任务 `/root` 对十个稳定手写产品路径执行独立只读 QG-02 审查并通过，无补丁建议。原始审查消息保留在 `coordination-009.txt`；根任务没有代写产品补丁。

QG-01：最终 `final-static-gates.log` 记录设计检查、229 个 C++ 文件的 clang-format 21 检查和 `git diff --check` 全部通过。工具和生成器使用冻结环境，未改格式或测试阈值。

QG-02：`schemas/operations/basic.json` 声明 `mesh.create_tri3` 的 typed 输入和文档/epoch/revision/幂等上下文。`features/mesh_editing/src/mesh_editing_operations.cpp` 校验输入语法及三节点基数，保留连接顺序作为拓扑和幂等签名，在既有 `register_handlers` 注册；prepare lambda 只通过 `EditSession.put/prepare` 产生计划，权威发布由现有 `RecordApplication.execute` 完成。没有第二份可写业务模型，失败在发布前返回已有诊断合同。

实体 schema 声明 Node 和 Mesh 目标类型。未改的生成器产生 Tri3 字段编码、typed MeshId、引用访问和字段验证；持久化、反向引用、删除依赖、查询和历史复用既有机制。`modules/document/src/records_rules.cpp` 的规则负责节点所属网格和正有限面积。该规则也约束已有 `node.move` 对被引用节点的修改；避免只在新操作入口验证而留下其他入口绕过规则的漏洞。错误连接、错误实体类型、未知 ID、重复节点、共线节点、网格不一致以及引用删除都由测试证明原子拒绝。

`modules/query/src/render_projector.cpp` 为 Tri3 提供三点 polygon 贡献，复用已有 variable-arity 载体和 VTK 消费者，wire、typed host 和连接管理未改。新的 Qt/VTK 冻结矩阵测试对真实后端的稳定实体 ID 集合做全等比较；没有将合成命中计作图形证据。最终图形结果另见实际原始日志。

`profiles/nastran/src/nastran_package.cpp`、`features/analysis/src/analysis_input.cpp` 和 `analysis_checks.cpp` 在输入冻结/分析检查/正式导出前明确返回 unsupported capability。`adapters/engine_api/src/ipc_model.cpp` 的 legacy export preview 在投影为旧 Model 前检查权威 records，防止 Tri3 被旧投影丢弃后仍宣称导出可用。这四处保护不同公开调用路径；没有改变 shell 求解或 codec 来假装实现。

QG-03：`tri3_tests.cpp` 覆盖实际生产 registry、单事务、重试/同键异参、旧上下文、完整撤销重做、引用删除、依赖面积校验、生产投影和不支持的工程能力。同一文件在内存和实际 SQLite 适配分别执行；SQLite 测试包含保存、正常打开、关闭并重开存储后 recovery。`tri3_ipc_tests.py` 经真实 engine/CLI 创建 11 节点/10 Line2 的 M，追加 Tri3，证明其他记录不变，并执行实际保存/进程终止/重启 recovery、实体查询/反向引用和旧 selection 拒绝。公开消费者编译 69 个头文件。

最终核心套件 40/40、相关核心/SQLite/IPC 套件 14/14 通过。最初整体核心套件和一次单独冻结元测试受到 90 秒超时影响；保留失败日志，未提高阈值，最终相同冻结测试 47.71 秒通过。新增核心测试在格式修正前后的直接执行均通过。全部失败与修复另见 delivery-summary.json，图形验收以 GUI 槽释放后的原始日志为准。

结论：入口、权威状态、不变量和外部适配职责可定位；没有未解决的可读性阻塞。Tri3 是持久网格拓扑扩展，未声明壳分析、Nastran 壳导出、求解器或完整 P0 验收通过。
