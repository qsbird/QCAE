# EXT-01 Material 可选说明

本扩展独立基于 `b740b374c934f3b4624a21247b9a5deeb2ca6bf4` 实现。Material 的持久 schema 升为 v2，字段 5 为可选 `description`；未提供字段的 v1 记录读为 `std::nullopt`。说明只保存文本，不修改名称、稳定 ID、弹性模量或泊松比。

公开 `MaterialSetDescriptionInput` 与 `prepare_set_description` 使用既有材料功能注册入口。`material.set_description` 携带 document、epoch、expected_revision 和 idempotency_key；提供非空字符串设置值，省略 description 清空。显式 JSON null、空字符串、错误类型、控制字符和超过既有 1024 字节文本限制的值拒绝，且不提交。真实 `entity.query` 返回字符串或显式 null；`entity.fields` 保持既有可选字段省略惯例。

入口依次为生成的 InputTraits 校验、材料 typed handler、prepare 中的 EditSession、唯一 RecordApplication.execute、持久提交与历史。prepare 复制当前 Material，只替换 description；所有其他记录继续使用原不可变记录。IPC 查询通过现有 RecordDescriptor 识别后续版本引入的可选字段及其 null 默认值，没有 Material 专用 wire 分支。旧 revision、旧 epoch、同键异参失败，已提交键重试仍返回原事务，undo 后重试不会重新应用。

旧输入 `tests/fixtures/ext01-material-v1.record` 在修改 schema 前由共同基线的编码器生成，109 字节，SHA256 为 `ae21094749106dfe32cdce3ce20e0a0a728c14de2894524c820f01cb341422ed`。它包含真实 v1 Material 编码，不含字段 5；保留原字节。测试同时检查旧物性、null 默认、当前编码往返和未来版本拒绝。生产 IPC 测试使用实际 engine/CLI 与显式 SQLite 工作库，覆盖字符串/null 查询、单事务、类型错误、幂等、undo/redo、保存正常打开和两次进程终止后的 recovery。

手写产品文件共 6 个：两个 schema、材料功能公开头/实现/CMake 和 engine_api 的查询映射。records.hpp 由原哈希生成器生成；没有第三方依赖变更，39 个保护文件触碰为 0。

验证原始命令与日志交付在独立执行证据目录中。冻结环境核心 CTest 45/45、原生 Qt/VTK 桌面回归 7/7、独立公共头消费者 48 个通过。初始基线 record_document/model_operations 2/2 与修改后相同两项均通过，格式、design 和 diff 检查通过。首轮工具环境误选 Homebrew Python，冻结工具测试出现 ps 权限错误/90 秒超时；改用已冻结的 `/Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14` 后原阈值下通过，旧失败日志保留。实现中字段差量断言、空字符串策略、不支持 null 的临时 getter 和未重建生成契约造成的失败也保留并登记。

QG-02：root 已只读审查六个手写产品路径，通过且无需产品补丁。权威状态、事务副作用和适配路径如上；材料功能负责编辑，adapter 只负责查询 JSON 表达，生成器负责字段校验与编码。审查核对了 introduced_version > 1 的 optional 字段映射与原生成器只允许 null 默认值的一致性。QG-03：基线与最终相关测试、生成检查、核心/IPC/桌面回归及消费者构建随原始日志交付。

读取账本保留重复读取、搜索和构建诊断，真实 token 统计为 null。第二轮八份初始文件完整交付，内容合计 42078 字节、含工具 heading 合计 42601 字节；第一轮和若干早期读取存在截断或未插桩，完整实际上下文成本未证实，不能把初始包大小当作全部成本。本扩展独立验证不替代共同最终 Release 集成，也不宣称完整 C3/C4、SK 或 P0、真实 AI 或求解器验收完成。
