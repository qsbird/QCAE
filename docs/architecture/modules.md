# 模块职责、接口与依赖

状态：设计基线1.1；平台骨架已实现C1/C2限定范围，完整模块职责仍为后续目标。本文细化[架构总览](README.md)，以下为逻辑职责。当前源码已按modules/features/adapters/apps/ui等目录归属，实际构建目标见[目标清单](../../modules/targets.json)，实现链路见[源码导读](../engineering/code-walkthrough/README.md)；不要求每个逻辑模块单独生成库。

## 1. 核心模型与数据归属

### domain/document

维护 DocumentSnapshot 与稳定 EntityId。节点坐标、连接等密集数据使用有明确所有权的数组；P0 先以中小模型验证，容器布局允许后续分块优化。内部引用只使用EntityId；原始编号保存在SourceIdentifier，导出编号保存在按目标/命名空间/运行冻结的ExportIdentityMap，见[求解器能力包](solver-profiles.md)。

子域包括：

- Mesh：Node、Element、必要坐标系；连接及局部方向。
- Organization：Part、Assembly、EntitySet、IncludeDocument。
- Physics：Material、Property、Load、Constraint、AnalysisDefinition及受控NativeExtension。
- Context：单位、分析意图、输入来源、假设、场景版本。

装配层次、部件归属、集合成员、文件归属和物理引用分别记录；UI 模型树是投影。共享节点不能为满足树归属而复制。P0 装配先按层级组织，不悄然包含多实例变换系统。

正向引用属于模型事实；反向引用索引可重建，但每次提交发布的模型与反向索引必须一致。域内不变量检查是纯逻辑，不依赖数据库或渲染器。

P0确定的最小对象职责字段（概念字段，不是已冻结的C++结构体）：

| 对象 | 关键字段/关系 |
|---|---|
| Node | EntityId、坐标、必要坐标系引用；不内嵌单值求解器编号 |
| Element | EntityId、拓扑/公共物理类别、节点引用、截面/属性引用、局部方向；求解器实现选项归分析目标 |
| Material / Property | EntityId、公共材料/截面语义、强类型字段和引用；卡片投影由profile/codec负责 |
| Part / Assembly | 稳定身份、名称、部件成员、装配层级；不隐含复制共享节点 |
| EntitySet | 名称、实体类别、明确成员；P0查询保存为成员快照，不默认动态规则集合 |
| IncludeDocument | 来源资源、相对路径基准、包含关系、实体归属；检测循环及重复定义 |
| Load / Constraint | 类型、目标实体/集合、量值与单位、坐标系、必要自由度 |
| AnalysisDefinition | analysis_id、模型和物理设置引用、场景版本、输入/假设来源、TargetBinding |
| TargetBinding / NativeExtension | 不可变ProfileRef、目标实现选择及带schema/引用的专有数据 |
| SourceIdentifier / ExportIdentityMap | 有来源模型和namespace的原编号；按导出/运行冻结的目标编号映射 |
| DocumentSnapshot | DocumentId/Epoch/Revision、ContentStateId、实体表/数组、组织关系、分析上下文 |

这些对象组成可查询的关系图；持久化时才映射为SQLite表/块数据。数值字段的合法性由公共物理类型及已绑定目标/场景规则共同定义，不能用任意字符串属性包替代核心物理类型。

### contracts 与 types

types：稳定ID、Quantity、Revision、ContentStateId、ProfileRef、基础错误类型。

contracts：命令/查询/任务 DTO、批量数据描述、能力定义、诊断与事件。使用标准 C++ 类型；JSON/MCP只是边缘编码。边界中的大整数身份使用字符串表达。

### query

输入 ReadView、QuerySpec、SelectionScope；输出 SelectionHandle、计数、分页实体/摘要。支持 P0 类型/ID/名称/归属/属性等组合、集合交并差、必要空间条件。

SelectionHandle 绑定 document_id、document_epoch、revision、候选范围；不是可修改的实体指针。显式区分隐藏、被遮挡、穿透、完全包含/相交、反选候选域。

普通查询读取领域索引。精确可见性选择通过 IVisibilityQueryPort 请求显式视图的可见 ID 集，再与业务候选集合相交。GUI 可以用 VTK 先取得候选 ID，但提交前必须由 engine 校验版本、实体和范围。无界面的同类图形选择使用同一显式相机/视图描述；缺图形能力时返回能力不可用，不能悄悄切换成穿透选择。

### commands 与 history

commands 将领域操作转换为 PreparedChange：DocumentId、DocumentEpoch、基础Revision、目标集、before/after、组织/引用变化、资源预估及影响说明。P0 的操作使用显式命令，不提供任意表达式写模型。

history 管理线性逻辑编辑与游标；undo/redo 产生补偿变更，交给同一个提交协调器。正常新编辑截断 redo 分支；事务审计与幂等结果保留策略独立，不能随 redo 被删而消失。

修改共享属性与为选中对象分配新属性是不同命令。一次复合操作可成为一个可撤销事务；其子修改不能各自提前发布。

### analysis

维护 AnalysisIntent、AnalysisSetup、InputEvidence、Assumption、场景版本及输入完整性。悬臂梁场景将受控输入转换为核心支持的命令计划，并提供缺参问题、检查目标与解释用事实。

不调用大模型、不把提示词当物理规则、不直接写模型。外部 AI 负责自然语言，analysis 模块负责可验证的场景语义和适用范围。

### solver_profiles

纯C++注册/解析逻辑；定义ProfileDefinition、能力与扩展schema版本，按AnalysisDefinition的TargetBinding解析不可变ProfileRef。它不依赖具体Nastran实现；IProfileProvider由ports定义，宿主静态注册profile_nastran。

公共字段、专有字段及派生投影只有一个权威来源。目标字段、引用、校验和复杂编辑行为在GUI/AI共用服务中执行。P0只提供一个Nastran profile，目标版本未配置时明确不可用；不做全局当前求解器或动态插件体系。

### validation

维护两个层次：提交前必要不变量由 domain 提供；可独立运行的模型/分析/结果检查由 validation 编排规则函数，生成带版本的 Issue 和 CheckEvidence。

检查结果没有权力直接修复文档；修复建议转为 commands。结果包括规则/版本、对象、严重度、实际/期望、容差和证据引用。用户能区分未检查、通过、失败、过期。

## 2. 应用协调与任务

### application

公开 ProjectService、QueryService、ChangeService、AnalysisService、ValidationService、JobService、ViewService。接口名表示职责，不要求立即拆成多个对象框架。

负责：文档生命周期、连接上下文、工具能力、预览注册、请求授权上下文、幂等协调、串行提交以及业务事件。**只有这里的 DocumentCommitCoordinator 能发布新文档状态。**

创建/打开/关闭的宿主级意图由工作区登记簿保存，使用同一storage_sqlite适配提供的登记能力；文档建立前即可按可信调用者和幂等键查回结果。它只保存生命周期事实和工作库位置，不成为第二份模型权威，也不要求新增服务进程。

所有入口使用同一 Dispatcher。ChangeService 不直接依赖 SQLite 实现，而通过 IWorkspaceStore 提交已准备变更。AnalysisService 调用分析场景、任务服务和求解端口；避免 jobs 与 application 双向编译依赖。

### jobs

维护任务记录、状态转换、取消令牌、并发/内存/临时磁盘配额，以及可恢复的运行标识。通过 IExecutorPort 提交工作函数；工作函数读取冻结快照，返回结果或 PreparedChange。

jobs 不直接写领域模型。任务完成后应用层同时校验DocumentId、DocumentEpoch和Revision是否仍可提交；任一冲突返回明确状态。求解作业可以完成旧快照计算，但结果明确属于原输入及原工作库。关闭/放弃后的原上下文保留到任务终态，不能将旧结果挂到新打开的文档。

资源控制先是可计量的额度和有限并发；非合作式第三方算法的硬中止/硬内存上限依赖进程隔离和平台能力，不虚称单个计数器能保证。

### ports

只为实际外部边界定义接口：IProfileProvider、IWorkspaceStore、IProjectSnapshotStore、IModelCodec、ISolverRunner、IResultReader、IArtifactStore、IExecutorPort、IVisibilityQueryPort、ISnapshotRenderer。

方法形状以实际用例决定。不要为每个函数新增抽象。适配器依赖 ports 及相关类型，核心不 include 适配器；运行宿主负责注入。

| 端口 | 输入→输出方向 |
|---|---|
| IWorkspaceStore | PreparedCommit→持久化CommitReceipt；恢复请求→已提交状态/历史/操作结果 |
| IProjectSnapshotStore | 固定内容与save intent→临时工程及快照清单；发布/核对→保存结果 |
| IProfileProvider | 不可变ProfileDefinition、能力/扩展schema与目标校验；P0为静态Nastran提供者 |
| IModelCodec | 输入资源＋来源目标→候选模型/来源映射/报告；冻结分析＋ProfileRef→目标格式ArtifactPlan/导出映射/报告；P0为BDF/INCLUDE |
| ISolverRunner | 已核验目标产物＋兼容RunConfiguration→进程运行句柄；查询/取消/核实→真实状态 |
| IResultReader | 固定运行/编号映射与结果资源→带量纲、位置、坐标基、工况/采样及来源的ResultBundle；P0只读位移/反力 |
| IArtifactStore | 临时资源→发布清单；资源ID→受控读取及可用性 |
| IVisibilityQueryPort | RenderPacket与ViewSpec/候选范围→精确可见实体ID及版本 |
| ISnapshotRenderer | RenderPacket与ViewSpec→图片资源及模型/视图版本 |

PreparedCommit包括模型差量、历史、幂等、修订和相关任务结果，避免存储适配器只能分多次写入这些数据。RenderPacket定义于contracts，包含模型/打开代号/修订、坐标与连接数据块、真实EntityId映射、显示属性和局部更新标识；ViewSpec包含相机、投影、裁切、显隐及ViewRevision。图形进程只接收显示所需数据，不持有领域对象。

## 3. 适配器与入口

| 计划模块 | 实现内容 | 禁止承担的职责 |
|---|---|---|
| adapters/storage_sqlite | 工作库事务、工程快照、save intent、恢复读取、历史/幂等记录 | 不决定领域命令是否合法 |
| adapters/artifacts_local | 运行目录、临时产物、发布清单、hash与资源读取 | 不凭文件存在就判定求解成功 |
| profiles/nastran | 提供一个版本化能力包、字段/规则/映射描述，由宿主静态注册 | 不改核心公共类型，不直接提交模型 |
| adapters/nastran_codec | 支持子集读写、INCLUDE解析、编号映射、文件/行诊断 | 不在解析中直接改活动模型 |
| adapters/solver_local | 固定可执行配置、进程启动/取消、日志与退出信息 | 不执行AI给出的任意shell串 |
| adapters/result_reader | 位移/反力读取及输入运行关联 | 不输出无依据的工程结论 |
| adapters/runtime_local | 任务执行、进程能力、时间/配额观测 | 不再建一套任务状态权威 |
| adapters/graphics_local | 转发离屏出图/可见性请求到图像辅助进程 | 不在engine创建VTK图形上下文 |
| adapters/ipc_local | 帧边界、JSON控制消息、二进制块、版本协商、连接事件 | 不在传输层实现载荷/属性业务规则 |
| visualization/render_data | engine侧纯C++ producer：ReadView转换为contracts中的RenderPacket、实体映射、块描述 | GUI/VTK/离屏进程不得链接此producer或domain |
| visualization/vtk | 仅消费RenderPacket/ViewSpec的VTK场景、拾取、高亮、相机及图像/可见性渲染 | 不读取domain、不链接render_data，不写文档或定义默认选择域 |
| clients/sdk | GUI/CLI使用的契约调用、重连、事件与资源访问 | 不对超时修改自动换幂等键重发 |
| clients/desktop | Qt工作区、ViewModel、可见性/相机、表单草稿 | 不持有权威实体/事务/数据库连接 |
| clients/mcp | MCP工具说明与参数/结果转换，转发到engine | 不实例化领域核心或绕过事务 |
| apps/engine | 组装服务、适配器、线程和本地端点 | 不把宿主生命周期写入domain |

## 4. 数据谁负责

| 状态 | 唯一负责者 | 其他模块如何使用 |
|---|---|---|
| 当前已提交模型、修订 | engine应用层；workingDB持久化 | 只读快照/查询/事件 |
| 部件、集合、INCLUDE、物理引用 | domain文档 | 多种组织投影 |
| 预览与历史 | engine的ChangeService/协调器 | UI与AI共享查询和操作 |
| 任务与输入快照 | engine的JobService/AnalysisService | 客户端订阅或查询 |
| GUI相机、临时高亮、未提交表单 | 对应客户端视图会话 | 需要共享时发布显式view_session_id |
| 命名集合 | domain文档 | 持久化、可撤销 |
| 临时选择句柄 | engine查询会话 | 带版本、有限寿命，不混为持久集合 |
| VTK缓存与反向渲染ID映射 | 显示适配器 | 可丢弃重建 |
| 项目已保存状态 | ProjectService + 快照存储 | 查询clean/dirty与保存身份 |
| 求解文件和图像 | ArtifactStore | 资源ID和清单，不将裸路径当权限 |

“AI操作我当前选中的对象”需要显式关联GUI视图会话并读取其选择；没有关联时不能读取一个全局隐式选择。AI和GUI可有独立视图，但始终共享文档。

## 5. 依赖规则

基础类型无外部依赖；domain与contracts只依赖基础类型。query、commands、history、analysis、solver_profiles、validation、jobs 是纯C++业务模块。application汇总它们及ports；适配器实现ports；apps只做组装。

GUI依赖contracts、client SDK和消费端VTK显示适配，不链接application/domain或engine侧render_data。MCP依赖契约/传输桥而不链接领域核心；若MCP SDK采用另一语言，复用协议契约而不强求链接同一C++客户端二进制。测试可直接链接核心服务，以最小依赖验证业务；进程级测试另覆盖真实IPC。

依赖清单把逻辑组件作为节点，先做静态DAG检查。真正实施时再依据目录体量合并CMake target；纯核心目标必须能在禁用Qt、VTK、SQLite、MCP的构建配置下编译和运行内存适配测试。

## 6. 计划目录

```text
src/
  types/  contracts/  domain/
  query/  commands/  history/  analysis/  solver_profiles/  validation/  jobs/
  application/  ports/
  profiles/nastran/
  adapters/
    storage_sqlite/  artifacts_local/  nastran_codec/
    solver_local/  result_reader/  runtime_local/  ipc_local/  graphics_local/
  visualization/
    render_data/  vtk/
  clients/
    sdk/  desktop/  mcp/  cli/
  apps/
    engine/  desktop/  mcp/  snapshot/
scenarios/cantilever/
tests/
  unit/  contracts/  persistence/  integration/  ui/  ai/  e2e/
```

该目录是后续实施布局，不在本轮创建空壳源代码。核心接口与一致性规则优先，避免先造完整目录再补行为。


## 设计基线1.1吸收的接口规则

OperationDescriptor由contracts定义、application登记实际处理器，向GUI/CLI/MCP提供一致参数、单位、错误和能力描述；不为每个入口手工复制业务语义。

ImportReport/ExportReport属于格式适配结果，由validation/application决定是否允许提交或发布；未知/丢失语义不能只记录日志后返回成功。

ChangeImpact在PreparedChange中记录，沿实际引用传播到分析输入、诊断和显示缓存。只改变相机不触发物理重算，载荷引用的集合成员变化必须使相关分析结果过期。


## 1.1 补充规则

目标能力、编号映射、公共/专有字段所有权、绑定与转换、版本及结果语义以[求解器能力包设计](solver-profiles.md)为准。P0只实现一个Nastran包；共享对象变更必须诊断全部受影响分析，不以当前GUI分析为唯一范围。
