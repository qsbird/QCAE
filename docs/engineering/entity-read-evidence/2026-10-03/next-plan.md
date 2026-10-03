# 下一组成组操作契约：view 会话 — 只读方案203

**建议一起补齐 `view.create`、`view.update`、`view.render_data`。** 当前生成器的字符串、optional 和允许空数组的 entity_id_array 已可表达全部语义输入；无需扩展 Value、generator、registry 或 SelectionService。三个入口都通过同一 `dispatch_selection` 和 caller-owned ViewSession 工作，成组处理创建、更新、读取可以一次保护重绑定和失效语义。

状态：**NOT IMPLEMENTED / NOT RUN**。本次只读冻结候选源码与基线，并在仓库外写此 Markdown 和 JSON 同伴；未修改任何仓库文件或 entity193 冻结源码，未运行 Git、生成器、格式化、检查、构建、产品、测试。来源为 `/private/tmp/qcae-framework-delivery80` 当前静态文件，不是产品验证。按 REQ-16、TST-F08、TST-A01 与 QG-02 规划；不宣称完整 REQ-16/SK/P0，原图形累计失败、旧包、真实 solver/AI 与性能范围保持未关闭。

## 真实路径与状态

生产 `apps/engine/engine_host.cpp` 持有一个 `SelectionService`，将可信 `Caller{"local-user"}`、该服务指针和同一应用传入 `ipc::dispatch`。CLI 与两个 MCP 客户端当前属于相同本地 OS-user principal；不能把客户端数量当作不同安全主体。普通 JSON actor 不能选择 caller。跨 principal 拒绝可在直接 C++ fixture 中用 Alice/Bob 验证。

三个操作均走 `ipc_api.cpp` 全局入参检查 → `dispatch_selection` → `RecordApplication::snapshot` 的不可变 `DocumentView` → `SelectionService`。界面 SDK 不成为模型事实权威，view/render 的身份仍由现有 ViewSession 与 RenderPacket 承载。它们不走模型/history/store 提交。

`view.create` 创建新的临时 caller-owned view，model revision 绑定当前快照，view revision 初始1；hidden IDs 在服务内排序去重。它会先清退其他 document/epoch 的旧 views/handles，再检查活跃 view 配额；所以不能笼统声称所有旧业务失败均不改变任何会话表。ID 不幂等，重复调用原本就产生不同 view；不要新增写 key、重放事实或持久化。

`view.update` 先 inspect_view，允许模型修订过期的 view 进入重绑定；比较其旧 expected_view_revision 后再更新。服务只在模型版本、排序去重后的 hidden set 或原 camera 字符串改变时推进 view revision。相同更新不推进；改变后旧选择句柄通过原有版本检查失效，不需要新增 eager 删除机制。

`view.render_data` 使用严格 get_view：view model revision 必须等于当前快照。返回原 points/beams/geometry_lines 和真实实体/连接映射。它没有图形上下文或截图动作，也不等同 `view.render_resource` 的资源/rebase 协议。

## 精确生成字段与 raw 接受范围

建议新增 `schemas/operations/view_sessions.json`，三个版本均1，schema_id 分别 `qcae.operation.view.create.v1`、`qcae.operation.view.update.v1`、`qcae.operation.view.render_data.v1`。context 均 `{document:true,epoch:true,expected_revision:true,idempotency_key:false,expected_profile:false}`。

| 操作 / 生成类型 | field_id / 字段 | 当前 raw 类型及默认 | 生成类型 |
|---|---|---|---|
| create / ViewCreateInput | 1 hidden_ids | 可省略默认[]；成员非空 string；[]/重复合法 | optional entity_id_array，allow_empty=true |
| create | 2 camera_fingerprint | 可省略默认空串；任何 string，包括空/空白 | optional string，allow_empty=true |
| update / ViewUpdateInput | 1 view_session_id | 必需非空 string；不是工程 EntityId | string |
| update | 2 expected_view_revision | 必需非空 decimal string，按 uint64 from_chars 校验 | string |
| update | 3 hidden_ids | 必需，[]/重复合法；成员非空 string | entity_id_array，allow_empty=true |
| update | 4 camera_fingerprint | 必需；任何 string，包括空/空白 | string，allow_empty=true |
| render_data / ViewRenderDataInput | 1 view_session_id | 必需非空 string | string |
| render_data | 2 expected_view_revision | 可省略；存在时 decimal string、按 uint64 from_chars 校验 | optional string |

生成 effect 为 create/update=`auxiliary_write`，render=`read_only`，使用现有枚举说明临时辅助状态；旧发现 effect 保留 `session_write`/`query`。不存在新文档写入。所有字段 units 为空。

**视图修订不能改成 positive_uint32 或 finite_number。** 原 integer() 读取字符串并按完整 uint64 from_chars 解析；0 和前导零是合法语法（0通常再因不匹配被 conflict 拒绝），符号、空白和溢出拒绝。字符串 DTO 完全适合已校验的语义投影，不需要新 unsigned 类型，不丢 uint64 精度，不改原字节。顶层 expected_revision 也采用这一规则。

`hidden_ids` 原 raw 接受重复值，生成 decoder 拒重复。因此只在完成旧 raw 检查后按集合语义稳定去重，再送 InputTraits。最终排序与存在性仍由 SelectionService::valid_ids/sort/unique 决定；该服务要求 ID 对应存在且 query_kind 非空的记录。白名单 kind 不冻结；成员仅检查 nonempty，不 trim；空白 ID 的 raw shape 合法，通常由服务拒绝不存在。camera、view_session_id 和 document identities 都不能新增 trim/maxLength 或空白拒绝。

## 必须保留的上下文与错误顺序

全局 envelope/API/operation/parameters-object 检查和 expected_profile 拒绝仍更早。仅将这三个入口加入 generated admission，并在 dispatch_selection 取得快照前复用已有 version_rejection。错误 requested_version 新增原共享 INVALID_INPUT/requested_version 或 SCHEMA_UNSUPPORTED/requested_version；版本省略或1保留旧执行顺序。

1. parse document_id → document_epoch（原非空 string，不 trim）→ RecordApplication snapshot；失败由原 encode 转 envelope。
2. 三个 view 都 parse 顶层 expected_revision 并和快照修订比较；缺失/非法 string 是原 INVALID_INPUT，过期是 conflict / REVISION_CONFLICT / Model changed。比业务参数更早，且不要求 idempotency_key。
3. create 才检查 closed fields(hidden_ids,camera_fingerprint)，校验存在的 optional 输入，建立 Value 语义投影并 from_value，再用 DTO 的隐藏集合与相机调用 create_view。服务原顺序为 caller → hidden existence → 清退旧epoch会话 → 活跃配额 → sort/unique → 创建。
4. update/render 先 parse view_session_id，**在 parameters 的 closed fields 检查前**调用 inspect_view(update) 或 get_view(render)。未知 view、ownership、epoch（render还含 stale model）保持先于多余/非法业务字段拒绝。
5. update 必需、render 仅在提供时 parse expected_view_revision，并先比较当前 view revision，再检查各自 closed fields。update 再读 hidden/camera并投影 DTO；render仅投影已校验 view ID与可选revision。DTO 的服务字段必须供给最终 update_view/render_packet，不能只是发现空壳。原上下文/视图修订 guard 仍是 adapter 权威，不为凑 DTO 重复或提前全参数解码。

原 BadSelectionRequest envelope 没有 field；encode 服务失败也会省略核心 Diagnostic.field。该切片不能改成 entity-read error() 的 field 格式或新增需要输入状态。未知/null/额外 raw shapes 必须由原检查拒绝，不能被投影丢弃。顶层 idempotency_key 不被读取，任何 JSON 继续可忽略；expected_profile 仍由原全局兼容 guard 拒绝。

**一个真实的错误优先级边界：** create/update 目前把 hidden_ids 与 camera raw 解析放在同一 C++ 调用的不同实参中，语言不定义它们之间的求值先后。本方案不虚构二者同时错误时的统一历史顺序。实现需保持已确定的 snapshot/context/view检查/fields 阶段；如要求同工具链双错误的完整 envelope 对照，应在更改前由实际旧产品记录其行为，或者保留该实参解码结构。不能借此把全 generated decoder 移到 view/context 检查前。

## 发现与完整 arguments schema

保留可用路由、原 symbolic labels 和序列化：create=`CreateView`→`ViewSession`，update=`UpdateView`→`ViewSession`，render=`RenderDataQuery`→`RenderPacket`；target_context 均 none；requires_document/epoch/revision=true，key/profile=false。新增 installed version/schema_id、生成 wire_input_type、requested_version_field、omitted_version_policy，以及从生成定义取出的 field_id/顺序/type/required/allow_empty/units。wire_output_type 只重述原 serializer 标签，不建输出 DTO。

提供每入口自己的 closed raw parameters_schema：create required=[]，update required全部4字段，render required view_session_id。数组 item 为非空 string，允许[]，不添加 uniqueItems；camera为 string不添加minLength；view ID与revision string minLength1。revision语法/上限由原 engine parser校验，并在description解释。现有 MCP checked_parameters_schema 不支持 pattern，**不要为了 uint64 字符串添加 pattern 并修改 bridge**，也不要用数值schema替代。

使用本轮已有 arguments_schema 通道，root properties 只 parameters、document_id、document_epoch、expected_revision、idempotency_key、requested_version。parameters 必须完整等于 entry.parameters_schema；三项文档上下文为非空 string，expected_revision说明精确uint64解析；unused key={}；版本为可选正uint32。create 由于全参数可选、bridge已默认{}，required仅 doc_id/epoch/expected_revision；update/render required再含 parameters。expected_profile不列属性。无需 oneOf，不加入MCP生产route或业务验证。

## 最小修改面与一次验证

生产仅三文件：NEW `schemas/operations/view_sessions.json`、`ipc_api.cpp` 的同组version/发现、`ipc_selection.cpp` 的局部 raw后投影/DTO→现有服务。不改 SelectionService、app/history/store、generator、Value、TypedHost、MCP生产、CMake、依赖或显示协议。元数据仍通过现有rawschema/arguments_schema方式呈现；只需有界adapter helper，不创建新registry、协调器或递归联合抽象。dispatch_selection中其他 selection 分支共用 guard，不能顺手把 selection.evaluate/combine/get 纳入新的decoder。

建议最多扩三个既有测试文件：`typed_host_tests.cpp` 直接将 SelectionService传给 ipc::dispatch并使用现有 populated/OpenContractStore 对照；`c3_display_ipc_tests.py` 复用真实SQLite/CLI geometry+mesh fixture（ON/OFF都已有 CMake目标）；`mcp_bridge_tests.py` 复用共享SQLite、双MCP/CLI和capability文本/schema一致性。现有 record_query/query/render_service/c3_sync 回归保护服务；m23_ipc仅Nastran ON；desktop_selection保持已有证据/失败边界，不宣称图形门禁解决。

验证成组进行：

- 一张 descriptor/DTO/rawschema/args 表：字段ID、optional/allow_empty、effect映射、string uint64、无新写key/profile，MCP完整schema原样。
- 同应用的省略/v1完整envelope对照：创建返回的session ID本来每次不同，不能把两次create直接当作应相同的重放；可在独立等价fixture比较除服务nonce/ID和request关联以外的结构。update用相同无变化状态对照，render用同view完整对照。
- 上下文优先级与版本拒绝：错doc/epoch/revision/view/expected_view_rev结合null/extra/坏数组；新future/malformed版本更早拒绝且没有创建、推进视图或消耗quota。保留旧无field业务envelope。
- duplicates/[]/省略hidden、empty/white camera、完整uint64字串、no-op update、实际hidden/camera改变、stale-model view通过inspect重绑、过期expected_view_revision先拒绝、render省略view修订合法但stale model仍拒绝。
- 明确分开持久模型和临时会话：全部view操作不改变完整DocumentView/metadata/save token/dirty/generation/history/retained facts/store I/O；create/update成功按原服务改变view/handle可用性，render只读。旧create清退规则不是“所有失败全部会话不变”。直接C++覆盖quota/caller，真实双MCP/CLI覆盖同local-user共享view、隐藏集合、版本/错误和SQLite无写。
- root统一冻结后一次运行既有generation/freshness、公共头consumer、design/format/diff gate和适用core/local/SQLite/packageON/OFF组。测试和产品hash只可由实际执行报告；本方案不运行、不预填通过项。

无需替代更简单组：view组三入口的类型和生命周期可以在现有机制内有界实现。selection.evaluate的递归predicate/scope与bool、conditional previews和render_resource的重绑定资源协议继续留后续。

## 静态源码 SHA256（并非实施或执行证据）

| 路径 | SHA256 |
|---|---|
| `adapters/engine_api/src/ipc_selection.cpp` | `2968f0d296b0555c3d8d8a94bf34a9196930fb034bc53dcd2b65ab4bea2ca85f` |
| `adapters/engine_api/src/ipc_api.cpp` | `7b1e5e8ea102f97de10773f260df44aff667478cad3ee7f1abde47ffd4c927b3` |
| `modules/query/src/query.cpp` | `8274a1e46030a7f076a692756da86a71a82de5d1ebdc24232343380f5c9b3739` |
| `modules/query/include/qcae/query.hpp` | `9105cc5d21fce171031193450b576ffefa96e3f1da68caa0b7dd798285d94fb5` |
| `modules/contracts/include/qcae/view_session.hpp` | `0589f97fa502fbbde4e2c83b673e4647cc7c85ef80c162fbcc7ccf2a571abf51` |
| `modules/operations/include/qcae/operation_registry.hpp` | `a2f43f7421b8dd637f11c8b6eb51d4d91c1fd5251b4e46f35d3b620a121e1306` |
| `modules/operations/src/operation_registry.cpp` | `a4d2ac049137e4e542674b528a766195776a654ab05d2d16440bede695cae5ca` |
| `tools/generate_operation_contracts.py` | `c9bfd4f9ead15951ba51ae49ee90e9565bc1974fb83a7560296f9752c8444db2` |
| `docs/contracts/operations.json` | `1f4edc5eff997ae37780d8d6c2216e2442bd6dad2a8744bc86558ad2729d27cd` |
| `apps/engine/engine_host.cpp` | `6b7286d3dc8fef8d33c0c5deae3ad928fff573320d28ff58424fb42b2fe27988` |
| `apps/mcp/bridge.py` | `6b3f94d2605b4aae81b3bc4c6e84e61262401a8532ba1631e70de426ff6da257` |
| `tests/typed_host_tests.cpp` | `fb00e3c4d2f346684404105bffabc0ab82dfc56a427668dd600171f7d1a158b5` |
| `tests/query_tests.cpp` | `40e2e83f502ed287058588807ca8f5117841228f582c63d9cc82e9af327dcc99` |
| `tests/record_query_tests.cpp` | `0ea2529cd6ce6f66e83912e37bd2225eefa103b3f2f1c66432d6b1285193de6f` |
| `tests/m23_ipc_tests.py` | `7f45000155421595d54f53a29a070aa2d1aa309486c06e24c3b6d1a9a4a64d96` |
| `tests/c3_display_ipc_tests.py` | `afeb02c28ca5f9102f9e66abb5c7443867570946f6433e45501033f3d5aee736` |
| `tests/c3_sync_ipc_tests.py` | `216c2f9e76c293ef7f36e9000e0646f984d5afbdaf93239e9c6f13a3dbe8f02b` |
| `tests/mcp_bridge_tests.py` | `931019fa2038d45146e36d47020a88c07611f83b2900eca40d6cb65a17616936` |
| `CMakeLists.txt` | `6906b75127d94d16e7721e9af663e1f0fcef025ef2ce912e2a405919228ef1aa` |
