本方案在本轮矩阵运行期间形成；下文保留方案形成时的状态。`operations.get` 的实际完成范围见[本轮记录](../../operation-lookup-contract-2026-10-03.md)。本方案的实体读取实现仍为未执行。

# 下一组成组操作契约：记录读取 — 只读计划188

建议一个有界切片，同时补齐 **`entity.query` 与 `entity.references`** 的生成语义输入、命名版本合同及发现一致性。它们已经具备真实业务处理器和显式参数 schema；这是合同质量补强，不是新增查询功能。复用现有生成器、`RecordApplication::snapshot`、记录查询服务和 IPC/MCP 路径，不创建 registry、输出联合或新协调器。

状态：**PLAN ONLY / NOT RUN**。本次只读取 `/private/tmp/qcae-framework-delivery80` 源码、文档和静态 SHA256，并在该目录外写此文件及 JSON 同伴。仓库源码、Git 状态和产品均未修改；未运行 Git 命令、生成器、格式化、检查、编译、测试或产品。父任务的 `operations.get` 候选当前正在实际验证，本计划不宣称该切片通过。原图形累计失败、旧本机包、真实 solver/AI 环境、性能、完整 REQ-16/SK/P0 均保持未关闭范围。

## 同质组和真实实现

两入口都读取同一活动文档的不可变 `RecordSnapshot`，要求 document/epoch，不要求写 revision、幂等键或 Profile。记录读取无需创建视图、选择句柄或预览，也不依赖 Nastran codec 是否启用。两者现在均有 closed `parameters_schema`，MCP 已转发该 schema；它们仍被排除在 IPC 的 generated-version admission 之外，所以任何 `requested_version` 目前都会触发旧 legacy 拒绝。

| 源码 | 已有行为 / 本组要保留的权威 |
|---|---|
| `adapters/engine_api/src/ipc_api.cpp` 的 `supported`、`read_parameters_schema`、`capabilities.list` 和 generated admission | 真实兼容路由、参数发现和上下文标志已经存在；补 DTO/version 元数据，不重建业务注册 |
| `adapters/engine_api/src/ipc_model.cpp` 的 `dispatch_model` | 两入口先取得 `app.record_application().snapshot(ref(request))`，再进入参数解析；没有 legacy Model 分支 |
| `ipc_model.cpp` 的 `entity_query` / `query_page` | 固定字段解析后使用 `query_entities(DocumentView, EntityFilter, offset, limit)`；返回现有实体字段、sources、total/offset/limit 与内外 revision |
| `ipc_model.cpp` 的 `entity_references` | 使用 `query_references` 的有界内部页和 `query_affected_analyses`；响应仍包含所有 references，不把内部页变成新的公开分页 |
| `modules/query/src/entity_queries.cpp` | kind 来自实际记录 registry；组织 view/owner 检查、组织中间行与引用角色已有实现。未知 kind、未知/错误 owner 由现有服务拒绝 |
| `tests/entity_query_ipc_tests.py` | 已用真实 SQLite engine/CLI 覆盖 Geometry/Mesh/Node/Beam、分页、组织多视角、字段和来源一致性、引用及超过一内部页的完整结果；包关闭只跑 geometry，已有 CMake 配置，无需新 target |
| `tests/mcp_bridge_tests.py` | 已有共享 SQLite 选项、双 MCP 客户端、CLI、实际发现 schema 与失败不改修订检查；复用该 fixture |

当前兼容 `supported` 静态列表有24个入口；候选生成输入覆盖14个（包括正在验证的 `operations.get`），其余10个为本组两读取、三个 view、三个 selection、`changes.preview` 和 `model.export_preview`。这不是10个功能缺陷，也不是完整运行目录计数；TypedHost 的四个 intrinsic 入口及宿主 transport/event/resource 路由不纳入该计数。旧文档的“28个剩余legacy”是前一阶段范围，不能继续当作当前缺陷列表。

## 精确参数、语义投影与上下文

新增一个 `schemas/operations/entity_reads.json`，仅包含两个 v1 定义，effect=`read_only`，context=`{document:true,epoch:true,expected_revision:false,idempotency_key:false,expected_profile:false}`。

`entity.query`：schema_id=`qcae.operation.entity.query.v1`，type_name=`EntityQueryInput`。字段固定如下：

| ID / 字段 | 已有原始输入和默认值 | 可复用的生成语义类型 |
|---|---|---|
| 1 `kind` | 可省略；存在时 string，不能为空/全空白；区分大小写；未知 kind 保持服务诊断 | optional `string`，allow_empty=false |
| 2 `name_contains` | 可省略；string，可为空；空串与空白均保留查询含义，不 trim | optional `string`，allow_empty=true |
| 3 `ids` | 可省略表示不限 ID；`[]` 表示空集合；成员为非空 string；未知 ID 可合法匹配零行；重复 ID 当前接受 | optional `entity_id_array`，allow_empty=true；仅在语义投影中去重 |
| 4 `view` | 可省略默认 `all`；存在时非空/非全空白 string；现有服务支持 all、part、assembly、set、include、material、property | optional `string`，allow_empty=false |
| 5 `owner_id` | 可省略；存在时非空/非全空白 string；实际 view/owner 关系保持服务校验 | optional `entity_id` |
| 6 `offset` | 可省略默认0；JSON integer，0..100000，包括0和数值形式100000.0 | optional `finite_number`，units=[1]；在旧 raw 整数/范围校验后投影 |
| 7 `limit` | 可省略默认100；JSON integer，0..1000，包括0；零页仍返回 total | optional `finite_number`，units=[1]；在旧 raw 整数/范围校验后投影 |

`entity.references`：schema_id=`qcae.operation.entity.references.v1`，type_name=`EntityReferencesInput`，字段1 `entity_id:entity_id` 必需，字段2 `direction:optional string`；direction 省略默认 incoming，显式 incoming/outgoing 有效，其他值保持 `Unknown reference direction` 拒绝。所选 entity_id 的原始全空白检查及未知实体诊断保持不变。

现有生成器已经支持 optional、allow_empty、finite_number 和 entity_id_array，没有必要新增零值 unsigned 类型、任意 JSON、null、递归结构或 generator 功能。不能用 `positive_uint32` 表示分页，因为现有接口接受0。`finite_number` DTO 是通过旧整数/范围校验后的语义投影，整数范围的原始 `parameters_schema` 继续具有权威性。

**重复 IDs 是必须保护的兼容差异。** `ipc_model::ids` 当前接受重复值，`query_entities` 在服务内部本就形成 ID 集合；现有 generated entity_id_array 解码器拒重复。因此在已经验证每个 raw 成员后，对这个过滤集合做稳定去重再送生成 DTO，并从 DTO 形成同一 `EntityFilter`。不得在 raw schema 添加 uniqueItems，不得让原先有效的重复 ID 请求失败。保持省略与显式空数组的区别；不 trim/lowercase/改写任何 ID/name/kind/view。只保证语义集合投影，不宣称原始重复数组字节 roundtrip。

在 `ipc_model.cpp` 中保留旧 fields/string/array/page_index 解析和顺序，然后局部构造生成器可接受的已验证 `Value::Object` 并调用 InputTraits。将解码结果用于已有过滤/引用调用，使 DTO 真正参与执行，而非仅为发现添加空壳。无需暴露 TypedHost 的通用 JSON decoder；这些有限字段可以在该 adapter 内投影，无需解析任何 null 或任意嵌套对象。

两入口只提取 document_id/document_epoch；当前 expected_revision 与顶层 idempotency_key 即使提供，也不会参与读取或取得新写键。保留这种行为，包括省略和无关 JSON 值。调用者身份仍由可信传输建立；这些读取共享文档事实，不要把 action-outcome 的 caller/key 隔离规则错误套到普通实体查询上。

## 发现和 MCP 保持真实

保留现有 available、effect、symbolic input/output/target labels：query=`EntityQuery` / `SelectionResult` / `conditional`，references=`ReferenceQuery` / `ReferenceResult` / `none`。不要把 query 响应改成选择句柄，也不要发明生成输出 DTO。

补 installed version、schema_id、wire_input_type、requested_version_field 和 omitted_version_policy；从固定生成定义取得字段身份/要求/单位。adapter-owned raw metadata 对分页描述为 integer，说明 DTO 是已经校验整数后的 finite-number 投影；显式 raw `parameters_schema` 继续权威。query 的 kind 不能枚举成固定当前列表，实际 record registry 是权威，私有贡献的 query kind 不能被 schema 冻结掉。

保留已存在的 closed raw 参数 schema、原有名字、零分页上限、refs direction enum。给 optional strings 的空白检查、view/owner 的既有服务条件及 ID 集合投影写清描述；这个切片无需改造 schema 验证器来支持 pattern 或跨字段条件。真实响应/服务拒绝与原始结构 schema 分层验证，不把 raw 参数 schema 通过当作工程或实体存在性验证。

复用本轮候选已有的 `arguments_schema` 传递机制，不改 MCP 生产代码：为两入口提供同质的完整 closed arguments 对象，properties 仅 `parameters`、`document_id`、`document_epoch`、`expected_revision`、`idempotency_key`、`requested_version`；required=`[parameters,document_id,document_epoch]`。parameters 必须与 entry.parameters_schema 完全相同；document identities 用 nonempty strings；expected_revision 和顶层 key 用 `{}`（unused）；requested_version 为可省略正uint32；不提供 expected_profile，因为 engine 旧入口仍拒绝它。这无需 conditional oneOf：两入口的文档上下文都是无条件要求。MCP 保持元数据校验/转发，不加入 route literals、分页或去重规则。

## 版本和错误顺序

仅把这两个入口加入 `ipc_api.cpp` 的 generated admission。复用既有 `version_rejection`，在两读取进入 dispatch_model/snapshot 之前统一核对各自 installed v1；不要复制版本解析器。省略/v1 使用安装定义；非整数、非正、非uint32或非数值为 INVALID_INPUT/requested_version；其他合法uint32为 SCHEMA_UNSUPPORTED/requested_version。

全局 envelope/API/op/parameters-object 和 expected_profile 检查保持现有更早顺序。新版本拒绝须先于文档/epoch和业务参数解析。**省略/v1 路径保持旧 snapshot/context-before-parameters 顺序**：例如旧 epoch 与错误 query 参数同时出现时，仍由旧文档检查先拒绝，不能把全局 generated decoder 移到 snapshot 前面。旧 BadInput 异常继续使用原消息和无新增 field 的 envelope；应用/查询服务的诊断 field/status 也保留。只新增命名 version 诊断，不重排旧解析与服务顺序，不添加写 revision/key/profile 要求。

## 最少改动文件与一次冻结验证

生产文件仅3个：

1. 新 `schemas/operations/entity_reads.json`：两个生成语义 DTO。
2. `adapters/engine_api/src/ipc_api.cpp`：两个版本 admission/check、生成元数据与已存在 raw schema 的发现整合、完整 document-read arguments_schema。
3. `adapters/engine_api/src/ipc_model.cpp`：旧 raw 解析后的有限语义投影/生成解码；继续原 snapshot、query/ref 服务和原 serializers。

测试最多3个现有文件：`tests/typed_host_tests.cpp` 的一组成对合同/完整状态检查，`tests/entity_query_ipc_tests.py` 的现有 SQLite/CLI fixture，`tests/mcp_bridge_tests.py` 的现有双客户端 fixture。本次 generator/freshness 测试自动复制整个 schemas/operations 目录，无需修改 `tests/test_operation_generation.py`；现有 record_query 语义回归和现有 CMake targets 直接复用。若真实 CLI helper 需要版本请求，用该测试内的薄请求 helper，避免修改所有共享 workflow 客户端。文档状态/实际验证记录在实施后的 root 收敛，不在此计划更新仓库基线。

一次成组验证包含：

1. 一个发现/DTO/raw schema/arguments schema表，检查每入口唯一 available、精确 v1/context flags、字段ID/顺序、MCP schema与engine元数据相同；生成投影省略/空数组、空 name、去重语义和整数0边界。
2. 一个 populated application/OpenContractStore 场景，同组执行成功/失败/省略v1全 envelope 比较；检查完整 DocumentView、metadata/dirty/save token、rows、generation、history/cursor、host/document retained facts、store I/O counters均不变。错误版本与坏文档/epoch/参数组合拒绝；raw null、嵌套无效shape、extra fields不得被投影静默丢弃。普通读取无 expected_revision/新 write key，提供无关 JSON envelope 值仍保持相同结果。
3. 复用当前真实 SQLite/CLI entity fixture，包ON/OFF各用实际可用范围；保持 Geometry/Mesh与组织view、空IDs/重复IDs/未知IDs、limit0/边界、现有分页次序、sources、incoming/outgoing/affected analyses 与超过内部页的 refs完整响应。该既有多页案例是语义保护，不扩成性能压力。
4. 同一共享 SQLite engine 的双 MCP/CLI continuation，省略/v1版本返回相同数据/内外revision（request_id单独保留关联），future/malformed版本 engine refusal，MCP传递 exact完整schema、无意外写 context 要求；失败与成功读取均不产生模型/history/store变化。普通两个客户端读同一结果，不伪造 actor字段。
5. 冻结后 root 一次运行既有 generation/freshness、公开头consumer、design/format/diff gate及适用core/local/SQLite/packageON/OFF/专项配置；按 actual source/product hashes、命令、结果和 NOT RUN 边界记录。与相同影响范围的现有回归一起验证，不逐入口反复全量构建。

不修改 application/history/storage、registry、Value、TypedHost、generator、MCP生产解码、Qt/VTK、依赖或 CMake；不增加服务器/插件/空架构壳，不把 read schema 通过冒充 full SK-04/REQ-16/P0。

## 其余组明确留在后续

三个 view 与 selection.combine/get 已有服务，但含 disposable session/handle 副作用、model/view revision和不同分页上限，应在后续同一 SelectionService合同组处理；selection.evaluate另有有界递归 predicate/scope 与 bool，不应为本组顺手扩 generator。`changes.preview`已有 material/组织/import等真实分支及supported_commands，conditional command联合和codec开关须成组单独设计。`model.export_preview`已有 codec/Profile冻结转换报告，参数内 expected_profile_ref 与顶层expected_profile不同，仍要求冻结revision，保留published=false；不能误记为没有实际导出能力。TypedHost intrinsic已有 version/实际处理器与字段元数据，是否补生成输入属于后续另一同质组，不在此切片更换其作用域或行为。

## 静态源码 SHA256（并非已执行候选）

| 路径 | SHA256 |
|---|---|
| `adapters/engine_api/src/ipc_api.cpp` | `252409df3721da662b9585cae8b1657b0f296ebaf5435c8312e96dcfdc657e25` |
| `adapters/engine_api/src/ipc_model.cpp` | `f18fc68a097bda33949d1c1e38df58460f96692cdb5b0c20b10dbfd4c1e1b7fb` |
| `adapters/engine_api/src/ipc_selection.cpp` | `2968f0d296b0555c3d8d8a94bf34a9196930fb034bc53dcd2b65ab4bea2ca85f` |
| `adapters/engine_api/src/typed_host.cpp` | `39b7478957cd45f6f1d7593c0df83dda67a85f023eaaf445712a81ce32700c98` |
| `schemas/operations/basic.json` | `162fc8e50f3d186ffc85637020820dae45b08fb3c0c141f6fb8fc23e8b2cd3ee` |
| `schemas/operations/operation_lookup.json` | `1bb29046685907b3d1fb694875ca3a2c4766b7ba3cf2ba1bfdcd6f8fe1198ee7` |
| `tools/generate_operation_contracts.py` | `c9bfd4f9ead15951ba51ae49ee90e9565bc1974fb83a7560296f9752c8444db2` |
| `modules/operations/src/operation_registry.cpp` | `a4d2ac049137e4e542674b528a766195776a654ab05d2d16440bede695cae5ca` |
| `modules/query/src/entity_queries.cpp` | `b185497ef754d506b857b6236246aec4de94824ba7efdfbb221e58095500feba` |
| `tests/entity_query_ipc_tests.py` | `05ba30e142c88584e7853f17edebc267e649caf3b4aa5667f264c37893afd9ad` |
| `tests/mcp_bridge_tests.py` | `920799449a187200cdc84f99dbe9a58371c1e65e88bb7f32acccc623704d7041` |
| `tests/typed_host_tests.cpp`（父任务格式化后的候选） | `ac21252f9fab87799c4b9cdcfded1a9e362a41fd4e2c5a418ea1db75c9de7000` |
| `tests/test_operation_generation.py` | `78465b3c364b943b5ab0a34f9a07db0629ee4de1f97408f392514041a2bec496` |
| `docs/contracts/operations.json` | `1f4edc5eff997ae37780d8d6c2216e2442bd6dad2a8744bc86558ad2729d27cd` |
| `docs/baseline/README.md` | `5d77f714575ff330d6ffbfcc2a49fced4f220e97e0c5b87839475a12caaebef6` |
| `docs/architecture/module-dependencies.json` | `3c86ec0dd866c0eb3acbf4cdeb0641cdb477b456c953418eb635d90eb0030645` |
