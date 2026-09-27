# 文档索引

## 当前开发依据：设计基线 1.1

| 文档 | 用途 |
|---|---|
| [基线入口](baseline/README.md) | 确定决策、范围、文档优先级 |
| [需求](baseline/requirements.md) | P0功能、约束和非目标 |
| [模块架构](architecture/README.md) | 模块、调用、依赖与运行部署 |
| [求解器能力包](architecture/solver-profiles.md) | 多求解器差异的职责与数据边界；P0只实现Nastran |
| [运行时与数据](architecture/runtime-and-data.md) | 文档身份、事务、保存、恢复、任务 |
| [接口契约](contracts/README.md) | OperationDescriptor、请求/响应、语义报告 |
| [组件与环境](baseline/components-and-environment.md) | 固定组件角色及开发环境门禁 |
| [开发计划](baseline/development-plan.md) | M0—M6、C1/C2当前状态及入口补强的接续任务 |
| [验收](baseline/acceptance.md) | 需求追溯与完成证据 |
| [设计验证记录](baseline/validation-record.md) | 已执行的文档检查与尚未执行的产品验证 |

冲突时以最新用户明确指令及这些基线文档为准。历史方案不能重新扩大P0。

## 工程化重构提案

[以简单场景验收平台骨架](architecture/engineering-skeleton-proposal.md)：重构前问题、目标模块、实体/操作扩展机制、核心链路、R0—R5顺序和EXT-01—05验收。已实现C1/C2范围，完整目标尚未完成。

[Archer架构对照与对齐目标](architecture/archer-architecture-parity.md)：20个参考功能域的最低覆盖清单、原提案缺口、指定改进目标及可验收标准；参考库存及C1/C2已有证据，完整架构对齐尚未验收。

[骨架检查点与完成预期](baseline/skeleton-acceptance.md)：SK-01—14及97项数值/布尔指标、固定夹具、采样公式、扩展触碰上限与机器可读目标。

## 当前实现

[C2 交接与当前边界](implementation/c2-handoff.md)：当前停止在 C2，记录核心、后台线网格、CLI/薄客户端与保存/恢复已贯通。

[C2 扩展审查](engineering/extensibility-audit-c2.md)：生产查询、操作契约、装配及拓扑/显示的剩余缺口；接续任务纳入[开发计划](baseline/development-plan.md#c2之后的接续计划)。

[平台框架逐文件导读](engineering/code-walkthrough/README.md)：按四条阅读链说明现有 109 个生产源码与配置文件，区分记录主链、旧兼容边界和尚未贯通的入口。

[M0运行与验证](implementation/m0.md)：纯C++内存核心、Qt本地engine/CLI。

[M1文档与格式](implementation/m1.md)：实体/组织/引用、受控Nastran导入与导出预览、统一事务及限制。

[M2/M3持久化与桌面](implementation/m2-m3.md)：SQLite工程/恢复、查询/选择、真实Qt/VTK工作区及运行说明。

## 界面参考

[主工作区概念图](design/qcae-p0-main-v2.png)及[功能说明](P0预期功能与界面说明-v0.4.md)用于说明信息布局，不代表产品已经实现，也不是像素级开发合同。

## 研究与历史

- [Gmsh、CalculiX、FreeCAD FEM 架构参考](research/Gmsh-CalculiX-FreeCAD架构参考-v0.8.md)：架构启发，非依赖引入决定。部分本地源码链接仅适用于研究时的文件位置。
- 根目录中的 `*-v0.*.md` 保留讨论过程、预算推导与来源；相冲突的旧服务器方案等已被当前基线取代。
- `contracts/p0-ai-examples.json` 是早期示意；当前请求示例为 [current-examples.json](contracts/current-examples.json)。

本次基线不移动或删除历史资料，避免破坏已有引用。后续改动依据请落在基线/架构/契约及ADR中。
