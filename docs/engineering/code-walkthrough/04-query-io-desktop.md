# 查询、格式、存储与桌面逐文件导读

这一章从已提交的记录读视图走向查询结果、文件字节和屏幕。建议先读查询，再读 SQLite 与 Nastran，最后沿 `qcae-desktop → DesktopClient → engine → RenderPacket → VtkView` 跟一次刷新；GUI 只保留显示缓存和连接上下文，文档、事务与历史仍归 engine。

这里对应 REQ-02、05、06、08、09、10、18，以及 TST-F01/F03/F05/F06、TST-A01/A06/A07/A12 的实现边界，不能把这些文件的存在当成完整验收通过。当前记录原生查询与行存储已经存在，但 Nastran 格式边界仍使用 `Model`，桌面仍采用有大小限制的 JSON 帧与整包显示刷新；没有真实求解执行，也没有新增几何/网格创建 GUI。

## 查询：从记录读视图取得事实

### modules/query/include/qcae/query.hpp

[打开源码](../../../modules/query/include/qcae/query.hpp)。这是查询与选择的公共入口，定义 `EntityFilter`、`QueryPredicate`、`SelectionScope`、分页结果、`SelectionHandle` 和 `SelectionService`，把筛选、候选域、隐藏策略与版本写进类型。`EntityFilter` 组合 kind、ID、名称及组织视图，实体/字段/引用和选择接口直接接收 `DocumentView`；`ModelSnapshot` 重载是显式兼容入口，头文件不依赖 Qt 或 VTK。

### modules/query/src/entity_queries.cpp

[打开源码](../../../modules/query/src/entity_queries.cpp)。`query_entities` 按注册描述符枚举并组合 kind/ID/名称/组织范围，未知 kind 明确拒绝；`query_fields` 和 `query_references` 返回字段与双向引用页，内部记录不作为普通实体暴露。组织范围保留旧实体浏览的 owner 排除、装配中间行、梁端点和 INCLUDE 子层级语义，类型判断使用稳定 RecordTypeId。公共来源引用保留 `source.include` 角色；`query_affected_analyses` 扩展反向影响并保守关联梁分析。读取不物化旧 `Model`，但分页和影响传播仍有遍历成本。

### modules/query/src/query.cpp

[打开源码](../../../modules/query/src/query.cpp)。记录原生 `execute_query` 实现逻辑条件、归属/材料/截面关系、世界坐标框、候选域反选及隐藏过滤，`SelectionService` 则管理调用者拥有的视图、选择句柄和过期/配额检查。世界框支持节点、梁和 `GeometryLine`，但 `produce_render_packet` 只生成节点点与梁线，隐藏节点仍可能作为可见梁的连接端点进入包；默认最多 200000 个点/梁实体，不是完整几何显示。核心拒绝没有图形提供者的 `visible_only`，桌面用 `picker_candidates` 传入实际图形命中候选，再由核心核对实体与版本。

### modules/query/src/query_legacy.cpp

[打开源码](../../../modules/query/src/query_legacy.cpp)。所有旧 `ModelSnapshot` 重载先经 `records_from_model` 物化记录，再转调记录原生实现，因此这是可计量的兼容桥，不是第二套查询算法。它复制生成注册表并清空 mesh/geometry 的 `query_kind`，保持旧快照的实体暴露范围；阅读生产局部性路径时不要把这条全量转换当作正常记录查询成本。

## SQLite：保存提交事实与快照字节

### adapters/storage_sqlite/include/qcae/sqlite_store.hpp

[打开源码](../../../adapters/storage_sqlite/include/qcae/sqlite_store.hpp)。`SqliteWorkspaceStore` 同时实现旧 `IWorkspaceStore` 与新 `IRecordStore`：前者读写整块 payload，后者加载行并提交 `StoreBatch`，工程租约和快照发布也通过这个适配器提供。`StoreOptions` 限制字节、行数、批次及键长，故障钩子仅用于确定性测试；独立的 `read_legacy_workspace_readonly` 为显式迁移提供受保护的旧库读取，不自动迁移源文件。

### adapters/storage_sqlite/src/sqlite_store.cpp

[打开源码](../../../adapters/storage_sqlite/src/sqlite_store.cpp)。先看路径规范化、sidecar/inode 租约和数据库格式检查，再看 `commit_rows` 的代次比较、最终状态配额计算与 `BEGIN IMMEDIATE/COMMIT`；新行库使用 `record_state/store_rows`，非空旧 payload 库必须显式迁往另一个目标。`publish_project` 写临时 SQLite 快照、离开 WAL、校验并同步文件，再以 rename 发布，发布后的不确定失败通过 `StorageError` 交回应用层恢复。这里的 `validate` 检查数据库身份、schema 和完整性，存储层只保存不透明字节，不执行材料、连接、工况或数值工程验证；当前实现要求 POSIX 锁和原子 rename。

## Nastran：受控格式边界

### profiles/nastran/include/qcae/nastran_codec.hpp

[打开源码](../../../profiles/nastran/include/qcae/nastran_codec.hpp)。`NastranCodec` 同时实现格式端口 `IModelCodec` 和能力定义端口 `IProfileProvider`，向应用层提供固定 `ProfileDefinition`、`decode` 与 `encode`。其格式接口仍接收/返回旧 `Model` 表示，应在记录原生主线旁将它读作格式兼容边界；能力定义存在不代表真实求解器已配置或验证。

### profiles/nastran/src/nastran_codec.cpp

[打开源码](../../../profiles/nastran/src/nastran_codec.cpp)。内部 `Reader` 从调用者提供的资源 bundle 解析 INCLUDE、控制段以及 GRID/MAT1/PBAR/CBAR/FORCE/SPC1，检查安全路径、循环/深度/大小、编号、单位和严格支持范围，`decode` 最后调用 `validate_model` 后才返回候选。`encode` 按分析与 profile 校验目标，分离稳定 EntityId、来源编号及导出编号映射，并返回资源文本与组织/名称/词法变化报告；八字符数值不能准确表达时会拒绝。它实现的是受控 SOL 101 文本交换与内存导出预览，没有进程启动、结果解析或真实悬臂梁数值验收。

## 本地传输和桌面客户端

### adapters/transport_local/include/qcae/local_endpoint.hpp

[打开源码](../../../adapters/transport_local/include/qcae/local_endpoint.hpp)。这个小头文件集中定义 1 MiB 帧上限与 `default_endpoint`，在应用本地数据目录下创建仅属主可访问的 `run/engine.sock` 路径。engine、CLI 与桌面客户端共享它；它依赖 QtCore 的路径/权限能力，没有请求分派、业务状态或模型写入逻辑。

### modules/clients/include/qcae/desktop_client.hpp

[打开源码](../../../modules/clients/include/qcae/desktop_client.hpp)。`DesktopClient` 是 Qt `QObject` 客户端，公开连接选项、`start`、异步 `request` 回调以及 ready/error 信号。内部只有 socket、计时器、输入缓冲和待响应表，供桌面窗口持有；这里不拥有文档核心，所有业务请求都跨 IPC。

### modules/clients/src/desktop_client.cpp

[打开源码](../../../modules/clients/src/desktop_client.cpp)。沿 `start → connectNow → onConnected → runtime.handshake → request → consume/deliver` 阅读，即可看到 API 1.1 握手、按换行分割的紧凑 JSON 帧、64 个待响应请求上限以及超时/重连行为。连接失败时可以 detached 启动同目录 engine，并传入 endpoint/workspace；回调延后至 Qt 信号栈退出，避免回调关闭窗口时继续访问对象。连接丢失或写请求超时只报告结果待查询，不自行重放写操作，幂等事实仍由 engine 管理。

## VTK：把显示缓存映射回真实实体

### adapters/rendering_vtk/include/qcae/vtk_view.hpp

[打开源码](../../../adapters/rendering_vtk/include/qcae/vtk_view.hpp)。`VtkView` 接收公共 `RenderPacket` 和已选 EntityId，提供 fit、标准视角、框选模式与穿透开关，并发出 `picked`/`cameraChanged` 信号。`Impl` 隐藏 VTK 细节，窗口只使用 QWidget 接口；命中输出是节点/梁真实身份，不能把 VTK cell ID 当成模型 ID。

### adapters/rendering_vtk/src/vtk_view.cpp

[打开源码](../../../adapters/rendering_vtk/src/vtk_view.cpp)。`Impl::rebuild` 将包中的点、梁线及高亮构造成四个 actor，同时保存 cell 到包索引再到 EntityId 的映射；`setPacket` 和 `setSelectedIds` 都会重建显示数据，当前没有增量几何更新。可见命中结合 `vtkHardwareSelector` 与屏幕投影框，穿透命中用投影点/线测试；相机摘要经 SHA-256 和定时器通知窗口，再由窗口更新 engine 视图版本。此处只画节点与一维梁，不画新几何线、面/体网格或求解结果，也不能给无图形上下文提供等价遮挡选择。

## 桌面工作区与可执行入口

### ui/desktop/include/qcae/desktop.hpp

[打开源码](../../../ui/desktop/include/qcae/desktop.hpp)。这个入口头文件只声明 `run_desktop(argc, argv)`，供桌面 executable 调用。Qt 窗口组装留在实现文件，公开入口不暴露应用服务或文档实例。

### ui/desktop/src/desktop.cpp

[打开源码](../../../ui/desktop/src/desktop.cpp)。`run_desktop` 配置 Qt/VTK 图形环境与启动参数，内部 `DesktopWindow` 组装模型树、属性、历史、文件操作和视口；写操作带 document/epoch/revision，属性修改与 BDF 导入走 `changes.preview → changes.commit`，撤销/保存也调用 engine。`pollCurrent` 每 500 ms 查询修订，变化后重取树、历史和整个 `view.render_data` JSON 包，异步回调检查版本防止旧响应覆盖；树/归属列表和选择取回存在 1000 项显示限制，并非大模型分页 UI 已完整实现。当前属性 GUI 支持节点位置与材料 E，菜单支持工程生命周期/BDF 导入及基本显示，不提供新的几何创建、自动网格、真实求解或求解结果界面。

### apps/cli/main.cpp

[打开源码](../../../apps/cli/main.cpp)。CLI 从 stdin 读取一个不超过 1 MiB 的 JSON 对象，连接或启动 engine、完成握手，再通过 `exchange` 同步发送一次请求并输出 JSON 响应。它只依赖传输和契约，业务与桌面共享同一宿主；成功/业务失败/传输错误分别返回 0/2/3，响应不可用时提示查询原操作或使用原幂等键重试。它没有自己实例化核心、直接写库或运行求解器。

### apps/desktop/main.cpp

[打开源码](../../../apps/desktop/main.cpp)。`main` 唯一工作是转调 `qcae::run_desktop`，实际启动参数、窗口及客户端由 UI 层组装。这样的薄入口把 executable 与显示组件分开，也便于确认桌面启动没有嵌入另一份业务 engine。

## 本章 CMake 文件

这些文件只定义目标与依赖，实际能否装配取决于根构建选项和已配置的 Qt/VTK/SQLite 环境。

| 文件 | 阅读要点与调用关系 |
|---|---|
| [modules/query/CMakeLists.txt](../../../modules/query/CMakeLists.txt) | `qcae_query` 编入三份查询实现，公开依赖 document/contracts；记录原生路径与兼容桥进入同一静态库。 |
| [modules/clients/CMakeLists.txt](../../../modules/clients/CMakeLists.txt) | `qcae_desktop_client` 依赖 transport/contracts，启用 AUTOMOC 处理 QObject；不链接应用核心。 |
| [adapters/storage_sqlite/CMakeLists.txt](../../../adapters/storage_sqlite/CMakeLists.txt) | `qcae_sqlite` 公开契约、私有链接所选 SQLite target，数据库依赖停在适配器内。 |
| [adapters/rendering_vtk/CMakeLists.txt](../../../adapters/rendering_vtk/CMakeLists.txt) | `qcae_vtk_view` 依赖契约、Qt Widgets/OpenGLWidgets 和 VTK，启用 AUTOMOC 与 VTK 模块初始化。 |
| [adapters/transport_local/CMakeLists.txt](../../../adapters/transport_local/CMakeLists.txt) | header-only `qcae_transport_local` 用 INTERFACE 传播 include、C++20、QtCore/Network 与契约依赖。 |
| [profiles/nastran/CMakeLists.txt](../../../profiles/nastran/CMakeLists.txt) | `qcae_nastran` 链接 document/contracts，并由 `generate_profile.py` 根据源码、支持矩阵等生成定义摘要头；摘要生成不等于求解环境验证。 |
| [ui/desktop/CMakeLists.txt](../../../ui/desktop/CMakeLists.txt) | `qcae_desktop_ui` 链接客户端与 VTK 视口，启用 AUTOMOC/VTK 初始化；GUI 与业务实现通过 IPC 分离。 |
| [apps/cli/CMakeLists.txt](../../../apps/cli/CMakeLists.txt) | `qcae-cli` 只链接 transport/contracts，保持无窗口的请求入口。 |
| [apps/desktop/CMakeLists.txt](../../../apps/desktop/CMakeLists.txt) | `qcae-desktop` 链接 desktop UI 并完成 VTK 初始化，业务宿主是另一个 executable。 |

读完可尝试追踪两条路径：一次节点移动从 GUI 的预览/提交到 engine 修订，再回到记录查询和整包渲染；一次保存从应用层编码到 `publish_project`，注意数据库完整性、模型结构校验与真实数值验证分别在哪里发生。记录原生核心的局部变更优势不会自动消除 JSON 帧、整包重建、旧 Model 格式接口及当前小模型桌面的限制。
