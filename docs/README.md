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
| [开发计划](baseline/development-plan.md) | M0—M6切片和任务依赖 |
| [验收](baseline/acceptance.md) | 需求追溯与完成证据 |
| [设计验证记录](baseline/validation-record.md) | 已执行的文档检查与尚未执行的产品验证 |

冲突时以最新用户明确指令及这些基线文档为准。历史方案不能重新扩大P0。

## 当前实现

[M0运行与验证](implementation/m0.md)：纯C++内存核心、Qt本地engine/CLI。

[M1文档与格式](implementation/m1.md)：实体/组织/引用、受控Nastran导入与导出预览、统一事务及限制。

## 界面参考

[主工作区概念图](design/qcae-p0-main-v2.png)及[功能说明](P0预期功能与界面说明-v0.4.md)用于说明信息布局，不代表产品已经实现，也不是像素级开发合同。

## 研究与历史

- [Gmsh、CalculiX、FreeCAD FEM 架构参考](research/Gmsh-CalculiX-FreeCAD架构参考-v0.8.md)：架构启发，非依赖引入决定。部分本地源码链接仅适用于研究时的文件位置。
- 根目录中的 `*-v0.*.md` 保留讨论过程、预算推导与来源；相冲突的旧服务器方案等已被当前基线取代。
- `contracts/p0-ai-examples.json` 是早期示意；当前请求示例为 [current-examples.json](contracts/current-examples.json)。

本次基线不移动或删除历史资料，避免破坏已有引用。后续改动依据请落在基线/架构/契约及ADR中。
