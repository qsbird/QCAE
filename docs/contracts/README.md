# 业务契约基线 1.0

这是应用服务契约，不是MCP原始报文。GUI、CLI与MCP桥调用相同业务；传输适配器编码/解码后进入同一Dispatcher。当前目录为设计资产，没有可调用服务。

## OperationDescriptor

[operations.json](operations.json)给出首版公共操作目录及一致性要求。一个操作描述包含稳定名称、说明、效果类别、文档/epoch/修订前置、幂等要求及输入输出类型名。具体类型结构在M0实现为强类型DTO和schema；本目录不假装已经具备完整的生成器。

公共效果类别：query（只读）、preview（临时候选）、model_write（模型提交）、project_write（保存等工程操作）、job_start/job_control（任务）。工具元数据不能替代服务端授权和校验。

## 请求与结果

公共字段：api_version、request_id、operation、parameters；依操作要求携带document_id、document_epoch、expected_revision和idempotency_key。ID/版本在边界以字符串表达；数量字段显式携带值、单位和必要坐标系。

- request_id：一次通信尝试；不能用它判定逻辑操作已经执行。
- idempotency_key：同一有副作用意图的重试键；不同参数复用应拒绝。
- 正常打开生成新DocumentId/Epoch；故障恢复保留DocumentId及幂等事实，更新Epoch。
- 旧Epoch不直接写入；重连取得新上下文后查询原操作结果，再决定新操作。
- 原提交后来被undo，重试仍只返回原提交事实及当前效果，不重新应用。
- 创建/打开/关闭在宿主登记簿保存生命周期幂等事实；operations.get的host作用域按原操作名及键查找，不要求调用者先知道DocumentId。document作用域查询必须提供工作文档身份。
- 新建/打开先分配并持久化意图和目标DocumentId，故障时核对既定工作库，不在每次重试时重新创建文档。
- 客户端不提供可信actor或approved=true；身份、范围和授权来自宿主上下文。

响应状态至少区分success、needs_input、conflict、accepted、failed，并返回结构化诊断及相关版本。修改成功有transaction_id，后台工作有job_id，资源使用受控资源ID。错误不只写日志或依赖GUI弹窗。

[current-examples.json](current-examples.json)演示缺单位、预览、提交、同意图重试、冲突、任务、陈旧结果、旧会话及另存。它们是契约示意，不是运行数据；早期[p0-ai-examples.json](p0-ai-examples.json)保留为历史。

## 变更与格式报告

PreparedChange绑定DocumentId/Epoch/Revision、规范化命令、稳定目标、字段/引用before-after、全部影响者、资源预估及ChangeImpact。预览固定这些内容；提交重新校验版本和授权。

ChangeImpact至少表达：显示表示、模型组织/引用、分析物理输入、检查结果的影响。影响需要沿实际引用传播：被载荷引用的集合变化影响分析，纯相机变化不影响物理结果。

ImportReport/ExportReport至少包含识别/写出类型与计数、支持范围、未支持/丢失项、编号和集合映射、文件/卡片诊断、是否允许成功提交或发布。默认严格拒绝无法保证支持子集语义的转换，不静默丢弃信息。

## 核心错误语义

| 错误 | 含义与调用者行动 |
|---|---|
| MISSING_INPUT / INVALID_UNIT | 补充或修正字段；未提交模型 |
| UNSUPPORTED_CAPABILITY / UNSUPPORTED_CARD | 明确超出支持范围，不用近似能力冒充 |
| ENTITY_NOT_FOUND / REFERENCE_INVALID | 查询目标或修正引用 |
| DOCUMENT_EPOCH_EXPIRED | 获取新会话，核对旧请求结果 |
| REVISION_CONFLICT / PREVIEW_EXPIRED | 读取最新状态，重新生成候选 |
| IDEMPOTENCY_KEY_CONFLICT | 同键参数不同；不得误报已成功 |
| RESOURCE_LIMIT | 说明超限项，文档保持一致 |
| STORAGE_COMMIT_UNCERTAIN | 停止写入并核对事实，不盲目重执行 |
| SOLVER_FAILED / RESULT_INVALID | 区分执行和解析问题，保留输入与日志证据 |
| GRAPHICS_UNAVAILABLE | 图形能力不可用，不静默改成另一种选择语义 |

更多运行约束见 [运行时设计](../architecture/runtime-and-data.md)。
