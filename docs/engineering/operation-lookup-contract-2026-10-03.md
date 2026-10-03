# 原操作结果查询的条件契约

实现提交 `cf2f4c447800ffecffa31263007fe21cce548b20` 已补齐既有 `operations.get` 的输入版本与真实参数/上下文发现。统一421输入的五配置实际261/261通过；原图形累计失败、本机旧包与完整SK/P0验收仍未关闭。

`OperationsGetInput` 为生成的版本1语义字符串DTO，schema为 `qcae.operation.operations.get.v1`。三个必需非空字段为lookup_scope、original_operation及原操作idempotency_key；original_mode仅在host/project.open查询中有效，省略选择normal，也支持recover。其他宿主或任意文档动作原来忽略的模式仍接受全部JSON值，适配器仅投影掉该字段再解码；未扩展Value或生成器。未知原始字段仍拒绝，原scope/operation/used-mode/key诊断保留。

闭合parameters_schema使用三个互斥分支：host-open、其他宿主生命周期入口、document。完整arguments_schema复用相同参数schema，以两个分支描述上下文：宿主仅需parameters，文档还需非空document_id/document_epoch；未使用的revision及顶层key保持可忽略，宿主文档上下文也保持可忽略。requested_version可省略，或为正uint32；非法值返回INVALID_INPUT/requested_version，其他安装外版本返回SCHEMA_UNSUPPORTED/requested_version，先于查询分支调用。

MCP复用现有16层/128节点schema验证器，检查完整根对象闭合、参数名已识别、parameters与原描述严格相等，再原样公布inputSchema。没有新增业务解码或操作名字分支；原工具路径保持。符号OperationLookup/OperationOutcome、query效果和无无条件上下文要求保留；wire_output_types和output_by_lookup_scope仅说明既有DocumentInfo/ChangeReceipt序列化，不是新增输出合同。

查询继续调用同一RecordApplication的宿主、commit/undo/redo或任意贡献动作保留事实；不要求原handler仍安装，不产生事务或新写键。宿主与任意动作直接返回，mapped history/commit保留顶层revision的原差异。应用、存储、registry、TypedHost、核心公开接口、依赖和性能机制均未修改。

| 配置 | 实际执行/通过 |
|---|---:|
| 纯核心Release | 40/40 |
| 本地Debug | 55/55 |
| SQLite/Nastran ON，无桌面 | 87/87 |
| SQLite/Nastran OFF，无桌面 | 78/78 |
| ASan/UBSan typed_host专项 | 1/1 |
| 合计 | 261/261 |

所有构建严格C++20、零编译警告；生成一致性及公共头消费者保留于对应构建/CTest。修改前五项既有C++/IPC/MCP/协议基线均通过，单列于归档，不计入261分母。ASan/UBSan只执行typed_host一项，LSan未测；五配置均关闭桌面，没有新图形通过证据。

C++复用原生命周期、历史与停用handler场景，逐次比较完整DocumentView、持久假存储、generation、history与caller事实，并校验语义投影、原诊断顺序和完整返回包。两个真实SQLite场景各新增35次查回、31次拒绝，保留原101次拒绝、5组读取、8次历史写及6次重放；实际全逻辑行/BLOB/generation/document/history比较不变。独立进程恢复保留原身份与事实、更新epoch；discard后normal-open恢复保存模型并建立新身份。两engine均SIGTERM退出-15，无强杀或物理WAL字节相等主张。

真实两个MCP客户端及CLI验证完整发现schema/说明透传、省略/版本1包相等、忽略JSON值、文档上下文与版本拒绝、原action/commit/undo/redo及关闭后的宿主事实。协议替身33场景只验证metadata/transport边界，不充当真实模型验收。运行入口固定trusted caller local-user；不同调用者隔离由C++可信Caller测试证明，不伪造JSON actor。

独立QG-02审查发现两处测试问题，参数变量覆盖与keep_recovery后直接normal-open均在运行前修复，最终无静态阻塞。它们不是实际测试失败或应用行为修改。设计、clang-format21与diff门禁实际通过；收尾文档门禁单列。

[机器结果](operation-lookup-evidence/2026-10-03/validation.json)、[索引](operation-lookup-evidence/2026-10-03/archive-index.json)和[原始归档](operation-lookup-evidence/2026-10-03/evidence.tar.gz)保留方法、命令/日志、源码/产品及实际复制桥脚本hash、SQLite观察/请求、格式前后源码和独立审查。归档138成员、851451字节，SHA256 `f8389d90c77b40a0b2ddde7b028795ca75e15f80c6850892cbe235d6f3544bbb`，所有成员与原来源回读相等；没有数据库、二进制或构建树。冻结字典SHA256 `c81d0e14a4b42bd7b86a2eb417a4e4531bf3a6e90604e428434a52c511960db4`，算法为canonical排序相对路径→文件SHA256 JSON，非C4-source-v1。

说明阶段仅engine_api README属于冻结输入中的文档差异，其他420输入与执行产品保持；没有为文档重跑产品。其余legacy合同与核心正式验收继续推进；原图形180秒累计失败、旧二进制隔离诊断、旧f2d包、真实求解/完整AI、性能、未知SDK覆盖与已测预算失败仍保持各自来源边界。此切片不完成全部REQ-16、SK-04、C3/C4或P0；不追加非必要性能优化。

下一组按[只读方案188](operation-lookup-evidence/2026-10-03/next-plan.md)成组补齐 `entity.query/entity.references`，已有真实处理器与raw schema，重点保留空/重复ID、零分页、原上下文检查顺序及完整引用结果；方案尚未实施。当前24个兼容列表入口中的14个已有生成版本输入，剩余10个并非功能缺陷，不能延用此前“28个legacy”作为当前缺陷数；该计数不覆盖TypedHost固有入口或传输/资源路由，也不代表完整SK-04。

归档独立核对无异议，结果见[独立核对](operation-lookup-evidence/2026-10-03/archive-review.md)与[收尾机器记录](operation-lookup-evidence/2026-10-03/closeout.json)。原始138成员归档未变；独立核对、后续未执行方案和收尾记录另存。
