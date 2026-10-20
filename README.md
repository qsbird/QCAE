# QCAE

面向本地中小规模模型的 CAE 前处理平台。P0 聚焦受控 Nastran 悬臂梁流程，提供桌面 GUI 与外部 AI 共用的无界面业务接口；服务器和亿级实现后置。

**当前状态：** 统一事务/历史、SQLite保存恢复、CLI/GUI/MCP共享engine已贯通；显式MYSTRAN 19.0.0本机后端完成真实悬臂梁数值核验。完整SK/P0、真实AI会话、性能与发布尚未验收。逐项状态、已确认决策和当前工作轨道见[开发计划](docs/baseline/development-plan.md#当前状态)。

## 从这里开始

1. [设计基线与已确定决策](docs/baseline/README.md)
2. [P0 需求](docs/baseline/requirements.md)
3. [模块及架构](docs/architecture/README.md)
4. [接口契约](docs/contracts/README.md)
5. [开发计划](docs/baseline/development-plan.md)
6. [验收标准](docs/baseline/acceptance.md)

阅读现有实现可从[平台框架逐文件导读](docs/engineering/code-walkthrough/README.md)开始，按构建/入口、数据/提交、业务功能、查询/存储/桌面的顺序阅读当前生产源码与配置文件。

```mermaid
flowchart LR
    GUI[Qt 桌面与 VTK 视口] -->|本地 IPC| Engine[qcae-engine]
    AI[外部 AI 客户端] -->|MCP stdio| Bridge[工具适配桥]
    Bridge -->|同一本地 IPC| Engine
    CLI[无界面客户端] --> Engine
    Engine --> Work[(工作恢复 SQLite)]
    Engine --> Snapshot[(工程快照)]
    Engine --> Solver[Nastran 求解器进程]
```

## 构建与测试

纯核心需要C++20编译器、CMake 3.24+和Python 3.10+，不需要Qt或其他产品框架：

```sh
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DQCAE_BUILD_IPC=OFF
cmake --build build-core
ctest --test-dir build-core --output-on-failure
```

本地engine/CLI启用`QCAE_BUILD_IPC=ON`，SQLite持久化启用`QCAE_BUILD_STORAGE=ON`，桌面另启用`QCAE_BUILD_DESKTOP=ON`并配置带Qt支持的VTK。运行`./build-desktop/qcae-desktop`启动工作区，详见[M2/M3构建与运行](docs/implementation/m2-m3.md)。不传`--workspace`的独立engine仍为内存模式。

## 仓库内容

| 路径 | 内容 |
|---|---|
| `docs/baseline/` | 当前需求、决策、组件、开发顺序与验收 |
| `docs/architecture/` | 当前模块、依赖、运行时与数据一致性 |
| `docs/contracts/` | 业务契约、操作描述和交互样例 |
| `docs/implementation/` | 各子系统的构建、运行与实现说明 |
| `docs/engineering/` | 源码逐文件导读与C4扩展实验任务包 |
| `modules/`、`features/`、`schemas/` | 通用核心、具体业务与生成契约 |
| `apps/`、`adapters/`、`profiles/`、`ui/` | 进程入口、外部适配、格式能力包及桌面 |
| `tests/` | 核心、契约、分配失败和真实IPC回归 |
| `tools/` | 操作描述生成与设计一致性检查 |

完整导航见 [文档索引](docs/README.md)。开发前阅读 [贡献与验证约定](CONTRIBUTING.md)。

## 许可

QCAE按[WTFPL第2版](LICENSE)发布，不提供任何担保：

> This program is free software. It comes without any warranty, to the extent permitted by applicable law. You can redistribute it and/or modify it under the terms of the Do What The Fuck You Want To Public License, Version 2, as published by Sam Hocevar. See http://www.wtfpl.net/ for more details.
