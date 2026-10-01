# C3 局部刷新合并请求合同审查

2026-09-30。只读审查当前可变源码，未运行构建、GUI或测试，未修改产品或测试。
本文是实施前合同和短计划；C3尚未通过完整负载验收，C4共同基线尚未冻结。
上游同轮实测报告节点约257–284KB、材料约115–118KB；本文未重测这些值，
不能据此声称材料68KiB上限或任何未知库路径已通过。

依据：[C3收敛计划](c3-closure-plan.md)、[设计基线](../baseline/README.md)、
[运行时合同](../architecture/runtime-and-data.md)、[架构边界](../architecture/README.md)、
[模块依赖](../architecture/module-dependencies.json)。相关门禁为REQ-05/06/09/15、
TST-F03/F06、TST-A07/A11/A12及SK-10/12子集。

## 审查结论与短计划

普通同文档、连续修订、非拓扑编辑可以把原来的
`edit → project.current → view.update → view.render_resource → entity.query`
合并为`edit → view.render_resource`，仍接收原DocumentChanged/HistoryChanged通知。
这只是减少三次常规请求；非空显示差量仍使用原资源通道，首次加载、事件缺口、
拓扑/引用变化、缺失能力及版本冲突继续走原回退。

1. 先共享两个既有IPC序列化函数：DocumentInfo摘要，以及同一RecordSnapshot上的实体行DTO。
2. 事件只在真正当前修订的最后一条DocumentChanged附摘要；桌面通过共同的摘要应用函数更新上下文。
3. RenderService在显式选择的新路径中检查旧模型/视图基线，然后复用SelectionService的模型重绑定；视图版本精确递增一次。
4. 同一render响应携带完整、有界的changed-ID实体行；桌面成功安装显示后用同一行应用函数更新树。
5. 先跑版本、旧选择、相机/显隐、迟到回包、旧能力和资源负样本，再按原三档×20次固定夹具重测完整账本。

不能通过删事件、停止配额校验、跳过行刷新、放宽版本或缩小夹具取得负载通过。

## 现有源码的关键事实

| 现有位置 | 合同与复用点 |
|---|---|
| `adapters/engine_api/src/ipc_api.cpp::info_json` | project.current/create/open/save等共同使用的DocumentInfo序列化；含文档/epoch/修订、content_state、name、material_count、dirty/durable、project_id、保存路径/状态。当前位于匿名命名空间。 |
| `adapters/engine_api/src/event_stream.cpp::refresh` | 从current_document读取真实当前DocumentInfo，借用changes_since逐提交发DocumentChanged/HistoryChanged。积累多个提交时，复制最新info后仅改revision，不能将该副本其他字段冒称历史摘要。 |
| `adapters/engine_api/src/render_service.cpp::dispatch` | 当前先要求get_view对应当前模型，再校验expected_view_revision；投影器与客户端base不符时全量回退；changed_ids/refresh_tree已来自同次journal。 |
| `modules/query/src/query.cpp::inspect_view/update_view` | inspect核对owner/doc/epoch但允许旧模型修订；update校验hidden IDs，排序去重，并在模型、显隐或相机变化时递增view_revision。原get_view严格要求当前模型修订。 |
| `modules/query/src/query.cpp::page` | 检查live view对应当前模型，再核对handle.view_revision；没有单独比较handle.model_revision。因此仅改model_revision且不递增view_revision会错误接受旧选择。 |
| `adapters/engine_api/src/ipc_model.cpp::entity_query/record_entity_json` | query_entities筛选与分页；record_entity_json按注册记录字段生成原entity.query行，保持`*_id`、`*_mm`、sources、profile_ref、beam.section_id等既有名字/空值。 |
| `ui/desktop/src/desktop.cpp::pollCurrent` | 权威摘要驱动上下文、标题、dirty、选择失效、视图更新及必要全量回退；event_generation保护事件之前发出的旧project.current响应。 |
| `ui/desktop/src/desktop.cpp::renderResource/finishRenderResource` | render_generation及doc/epoch/revision/view/view_revision共同拒绝迟到响应；manifest/encoding/bytes版本和VTK applyDelta均校验后安装。 |
| `ui/desktop/src/desktop.cpp::refreshChangedRows` | 原请求只有`ids`和`limit=1000`，无组织filter；同一DTO只更新已有tree_items与owner_rows，必要时重建owner标签。可抽出共同applyRows，不能复制这段行为产生另一套规则。 |
| `modules/clients/src/resource_client.cpp` | context/generation销毁旧传输；manifest、块顺序/尺寸、Base64与SHA256保护后才回调；废弃资源仍release。无需为合并刷新放宽这些规则。 |

引擎当前在同一事件循环串行执行此dispatch；主机Caller仍是可信`local-user`，不是请求JSON身份。
view_session_id、实际存储的view revision和实际投影基线必须分别校验，不能把连接当作这些校验的替代。

## DocumentChanged携带同版本摘要

可新增独立能力版本，例如`events_document_summary_version=1`；
`events.subscribe/read`明确选择`include_document_summary=true`，subscriber保存选择结果。
未选择的旧客户端继续收到原事件格式，新客户端未获能力时不发送新参数。
事件序号、订阅generation、retention/gap、50ms通知链和HistoryChanged保留。
也可用一个明确的`lean_refresh_version=1`表示三项业务扩展均已实现，但三个请求选择仍各自显式，
不能由render_wire_version=3或inline-empty能力隐式启用。只实现部分时不广告这个完整能力版本。

摘要只能是从这次成功的`current_document()`取得的DocumentInfo经共享序列化生成的有界查询结果。
摘要自己的document_id/epoch/revision必须等于外层事件。不得由GUI推算dirty/count，
不得用已提交回执推算当前内容，也不得再次获取一个可能已经更晚的快照填入旧事件。

特别是refresh一次追回多个提交时：保留每个提交的事件顺序，只给
`change.revision == info.revision`的最后一条DocumentChanged附摘要。
前面的事件没有可证明同版本的DocumentInfo，须保留旧查询回退；不修改事件数量来掩盖该情况。
当前`version = *info; version.revision = change.revision`只能作为事件身份，不能成为旧修订的完整摘要。

桌面快路径仅适用连续、正常增量、同doc/epoch的DocumentChanged：
外层及summary类型合法、摘要版本相等、`base_revision`等于客户端已知修订，
无gap、无`resync_required`、无关闭/新激活context。缺失或畸形摘要触发原project.current，
不能部分更新标题或revision后继续渲染。HistoryChanged只保留原通知行为。
save/save-as的同修订ProjectMetadataChanged保留原查询回退即可，避免扩大本片范围。

接收事件时先递增event/render/selection generations、停止旧传输并标记旧scene无效，
随后完整解析摘要，再复用pollCurrent的上下文应用逻辑。先前正在飞行的project.current响应
仍因event_generation不同被拒绝；客户端不能因有摘要而取消缺口/重连/周期性补偿查询。
架构所说“事件不能替代当前状态查询”仍有效：这里是把一份冻结的有界查询结果附在通知上，
不是按事件在客户端累计第二份业务状态。实际wire说明应记录这项补充。

## render_resource中的严格模型重绑定

独立能力可为`render_model_rebase_version=1`，参数可为`allow_model_rebase=true`。
未选择时现有get_view严格模型检查完全保留。选择只允许同一个存储view的model revision前进，
不得携带或覆盖hidden_ids/camera_fingerprint；相机/显隐修改继续使用原view.update。

令请求目标模型修订为R、已安装客户端基线为B、旧view revision为V。
读取当前不可变snapshot并通过inspect_view后，必须同时成立：

| fence | 要求 |
|---|---|
| 活动文档 | 请求doc/epoch等于snapshot.version；R等于当前authoritative snapshot revision。 |
| view身份/权限 | inspect_view成功，真实view属于同doc/epoch及Caller。 |
| 旧模型基线 | 真实stored view.model_revision恰好等于请求base_revision B，且R>B。 |
| 旧视图基线 | 真实stored view.view_revision == expected_view_revision == base_view_revision == V。 |
| 参数和范围 | 两个base字段成对存在、类型严格；显式rebase选择合法；hidden IDs仍存在；计数不能溢出。 |
| 投影基线 | 实际projector的doc/view/model/view revision才决定能否delta；客户端声明不能授权假基线，仍可full回退。 |

检查必须发生在任何视图状态变更之前。在当前串行dispatch内可直接复用
`update_view(snapshot, caller, view_id, inspected.hidden_ids, inspected.camera_fingerprint)`，
并验证返回model_revision==R、view_revision==V+1，隐藏与相机未变。
这保留原核心SelectionService规则，最小方案无需更改query.hpp/query.cpp。
若实现独立rebase helper，应同样验证且只发布此会话元数据，不放宽get_view、select或page。

**model rebase必须递增view_revision一次。** 即便材料修改没有显示几何payload，
旧selection.get/combine/evaluate与旧picker候选仍不得被接受。
版本不匹配、已恢复epoch、另一个消费者更新view、相机更改先到、base缺失/伪造、
hidden实体已删除时应结构化拒绝或原重同步，不能自动以较新view替代客户预期。

响应的manifest或version_only acknowledgement应绑定目标`(doc, epoch, R, view, V+1)`；
ack/delta仍绑定原`(B,V)`。客户端可以按此精确预测目标版本，不能接受任意`view_revision>V`。
原资源describe/read保持当前view及模型版本校验。

rebase之后编码/发布失败不会撤销已经完成的模型事务。原渲染器也可能已前进；
客户端必须进入明确恢复（重新查询并创建/重建view或原严格view.update），
不能无限重发同一个旧V，也不能猜测server最新V。若采用候选式发布降低这类失败成本，
那只是可丢弃视图/投影事务，不是第二个文档提交协调器。

## 同一render响应附实体行

独立能力可为`render_changed_rows_version=1`，参数可为`include_changed_rows=true`。
使用与当前render同一个RecordSnapshot和journal，复用`query_entities`及原
`record_entity_json`序列化。将已有内部函数抽为adapter级共享函数（例如
`entity_rows_json(snapshot, ids, limit)`）并供原entity.query与render调用，
不要调dispatch_model再取第二次snapshot，也不要在core引入QJson类型。

只有`refresh_tree=false`且changed ID完整、有界时返回可以直接应用的行。
响应明确绑定doc/epoch/R/view/目标view_revision，并声明`rows_complete=true`；
少于上限的完整结果可以为空，不能把截断分页伪称完整。客户端仅在显式选择、
完整行元数据合法且版本一致时省略entity.query。能力缺失、rows_complete缺失/false、
结构异常或超限保留原查询/树重载，必要时应让局部刷新失败而非宣称完成。

ID来自journal稳定identity，不能使用存储rowid、投影数组下标、solver number或VTK ID。
沿用原1000 ID/行边界和控制帧上限，并为可变大字段增加编码字节预算；
集合members或sources不能因为行数很少就获得无限控制消息。
创建/删除、引用/组织变化、journal缺口及full显示继续置refresh_tree并运行原loadOwners/loadTree。
不能把某种字段改动仅因显示delta为空就视为树也无需刷新。
非query-kind记录（例如SourceIdentifier）的identity不会由原entity.query变成实体行；
这类记录还可能改变其他实体的sources投影。最小快路径遇到此类变更应置refresh_tree回退，
不能将空changed_rows解释为相关实体行已经全部刷新。

收到响应时先保留行DTO；完成manifest/块/encoding/delta的全部校验和安装之后，
再调用共用applyRows更新已有tree_items与owner_rows并设置treeRevision。
如果响应迟到或资源失败，行也不能发布；首次树、全量组织查询及source字段语义保持原行为。
同一摘要/显示/行版本只证明这些投影来自engine，GUI仍只保存可丢弃显示/表格缓存。
正式编辑、预览、undo/redo与保存仍经过原application事务和历史。

## 相机、排队显隐与迟到回包

桌面快路径的前提还包括：没有view.update in flight、没有queued_hidden、没有待执行camera debounce，
当前viewport camera fingerprint等于该已安装view的已确认fingerprint，rendered_version对应B/V。
任何一个条件不满足，继续原updateView队列。不能因新文档summary先到，就把旧camera update认作无关。

模型rebase的render pending期间，新的camera/hidden intent仍按原顺序排队，
等目标资源成功安装后才发正常view.update；其expected_view_revision应为V+1。
不应因排队camera/hidden就提前覆盖请求的旧V或隐藏集。
如果选择设计为废弃pending render，则必须保证旧server view变更的结果可恢复，
不能只递增客户端generation而留下未知server V。

回包同时核对request generation、doc/epoch/R/view及请求时V；响应目标只能为精确V+1。
在客户端view_revision改为V+1后，ResourceClient context及后续字节callback也要匹配新目标；
不能继续使用捕获旧V的matches条件导致自己的新资源被拒绝。
rendered_version、scene_valid及treeRevision只能在完整安装后发布。
旧响应不能清除新文档/新请求的pending状态；旧manifest仍release，旧version_only响应没有资源可release。

原选择意图generation、模型/视图packet gates、同文档/epoch边界与view.update队列继续有效。
应拒绝：任何旧模型句柄，旧相机/显隐句柄，旧view句柄，其他Caller句柄，旧epoch/DocumentId句柄，
尚未安装新资源时来自仍显示旧packet的拾取，以及迟到的selection.evaluate/get返回。
完成过的选择意图不能在后续刷新重复播放；未完成且合法的最新树选择仍在刷新后重新评估。

## 必要负样本与原测试保护

| 最少新负样本 | 必须验证的结果 |
|---|---|
| 一次refresh累计edit/undo/redo三个修订 | 原六事件及顺序保留；仅当前末条DocumentChanged有同revision真实摘要，旧content_state/dirty不得伪造。 |
| 缺失、畸形或跨doc/epoch/revision摘要；event_gap；storage uncertainty | 不快用摘要；旧scene/选择立即失效；原authoritative resync保留。 |
| 新能力0/缺失、旧client无opt-in、三个opt-in参数各错误类型 | 旧路径兼容；新字段严格拒绝；不能猜测能力或默默变语义。 |
| rebase wrong stored-model base、wrong expected/base view、base缺一、跨Caller/doc/epoch | 结构化拒绝，camera/hidden不变，文档修订/历史不变。 |
| 材料empty delta合法rebase | 新view_revision恰好V+1，旧selection.get/combine拒绝；资源read/release为0仅在inline-empty实际选中且合法时成立。 |
| 同模型rebase重试、假server view_revision V+2、view revision溢出 | 不能二次递增或接受任意新V；走明确恢复；无旧选择通过。 |
| held project.current/held render/held selection回包跨事件或新文档到达 | 旧摘要、显示、行和选择均不能安装或改变新请求pending状态。 |
| camera timer先到rebase、render in flight期间camera/hidden排队 | 旧V不能覆盖新camera；排队意图只在目标安装后使用目标V，原队列次序保留。 |
| changed rows错误revision、重复/非法ID、畸形fields、超行数/字节、rows_complete=false | 不更新treeRevision或部分安装错误行；原查询/重载回退。 |
| render含rows但manifest/块/encoding/delta无效或迟到 | rows也不发布；资源照旧清理，旧VTK与树不被部分替换。 |
| 创建/删除、引用变化、源编号/组织成员改变、journal gap | 原refresh_tree/full回退语义保留，无截断局部行冒称组织完整刷新。 |

fake engine不能只在握手报能力：应真正实现同version summary、新旧view检查与V+1回包、
完整行version标记，保留held current/render/update及故意损坏response接口。
测试setup应以只读pipelineIdle/请求barrier等待真实camera debounce和资源安装完成；
负race用单次操作，不能通过重复点击把旧packet阻止变成成功选择。

继续保护现有`event_stream_tests`/`event_client_tests`的ordered/gap/metadata/reconnect事实，
`query_tests`的camera/hidden/model旧选择失效，`render_service_tests`的wire协商及空ack版本，
`resource_tests`的分块/lease/digest/迟到callback，以及`desktop_selection_tests`的
旧文档pending、事件前current、相机等待资源、最新树选择、旧packet拾取和非法ack回退。
`c3_sync_ipc_tests.py`原资源/版本/事件矩阵及8项点线PICK、工具30次继续执行。
新增用例优先进入这些既有test目标，无新依赖、无新GUI生产计时行为。

## 最少文件触点与验证边界

复用现有公开接口的最小产品触点可为10个既有文件：

- `adapters/engine_api/include/qcae/ipc_api.hpp`与`src/ipc_api.cpp`：共享DocumentInfo序列化。
- `adapters/engine_api/src/event_stream.cpp`：摘要/订阅opt-in，历史末条摘要规则；State/Subscriber已为私有内部实现，无需改公开header。
- `adapters/engine_api/include/qcae/ipc_model.hpp`与`src/ipc_model.cpp`：复用现有实体行序列化。
- `adapters/engine_api/src/render_service.cpp`：严格rebase、版本回应及有界完整行。
- `apps/engine/engine_host.cpp`：三项能力只在真实实现可用时广告。
- `modules/clients/include/qcae/desktop_client.hpp`与`src/desktop_client.cpp`：三项能力读取/断线重置。
- `ui/desktop/src/desktop.cpp`：共用摘要/行应用，快速路径及camera/hidden/generation fences。

复用原SelectionService的update_view可避免修改core query接口；RenderPacket/Delta wire、
ResourceClient/resource DTO及VTK均不需要放宽或新协议。若抽独立纯C++ rebase helper，
query.hpp/query.cpp是额外触点，必须维持上述递增与权限规则。
若将能力常量或订阅选择做成新的公开DTO，event_stream.hpp可作为额外触点，非最小方案要求。
wire说明及既有相关test文件是另计的合同/回归触点，不需修改根CMake或目标注册。

本文只完成源审。实现后应执行check_design、check_cpp_format、diff检查、受影响core/local/desktop测试，
并按原固定N、块容量、记录大小、节点度和60次样本重测从命令接收到显示/树更新结束的完整账本。
新增摘要/rows的JSON构造、编码、解码及套接字成本仍计入；未知路径保持unknown，
不能只据roundtrip从五变二宣布68KiB通过或冻结C4。
