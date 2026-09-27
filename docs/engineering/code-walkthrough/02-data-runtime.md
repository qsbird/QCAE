# 02：文档数据、应用提交与任务运行时

本篇逐文件阅读 `modules/document`、`modules/application`、`modules/runtime`（含 `record_application` 桥）和 `features/legacy_api` 的 C++ 源码及构建文件。建议先读记录类型与只读视图，再读应用提交，最后读任务发布与旧接口；这里描述当前实现，不将架构规划当作已交付功能。

最关键的关系是：宿主共享一个 `RecordApplication`，它拥有当前已提交的 `DocumentView`、修订、历史和操作事实。业务回调生成候选；`EditSession::prepare()` 不发布模型；任务工作线程也只返回候选；真正提交都回到应用协调器。旧 `MemoryApplication` 是同一协调器的兼容外观，旧 `Model` 是导入、导出、读取及迁移边界上的值。

## 文档层：当前记录表示

### modules/document/include/qcae/record_registry.hpp

[打开源码](../../../modules/document/include/qcae/record_registry.hpp)。定义 `RecordTypeId`、`RecordFieldId`、稳定键 `RecordKey`、版本 `RecordVersion` 和不可变 `RecordImage`，是当前记录体系的基础。`RecordDescriptor` 描述字段、编码、引用与校验，`RecordRegistry` 注册并冻结这些规则；`RecordTraits<T>` 将生成的强类型 C++ 值接入注册表。应用、编辑会话和文档视图依赖它，`RecordInput` 只是解码中间值，不能替代权威类型；`RecordStats` 用于观察复制、编码及整模型工作。

### modules/document/src/record_registry.cpp

[打开源码](../../../modules/document/src/record_registry.cpp)。实现注册表冻结、描述符检查、强类型记录创建，以及 `record_wire` 的有界二进制读写。读取时检查类型、schema、字段类型和单位等信息，拒绝畸形或不支持的记录；数值读写也拒绝非有限值。它由 `DocumentView`、生成的 traits 和编辑会话调用，只提供数据机制，不决定文档提交或持久化时机。

### modules/document/include/qcae/records.hpp

[打开源码](../../../modules/document/include/qcae/records.hpp)。这是 `tools/generate_entities.py` 生成的文件，开头带语义 schema 摘要，不应直接手改；结构定义应回到生成器与实体 schema。`records` 命名空间中的材料、节点、梁、组织、分析、来源编号，以及当前直线几何和网格记录，配套 `RecordTraits`、字段描述、编码和局部校验。当前 `DocumentView` 中的记录以这些强类型值为依据；`GeometryLine`/`Mesh` 表示现有受控能力，不意味着已经具备任意几何拓扑或通用网格系统。

### modules/document/src/records_rules.cpp

[打开源码](../../../modules/document/src/records_rules.cpp)。补充需要跨记录查询的领域规则：梁长度与方向、梁和节点的网格归属、约束自由度、直线端点、派生网格来源与过期标记。`validate_relations()` 检查部件归属、装配/INCLUDE 无环、集合类型一致和来源编号唯一性；`make_record_registry()` 将生成描述符与关系规则注册后冻结。文档校验调用这些权威记录规则，界面不应另建一套同名业务规则。

### modules/document/include/qcae/document_view.hpp

[打开源码](../../../modules/document/include/qcae/document_view.hpp)。`DocumentView` 是共享不可变状态的只读句柄，提供 `find`、`visit`、`count`、版本与校验；它不向调用方暴露可写实体容器。`RecordChangeSet` 保存稳定键与 before/after 图像，`apply_record_changes()` 支持正向/反向应用，差量还可单独编码。应用层持有当前视图，业务读者取得视图副本，候选编辑不会自动改动当前状态。

### modules/document/src/document_view.cpp

[打开源码](../../../modules/document/src/document_view.cpp)。内部按类型组织记录表，使用 1024 槽的不可变页、身份索引和共享记录图像；变更时复制受影响的表/页，并复用未改图像。`apply_record_changes()` 校验键、注册表和 before 图像是否匹配，`validate()` 检查配额、引用目标/类型及注册规则。这里的槽位是内部存储位置，不能拿来替代稳定实体 ID；差量应用产生新视图，发布权仍属于应用层。

### modules/document/include/qcae/edit_session.hpp

[打开源码](../../../modules/document/include/qcae/edit_session.hpp)。`EditSession` 是基于只读视图的私有编辑覆盖层，支持 `put`、按强类型 `update`、`erase` 和覆盖层查询。`PreparedRecordChange` 返回基础版本、候选视图、记录差量与工作统计；更新不允许改变实体身份。业务处理器依赖它准备修改，`prepare()` 本身不会推进应用修订或写库。

### modules/document/src/edit_session.cpp

[打开源码](../../../modules/document/src/edit_session.cpp)。用 `pending_` 合并同一稳定键的多次修改，保留原 before 图像，并消除最终没有变化的编辑。`prepare()` 比较字段载荷生成变化字段列表，再应用差量和校验候选。它把业务草稿整理成提交输入；是否接受版本、追加历史并发布，由 `RecordApplication` 再检查。

## 文档层：保留的 Model 与交换边界

### modules/document/include/qcae/model.hpp

[打开源码](../../../modules/document/include/qcae/model.hpp)。定义旧 `Model` 的十二组工程集合、`Vec3`、实体结构、引用摘要和 `ModelEdit` 变体；其数值约定为 mm、N、MPa。旧格式接口、兼容快照、旧历史迁移仍使用这些值，因而文件尚未删除。它不再是活动文档的独立可写权威，普通新记录编辑应从 `DocumentView`/`EditSession` 进入共享应用。

### modules/document/src/model.cpp

[打开源码](../../../modules/document/src/model.cpp)。实现旧模型的实体枚举、引用枚举、合法性检查和 `affected_analyses()` 引用传播查询。校验覆盖身份、材料/梁字段、组织关系、引用、来源编号等受控模型条件；返回诊断值而不修改模型。它服务于仍消费 `Model` 的边界与迁移验证，当前记录提交的规则由记录注册表及 `records_rules.cpp` 承担。

### modules/document/include/qcae/read_view.hpp

[打开源码](../../../modules/document/include/qcae/read_view.hpp)。只有一个很小的兼容结构：`ModelSnapshot` 继承旧 `Model`，再附上 `DocumentInfo`。旧查询/读取消费者可在一个值中取得模型与文档版本；`MemoryApplication::snapshot()` 显式生成该值。它是读出物化结果，不是另一份应用状态或可直接回写的句柄。

### modules/document/include/qcae/model_codec.hpp

[打开源码](../../../modules/document/include/qcae/model_codec.hpp)。声明格式交换的 `IModelCodec` 端口，以及资源文本、导入/导出报告、来源与导出编号、`ArtifactPlan` 等 DTO。解码返回可选 `Model` 候选，编码根据显式分析与 profile 返回文本资源计划，导入单位必须明确声明。具体格式适配器实现这个接口，应用/宿主决定是否提交候选或发布文件；文本计划不等于文件已发布，更不等于求解成功。

### modules/document/include/qcae/records_model_bridge.hpp

[打开源码](../../../modules/document/include/qcae/records_model_bridge.hpp)。声明 `records_from_model()`、`model_from_records()` 与 `record_changes_from_models()`，把旧集合值和当前记录视图相互转换。接口注释明确限定为完整导入、导出或迁移，普通局部编辑不应经过整模型桥。兼容外观和旧状态迁移调用它，转换结果仍须通过同一应用提交路径才能成为当前文档。

### modules/document/src/record_model_bridge.cpp

[打开源码](../../../modules/document/src/record_model_bridge.cpp)。逐集合转换旧值和生成记录，转换旧节点/梁时补上导入网格记录，并为旧来源编号构造确定的迁移身份。反向转换显式物化 `Model`，迁移差量按持久身份比较而非沿用旧 vector 槽位；代码记录整模型编码/物化工作。阅读时留意旧表示能够表达的字段范围，它是兼容边界，不能用它往返来承诺所有新记录语义均被保留。

### modules/document/include/qcae/model_delta.hpp

[打开源码](../../../modules/document/include/qcae/model_delta.hpp)。`ModelDeltaRecord` 以集合字段号和 vector 下标保存旧模型记录的 before/after 字节，声明生成、应用和读写函数。它被旧历史读者和迁移使用，保留的是历史格式兼容能力。新应用历史使用稳定 `RecordKey` 的 `RecordChangeSet`，不要将这里的 index 误读成当前实体身份。

### modules/document/src/model_delta.cpp

[打开源码](../../../modules/document/src/model_delta.cpp)。按旧集合顺序和下标比较差异，将单条旧实体包在模型编码中，并根据方向执行替换、插入、删除。应用时核对原值与索引，读写时检查差量格式；迁移借此回到旧历史基线并完整重放。该算法维护旧历史语义，不参与当前普通记录事务的增量写入。

### modules/document/include/qcae/state_codec.hpp

[打开源码](../../../modules/document/include/qcae/state_codec.hpp)。声明旧状态二进制 `Writer`/`Reader`、`CodecError` 和完整 `Model` 编解码，限制总字节数为 64 MiB。旧工作区/工程读取与 `ModelDelta` 使用它，不依赖 SQLite 或 Qt。当前应用的记录行和元数据有自己的编码路径，不把这个整模型 codec 当作普通提交基础。

### modules/document/src/state_codec.cpp

[打开源码](../../../modules/document/src/state_codec.cpp)。实现小端整数、有限浮点、带长度文本、布尔及旧十二集合编码，检查截断、尾随字节和数量上限。`encode_model()`/`decode_model()` 使用 `QCAE-MODEL` 版本 1 标识，供旧边界与历史差量解码。代码能够读取旧值并不代表它会直接修改活动文档；激活与恢复仍须应用协调。

## 应用层：唯一提交协调器

未注入记录存储时，应用只发布内存状态；下述持久化步骤指显式配置存储的运行模式。持久化时必须先得到存储明确成功，再发布 RAM 候选。

### modules/application/include/qcae/record_application.hpp

[打开源码](../../../modules/application/include/qcae/record_application.hpp)。公开 `RecordApplication` 的生命周期、快照、预览/提交、直接 `execute`、历史及操作事实查询；`RecordApplicationOptions` 注入注册表、记录存储、项目存储与特性校验。`RecordPrepare` 在单写者临界区内接收不可变视图和 ID 分配器，只返回 `RecordPreparedOperation`，不得重入应用。`OwnedRowHandler`/`OwnedRowUpdate` 接入任务等侧行的 owner、schema、恢复和精确载荷比较；`RecordStateImage` 是脱离活动状态的恢复值，不是可写后门。

### modules/application/src/application_state.hpp

[打开源码](../../../modules/application/src/application_state.hpp)。私有 `record_detail::Data` 在恢复图像之上增加预览表、待提交行与统计，`Prepared` 绑定调用者、版本上下文和候选。声明候选校验、项目编解码、差量/侧行排队、恢复以及 `persist()`。只供应用实现使用，特性模块应依赖公开接口，不能直接改 `Data` 来绕开版本与原子提交。

### modules/application/src/application_state.cpp

[打开源码](../../../modules/application/src/application_state.cpp)。编码/解码记录状态、历史、操作事实、项目快照与 owned rows，并核验 schema、历史一致性和配额。`queue_changes()` 只排入受改记录，`queue_owned_rows()` 核对 owner 与期望载荷，`persist()` 将元数据和待变更行合并成带 expected generation 的 `StoreBatch`。存储明确成功后协调器才能发布 RAM 候选；不确定存储结果设置 poisoned 状态，要求显式恢复，普通编辑不会调用旧整模型 codec。

### modules/application/src/core.cpp

[打开源码](../../../modules/application/src/core.cpp)。实现所有公开方法及互斥的单写者入口：校验 DocId/Epoch/Revision，注册预览，核对幂等签名，提交差量、历史、操作事实和可选 owned rows。`commit()` 在候选中推进修订，先持久化再交换当前状态；`execute()` 复用准备/提交并清理内部预览，undo/redo 同样提交新的修订。创建、正常打开、恢复、保存/另存和关闭/放弃是独立路径；这里是共享权威的协调位置，具体工程实体规则由注入回调和记录规则提供。

业务编辑调用链：

```text
特性处理器或 MemoryApplication
  → RecordApplication::preview / execute
  → RecordPrepare(view, allocator)
  → EditSession::prepare → PreparedRecordChange
  → 应用核对基础版本、候选与幂等事实
  → StoreBatch（记录 + 历史 + 操作事实 + 元数据 [+ owned rows]）
  → 有存储时持久化成功 / 无存储时仅内存提交 → 发布 DocumentView
```

## 运行时：工作计算与提交分开

### modules/runtime/include/qcae/task_service.hpp

[打开源码](../../../modules/runtime/include/qcae/task_service.hpp)。定义任务状态机、冻结的 `TaskInputContext`、带序号的事件、任务记录和 `TaskControl` 合作式取消/进度接口。`TaskWork` 返回不可变 `TaskPayload`，`TaskPublisher` 注入会话检查、输入检查、载入、持久化与发布；`TaskService` 提供启动、查询、取消、等待和显式协调恢复。默认两个 worker、八个等待位置，profile 全空表示与求解器无关；它是中立运行机制，不自带工程模型或真实求解器。

### modules/runtime/src/task_record.cpp

[打开源码](../../../modules/runtime/src/task_record.cpp)。实现任务状态命名、终态判断和有界任务载荷编码，校验身份、事件顺序、状态转换、进度、receipt 与诊断的一致性。当前载荷版本 2 可读取版本 1，旧 receipt 缺失的 primary entity 保持为空。应用桥和运行服务共同调用它以保持持久任务事实可验证；它不把工作函数返回或进程退出码自行解释为工程验证通过。

### modules/runtime/src/task_service.cpp

[打开源码](../../../modules/runtime/src/task_service.cpp)。维护有限队列、worker、条件变量、调用者作用域的幂等任务事实，以及合作式取消；工作函数执行在发布互斥锁外，状态/进度与最终发布在锁内串行处理。启动先核对会话，匹配已有事实时重放，再对新任务校验修订；完成还要检查取消并经 publisher 发布，过期候选成为 conflict。持久化不确定时停止后续写入并保留 `outcome_unknown`，显式恢复后 `reconcile()` 读取事实，未完成任务标为 interrupted 而不自动重新运行。

### modules/runtime/record_application/include/qcae/task_application.hpp

[打开源码](../../../modules/runtime/record_application/include/qcae/task_application.hpp)。`RecordTaskPayload` 将工作结果包装为 `RecordPreparedOperation`，`task_row_handler()` 提供任务侧行规则，`record_task_publisher()` 把中立运行时接到一个传入的 `RecordApplication`。宿主必须先把 handler 放入应用选项，再创建应用与任务服务；任务服务及捕获应用的回调须先于应用销毁。它是独立桥接目标，应用层不反向依赖运行时。

### modules/runtime/record_application/src/task_application.cpp

[打开源码](../../../modules/runtime/record_application/src/task_application.cpp)。用 owner `qcae.runtime.task` 的 owned rows 保存任务，普通状态/进度调用 `update_owned_rows()`，不推进模型修订或 undo。最终 `publish` 调用同一应用的 `execute()`，在 `CommitOwnedRows` 回调中把成功任务及 receipt 与模型差量、历史、操作事实放进一个批次，并再次核对 profile。handler 将恢复中的未完成任务转为 interrupted，活动任务阻止关闭；它不另建文档写者，也不会以失败覆盖可能已经持久化的成功。

任务调用链：

```text
TaskService::start → 保存 queued → worker 保存 running
  → TaskWork(TaskControl) → RecordTaskPayload（候选，未发布）
  → 保存 committing → TaskPublisher::publish
  → RecordApplication::execute + 成功任务 owned row
  → 发布成功：模型与成功任务事实同批持久化 → succeeded
  → 确定冲突/失败：不发布模型，另行保存任务终态 → conflicted / failed
  → 存储结果不确定：停止后续写入，要求显式恢复 → outcome_unknown
```

## 旧 API：同一应用的兼容入口

### features/legacy_api/include/qcae/core.hpp

[打开源码](../../../features/legacy_api/include/qcae/core.hpp)。保留 `MemoryApplication` 的材料命令、旧导入/编辑预览、快照、生命周期与历史 API。`record_application()` 明确暴露它内部共享的同一个协调器，供新特性直接复用；构造参数可传入 owned-row handlers。名字中的 Memory 不意味着默认持久化，只有显式存储才提供 durable 能力；接口兼容不形成第二套文档或历史。

### features/legacy_api/src/core.cpp

[打开源码](../../../features/legacy_api/src/core.cpp)。`MemoryApplication::State` 只拥有一个 `RecordApplication`，生命周期/提交/历史直接转发，材料和组织编辑通过生成记录与 `EditSession` 准备。旧 `snapshot()` 显式调用桥物化 `ModelSnapshot`，完整旧导入也只生成候选；单位归一化依赖参数模块。仅给旧 blob 存储端口的嵌入者使用 `BlobRecordStore` 全图像兼容适配，生产 SQLite 实现 `IRecordStore` 时直接走记录增量路径。

### features/legacy_api/include/qcae/legacy_migration.hpp

[打开源码](../../../features/legacy_api/include/qcae/legacy_migration.hpp)。公开旧工作区与旧工程迁移函数，输入旧存储载荷，输出脱离活动应用的 `LoadedRows` 或 `RecordProjectImage`。注释规定调用者应向新的目标存储提交，读取不能改写用户提供的旧源。宿主使用它完成迁移边界，通用应用只接收转换结果与注入的旧工程解码器。

### features/legacy_api/src/legacy_migration.cpp

[打开源码](../../../features/legacy_api/src/legacy_migration.cpp)。解码旧工作区后复制文档身份、游标、操作事实和保存意图，从当前模型反向回到历史基线，再正向转换完整历史（包括 redo 后缀）为稳定键记录差量。保存 token 与原 snapshot 字节保持原值，返回源 generation 作为来源信息而非新目标 generation。旧工程转换只产生项目图像，源文件及活动文档都不会在这个函数中被修改。

### features/legacy_api/src/legacy_state.hpp

[打开源码](../../../features/legacy_api/src/legacy_state.hpp)。私有 `legacy_detail::Data` 保存旧 `Model`、下标式差量历史、幂等事实及保存意图，声明旧项目/工作区读者。它仅在旧解码与迁移中表示历史格式，不被 `MemoryApplication` 用作当前活跃状态。依赖旧 codec 与旧差量，是有意隔离的兼容结构。

### features/legacy_api/src/legacy_state.cpp

[打开源码](../../../features/legacy_api/src/legacy_state.cpp)。读取 `QCAE-WORKSPACE` 版本 2 与 `QCAE-PROJECT` 版本 1，检查配额、文档身份、历史游标、当前模型和所有历史重放状态。保存意图还要核验原项目身份与精确快照内容，畸形输入抛出 codec 错误。它保留旧持久格式的校验能力；正常打开与故障恢复的身份处理、目标持久化及发布由当前应用/宿主完成。

## 构建文件逐个定位

| 文件 | 阅读重点 |
|---|---|
| [modules/document/CMakeLists.txt](../../../modules/document/CMakeLists.txt) | 构建 `qcae_document`，包含当前记录实现与保留的 Model/codec/桥；公开依赖 foundation/contracts，要求 C++20。测试开关下登记记录文档测试与生成实体一致性检查。 |
| [modules/application/CMakeLists.txt](../../../modules/application/CMakeLists.txt) | 构建 `qcae_application` 的协调器和私有状态编码，公开链接 document/contracts，私有链接线程库。没有 legacy、runtime 或具体 SQLite 依赖。 |
| [modules/runtime/CMakeLists.txt](../../../modules/runtime/CMakeLists.txt) | `qcae_runtime` 只链接 foundation/contracts/线程；另建 `qcae_task_application` 链接 runtime 与 application，保持桥的依赖方向。测试开关下注册 runtime 测试。 |
| [features/legacy_api/CMakeLists.txt](../../../features/legacy_api/CMakeLists.txt) | 构建兼容外观及旧读取/迁移，公开依赖 application/document/contracts，私有链接 parameters。旧 API 作为特性消费应用层，不能让通用应用反向依赖它。 |

阅读这几层时，可以持续检查三个事实：手里的值是只读快照还是候选，谁负责核对基础版本，以及在哪个持久化成功点才替换当前状态。当前后台任务机制和受控记录能力已存在，但这些文件本身不提供真实 AI 接入、任意几何拓扑或真实求解器的数值验收。
