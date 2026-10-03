# 两个实体读取接口的生成契约

实现提交 `a8f557347920e8c4c16ee50d4e84401c03ebb859` 已为既有 entity.query/entity.references 补齐版本1语义输入、真实参数和完整上下文发现。五配置初轮实际257/261通过；两处测试夹具修正后的四个目标定向复验均通过，其余257项绑定未变产品。累计269次执行261通过/8原失败，六项修改前基线另列；原图形累计失败、本机旧包与完整SK/P0仍未关闭。

EntityQueryInput 的七个可选字段为kind、name_contains、ids、view、owner_id、offset和limit；EntityReferencesInput要求entity_id，direction可省略，默认incoming，也支持outgoing。schema分别为qcae.operation.entity.query.v1和qcae.operation.entity.references.v1。DTO实际供给原query_page、query_references及query_affected_analyses，状态仍由RecordApplication的不可变快照提供。没有新输出DTO、业务实现副本或事务路径。

先保留原字段/字符串/ID/分页校验，再投影生成输入。重复ID仍合法，仅稳定去重后供给语义数组；空数组匹配零实体，省略不限制ID，非空ID字节保留。name_contains允许空和空白。分页仍接受0、整数值的浮点数及原offset上限100000/limit上限1000，省略分别为0/100；原整数/range校验后供给无量纲finite_number字段，发现明确区分raw integer与semantic类型。kind仍来自实际record registry。引用结果保留全部引用及受影响分析，原1000大小内部页循环未改变。

两个入口仅要求document_id/epoch，原未使用的expected_revision及顶层idempotency_key继续忽略任意JSON，不保留写键。完整arguments_schema复用原闭合parameters_schema。MCP的entity.query parameters继续可省略并由原桥填成空对象，references则必需；实际IPC envelope仍要求parameters对象。MCP生产源码未改，继续原样公布经过有界元数据校验的schema。

requested_version可省略或为安装版本1。非法值返回INVALID_INPUT/requested_version，其他合法版本返回SCHEMA_UNSUPPORTED/requested_version，在模型快照之前拒绝；原envelope/parameters对象及expected_profile检查仍更早。省略/版本1保持原context/snapshot先于参数的错误顺序，输出内外revision与原序列化标签不变。

| 配置 | 初轮通过/执行 |
|---|---:|
| 纯核心Release | 40/40 |
| 本地Debug | 54/55 |
| SQLite/Nastran ON，无桌面 | 86/87 |
| SQLite/Nastran OFF，无桌面 | 77/78 |
| ASan/UBSan typed_host专项 | 0/1 |
| 初轮合计 | 257/261 |

初轮四个失败均为新夹具签名手写值不一致。改用现有规范化helper后四次专项又发现自定义事实误走只支持commit/undo/redo的旧wrapper；只在测试改用同一应用的action_outcome后四次专项全部通过。累计269实际执行261通过/8失败，原失败保留；修复仅改变测试CPP和四个测试二进制，其余421输入及13记录产品不变，没有重跑无关产品或声称修复后新全量矩阵。

严格C++20、零编译警告；ASan/UBSan只运行typed_host，LSan未测。六项修改前C++/IPC/MCP/协议/实际实体查询基线全部通过，未计入261。所有配置关闭桌面。

真实SQLite ON/OFF新增契约请求分别92/88次：geometry各53观察、88请求，其中26成功/58失败/4冲突；ON额外organization两组省略/v1对、4成功请求。逐请求对比完整逻辑行、模型、保存内容身份、文档元数据、generation及历史不变；先实际save_as形成非空保存点。保留既有501段网格的1003引用完整结果，以省略/v1和全状态比较保护内部分页，未扩大性能测试。C++使用真实应用及已有测试存储，覆盖组织闭包、动态registry kind、保存后编辑历史、可信caller与原诊断文本；不冒充SQLite证据。两个实际MCP及CLI覆盖原始schema透传、完整返回包、省略参数、零分页、ID过滤、版本/上下文拒绝和共享状态。

独立QG-02发现refs测试精确预期缺既有内层revision。首次构建在任何CTest前取消，原方法/candidate/日志保留；修正预期及全部成功读的内外revision断言后，使用新目录重新冻结构建。此为静态测试缺陷，未冒充实际产品测试失败。两次实际测试失败原因、修复及定向复验均单列。最终六文件独立审查无阻塞；设计、clang-format21、diff门禁通过。

[机器结果](entity-read-evidence/2026-10-03/validation.json)、[索引](entity-read-evidence/2026-10-03/archive-index.json)与[原始归档](entity-read-evidence/2026-10-03/evidence.tar.gz)保留实际命令/日志、422输入/产品/复制桥脚本hash、SQLite观察与请求、静态审查及取消尝试。归档290成员、955917字节，SHA256 `f920e94cdb39daf627ca459b0486fa3294dab9da2ede824bce914c7ac713eb3e`，成员及原来源回读相等；没有数据库、二进制或构建树。冻结字典SHA256 `7cca4e73fe40063582588ca9b301055b2b6ca7617b0fa91b3afc5a92824f55f8`，canonical排序路径到文件SHA256 JSON，非C4-source-v1。

说明阶段仅engine_api README是冻结输入内文档差异，其余421输入及产品保持；不为文档重复运行产品。24个兼容列表入口中已有16个生成版本输入，其余8个是已实现接口的合同补齐，计数不包括TypedHost固有入口或资源/传输路由，也不代表完整SK-04。原图形180秒累计失败、旧隔离诊断、未刷新f2d包、真实求解/完整AI、性能、未知SDK与原已测预算失败保持原范围。本切片不完成全部REQ-16、C3/C4或SK/P0，不追加非必要性能优化。

归档独立核对无异议，结果见[独立核对](entity-read-evidence/2026-10-03/archive-review.md)与[收尾机器记录](entity-read-evidence/2026-10-03/closeout.json)。原始290成员归档未变；独立核对、后续未执行方案和收尾记录另存。

下一组按[只读方案203](entity-read-evidence/2026-10-03/next-plan.md)成组补齐 view.create/view.update/view.render_data。现有生成类型足够，保留uint64字符串、stale视图区别和原会话清退副作用；方案尚未实施或运行。
