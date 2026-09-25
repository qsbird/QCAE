# 开发约定

## 当前阶段

M0的材料内存切片及本地IPC、M1的文档与受控格式切片已经实现，见[M0说明](docs/implementation/m0.md)和[M1说明](docs/implementation/m1.md)。后续按[开发计划](docs/baseline/development-plan.md)接续；完整P0尚未完成。

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

当前还需按[M0说明](docs/implementation/m0.md)运行CMake/CTest；后续按切片补充：核心单元/契约、事务故障注入、IPC/生命周期、选择显示、格式往返、真实求解和AI联合验收。现有构建配置与命令已由M0记录；不要在文档里报告未运行的测试。

## 代码可读性门禁

- 所有手写C++源码和头文件（`include/`、`src/`、`apps/`、`tests/`）遵守仓库根目录的`.clang-format`。运行`python3 tools/check_cpp_format.py`；脚本要求clang-format 21，缺少工具或存在格式差异均为失败。CTest也运行同一检查；构建目录生成文件不属于手写源码。
- 修改代码时先保持相关回归测试通过，再做机械格式化；格式化和语义改动分开审查。可用`QCAE_CLANG_FORMAT=/path/to/clang-format-21`选择本机程序，然后对改动文件执行`clang-format -i`。
- 格式检查只能证明排版一致。审查者还须能沿入口、应用服务、领域和适配器追踪修改路径，说明状态所有者、错误处理和测试依据；命名含糊、职责混杂、重复业务规则或需要逐行猜测的压缩表达应阻止阶段通过。例外必须在审查记录中指明具体文件、理由和后续处理，不以测试通过替代可读性审查。
- 每个开发切片完成时记录格式检查、编译/测试与人工可读性审查结果；任一项未完成，该切片的工程质量门禁不通过。对应验收口径见[质量门禁](docs/baseline/acceptance.md#8-代码可读性门禁)。

## 环境门禁

具体OS/硬件、依赖兼容小版本、实际Nastran方言与二进制、AI客户端连接等按 [组件与环境](docs/baseline/components-and-environment.md) 落实。它们是对应开发阶段的进入条件，不是无限期保留全部架构为“待定”。
