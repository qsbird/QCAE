# MYSTRAN 本地接入验证（2026-10-03）

候选为本轮未提交工作树；共享 Nastran Profile/BDF 规则保持原定义，新增运行
适配依据见 [ADR-09](../architecture/mystran-backend.md)。本记录覆盖真实本机
MYSTRAN 19.0.0、小型预登记悬臂梁和独立适配回归，不关闭全部 M4/SK/P0。

## 真实进程与数值

使用 `build-local/qcae-engine`、`qcae-cli`，显式 SQLite 工作库和真实
`runtime/solvers/mystran-19.0.0/bin/mystran`。本机原始证据保存在忽略目录
`runtime/m4-mystran-20261003/real-run-lvzj9mar/`：`report.json`、IPC 全请求响应、
工作库、保存工程、输入 artifact、三个 run 的原始/发布文件与完成清单均保留。
这组为首次完整通过的开发候选。最终固定源码的完整 CTest 又运行三个真实
进程，其原证据位于 `build-local/m4-mystran-real-evidence/real-run-a8ec4ddo/`，
完整归档副本保存在 `runtime/m4-mystran-20261003/final-real-run/`；冻结目录
字段保留原路径，归档不改写事实。脚本和可复现命令见
[运行说明](../implementation/mystran-local.md)。

三个独立真实进程均零退出、完成 F06/ERR 收集、严格解析和结果发布。
每次比较 21 个 GRID 的六个位移分量及一个支座的六个反力分量，分别
**132/132 通过，共 396/396**。数据取自 solver 输出，没有解析时填入解析解。
实际端点位移 Y 为 -1.904763 mm、转角 Z 为 -0.002857144 rad，支座反力 Y
为 1 N、弯矩 Z 为 1000 N·mm，按原预登记基准与容差核验。

实际链同时验证：

- 首次 `--version` 观察为 `mystran.version.v1`、19.0.0、非 synthetic；冻结配置
  与进程来源明确为 MYSTRAN/external_solver。
- 原 artifact、run/input 和 run/work 的四个 BDF 文件逐字节一致；现有生产
  codec 的导出直接使用，运行过程没有改写冻结输入。
- 三个结果完成清单各包含 F06、ERR、stdout、stderr 与 parsed-result 的长度/
  SHA256；独立数值报告持久化，原 run 的 numerical_validation 仍为 not_run。
- 相同 start/validation 键重试复用原 task/report，无新增进程或 owned-row。
  运行及核验没有改变模型修订或 undo 历史。
- 错 Profile、旧修订和同长度结果破坏被拒绝，不写半份事实；破坏输出不会
  擦除已保存数值历史，但当前文件核验为 false。
- 改变 Young 模量使旧结果陈旧，undo 恢复原物理后结果重新适用。
- 完成后 kill engine、独立进程 recover，三个原始结果及数值报告保持；真实
  CLI 查询同一 engine 返回同一报告，没有再次运行 solver。

本轮只验证终态恢复；真实求解中的取消、强杀/启动窗口恢复尚待补证。
正常 MYSTRAN 输出的 INCLUDE warning 原文保留，不能把本模型通过推广为
任意 INCLUDE 布局或任意 Nastran 方言等价。

## 回归与首次失败

适配专项最初六项 CTest 为 5/6：result-store 新测试误把已有 reader-bearing
owned-run wire 版本写成 2，实际为 3。产品 roundtrip 已正常；只修测试预期，
最终固定源码的相关测试与完整回归均已通过。

首次沙箱纯核心为 40/41，C4 freeze-contract 在原 90 秒门槛超时；单独以本机
权限重跑同测试 34.25 秒通过，未修改产品、测试或门槛。确切首次超时根因
未知，不能用复跑抹去原失败。首次 socket 运行受沙箱 QLocalServer 权限限制，
以已授权本机权限执行后可运行。

真实链首轮已解析成功，但保存数值事实时旧 reader 白名单只接受 MSC。
修复为显式接受 MYSTRAN reader，保留原比较算法、基准、容差和 immutable
run；修复后三次完整真实链通过。失败运行和原始文件继续保留在同级私有目录。

第二次核心回归发生在并行编辑尚未完成时，为39/41：新数值测试尚未格式化，
新ADR的验证链接尚未落盘。文件稳定后格式与链接门禁通过，再执行以下完整
最终回归。该中间日志保存在 `runtime/m4-mystran-20261003/core-intermediate.log`。

## 最终固定源码回归

以 `5ff70bf6f1553f806d90a1ce5f75dfe4e4e7c87f` 为起点的未提交候选，生产、
schema、测试、工具、CMake和原Nastran定义范围共432文件；范围清单为
`runtime/m4-mystran-20261003/source-manifest.json`，其聚合SHA256为
`824fb1cfac35c2be8475e174924763a588525079ebd90bfe6cc5cd0e4dffaa28`。
完整回归完成后逐文件重新核验，一致。文档状态更新和用户已有AGENTS修改不
纳入这一源码摘要；AGENTS原有改动保持。

| 配置/检查 | 最终实际结果 |
|---|---|
| 纯核心 Release，IPC/storage OFF | **41/41 CTest通过**，48.89秒 |
| Qt/SQLite Debug，desktop OFF、真实MYSTRAN显式启用 | **90/90 CTest通过**，221.39秒 |
| 其中真实MYSTRAN完整链 | 三次运行，**396/396分量通过**，15.46秒 |
| C++可读性格式 | 242文件符合clang-format21 |
| 设计检查 | 212可移植链接、1153 JSON、32模块无环及41操作/38验收定义通过 |
| `git diff --check` | 通过 |

两配置使用CMake+Ninja和严格C++警告构建通过，无新增依赖。实际环境为
macOS26.6.2 arm64、Apple clang21、Qt6.11.1、SQLite3.51.0、CMake4.3.0；CTest
Python3.14.7，手动脚本Python3.14.0，各自登记在私有 `environment.json`。
原始完整日志为同目录 `core-final.log`/`local-final.log` 及两份
`*-ctest-details.log`；`validation-summary.json`记录配置、来源、二进制与日志摘要。

最终真实solver二进制SHA256为
`6abab091442a4ebd3d57b1fbbcad60da9804c0f7bcbf2abef2a848aef94cd956`。
用户指定归档SHA256仍为
`3d8740e23de60ce96499a5c3ad353039088a226365987a44237795938db9e928`，没有修改。
共享Profile仍为0.2.0，定义摘要
`sha256:a6604ba3d6970cbbec955389578a0f2cfaf2da8bda400730a3b87f0e315958bc`。
适配回归与真实运行不是同一来源；不得把131次跨配置CTest执行解释成131条
独立工程场景。reader早期独立ASan/UBSan通过；最终集成全套没有再次运行
sanitizer或桌面/包关闭矩阵，不扩展旧源码证据到本候选。

## 可读性审查 QG-02

入口 `analysis.start` 在既有单一 coordinator 校验语义、来源及额外输入门禁，
TaskService/SQLite 持久化 run，再进入 LocalSolverRunner 的进程闸门。
`solver_backend.hpp` 集中厂家与版本协议选择；`mystran_input` 是只读拒绝门禁。
独立 F06 reader 只消费明确上下文和冻结 GRID 映射，不访问应用、Qt 或存储。
`solver_result_store` 校验原始摘要、错误诊断与 reader/backend 绑定，先保存发布
意图，再通过最终清单发布；数值报告沿现有 `solver_validation_store` 独立 CAS。
权威模型和历史仍只由既有应用服务持有。

版本、输入、执行、解析、文件发布与数值报告的失败分别保留；错误、截断、
重复/未知 GRID、非有限值、不支持坐标、混合厂家、错 reader 和文件破坏均有
拒绝回归。审查由独立 reader 代理和 root 检查，发现的 ERR 诊断遗漏已修复。
适配测试中的 fabricated execution/version 字段明确标为合成输入；它们验证
绑定与拒绝，不能替代上节真实进程的工程证据。

## 未关闭范围

TST-F07 的全修改/组织变体、TST-A08 的真实在途取消与故障窗口、TST-A09 的
求解期间并发编辑、GUI/MCP/真实 AI 端到端、目标硬件性能、其他 OS 与求解器
版本均未在本轮完整验收。现有桌面累计超时、旧运行包、SDK unknown 与性能
缺口保留此前范围。小模型真实求解通过不等于完整 M4、C3/C4、SK 或 P0 通过。
