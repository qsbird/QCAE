# 开发约定

## 当前阶段

本仓库处于设计基线阶段。后续明确的实现任务从 [开发计划](docs/baseline/development-plan.md) 的 M0 开始；无需重启整轮需求访谈。尚未安装产品依赖，也没有可运行产品。

开始一个开发切片前，读取对应需求、架构边界和验收ID，先定义该切片的完成证据。不要一次性生成所有模块空壳或扩大未列功能。

## 基本规则

- C++20 是业务核心基线；Qt、VTK、SQLite、MCP SDK 只进入宿主或适配器。
- GUI、CLI 和 AI 共用应用服务，禁止直接改领域对象或数据库。
- 新增第三方依赖需说明必要性、许可、版本及依赖方向，不以参考研究作为自动安装授权。
- 没有用户明确要求，不复制 ArcherPre/Gmsh 等参考仓库代码，不修改它们。
- 模型提交、撤销、恢复、选择、格式转换的正确性都需测试；涉及真实求解的验收不能用mock替代。
- 维护文档中的确定决策与实现一致性。改动已接受的架构决策时更新ADR及受影响测试。

## 分支和提交

新工作分支默认使用 `codex/` 前缀。初始设计基线保留在仓库的 `main`。

提交遵守 Lore：首行解释意图，正文说明约束和取舍，使用有价值的原生Git trailers，例如：

```text
Keep GUI and AI edits consistent through one transaction path

Route the new operation through the existing document commit coordinator.

Constraint: A document has one writer and one undo history
Confidence: high
Scope-risk: narrow
Tested: Contract, undo/redo and stale-revision tests
Not-tested: External solver integration
```

不要将 `.omx/`、本地凭据、编译产物、工作恢复库和实际求解运行目录提交到Git。测试夹具应放入专用测试数据目录并说明来源，不能把真实用户工程当默认夹具。

## 验证

当前文档基线：`python3 tools/check_design.py` 和 `git diff --check`。

实施后按切片补充：核心单元/契约、事务故障注入、IPC/生命周期、选择显示、格式往返、真实求解和AI联合验收。构建配置与命令由 M0 实际创建并记录；不要在文档里报告未运行的测试。

## 环境门禁

具体OS/硬件、依赖兼容小版本、实际Nastran方言与二进制、AI客户端连接等按 [组件与环境](docs/baseline/components-and-environment.md) 落实。它们是对应开发阶段的进入条件，不是无限期保留全部架构为“待定”。
