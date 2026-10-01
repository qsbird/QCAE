# 检查报告桌面显示的有界修复

本轮只修改 `ui/desktop/src/analysis_tools.cpp` 和既有 `tests/c3_desktop_workflow_tests.cpp`。engine、CheckService、规则、物理字段及共同构建配置均未修改。生产及测试源码哈希见 `source-checkpoint.json`；这里不是完整 P0／M4／M5 验收记录。

## 实际问题与修复

原 build38 的独立 native GUI-M-0 在初次检查失败：两条缺失载荷／约束 issue 应显示 2 条，实际为 0（`native-old-red.log`，19.090s）。原六行共同 RED 仍保留在上级 package 日志中。

producer 的 `rule_version`／`entity_type` 是 int64 编码后的十进制 JSON 字符串，旧 UI 要求规则版本为 JSON number，且用 `toInt()` 显示为 0。UI 现在校验 canonical positive uint32 字符串，并直接显示原字符串。

新检查的 transport status 与报告 outcome 相同；读取持久报告的 transport status 为 success，而原报告 outcome 可继续为 needs_input／failed。UI 现在分别校验这两层，保留 input/current 版本、check／analysis 身份、规则目录及 profile 形状、issue 来源／状态一致性检查。文档／epoch generation 和销毁／断连围栏保留；历史报告只按实际 input 与当前上下文标记 stale，不改写原结论。

## 已实际执行的证据

| 验证 | 实际结果 | 日志／原始数据 |
| --- | --- | --- |
| 严格 Release UI／workflow target 构建 | exit 0，`-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror` | `strict-release-*.log` |
| 真实 local socket／DesktopClient 的合成报告矩阵 | 52 场景通过；Qt 总计 54，包括 init／cleanup，13.993s | `local-wire-green.log` |
| 原 GUI-M-0，保留原 2 条 issue 预期 | 全流程通过；Qt 总计 3，28.911s | `native-one-green.log`；`GUI-M-0-*-check-missing-current.json`／`missing-stale.json` |
| 真实单 LC／双载荷场景经 CheckService 到 AnalysisTools | 通过；Qt 总计 3，1.290s；failed current→success 读取／failed stale；3 条 issue 保留，检查及重读不改变物理 history／revision | `real-failed-report-green.log`；`failed-report-*-check-run.json`／`stale-read.json` |
| 设计／两个 owned 文件格式／全仓 diff whitespace | 设计通过，两个文件格式通过，diff exit 0 | `design-check.log`、`focused-format-check.log`、`workspace-diff-check.log` |

52 个矩阵场景含 6 个合法 success／needs_input／failed 的 current／stale 重读、38 个畸形报告 run／read 负例、8 个跨文档／epoch／suspend／销毁的迟到回复场景。它们使用合成 DTO 和真实 Qt IPC framing，不能替代实际工程检查规则的验收；上表的两个真实 CheckService 场景提供独立 producer→consumer 证据。

当前实际 needs_input JSON 为外层 success、outcome needs_input，input revision 11；物理编辑后的 current revision 12，报告及两条 issue 都为 stale，input revision 仍为 11。真实 failed JSON 包含 cantilever-single-load、missing-constraint、missing-section 三条 issue；重读保持全部原 input 来源及 failed outcome。

## 保留的失败与范围

- `sandbox-gui-environment-failure.log`：沙箱内 native GUI 在平台服务访问时 SIGSEGV，尚未到业务检查；之后按已授权的 native 环境重跑，实际业务 RED 如上。该环境失败未作为检查报告业务证据。
- `real-failed-fixture-first-red.log`／`real-failed-fixture-domain-red.log`：最初独立 fixture 试图创建含两个 LC 的 analysis；领域不变量明确拒绝，完整诊断是 controlled subset at most one LC。这不是 CheckReport 显示失败。最终 fixture 改为合法单 LC 引用两条真实 nodal force，实际场景检查生成 failed 报告。
- `real-failed-fixture-task-name-red.log`：独立 fixture 误用不存在的 task.get；已依据现有真实契约修正为 task.status 加当前文档上下文，原失败保留。
- `format-check.log`：中间全仓格式检查在另一个 owner 正在修改的 solver_contribution.cpp:964 上失败；本轮没有修改该文件，不把 focused 通过冒充该中间全仓检查通过。

以上 GUI-M-0 在最后新增独立 failed fixture 前执行，随后生产 UI 与原六条 workflow 的行为代码均未变化。最终六条 GUI／MIX 全量回归交给主代理接续执行，这里尚未标记通过。场景检查只证明必要条件／结构化诊断可审查，不证明外部 Nastran 或数值验收，也不改变任何冻结性能门槛。
