# 业务契约基线 1.1

这是应用服务契约，不是MCP原始报文。GUI、CLI与MCP桥调用相同业务；传输适配器编码/解码后进入同一Dispatcher。本目录定义完整P0目标；M0已有部分内存服务，实际实现范围由capabilities.list的implementation_status/implementation_scope标识。未实现操作不冒充可用。

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


## 基线1.1：分析目标上下文

`ProfileRef={profile_id, profile_version, definition_digest}`固定语义定义。目标属于AnalysisDefinition的TargetBinding；`RunConfiguration`只表达本机执行配置。不能从文件扩展名或GUI全局选择推断全部目标语义。

OperationDescriptor新增target_context（none、optional_profile、source_profile、analysis、conditional、from_preview、from_run、from_history）及requires_profile_match。analysis上下文要求parameters.analysis_id；需要匹配时要求expected_profile_ref。只读获取设置可以返回当前绑定供后续使用；提交、运行和导出需固定定义。conditional按命令/筛选是否依赖目标判断；from_preview/run/history由已登记对象解析并核对，不另取GUI当前模板。

`capabilities.list`无目标时列出已声明包与配置/验证状态，给定profile_ref或analysis_id时返回该目标能力；按analysis_id查询还必须提供对应document_id/document_epoch，不能从全局活动窗口推断；同时指定两者必须一致。P0只有一个Nastran包。`analysis.start`使用analysis_id、expected_profile_ref和run_config_id；原示例中含混的solver_profile_id已移除。

当前样例为api_version 1.1（设计契约修订，并非已发布运行协议迁移）。新增[能力包与映射样例](solver-profile-examples.json)说明单生产目标范围、编号空间、唯一字段权威和通用结果元数据。所有digest均为示意，不是实际软件验证证据。

格式端口输出目标ArtifactPlan、ExportIdentityMap和转换报告；结果端口输出ResultBundle，P0仅实现BDF、位移/反力。目标报告/预览/运行包含ProfileRef、映射/导出/解析版本；未处理条件或不支持项不得以成功掩盖。详见[架构补充](../architecture/solver-profiles.md)。

新增错误语义：TARGET_NOT_CONFIGURED、TARGET_VERSION_UNSUPPORTED、PROFILE_MISMATCH、PROFILE_DEFINITION_UNAVAILABLE、EXTENSION_SCHEMA_UNSUPPORTED、MAPPING_REQUIRES_DECISION；仍通过现有failed/needs_input/conflict响应承载，扩展不绕过通用事务。


目标上下文的补充约束：source_profile要求导入参数source_context中的格式/profile定义；conditional按实际命令/查询判定，不依赖当前GUI状态，共享对象变更由服务端收集所有受影响分析；from_history恢复扩展时需要兼容的schema/规则，否则明确拒绝而不猜测；from_run始终使用原运行上下文，允许查询历史结果但不得冒充当前结果。

## M1运行子集补充

增加 `model.export_preview`：带文档/epoch/expected_revision、analysis_id和expected_profile_ref的只读编码查询，返回文本资源及冻结编号映射，不产生作业或发布文件。已有 `model.import`/`model.export` 后台作业契约保持 planned。同步受控导入通过 `changes.preview` 的 `model.import` 命令生成候选，再使用既有 `changes.commit` 提交。新领域查询和命令边界见 [M1说明](../implementation/m1.md)。完整设计目录现为28项，运行能力按 `implementation_status` 及具体 scope 判断。

## M2/M3运行子集补充

目录现为35项。持久化宿主支持project.current、normal/recover两种project.open、save/save_as及明确policy的close；只有带SQLite工作库时才声明durable。host作用域的operations.get可查询生命周期结果，恢复的open通过original_mode区分。图形/查询入口增加view.create/update/render_data与selection.evaluate/combine/get；请求明确模型版本和视图版本，详见[M2/M3说明](../implementation/m2-m3.md)及tests/m23_ipc_tests.py。

picker_candidates表示图形客户端给出的明确候选，核心不把它冒充独立遮挡计算；无图形端口的visible_only继续返回不支持。当前RenderPacket走有界JSON而非既定最终批量通道，事件也由显式查询/轮询补偿。这些临时边界见[实施ADR](../architecture/m2-m3-decisions.md)。

## NEXT-02 当前 typed IPC 补充

已实现的 typed 操作使用数值型正整数 `requested_version`（uint32），省略时选择已安装版本；它与字符串 `api_version`、修订号是不同字段。需要目标上下文的 typed 操作通过 `expected_profile={profile_id, profile_version, definition_digest}` 传递严格对象，能力字段为 `requires_expected_profile`。具体 handler 在应用准备阶段校验适用性，先前成功操作优先按保留事实重放。五个宿主工程生命周期入口、`changes.commit`、七个读取/历史入口、`operations.get` 及两个实体读取入口同样接受操作版本，见下节；其他旧非 typed 路径保持拒绝新增字段的规则。完整 P0 目标契约中的 `expected_profile_ref` 不代表已为所有规划操作提供运行实现。

`operations.get` 的 document 作用域可以在贡献停用后查询原调用者的已保存回执，无需重新启用 handler；未知或未保留事实返回 `ENTITY_NOT_FOUND`。来源、测试及未完成范围见[NEXT验证](../engineering/next-validation.md)与[实际 wire 说明](../../adapters/engine_api/README.md)。

## 当前宿主工程生命周期输入契约

已有 `project.create/open/save/save_as/close` 使用生成输入，schema为 `qcae.operation.<operation>.v1`、版本1。发现保留设计目录符号类型，同时用 `wire_input_type` / `wire_output_type` 公布实际DTO / `DocumentInfo`，参数对象封闭。

| 操作 | 实际DTO | 参数 |
|---|---|---|
| `project.create` | `ProjectCreateInput` | 必需非空字符串 `name`，仍执行原应用名称配额 |
| `project.open` | `ProjectOpenInput` | `{mode:"normal",path:"非空路径"}` 或 `{mode:"recover"}`，恢复不接受path |
| `project.save` | `ProjectSaveInput` | 可选字符串path，允许空字符串，保留原保存位置语义 |
| `project.save_as` | `ProjectSaveAsInput` | 可选字符串path，允许空字符串，保留原成功/拒绝语义 |
| `project.close` | `ProjectCloseInput` | 必需policy，发现枚举 `discard` / `keep_recovery` |

可省略 `requested_version` 选择安装版本；正uint32的不同版本返回 `SCHEMA_UNSUPPORTED`，非法版本或参数返回 `INVALID_INPUT` 及字段。拒绝发生在原应用生命周期和宿主幂等登记之前。上述入口不接受 `expected_profile`；成功、重试及结果查询继续走原服务/host操作事实，不增加写入链或重复handler。MCP保持有界schema及open的两个封闭 `oneOf` 分支，Profile字段只为显式声明 `requires_expected_profile` 的描述符公布。当前实际回归/打包范围见[本轮验证](../engineering/core-local-delivery-2026-10-03.md)；先前open专项保留[原来源记录](../engineering/project-open-contract-2026-10-03.md)。其余legacy合同及完整SK-04仍待完成。

## 当前预览提交输入契约

`changes.commit` 使用生成 `ChangesCommitInput`，schema为 `qcae.operation.changes.commit.v1`，唯一参数是必需非空字符串 `preview_id`；额外字段不被接受。发现保留设计符号类型并用 `wire_input_type:ChangesCommitInput` / `wire_output_type:ChangeReceipt` 公布实际DTO。省略/版本1规则与上述生命周期入口相同；无效形状和版本先于原应用提交，拒绝不消费预览或幂等键。写入仍需原文档/epoch/expected_revision/key上下文，`expected_profile`不被接受。合法调用、撤销后相同键重放和持久结果查询沿用同一RecordApplication；恢复更新epoch，旧epoch不能据旧回执绕过检查。实际范围和失败/复验记录见[本轮证据](../engineering/changes-commit-contract-2026-10-03.md)。

## 当前静态贡献发现

生产 `capabilities.list` 的 `package_contributions_version:1` / `package_contributions` 来自实际装配和注册变更。每个contribution_id含core、operations、codecs、validation、ui、render六数组；字段及拥有式绑定见[实际wire说明](../../adapters/engine_api/README.md#actual-static-contribution-discovery)。全局操作目录仍包含宿主固有入口，六类目录只归属贡献新增的注册；没有新增通用回调操作或业务提交链。Nastran关闭后其owner缺席，公共平台能力继续运行。实际调用与当前图形回归未关闭范围见[本轮证据](../engineering/package-contributions-2026-10-03.md)；完整SK-11、SK-04及P0继续未验收。

## 已有读取与历史输入

`capabilities.list/project.current/project.status/model.summary/history.list/history.undo/history.redo` 使用生成版本1的空参数输入及闭合schema，省略版本选择安装版本，额外字段与非法/未安装版本在原应用调用前拒绝。前两项无文档上下文，中间三个读取只需document/epoch，undo/redo保持原document/epoch/revision/key。发现保留符号类型/效果并公布实际wire输入和既有结果标签；没有生成新的输出schema。`operations.get` 条件兼容仍单独保留。实际范围见[七入口验证](../engineering/host-controls-contracts-2026-10-03.md)，不据此声明完整目录/SK/P0通过。

## 原结果查询条件输入

`operations.get` 使用生成版本1语义输入与闭合原始三分支参数schema，模式仅用于host/project.open的normal/recover，其他查询继续忽略任意JSON模式。完整arguments_schema以宿主/文档两个分支描述最小上下文；宿主无文档要求，文档要求ID/epoch，原参数中的key是被查询操作的键。MCP有界验证后原样公布，版本/参数与caller事实仍由engine/应用校验；停用handler仍能查回原事实。现有结果wire标签与原返回包保留，实际范围见[验证](../engineering/operation-lookup-contract-2026-10-03.md)，其他legacy及完整SK/P0未验收。

## 实体读取生成输入

entity.query/entity.references 使用生成版本1语义DTO与真实闭合参数/完整上下文schema，仍仅要求文档ID/epoch。重复/空ID过滤、零分页、动态registry kind、MCP查询参数省略及全部引用返回保持原兼容；旧校验先于语义投影，版本拒绝先于snapshot。现有查询服务、输出标签与单一应用状态保留，读取及拒绝不写模型或历史。实际范围见[验证](../engineering/entity-read-contracts-2026-10-03.md)，完整目录/REQ-16/SK/P0仍未验收。
