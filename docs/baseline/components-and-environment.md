# 组件与环境基线

组件职责在本基线固定，兼容小版本由M0实际构建后锁定；目前没有安装或验证产品依赖。

## 产品组件

| 组件 | 已确定职责 | 边界 |
|---|---|---|
| C++20标准库 | 领域、应用服务、类型、容器与必要并发契约 | 核心不暴露Qt/VTK/SQLite/MCP类型 |
| Qt 6 Core / Network | engine宿主、本地IPC、进程等适配 | 不进入领域及应用公共契约 |
| Qt 6 Widgets / 所需图形集成 | 必要桌面工作区、树表、表单、视口容器 | GUI不直接改文档或工程库 |
| VTK | 显示、拾取及必要图形辅助 | 消费RenderPacket；不作为物理模型 |
| SQLite 3 | 工作恢复库、历史/幂等持久化、工程快照 | 数据库外产物使用独立发布规则 |
| MCP工具桥 | 外部AI入口，采用stdio对接客户端，再连接本地IPC | SDK语言不改变核心契约；M5进入前固定实现与打包方式 |
| 已有Nastran求解器 | 受控SOL 101悬臂梁的真实计算与验证 | 不自研或偷偷替换为CalculiX；实际方言/二进制由M1/M4门禁记录 |

Qt模块许可按构建清单核验；VTK为BSD-3-Clause，SQLite核心代码公共领域。此处不作未选构建包的整体许可保证。[Qt许可](https://doc.qt.io/qt-6/licensing.html)、[VTK](https://vtk.org/about/)、[SQLite](https://sqlite.org/copyright.html)

## 开发验证工具

CMake/CTest负责构建与测试；Catch2负责核心测试；Qt Test负责必要UI测试；pyNastran用于格式样本和交叉验证，默认不作为产品运行时依赖。真实求解验收仍需要指定Nastran程序。

当前 `tools/check_design.py` 仅使用Python标准库，不要求这些产品工具已安装。

## 暂不引入

OCCT、Netgen/Gmsh在首次实际CAD/网格用例时接入；ParaView/trame、Qt ADS、HDF5、CGAL/SMESH、用户脚本运行时和插件系统不进入当前依赖集。不会因架构参考研究而复制其他项目或增加其许可证依赖。

## 门禁记录

| 门禁 | 需要记录的配置/证据 | 未具备时可继续的工作 |
|---|---|---|
| ENV-01 / M0 | 开发OS、编译器、CMake与基础依赖小版本、本地IPC实现 | 类型/契约和不依赖框架的核心测试 |
| ENV-02 / M1 | 一个Nastran方言、不可变ProfileRef及卡片/语法/结果能力矩阵 | 领域、组织关系和其他框架测试；不得宣称格式完整 |
| ENV-03 / M2 | 历史/缓存/临时文件配额、恢复保留和同步策略 | 事务内存适配与确定性故障测试 |
| ENV-04 / M3 | 实测硬件、图形后端、节点/单元类型与规模分档、预先定义的响应阈值 | 基础图形和数据映射测试；不得宣称容量达标 |
| ENV-05 / M4 | 求解器路径/版本、可执行权限、输入夹具、数值参考和容差 | 格式与模拟失败路径测试；真实求解项未完成 |
| ENV-06 / M5 | 一个AI客户端/模型、工具桥SDK/运行时、数据去向、真实调用记录 | 契约测试；真实AI项未完成 |

真实路径、凭据及机器专属配置留在被忽略的本地配置中。公开仓库只记录非敏感的版本清单和可复现步骤，M0/M4按实际环境填写，不生成虚假的配置值。

## 最小Nastran边界

分析限定一维线性静力悬臂梁，节点、CBAR/PBAR、MAT1、SPC1、FORCE、必要Case Control/文件控制和INCLUDE为初始目标集合。首个验证模型使用明确的坐标与截面方向；采用的字段形式、续行、数值指数和单位转换在ENV-02锁定测试样本。

默认严格处理不支持的卡片/特性：返回结构化诊断，不将不完整模型作为成功导入。支持矩阵之外的原样保留、任意真实工程合并及高级单元不自动进入P0。工程文件保存平台组织语义，BDF只保存目标格式可表达的分析模型。


## 求解器能力包边界

具体方言/版本能力在ENV-02形成一个静态注册Nastran profile，运行时二进制与选项在ENV-05形成独立RunConfiguration。P0没有第二求解器或动态加载依赖。领域类型和结果端口的通用表述遵循[基线1.1能力包设计](../architecture/solver-profiles.md)。

## M1文本格式环境记录

已实现一个静态 `qcae.nastran.linear-static` 0.1.0 codec，语义按 MSC 官方版本化文档核对；支持矩阵见 [Nastran子集](../implementation/nastran-subset.md)。ProfileRef 的 SHA-256 摘要由代码/声明/矩阵生成。ENV-02的codec语义部分有测试证据，但尚无本机求解器方言/二进制版本验证；`configured=false`、`validated=false`。没有新增产品依赖，也未使用pyNastran交叉验证。

## M2/M3已验证环境

已使用SQLite 3.51.0、Qt6 6.11.1 Widgets/OpenGLWidgets/Network/Test及VTK 9.7.0，在macOS 26.6.2 arm64上构建工作库/桌面。VTK源码与二进制隔离在被忽略的build-vtk-deps目录；归档校验和、配置及运行入口见[M2/M3说明](../implementation/m2-m3.md)。

ENV-03已固定当前数量/字节配额、WAL/FULL、明确恢复与拒绝超限策略；未覆盖任务资源配额（M4）。ENV-04已有真实小模型图形测试和初步单次查询测量，尚无完整硬件分档、分位延迟和容量验收。Qt/VTK桌面依赖未进入engine的图形依赖，纯核心可同时关闭Qt/VTK/SQLite。
