# 初始设计基线验证记录

日期：2026-09-25。验证对象为提交前的设计文档与检查工具，**不是产品实现**。

## 基线1.0已执行（初始提交 f527271）

- `python3 tools/check_design.py`：仓库内可迁移链接、JSON解析、30节点依赖图无环、核心/客户端/图形依赖隔离、27项操作定义、10个契约样例、18项需求与30项验收映射。
- `git diff --check`：已跟踪变更检查；首次提交前还会用暂存区检查覆盖新文件。
- 临时复制文档后注入循环依赖、断链、修改请求缺Epoch：检查脚本均返回失败；还原副本后通过。临时副本不进入仓库。
- 独立只读审查发现并修复生命周期操作幂等查找缺口、环境配置提交范围矛盾；修订后再次检查。
- `.gitignore`覆盖本地OMX状态、环境文件、构建与运行数据目录。

## 未执行

C++产品编译、Qt/VTK集成、实际IPC、SQLite故障恢复、Nastran求解、AI真实交互和性能测试均未执行。验收基线中的全部产品测试仍为待实现/待执行。

## 验证复现

```sh
python3 tools/check_design.py
git diff --check
```

新增或修改设计后重跑。脚本属于设计一致性检查，不是完整JSON Schema校验器，也不能证明未实现业务满足这些契约。


## 基线1.1：求解器能力边界补充

本轮更新数据与接口设计，未实现产品或新增第二个求解器。已执行：

- `python3 tools/check_design.py`：32模块依赖图、框架隔离及通用registry不依赖Nastran提供者；27项操作、12个当前契约样例、20项需求、38项验收映射；能力包/编号/结果设计夹具。
- 临时副本故障注入：核心依赖具体Nastran、缺少预期profile、来源编号冲突、派生字段争夺权威、不支持转换却可发布、结果缺位置、合成目标冒充生产目标，共7种错误均被检查器拒绝。还原副本通过。
- 文档链接与空白检查通过。历史接口样例保留原版本，当前契约为1.1；没有已发布工程文件需要迁移。

这些只证明设计资产及检查工具一致，不证明实际profile校验、转换等价性、GUI行为、求解或AI运行已实现。产品验证仍按验收基线执行。


## M0 实现验证

纯核心Release 5/5、ASan/UBSan核心5/5、本地Qt Debug测试集6项；真实IPC联调、错误响应ID、并发自动启动和原子修改失败注入均有测试代码。独立Astra审查提出的发布后分配、操作名映射、帧上限及响应归属问题已修复。实际版本、命令和范围见[M0实现记录](../implementation/m0.md)。本轮未验证GUI、SQLite恢复、Nastran、AI或P0性能。

## M1 文档与格式切片验证

日期：2026-09-25，环境延续M0的macOS arm64、Apple clang 21.0.0、Qt6 Core/Network 6.11.1，无新增产品依赖。

- `cmake --build build-core && ctest --test-dir build-core --output-on-failure`：Release 8/8通过。
- `cmake --build build-sanitized && ctest --test-dir build-sanitized --output-on-failure`：ASan/UBSan 8/8通过。
- `cmake --build build-local`及`ctest --test-dir build-local --output-on-failure`：Debug 10/10通过，含原M0和新M1真实CLI/engine通信。测试使用临时本地套接字，获得本机监听权限后执行；测试清理自身进程。
- 领域测试覆盖稳定实体/分离组织、跨文件引用、循环/悬空引用拒绝、全模型撤销与重做、旧修订/错误调用者、配额，以及导入后的commit/undo/redo逐分配位置失败原子性。
- 格式测试覆盖受控多文件语义往返、独立编号空间、目标摘要匹配、未知卡片与不支持语法、精度拒绝、MAT1派生剪切模量及FORCE变换溢出/下溢、G0/方向向量区分、INCLUDE路径与范围、平台信息损失报告。
- M1 IPC测试覆盖候选导入/统一提交、部件/装配/集合编辑、组织视角与实体身份、引用与影响、节点修改/undo/redo、冻结导出预览及失败不污染活动模型。
- 独立Astra源码审查与临时ASan/UBSan探针完成；已修复审查发现的自由度、组织查询边界、重复控制语句、路径及数值语义问题，最终审查范围内无未解决发现。
- 最终设计检查通过：28项操作描述、12个契约样例、20项需求、38项验收映射；生成操作目录及profile摘要检查通过；`git diff --check`通过。

范围及复现入口见[M1说明](../implementation/m1.md)。仅支持文本资源包与内存ArtifactPlan：未验证实际工程磁盘保存/恢复、多文件发布、真实求解器、pyNastran交叉验证、GUI、Windows、AI或容量/交互性能。ENV-02的真实目标二进制兼容性及完整P0验收仍待完成。

## 代码可读性门禁建立与存量整理

日期：2026-09-26。先在未改动C++源码时复跑纯核心8/8、本地Qt 10/10及设计检查；再用clang-format 21统一21个手写C++文件，并将格式检查加入CTest。没有改动业务规则或对外协议。

- QG-01：`python3 tools/check_cpp_format.py`通过；`git diff --check`通过。CTest现包含`cpp_format`，缺工具或格式不一致会失败。临时放入一份格式错误的C++探针，检查确实失败；移除探针后恢复通过。
- QG-03：整理后Release纯核心9/9、ASan/UBSan纯核心9/9、本地Qt 11/11通过；生成的操作目录与Nastran能力摘要检查通过。能力摘要目前哈希原始源码，因此纯格式改动也会生成新digest；尚无已发布工程格式或持久模型需要迁移。
- QG-02：格式整理改善了布局，但存量结构审查仍发现`src/ipc_model.cpp`的`dispatch_model`合并导入、编辑、查询及导出分派，`src/nastran_codec.cpp`的读取/编码路径仍较长。它们是后续结构整理的具体审查对象；格式和测试通过不能代替职责拆分审查。当前不将存量代码标记为已通过完整人工可读性门禁。

## M2/M3 实现验证

日期：2026-09-26。已有可读性整理被保留为独立基线提交 `a4d7dda`，以下为本轮最终实现证据。

| 构建/检查 | 结果 | 范围 |
|---|---|---|
| Release纯核心，IPC/Storage/Desktop全部OFF | 12/12 CTest | 领域、差量、模拟存储故障、查询、格式及既有回归 |
| Debug ASan/UBSan，Storage ON、IPC/Desktop OFF | 13/13 CTest | 上述核心加真实SQLite适配、别名锁及崩溃窗口 |
| Debug本地IPC＋Storage | 16/16 CTest | 原M0/M1及新增M2恢复/M3选择跨进程流程 |
| Release桌面＋IPC＋Storage＋Qt/VTK | 18/18 CTest | 全部核心/存储/IPC测试、Qt交互、实际GUI截图冒烟 |
| QG-01 | 45个手写C++文件格式通过 | clang-format 21；git diff --check通过 |
| 设计与契约 | 35项操作目录及生成输出通过 | 32模块DAG、12个设计例子、20项需求、38项验收映射 |

核心故障证据包括数据库提交前/后的确定性注入、未完成保存意图、发布后丢失保存标记、相同键重试、另存不改变模型修订、旧Epoch拒绝、损坏状态/历史/配额校验和不支持Profile拒绝激活。SQLite适配测试包括实际子进程中止及路径/符号链接/硬链接锁。新增m23_ipc测试实际强制结束engine，再用同一工作库恢复，验证历史、操作事实、dirty保存点和普通打开的新文档身份。

图形证据来自实际VTK OpenGL上下文：QtTest验证可见/穿透框选、隐藏端点仍显示所属梁、请求关联、回调重入和回调中同步销毁客户端；desktop_smoke测试由CLI向真实engine导入仓库夹具，GUI读取同一模型并退出，断言DocId和修订未改变。最终[截图](../implementation/m3-workspace.png)已目视检查：实体名称回退、组织下拉框、真实梁显示和共享历史均可见。engine动态依赖检查只有QtCore/Network与SQLite等系统库，没有QtWidgets/VTK。

QG-02审查确认：detail::Data是模型/历史/宿主操作事实权威；application_state.cpp分离状态格式与校验，core.cpp负责生命周期/事务；SQLite适配器只负责字节事务、锁与发布；SelectionService维护可丢弃会话；GUI/VTK只消费协议投影。审查发现的保存、恢复、重试、损坏输入、配额、别名锁、图形选择及回调生命周期问题已修复并回归。

一次Release 1000节点盒查询测量为504微秒，仅为合成单次样例，不是分位性能/容量验收。未验证真实求解、AI、Windows/打包、磁盘硬件掉电、批量传输、完整事件订阅或完整M3/P0性能。详细实现边界及构建命令见[M2/M3说明](../implementation/m2-m3.md)。
