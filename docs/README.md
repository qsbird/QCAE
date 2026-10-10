# 文档索引

冲突时以最新用户明确指令及下列基线文档为准。

## 设计基线 1.1

| 文档 | 用途 |
|---|---|
| [基线入口](baseline/README.md) | 确定决策、范围与环境门禁 |
| [需求](baseline/requirements.md) | P0功能、约束和非目标 |
| [开发计划](baseline/development-plan.md) | 当前状态、已确认决策、当前工作轨道与里程碑 |
| [验收](baseline/acceptance.md) | 需求追溯、测试身份与代码可读性门禁 |
| [骨架检查点](baseline/skeleton-acceptance.md) | SK-01—14量化合同及[机器可读目标](baseline/skeleton-acceptance-targets.json) |
| [组件与环境](baseline/components-and-environment.md) | 固定组件角色、开发环境门禁与已验证版本 |
| [模块架构](architecture/README.md) | 模块、调用、依赖与运行部署 |
| [运行时与数据](architecture/runtime-and-data.md) | 文档身份、事务、保存、恢复、任务 |
| [求解器能力包](architecture/solver-profiles.md) | 多求解器差异的职责与数据边界；P0只实现Nastran |
| [架构决策](architecture/decisions-and-verification.md) | ADR与架构验收 |
| [接口契约](contracts/README.md) | OperationDescriptor、请求/响应、语义报告 |

## 实现说明

| 文档 | 内容 |
|---|---|
| [M0](implementation/m0.md) | 纯核心与本地engine/CLI的构建和测试命令 |
| [M1](implementation/m1.md) | 实体/组织/引用、受控Nastran导入与导出预览 |
| [M2/M3](implementation/m2-m3.md) | SQLite持久化、查询/选择、Qt/VTK工作区的构建与运行 |
| [Nastran子集](implementation/nastran-subset.md) | 受控卡片支持矩阵（Profile生成输入） |
| [M4本地进程适配器](implementation/m4-local-runner.md) | 求解进程、版本探测、F06解析与结果发布 |
| [MYSTRAN本地接入](implementation/mystran-local.md) | MYSTRAN 19.0.0配置合同与运行链 |
| [M5 MCP桥](implementation/m5-mcp-bridge.md) | stdio MCP桥 |
| [M6本机包](implementation/m6-local-package.md)、[本机包运行](implementation/core-local-handoff.md) | macOS打包与运行步骤 |
| [源码导读](engineering/code-walkthrough/README.md) | 按构建/入口、数据/提交、业务功能、查询/存储/桌面阅读生产源码 |
| [C4扩展任务包](engineering/c4-executor-task.md) | 独立扩展实验的初始上下文与规则（[保护路径](engineering/c4-protected-paths.json)） |

## 归档

阶段验证报告、原始证据、早期方案（`*-v0.*.md`、骨架提案、Archer对照）、研究资料和界面概念图已移出工作树，保存在标签 `archive/pre-cleanup-2026-10-10`，查看方法见[开发计划](baseline/development-plan.md#归档)。归档内容不作为与当前基线冲突时的依据。
