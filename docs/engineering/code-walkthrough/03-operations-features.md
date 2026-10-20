# 03：操作契约、参数工具与业务功能

这一章把“增加一个功能”拆为输入契约、纯参数处理、候选记录变更和宿主注册。同步功能返回候选再交给应用提交；后台功能返回冻结快照上的计算结果，再经任务桥回到同一应用。当前只有七项 typed 模型操作，不能推导为完整前处理工具集。

## 两份 schema 与生成结果

### schemas/entities/entities.json

[打开配置](../../../schemas/entities/entities.json)。定义当前 14 类强类型记录的稳定 type ID、field ID、版本、字段形式、单位、引用允许目标和局部规则，生成 `modules/document/include/qcae/records.hpp`。它定义持久化实体结构，不能任意重用既有 ID；跨记录规则仍在 `records_rules.cpp`。新增类型能复用记录提交/历史/存储，但复杂拓扑、查询、显示和 profile 的语义不会凭 schema 自动生成。

### schemas/operations/*.json

[打开目录](../../../schemas/operations/)。按功能分文件定义操作的 ID/版本、输入字段、单位与上下文要求：功能操作在与功能目录同名的文件中（如 [materials.json](../../../schemas/operations/materials.json)、[mesh_editing.json](../../../schemas/operations/mesh_editing.json)），宿主与适配器操作在 `host_*.json`、`solver.json` 等文件中；全部文件合并生成构建目录中的 `qcae/operation_inputs.hpp`，文件划分不影响生成结果。操作输入与持久化实体是不同结构，例如移动节点用实体 ID 与位置，不能让客户端提交任意未验证的记录字节。生成器支持现有受控字段形式及 `fields: []` 的无参数输入，后者仍只接受空对象；所有 API 操作尚未统一迁到这个目录。

## operations：描述、解码与调度机制

### modules/operations/include/qcae/operation_registry.hpp

[打开源码](../../../modules/operations/include/qcae/operation_registry.hpp)。先看中立 `Value`、`OperationContext`、`OperationDefinition`/`Descriptor`，再看 `InputTraits` 与 `register_typed`：注册的描述必须与生成输入契约一致，解码成功后才调用处理器。上下文包含可选请求版本和预期 profile；描述规定必需项，可用性由实际处理器产生。`Value` 是传输值，不是文档权威数据，领域合法性仍由功能处理器和记录校验完成。`declare_unavailable` 明确发布无处理器的能力。

### modules/operations/src/operation_registry.cpp

[打开源码](../../../modules/operations/src/operation_registry.cpp)。实现对象字段白名单、必填、有限数、ID、数量/单位和向量等解码，以及重复注册和上下文核验。请求版本必须匹配已安装定义，显式参数与上下文版本不能矛盾；省略版本使用已安装版本，并将解析结果传给处理器。注册表检查 profile 必需字段是否存在；目标匹配由应用准备回调核验，保留事实的幂等重放先于当前目标规则执行。

### modules/operations/src/canonical_value.cpp

[打开源码](../../../modules/operations/src/canonical_value.cpp)。将中立值确定性编码，供功能构造归一化请求签名，避免对象字段排列等无关差异影响幂等比较。它不对工程实体分配身份，也不自己创建幂等事实；签名交给共享应用使用。

### modules/operations/src/change_receipt_value.cpp

[打开源码](../../../modules/operations/src/change_receipt_value.cpp)。把 `ChangeReceipt` 投影成中立返回值，统一事务 ID、原提交/当前修订、重放标志和 primary entity。修订以十进制字符串输出，避免 JSON 数字表达大整数时损失精度；外层适配器再把它变成 Qt JSON。

## parameters：可复用的纯数值工具

### modules/parameters/include/qcae/quantities.hpp

[打开源码](../../../modules/parameters/include/qcae/quantities.hpp)。定义单位元数据、量纲转换、正交坐标架验证/局部全局坐标转换，以及带明确越界策略的一维标量表。这里已经有 `CoordinateFrame` 和 `ScalarTable1D` 的计算接口，尚未将它们完整接为工程实体、操作与 GUI 编辑器；理解“计算工具已有”和“产品功能贯通”之间的区别。

### modules/parameters/src/quantities.cpp

[打开源码](../../../modules/parameters/src/quantities.cpp)。实现受控单位表、有限值与维度检查、坐标变换和归一化表的线性插值，表求值明确选择 reject 或 clamp。材料和网格编辑同时调用这些纯工具以统一单位语义；此文件不持有文档、历史、数据库或界面状态。

## feature_support：功能操作的共享步骤

### modules/feature_support/include/qcae/feature_operation.hpp

[打开源码](../../../modules/feature_support/include/qcae/feature_operation.hpp)。新增写操作按固定步骤编写：`canonical_copy` 复制输入，`normalize` 换算单位，`check_record_fields` 复用生成的字段规则，`record_plan` 计算签名并在 `PlanEdit` 中描述记录修改、返回 `ChangeSummary`，`register_plan` 把plan接到唯一的 `RecordApplication.execute`。复制计量全部在这些步骤和按schema生成的 `InputTraits::observe_copy/moved_bytes` 中完成，功能代码不写ledger；向记录字段复制字符串等堆数据时用 `copy_into`，标量直接赋值。完整步骤与脚手架见[新增操作、字段与实体](../adding-operations.md)。

## materials：材料与截面

### features/materials/include/qcae/material_operations.hpp

[打开源码](../../../features/materials/include/qcae/material_operations.hpp)。公开创建材料、修改 E、创建梁截面的 plan 工厂与处理器注册函数，输入来自生成的 typed 契约。共享的 `OperationPlan` 包含归一化签名和 `RecordPrepare`，把业务准备与通用提交分开；使用者不能通过 plan 绕过文档版本检查。

### features/materials/src/material_operations.cpp

[打开源码](../../../features/materials/src/material_operations.cpp)。完全由上述共享步骤组成，是新增写操作的范例：先转换压力、面积和惯性矩单位，再在 `PlanEdit` 中构造强类型 Material/BeamSection 候选；截面引用材料的合法性由记录规则核验。注册函数将三项操作接到同一个 `RecordApplication.execute`，得到共享回执；材料取值检查不由 GUI 或 JSON 层各做一遍。

## mesh_editing：已有实体的局部编辑

### features/mesh_editing/include/qcae/mesh_editing_operations.hpp

[打开源码](../../../features/mesh_editing/include/qcae/mesh_editing_operations.hpp)。公开节点移动与批量梁截面分配的 plan 工厂和注册入口。与材料模块采用同样的准备/提交约定，但各功能保留自己的工程语义；这不是把任意实体写入权限开放给客户端。

### features/mesh_editing/src/mesh_editing_operations.cpp

[打开源码](../../../features/mesh_editing/src/mesh_editing_operations.cpp)。节点位置归一化后仅更新指定 Node，梁 ID 列表先拒绝重复项，再排序形成稳定签名并准备整批截面引用修改。候选需要满足长度、方向、引用等记录不变量，失败时不发布半批修改；两项操作都通过共享应用执行，所以 undo/redo 不需要各功能再写一套恢复算法。

## geometry：最小几何与派生失效

### features/geometry/include/qcae/geometry_features.hpp

[打开源码](../../../features/geometry/include/qcae/geometry_features.hpp)。`LineGeometryInput` 只有 mm 单位的起止点，公开创建线、修改终点及对应签名函数，返回统一 `RecordPrepare`。创建、求值和终点修改都已登记为 typed 操作（见下面的 `geometry_operations.cpp`）；终点修改尚无 GUI 工具。

### features/geometry/src/geometry_features.cpp

[打开源码](../../../features/geometry/src/geometry_features.cpp)。创建函数在应用提供的 allocator 下取得身份，把 `GeometryLine` 放进私有编辑会话；修改终点则同时增加 geometry revision，并把引用该几何的 Mesh 标为 stale。这个复合变更示范了几何变化如何影响派生网格，全部一起提交/撤销；它没有 CAD 内核，也没有提供曲面或一般几何拓扑。

### features/geometry/include/qcae/geometry_operations.hpp

[打开源码](../../../features/geometry/include/qcae/geometry_operations.hpp)。只声明 `register_handlers`：把上面的候选处理器登记为 `geometry.create_line`、`geometry.evaluate_line` 和 `geometry.move_endpoint` 三项 typed 操作。

### features/geometry/src/geometry_operations.cpp

[打开源码](../../../features/geometry/src/geometry_operations.cpp)。每项操作一个小函数。两个写操作把 typed 输入换成线或终点，用各自的签名函数经 `RecordApplication::execute` 提交，返回变更回执；求值在当前快照上算出点位，并带回所读的修订。几何沿用自己的签名函数而不是 `record_plan`，因此登记位置迁移前已提交的重试仍得到原回执。

## mesh_generation：真正的后台任务

### features/mesh_generation/include/qcae/line_mesh_task.hpp

[打开源码](../../../features/mesh_generation/include/qcae/line_mesh_task.hpp)。定义线网格输入、任务关联的 mesh identity 和 `line_mesh_task` 工厂，使用冻结 `RecordSnapshot`、caller、profile 与幂等键创建任务请求。核心输入允许可选截面，当前 typed 生成操作不传该项，而是在生成后用 `beam.assign_section` 分配。

### features/mesh_generation/src/line_mesh_task.cpp

[打开源码](../../../features/mesh_generation/src/line_mesh_task.cpp)。work 回调在捕获的不可变视图上解析几何，生成 task ID 派生的 Mesh、节点和 Line2 梁，循环中报告进度并检查取消。N 段产生 N+1 个节点、N 个梁和 1 个 Mesh，候选差量总数为 `2*N+2`；10 段即 22 条记录。最终返回 `RecordTaskPayload`，worker 不发布模型；原提交修订是否仍有效、成功任务事实与模型是否同批持久化，由运行时桥和应用处理。

typed 输入要求段数为正 uint32，线网格任务另将其限制为 1—100000，还要受到应用实体及文档总记录配额限制，不能理解为默认配置能实际生成 100000 段。它是内置均匀一维生成器，尚未接 Gmsh/Netgen，也不是通用自动网格引擎。

### features/mesh_generation/include/qcae/mesh_generation_operations.hpp

[打开源码](../../../features/mesh_generation/include/qcae/mesh_generation_operations.hpp)。声明 `register_handlers` 与 `StartTask` 回调。功能只构造 `TaskRequest`；任务服务和任务状态的 wire 格式属于宿主，由宿主以回调传入，所以功能不依赖 IPC 适配层。

### features/mesh_generation/src/mesh_generation_operations.cpp

[打开源码](../../../features/mesh_generation/src/mesh_generation_operations.cpp)。`mesh.generate_line` 与 `mesh.regenerate_line` 共用 `start_from_snapshot`：读当前快照、构造任务请求、写入调用方的期望修订，再交给 `StartTask`。任务去重比较原始提交上下文，所以请求带调用方的期望修订，而不是快照修订。

## 本章构建文件

| 文件 | 作用与依赖 |
|---|---|
| [modules/operations/CMakeLists.txt](../../../modules/operations/CMakeLists.txt) | 生成 typed 输入并构建 `qcae_operations`，公开依赖 foundation/contracts。`GLOB CONFIGURE_DEPENDS` 跟踪全部操作 JSON；文件成员清单也是生成依赖，新增、修改或删除 schema 都触发增量生成。 |
| [modules/parameters/CMakeLists.txt](../../../modules/parameters/CMakeLists.txt) | 构建纯参数工具，链接 foundation/contracts，不依赖应用、Qt 或具体求解器。 |
| [features/materials/CMakeLists.txt](../../../features/materials/CMakeLists.txt) | 构建材料特性，链接 application/operations/parameters，定义三项业务处理器。 |
| [features/mesh_editing/CMakeLists.txt](../../../features/mesh_editing/CMakeLists.txt) | 构建局部网格编辑特性，依赖结构与材料特性一致，通用提交留在 application。 |
| [features/geometry/CMakeLists.txt](../../../features/geometry/CMakeLists.txt) | 构建几何候选处理器及其 typed 登记，公开依赖 application 与 feature_support。 |
| [features/mesh_generation/CMakeLists.txt](../../../features/mesh_generation/CMakeLists.txt) | 构建线网格任务及其 typed 登记，依赖独立 task/application 桥与 feature_support，并登记 geometry_mesh 测试；不让通用应用反向依赖网格生成。 |

可以沿 `geometry.create_line → mesh.generate_line → beam.assign_section → node.move` 阅读一次完整功能链。新增已有类型上的操作复用候选/提交模式，并从静态贡献注册；新增面/体单元仍须补拓扑、查询/显示和格式语义。

把记录转换成其他表示的消费者各自声明理解的记录类型：分析输入与检查用 `analysis_record_types()`，Model投影（Nastran导出与导出预览）用 `model_projection_record_types()`。未列出的类型由 `unsupported_record_type` 统一返回 `unsupported_capability`，不会被静默丢弃。新增实体类型后，[`record_consumer_tests.cpp`](../../../tests/record_consumer_tests.cpp) 会要求逐个消费者归类：已支持则加入该消费者的列表，否则加入测试中的拒绝集合。
