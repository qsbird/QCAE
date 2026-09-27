# ArcherprePublic 架构对照与 QCAE 对齐目标

设计日期：2026-09-26；状态同步：2026-09-27。用户已同意的架构目标已推进至参考库存冻结及C1/C2限定实现，完整对齐尚未验收，证据见[C2记录](../engineering/c2-validation.md)。响应用户要求：QCAE至少在架构模块设计覆盖上对齐参考项目，并在若干平台机制上建立更严格的可验证保证。

参考目录为 `/Users/qs/Documents/ArcherprePublic`。该本地目录无可用Git元数据，本次依据实际源码/头文件、构建文件和开发文档检查，未编译或运行Archer，不对其运行性能、商业成熟度作判断。排除了ArcherPre3rd供应商实现；受忽略规则影响的template目录用显式路径读取。引用的行号/目录对应此次本地副本。

实施通过条件见[SK-01—14检查点](../baseline/skeleton-acceptance.md)。

## 1. 结论与“达到”的含义

原[工程化提案](engineering-skeleton-proposal.md)解决扩展机制和小场景核心链路，但不足以直接宣称覆盖Archer的全部平台架构。其交互工具生命周期、完整模板贡献、检查与修复、分析工况、结果组织、参数/坐标和产品配置等责任划分还需补齐。

可以将下列目标作为实施基线：

1. **架构覆盖对齐**：参考功能域在QCAE中有明确负责模块、数据权威、公开输入输出、依赖、扩展方式、状态/失败语义、存储版本和验证方法。
2. **骨架可执行**：核心模块进入真实构建图和公共链路，代表能力通过场景执行；未实现能力明确不可用，不能靠空类/空目录计为完成。
3. **指定机制改进**：领域隔离、统一写入、持久历史、版本化工具调用、扩展触碰范围和局部数据更新有检查证据。
4. **产品成熟度另行验收**：不由上述目标推出已经具有参考项目全部CAD/网格算法、模板数量、后处理能力或长期验证积累。

“全部”的分母必须来自参考库存，不能只挑QCAE已经具备的模块。以下20个功能域是本轮检查得到的最低覆盖清单；R0还需对参考项目各自有模块/模板分支登记归属和缺项，未审计或未映射项不能标为已对齐。

## 2. 架构覆盖矩阵

“原提案”列描述设计责任是否明确，不代表QCAE代码已实现。

| ID | 参考架构域与证据入口（相对参考根目录） | 原提案 | QCAE负责模块与必须补充的设计 |
|---|---|---|---|
| AP-01 | 应用/模块注册：`ArcherModelcore/src/mmodule.cpp`、`public/mappimp.h` | 明确 | application：宿主组装、会话、模块注册与能力发现；依赖方向及实例生命周期 |
| AP-02 | 实体/属性/引用：`ArcherModelcore/public/mentitymanager.h` | 明确但需细化 | document/schema：实体目录、字段、引用、单位、赋值优先级、删除/合并策略；机械遍历单一来源 |
| AP-03 | CAD内核与拓扑：`ArcherVCAD/include/ar_ivcad.h`、`src/occ/ar_ivcadocc.h` | 部分 | geometry：体/面/边/点、邻接与方向、曲线/曲面求值、投影、离散化、平台ID与内核句柄映射 |
| AP-04 | 几何—网格关联：`ArcherMeshLib/inc/ar_meshdata2.h`、`include/ar_brep.h` | 部分 | mesh关联记录：几何修订、支撑实体、边面替换历史、失效与重映射；关联随修改原子提交 |
| AP-05 | 网格引擎：`ArcherMeshLib/include/IMeshEngine.h`、`mesher/ARMeshEngine.cxx` | 部分 | meshing adapter：能力/参数、输入冻结、尺寸控制、生成、结果抽取、质量检查、取消和资源释放 |
| AP-06 | 网格存储与拓扑：`ArcherMeshLib/inc/ar_meshdatabase.hxx`、`include/ar_page_array.hxx` | 部分 | mesh/document：拓扑描述、节点顺序/阶次、连接、ID与存储位置分离、分块、反向邻接及缓存失效 |
| AP-07 | 网格编辑/变形：`ArcherPre/archer-py/model/mmorph.h`、`gui/guipanel/meshedit/` | 归在mesh，未展开 | mesh features：选区、编辑范围、约束、候选/预览、质量影响、重编号/替换映射；复杂算法按提供者扩展 |
| AP-08 | 装配与多视角组织：`ar_ivcad.h`中的AssemblyData、`ArcherModelcore/src/mmodeltreeinfo.cpp` | 部分 | organization：定义/出现位置、变换归属、Part/Mesh关系、集合与INCLUDE；树只是投影。重复实例实现可后置，责任必须明确 |
| AP-09 | 材料/截面/场/坐标：`ArcherPre/template/ArcherPreNastran/model/nastran_mmaterialmanager.h`、`nastran_mcoordinatesmanager.h`、`nastran_mfieldsmanager.h` | 部分 | physics＋parameters：材料/截面、标量/表格/场、坐标系、单位/量纲、引用与求值；profile拥有专有表达 |
| AP-10 | 完整模板包：`TemplateDevelop.md`、`setting_ar.cmake`、Nastran模板的model/modelrender/gui/test | 部分 | capability package：同一版本下声明领域扩展、操作、映射、检查、树/面板/编辑器/渲染贡献、测试和依赖；静态注册即可 |
| AP-11 | 分析配置与工况：`ArcherPre/gui/jobmanager/jobmanager.h`、Nastran的job/loadcase/loadstep managers | 部分 | analysis：分析定义、工况/步骤、载荷约束引用、求解控制、输出请求和目标绑定；与后台Task状态分离 |
| AP-12 | 操作与历史：`ArcherModelcore/public/command.h`、`command/commandmanager.cpp`、`src/mundomanager.cpp` | 明确 | operations/history：显式参数、preview/commit、幂等、ChangeSet、几何/网格/组织复合撤销和恢复 |
| AP-13 | 交互与拾取：`gui/nx_base/templateribbonpanelmanager.h`、`archer-py/model/mnodecreator.cpp` | 部分 | interaction：工具进入/退出、拾取阶段、过滤、预览、apply/cancel和清理；ToolSession与已提交文档分离 |
| AP-14 | 检查与问题修复：`gui/guipanel/checkitem.h`、`checkmanager2d.h`、`model/mfeaturechecker.h` | 缺独立设计 | validation：RuleCatalog、执行范围、版本化Issue、实体定位、容差/证据、失效及修复计划；修复必须走操作/事务 |
| AP-15 | 工程与格式交换：`ArcherModelcore/src/mmodel.cpp`、`public/mentitymanager.h`的archive/neutral接口 | 明确但需细化 | project＋exchange：工程快照、格式codec、中性公共语义、导入合并/编号、损失报告、资源清单、版本迁移 |
| AP-16 | 渲染与显示组织：`ArcherPre/archer-py/modelrender/`、各模板modelrender | 部分 | visualization：场景图层、几何/网格/辅助符号、显隐/裁切/颜色/标注、ID映射、局部更新、可替换消费端 |
| AP-17 | 工作区/表格/属性界面：`gui/modeltree/treeitem.*`、`gui/guipanel/referencedialog.cpp`、`gui/nx_base/toolpanelbase.h` | 部分 | ui：Shell、树投影、属性/表格编辑器、引用选择、工具面板、任务/问题面板；菜单与布局贡献、快捷键/偏好恢复 |
| AP-18 | SDK/脚本/扩展入口：`ArcherPreConsole/main.cpp`、`ArcherModelcore/src/mappwrapperpy.cpp`、`ArcherPythonInterpreter/pyinterpreter.cpp`、`gui/archerpygui/wrappergui.cpp`、`cloud/server_rendering.proto` | 部分 | clients/extensions：GUI/CLI/脚本/AI共用服务；bindings、贡献契约、输出/日志和资源作用域；传输可替换，远程实现仍后置 |
| AP-19 | 结果与后处理组织：`template/ArcherPrePostProcessing/model/post_variablemanager.h`、`post_framemanager.h`及HDF5格式文档 | 缺独立设计 | results：结果集/字段/分量/位置/坐标/帧、冻结输入及编号映射、结果读取器、派生结果、渲染/图表接口；完整后处理产品后置 |
| AP-20 | 运行支撑与配置交付：`ArcherModelcore/src/mconfigmanager.cpp`、`gui/utils/`、`cmakes/`、根构建文件、`ArcherPre/tests/` | 部分 | runtime/platform-support：后台任务、进度/取消、事件/日志、配置层次、诊断、资源路径、打包/依赖锁定、测试夹具和构建变体 |

AP-20不声称参考项目具有与QCAE拟议设计相同的持久Task状态机；参考中的GUI JobManager主要体现分析配置组织，不应据名称当作运行调度器。商业授权等产品策略不要求个人项目复刻；其可选策略边界与部署配置仍需在库存中记录，不能混入领域核心。

## 3. 必须保留的参考优势

### 模板是完整的能力包

已检查的Nastran模板静态注册模型managers，构建脚本同时收集model、modelrender、GUI面板、模型树和测试。QCAE的Profile不能只成为一个格式解析类或字段描述文件。设计上应提供：

```text
CapabilityPackage
  identity + dependencies + compatibility
  entity/field extensions + semantic rules
  operations + validation rules + exchange adapters
  UI contributions: tree, editors, panels, menu/actions
  render projections / symbols
  fixtures + contract tests + extension example
```

包中的核心部分不依赖Qt；UI贡献可作为独立可选目标，由宿主组装。同一Nastran包无需多个求解器即可验证这条链。

### 交互工具有完整生命周期

参考的创建节点动作和Ribbon工具接口体现了预览、选择阶段、apply、leave等职责。QCAE需明确ToolSession的enter → collect/pick → preview → apply/cancel → leave，规定相机/选区变化、模型版本变化和异常退出时的行为。工具退出清理草稿/预览，不能清理已提交模型。

### 检查、结果和配置各有归属

检查结果不是一个错误字符串；应能定位实体、标明输入版本、展示容差和建议修复。结果字段也不能只是“节点ID→数值”：位置、分量、坐标、工况/帧及来源属于数据契约。偏好配置、运行环境与工程语义则各自有保存和修改边界。

这些需要独立职责和最小可执行实现，不能仅在`features/analysis`或`runtime`目录下面留一句将来补充。

## 4. 指定的改进目标及证据

以下是QCAE应争取更严格的保证，不是当前已全面超过Archer的声明。本次没有穷尽参考项目的所有实现或测试。

| 改进目标 | 参考中可观察的结构/局限 | QCAE验收证据 |
|---|---|---|
| 依赖隔离 | GUI和ArcherModel构建包含模型、渲染、Python及多种外部库；另有BUILD_WITHOUT_VIEWMODEL分支 | 纯核心同时关闭Qt/VTK/SQLite；GUI/AI不能链接领域实现；检查实际目标图和私有头包含 |
| 统一机器调用契约 | `mModule`动作与帮助、JSON API、Python wrapper均有实际实现；参数和编辑元数据在多处代码构造 | 操作契约、能力发现、SDK和工具说明同源；GUI/CLI/脚本调用相同版本化handler |
| 一致修改与恢复 | 参考命令/undo接口存在，部分几何undo委托内核；所查路径未建立与QCAE相同的持久事务保证 | 几何资源、网格块、引用、历史、幂等在声明边界内一致；提交/保存故障注入、旧请求和重启测试 |
| 生成结果的真实性 | `ArcherMeshLib/mesher/ARMeshEngine.cxx:141`所查generate路径调用生成后直接return true | 后端失败、空/不完整网格、无效关联不能作为成功提交；有明确诊断与原状态保持测试 |
| 扩展成本可测 | 参考已有模板包和可扩展编辑器，不能忽略其扩展能力 | EXT字段/命令/拓扑扩展记录触碰文件；通用事务、历史算法、存储提交、传输不需增业务分支 |
| 数据量与局部修改 | 参考包含paged-array实现、ID/index分离及派生连接数据结构；本次未证明所有生产网格路径使用分页容器，不能凭QCAE的分块设想宣称性能更好 | 实测局部修改的复制/写入/传输量；与声明块大小和影响范围一致，再单独做性能基准 |

不要把“使用C++20”“多加接口”“有AI入口”作为超过的证据，也不能把一个具体参考适配器的局限推广成整个项目都没有相应保证。

## 5. 目标目录的补充

沿用原提案的模块化单体，补充以下职责。是否单独建库按依赖隔离和体量决定；设计职责必须分别明确。

```text
modules/
  parameters/          # 单位、坐标、曲线/表格/场及引用求值
  analysis/            # 分析范围、工况/步骤、目标、控制与输出请求
  validation/          # 规则、Issue、定位、修复计划与失效
  results/             # 结果来源、字段/位置/帧、读取器与显示接口
  interaction/         # 客户端工具会话与拾取/预览/apply/cancel协议
  extensions/          # 功能/模板贡献契约与版本依赖
  platform_support/    # 偏好、配置、日志、诊断、资源路径
features/
  exchange/            # 导入/合并/导出及损失报告
  checking/            # 实际检查与可事务化修复
  mesh_editing/        # 编辑、变形、重编号/替换等算法贡献
profiles/<profile>/
  core/  codecs/  validation/  ui/  render/  tests/
```

interaction和UI贡献依赖contracts/client SDK，不能反向依赖document。parameters核心计算可并入document/foundation；UI表格编辑器属于ui。results的数值语义归服务端，只读结果投影交给渲染/图表消费者。extensions提供注册契约，不把所有可选实现静态塞进核心。

## 6. 使“全覆盖”可验收

为AP-01—20逐项建立模块合同，至少包含：

1. 参考源码/头文件/构建/文档证据及其可靠程度。
2. QCAE责任模块、权威数据、输入输出和允许的依赖方向。
3. 对外可扩展位置，以及新增功能允许触碰的文件类别。
4. 同步/异步、取消、失败、失效、撤销与恢复语义。
5. 类型/协议/存储/profile版本和迁移规则。
6. 最小代表实现、合同测试、集成路径与明确不可用项。
7. 当前状态：未设计／合同完成／已编译接入／代表实现验证／产品能力验收。

架构完成至少要求所有纳入项的合同完成、实际构建边界通过检查，关键公共链路有代表实现。单纯文档覆盖不计为“骨架已完成”。不能以“P1才实现”为理由省略其必要数据和扩展边界；也不能反过来把接口设计完成当作P1功能已经交付。

主梁场景之外，需要少量辅助合同夹具：一个坐标/单位字段、一个Issue定位与修复、一个结果字段读取/来源校验、一个模板UI贡献和一个脚本入口。夹具不伪造真实求解器运行；真实CAD/网格后端、求解器及完整后处理算法仍按其实际实现验收。

这允许一个简单场景承担主要端到端验证，同时避免声称它能覆盖参考项目所有功能域的全部语义。

## 7. 对原实施计划的增补

- R0增加“参考模块库存与遗漏审计”，为AP-01—20建立可追溯映射；缺失源码或仅SDK的部分单独标记。
- R1按矩阵建立真实模块依赖；补齐parameters、analysis、validation、results、interaction、extensions和platform-support的责任划分。
- R2/R3实现实体/操作与完整能力包贡献合同，增加工具会话、通用字段/引用编辑器及检查规则入口。
- R4用任务、产物、事件、分块和结果来源组成公共运行链；真实模块不直接修改存储权威。
- R5执行原EXT-01—05，加上全部覆盖项的合同/依赖/代表实现审计。需要实际外部后端才能验证的项保持未通过，不能凭测试替身升级状态。

本次完成的是参考对照与设计目标补强，未改动参考仓库，也未实施QCAE骨架重构。达到或超过的结论应在上述证据完成之后作出。
