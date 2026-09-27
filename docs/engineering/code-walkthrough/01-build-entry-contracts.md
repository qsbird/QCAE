# 01：构建、公共契约与引擎入口

本章先回答三个问题：哪些代码进入同一个程序，哪些类型能跨模块传递，以及客户端的一条请求怎样进入应用服务。这里描述当前源码；运行证据与剩余边界见[入口补强验证](../next-validation.md)。

## 构建与模块边界

### CMakeLists.txt

[打开源码](../../../CMakeLists.txt)。根文件规定 C++20、编译告警和生成目录，并按模块装配库、可执行程序与测试。先看 `QCAE_BUILD_IPC`、`QCAE_BUILD_STORAGE`、`QCAE_BUILD_DESKTOP` 三个开关，再看 `add_subdirectory`；默认可构建不需要 Qt/VTK/SQLite 的核心，桌面要求 IPC 和存储同时启用。`qcae_core` 是给旧消费者的 INTERFACE 聚合目标，不是另一个文档核心；各模块真正的依赖以其 CMake 为准。

### modules/targets.json

[打开配置](../../../modules/targets.json)。登记生产构建目标的所有者、源码、公开/私有头文件和依赖，供架构检查与实际 CMake 图比较。它是工程检查数据，不负责运行时注册；增加目标、改变公开依赖时要同步更新。

### modules/source-migration.json

[打开配置](../../../modules/source-migration.json)。记录旧集中目录到当前模块目录的路径迁移，帮助检查遗留源码是否已归属正确模块。这里的 migration 是源码组织迁移，不是用户工程或 SQLite 数据迁移。

## foundation：身份、版本与错误

### modules/foundation/include/qcae/types.hpp

[打开源码](../../../modules/foundation/include/qcae/types.hpp)。从 `Id<Tag>` 开始读：`DocumentId`、`DocumentEpoch`、`EntityId`、`PreviewId` 和 `TransactionId` 是不同类型，`Revision` 表达文档修订；`DocumentRef` 和 `WriteContext` 把请求定位到一次打开会话及预期修订。`Quantity`、`ProfileRef`、`TargetBinding`、`Caller`、`Diagnostic` 和 `Result<T>` 是跨模块的基础值，不包含 SDK 类型。底部的 `Material` 保留旧接口兼容用途，当前权威材料记录在生成的 `records.hpp`；实体 ID 不能被求解器编号、vector 下标或 VTK cell ID 替代。

## contracts：跨边界的值与端口

### modules/contracts/include/qcae/document_info.hpp

[打开源码](../../../modules/contracts/include/qcae/document_info.hpp)。`DocumentInfo` 给客户端返回文档身份、修订、内容状态、保存路径/保存内容状态以及 dirty/durable。它是当前状态摘要，不是完整模型；比较保存内容状态可识别 undo 回到保存点，而不能简单用修订号判断是否已保存。

### modules/contracts/include/qcae/application_types.hpp

[打开源码](../../../modules/contracts/include/qcae/application_types.hpp)。定义修改回执 `ChangeReceipt`、线性历史摘要和资源 `Limits`，同时保留旧材料命令与预览 DTO。注意回执区分原提交修订和当前修订，幂等重放不表示再次执行；默认实体上限 100000、历史 128 项等是当前资源限制，不是容量或性能验收结果。

### modules/contracts/include/qcae/profile_provider.hpp

[打开源码](../../../modules/contracts/include/qcae/profile_provider.hpp)。`IProfileProvider` 只提供不可变 `ProfileDefinition`，包含能力包身份、求解器家族和分析类型。`configured` 与 `validated` 分别标记真实执行环境是否配置及验证，Nastran 文本编解码可用不能自动把这两个字段变为 true。

### modules/contracts/include/qcae/record_store.hpp

[打开源码](../../../modules/contracts/include/qcae/record_store.hpp)。`IRecordStore` 以不透明记录行加载状态、提交 `StoreBatch`；稳定 `StoreKey`、预期 generation 和事务 ID 让应用层能把模型、历史、操作事实、元数据及任务事实放进一个原子批次。`RowMutation.after` 为空指针才表示删除，空字符串仍是存在的值。`artifact_record` 是已预留的存储空间枚举，不能据此声称产物业务已完成；具体 SQLite 类型不出现在这个端口中。

### modules/contracts/include/qcae/workspace_store.hpp

[打开源码](../../../modules/contracts/include/qcae/workspace_store.hpp)。`IWorkspaceStore` 同时保留旧整块工作区接口与工程文件租约、读取、完整快照发布接口。`StorageError.uncertain()` 表示写入可能已经持久化，应用不能按普通失败直接重试并继续写入；工作恢复库的行提交与另存工程文件的发布是不同操作。

### modules/contracts/include/qcae/render_packet.hpp

[打开源码](../../../modules/contracts/include/qcae/render_packet.hpp)。`RenderPacket` 是只读显示数据，带模型/视图版本、节点坐标、梁连接以及真实 EntityId，供 VTK 适配器消费。`RenderBeam.points` 是包内局部索引，`RenderBeam.entity` 才是工程身份；当前包只有点和梁，尚不包含面、体、CAD 曲面或结果字段。

### modules/contracts/include/qcae/view_session.hpp

[打开源码](../../../modules/contracts/include/qcae/view_session.hpp)。`ViewSession` 保存隐藏实体、相机摘要和独立视图版本，让选择结果可以同时绑定模型与视图。它是可重建的交互状态，不是模型实体，也不应把旋转视图写进工程修改历史。

## engine：进程组装入口

### apps/engine/main.cpp

[打开源码](../../../apps/engine/main.cpp)。生产 executable 的薄入口：取得 `default_engine_contributions()`，再调用 `qcae_run_engine`。实际 socket 服务、生命周期和持久化装配位于 `engine_host.cpp`。

### apps/engine/include/qcae/engine_host.hpp

[打开源码](../../../apps/engine/include/qcae/engine_host.hpp)。声明接收启动参数与静态贡献列表的 `qcae_run_engine`。生产入口与测试专用贡献入口复用同一宿主函数，差别只在可信的启动贡献。

### apps/engine/engine_host.cpp

[打开源码](../../../apps/engine/engine_host.cpp)。创建 `QCoreApplication`、解析 socket/workspace/migrate-from 参数、检查 endpoint 锁和现存服务并监听 `QLocalServer`。先装配并冻结记录注册表，再创建 Nastran codec、可选 SQLite store、共享应用、选择服务和 `TypedHost`；迁移解码也使用同一注册表。兼容外观与 typed 入口共享 `RecordApplication`，没有 `--workspace` 时仍为内存模式。

继续读连接回调：按换行接收不超过 1 MiB 的 JSON 帧，要求先握手，再交给 `ipc::dispatch`，最终进入 Qt 事件循环。调用者由宿主固定为本地 OS 用户上下文 `local-user`，请求 JSON 不能冒充另一个 caller；这还不是多人认证系统。启动能发现工作恢复状态，但用户必须显式选择恢复，不把进程启动直接当作正常打开工程。

## engine_api：JSON 到业务接口

### adapters/engine_api/include/qcae/ipc_api.hpp

[打开源码](../../../adapters/engine_api/include/qcae/ipc_api.hpp)。声明公共 `failure` 和总分派 `dispatch`，接收同一应用、可信 caller 以及可选 codec/profile/选择/typed 服务。这里是 Qt JSON 适配边界，核心公开接口不应引入 `QJsonObject`；分派器负责解释请求，文档写入仍交给应用层。

### adapters/engine_api/src/ipc_api.cpp

[打开源码](../../../adapters/engine_api/src/ipc_api.cpp)。先读顶层字段白名单、API 版本和参数对象，再看 `typed → selection → model → 生命周期/历史` 的路由。`requested_version` 与 `expected_profile` 交给 typed 上下文解析，其他入口拒绝这两个顶层字段。`operations.get` 保留 commit/undo/redo 的兼容映射，其他文档操作直接查询应用保留的 action outcome，不受当前处理器是否安装影响；查询仍检查文档/epoch 和 caller 作用域。`capabilities.list` 合并 typed 注册描述与旧目录，旧材料预览仍是兼容路径。

### adapters/engine_api/include/qcae/typed_host.hpp

[打开源码](../../../adapters/engine_api/include/qcae/typed_host.hpp)。`TypedHost` 持有 `RecordApplication&`，提供能力发现和分派，内部拥有操作注册表及按需创建的任务服务。三参数构造接收 `OperationContributor`，将同一应用和延迟 task-service 访问交给启动贡献；两参数构造保留默认贡献。它不拥有第二个文档，引用的应用必须比它活得更久。

### adapters/engine_api/src/typed_host.cpp

[打开源码](../../../adapters/engine_api/src/typed_host.cpp)。构造时执行贡献并拒绝与 intrinsic、旧操作目录或 handshake 名称冲突的注册。`context` 解析正 uint32 `requested_version` 和完整 `expected_profile`，注册表核验版本及必需上下文后才调用处理器；省略版本明确使用已安装版本。能力中的版本、输入字段、profile 必需标志和可用性来自已注册条目。任务状态、取消、协调及字段读取仍是内置入口；profile 是否适用由处理器在应用准备阶段核验，不在传输层另设一份目标规则。

### adapters/engine_api/include/qcae/engine_contributions.hpp

[打开源码](../../../adapters/engine_api/include/qcae/engine_contributions.hpp)。`EngineContribution` 按稳定贡献 ID 收集记录/规则与操作注册回调，`EngineAssembly` 返回已冻结记录注册表和组合操作回调。它是可信静态启动接口，没有动态加载或独立文档状态。

### adapters/engine_api/src/engine_contributions.cpp

[打开源码](../../../adapters/engine_api/src/engine_contributions.cpp)。`default_engine_contributions()` 提供生产记录/规则，`default_operations()` 注册材料、网格编辑、几何创建和后台线网格。`assemble_engine` 拒绝空或重复贡献 ID，先收集记录与规则再 freeze；组合操作回调随后在同一个应用上执行。新增测试贡献无需修改通用提交、历史或存储算法。

### adapters/engine_api/src/typed_values.hpp

[打开源码](../../../adapters/engine_api/src/typed_values.hpp)。适配器私有的任务状态/事件 DTO 投影和 `Result<T> → Result<Value>` 转换，供启动注册包装器与 typed 分派复用。它不执行任务、不存储事实，也不拥有文档。

### adapters/engine_api/include/qcae/ipc_model.hpp

[打开源码](../../../adapters/engine_api/include/qcae/ipc_model.hpp)。声明 profile 的 JSON 投影以及可选 `dispatch_model`，把模型查询、兼容编辑与格式操作从总分派器拆出。返回 optional 表示这组分派器是否处理当前操作，不代表模型查询可以绕过应用取得可写容器。

### adapters/engine_api/src/ipc_model.cpp

[打开源码](../../../adapters/engine_api/src/ipc_model.cpp)。实体查询与双向引用读取统一取 `RecordSnapshot`，枚举、kind/ID/名称筛选、组织视图和分页均调用记录查询；字段和来源按记录描述序列化，未知 kind 或条件明确拒绝。省略 kind 不再转入旧 `ModelSnapshot`。旧编辑预览、导入和内存导出预览仍在格式/兼容模型边界完成。

### adapters/engine_api/include/qcae/ipc_selection.hpp

[打开源码](../../../adapters/engine_api/include/qcae/ipc_selection.hpp)。只声明接收同一应用与 `SelectionService` 的可选选择分派函数，把选择协议留在适配层。公共查询条件和选择句柄在核心 query 模块定义，Qt JSON 解析不应反向进入该模块。

### adapters/engine_api/src/ipc_selection.cpp

[打开源码](../../../adapters/engine_api/src/ipc_selection.cpp)。把视图创建/更新、显示数据、条件选择、集合运算与句柄读取请求转为中立查询参数，并序列化结果。它检查文档/视图上下文，显示命中候选由客户端图形实现提供；本地 engine 无图形上下文，不能自行判断屏幕遮挡，也没有因此实现无界面的可见性 picking。

## 本章其余构建文件

| 文件 | 作用与依赖 |
|---|---|
| [modules/foundation/CMakeLists.txt](../../../modules/foundation/CMakeLists.txt) | INTERFACE 目标只传播 foundation 头文件和 C++20 要求，不生成实体持久化实现。 |
| [modules/contracts/CMakeLists.txt](../../../modules/contracts/CMakeLists.txt) | INTERFACE 目标依赖 foundation，依据旧 `docs/contracts/operations.json` 生成能力目录 `operations.hpp`；与新 typed 输入生成分开。 |
| [adapters/engine_api/CMakeLists.txt](../../../adapters/engine_api/CMakeLists.txt) | 将分派与静态贡献装配编为 `qcae_engine_api`，依赖 legacy/query/typed 特性和 QtCore，不链接 Widgets 或 VTK。 |
| [apps/engine/CMakeLists.txt](../../../apps/engine/CMakeLists.txt) | `qcae_engine_host` 承担共享宿主实现并链接 engine API、传输和 Nastran；存储启用时才链接 SQLite/定义 `QCAE_HAS_SQLITE`。薄 executable `qcae-engine` 链接这个宿主目标。 |

读完后应能准确指出：`main.cpp` 选择生产贡献，`engine_host.cpp` 运行宿主，`engine_contributions.cpp` 组装记录与操作，`ipc_api.cpp`/`TypedHost` 分派请求，`RecordApplication` 拥有提交权。下一章继续看版本检查、历史与持久化。
