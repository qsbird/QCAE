# R0 参考库存与 AP 合同

状态：**L1 草稿证据**。本轮只冻结参考文件、架构归属与合同目标；AP-01—20 的公开消费者编译、代表实现和 BP 案例均未在本切片执行，L2 数量为 0。库存映射完整不能单独使 SK-01 通过。

参考是只读本地目录 `/Users/qs/Documents/ArcherprePublic`，没有可用的根 Git 元数据。本轮没有编译或运行 Archer，没有复制其源码。证据口径依据 [架构对齐目标](../architecture/archer-architecture-parity.md) 与 [骨架验收合同](../baseline/skeleton-acceptance.md)。

[reference-inventory.json](reference-inventory.json) 收录 6,113 个文件：5,854 个自有源码、构建、模板、UI 描述或开发文档文件，以及单独登记的 259 个 SDK/供应商公开边界文件。每条记录保留相对路径、完整 SHA-256、路径派生的稳定 inventory_id、source_kind、一个或多个 AP、QCAE 主责任模块、分类规则与理由。主责任模块取排序后的首个 AP；多域责任完整保留在 ap_domains 中。

库存用 pathlib/os.walk 遍历，不采用 Git 忽略规则，因此包含实际存在的全部 13 个模板分支。每个模板源码贡献还带 AP-10；渲染、树、检查、分析、参数等贡献同时归入相应功能域。测试源码按具名路径/责任规则归类，未细分的脚本/API 回归属于 AP-18，运行测试框架、配置和构建支持属于 AP-20。分类是源码架构归属证据，不是对每个回归语义的运行验证。

显式源码根、模板名、SDK 家族、可纳入文件后缀、第三方/构建/二进制排除模式和命中数量均被冻结。排除命中计数单位为“剪枝目录或跳过文件”，不冒充被排除树内全部文件数量。ArcherPre3rd、Qtitan 实现、Qt Solutions property browser、Boost/RTree/tinyobjloader、SDK 运行库与生成文档被排除；Qtitan/NCLicense 的公开 SDK 声明只登记为供应商边界。模型夹具、图片、二进制文档等非源码资源不属于本库存的源码分母；QCAE 旧格式迁移夹具由单独 R0 清单负责。

未知源码根、模板分支、SDK 家族、文件种类、未审查符号链接或无法分类的纳入文件会使工具失败。新文件或旧文件内容变化也使默认检查失败。`--write` 是重新冻结动作，只应在审查新增责任域和排除规则后使用；新增独立架构域需要修订验收合同，不能通过选择现有 AP 名称自动证明覆盖。生成器 SHA-256 也纳入冻结，避免分类算法无声变化。

库存另外登记 23 条自有 CMake 构建目标声明，含原始表达式、可从本文件读取的局部名称、文件/行号和证据 ID。这是静态声明证据；条件、跨文件变量和目标依赖没有经 CMake 配置求值，因此不作为实际成功构建图。供应商 Qtitan 构建实现不计入自有目标。

14 个已配置但缺失源码或只有 SDK 边界的组件保留在 missing_or_reference_only_components 中：ArcherGeom、ArcherGeomAdv、ArcherVTopo、ArcherRender、ArcherInteract、ArcherInteractModel、ArcherMorph、ArcherMeshCore、ArcherParaMesh、ArcherTet、ArcherHex、QuadMesh、ArcherTetRemesh、ArcherIO。缺失实现没有被算作不存在的功能责任，也不能由公开头文件推断真实算法可用。商业授权仅作为可选策略边界登记，远程传输仅作为参考协议边界登记；都不扩展本地 P0 范围。

20 份 [module-contracts](module-contracts/) 合同恰有八个顶层字段：reference_evidence、owner_module、authoritative_data、public_contracts、allowed_dependencies、extension_points、state_failure_version_rules、acceptance_case_ids。每份保留 AP 标识、L1_draft 状态、可追溯库存 ID、权威数据、具体公开输入输出、依赖方向、扩展入口、失败/失效/版本规则及对应 BP 案例。公开接口仍为未编译的设计合同；这些责任名称是逻辑模块，不证明已经建成独立库或实际依赖门禁。部分责任还需后续细化后方可升级合同完成状态。

| AP | 主责任 | 代表验收 |
|---|---|---|
| AP-01 | application | BP-01 会话身份、正常打开与恢复 |
| AP-02 | document/schema | BP-02 字段生成、编辑、历史与迁移 |
| AP-03 | geometry | BP-03 直线求值与稳定身份 |
| AP-04 | mesh/association | BP-04 stale 与严格拒绝未映射替换 |
| AP-05 | meshing_adapter | BP-05 真实后台离散器与候选校验 |
| AP-06 | mesh/document | BP-06 三类身份与拓扑保持 |
| AP-07 | features/mesh_editing | BP-07 复合移动与一次撤销 |
| AP-08 | organization | BP-08 多组织投影不复制节点 |
| AP-09 | physics/parameters | BP-09 单位、坐标与表格求值 |
| AP-10 | extensions/capability_package | BP-10 六类贡献与关闭能力包 |
| AP-11 | analysis | BP-11 工况引用与 Task 身份分离 |
| AP-12 | operations/history | BP-12 100 轮历史及精确修订 |
| AP-13 | interaction | BP-13 预览取消与应用生命周期 |
| AP-14 | validation | BP-14 Issue 定位、修复与失效 |
| AP-15 | project/exchange | BP-15 工程、迁移与编号映射 |
| AP-16 | visualization | BP-16 十二项真实拾取 |
| AP-17 | ui | BP-17 布局和快捷键恢复 |
| AP-18 | clients/extensions | BP-18 三入口共用服务 |
| AP-19 | results | BP-19 读取夹具、来源与陈旧状态 |
| AP-20 | runtime/platform_support | BP-20 任务配额、状态和事件序号 |

校验命令：

```sh
python3 tools/inventory_reference.py
```

命令重新读取参考文件并比较路径、ID、映射、SHA-256、排除与生成器规则，检查非空分母、唯一 ID/路径、20 域证据、20 合同字段与归属、证据 ID/路径/AP 关系和对应 BP 案例。它不运行 BP、故障、迁移、图形、求解或性能验收。无需参考编译依赖。

经本轮检查，默认库存/合同校验通过；另以临时夹具确认哈希篡改、重复 ID、缺责任、缺合同字段、错误证据关系及新独立源码根/模板/SDK 家族被拒绝。临时测试均在 `/private/tmp` 中完成，没有改动参考目录。完整 SK-01—14 仍保持待验证。
