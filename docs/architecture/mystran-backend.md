# ADR-09：MYSTRAN 本地后端

日期：2026-10-03。状态：按用户接续指令实施受控本机运行适配；验收范围以
[实际验证记录](../engineering/mystran-local-validation-2026-10-03.md)为准。
本决定补充 [ADR-08](decisions-and-verification.md#adr-08公共物理语义受控专有扩展与版本化能力包)
和 [能力包边界](solver-profiles.md)，不把完整 M4、SK 或 P0 标为完成。

## 驱动

本机已有从官方 MYSTRAN 19.0.0 源码构建的 macOS arm64 运行包及独立悬臂梁
可用性证据。MYSTRAN 是独立第三方求解器，读取 Nastran BDF；它与 MSC Nastran
在版本探测、输入语法、工作目录和结果表头上存在差异。仅替换 executable 路径
会使厂家版本、文件来源及结果解释不一致。用户要求继续接通现有任务闭环。

## 决定

在现有唯一 engine、TaskService、SQLite owned-row、LocalSolverRunner 和产物发布
链上增加显式 MYSTRAN 19.0.0 后端。RunConfiguration 固定
`solver_family=MYSTRAN`、`dialect=MYSTRAN`、`solver_version=19.0.0`，通过独立
`mystran.version.v1` 协议执行固定 `--version`；原 MSC 的固定 `help`、
`msc.help.v1` 和 test-only 路径保持原合同。版本观察绑定程序身份和配置摘要，
不能由配置中的声明文本或普通请求认证版本。

共享 Nastran ProfileRef、BDF codec 和定义/导出规则摘要保持原物理与交换语义。
MYSTRAN 额外门禁只检查已通过既有 codec 语义回读的冻结原字节，拒绝不能直接
运行的内容，不静默修正实数字段、INCLUDE、坐标、编号或模型。当前限定顶层
小写 `.bdf`、GRID/CBAR/MAT1/PBAR/FORCE/SPC1、单行自由格式、最多 8 字符的
带小数点实数字段，以及 SUBCASE 1/basic/mm-N-MPa。原手写 v3 benchmark 需
经过既有生产 codec 导出后才可执行。

保持摘要不变的范围是共享 BDF 和物理定义；它不表示原定义摘要已经覆盖新增
本地后端门禁、启动策略及结果 reader 源码。运行依据另行冻结 backend、实际
程序身份/版本、配置摘要和 `qcae.mystran.static-f06.v1` reader 版本。新增结果
适配不改变既有 MSC reader 或旧 run 的解释。

每个 run 精确复制两份相同已发布输入，保留私有 `input`，在私有 `work` 中
执行，通过绝对私有 TMPDIR 使用 `tmp`。两份输入都按原完成清单核验，求解后
再次核验；不让求解器在原冻结 artifact 目录工作。现有闸门、OS 创建身份、
startup intent、CAS 和预算继续约束进程启动、观察和取消。

F06/ERR 和日志均保留冻结资源摘要，MYSTRAN ERROR/FATAL 诊断不能因退出码 0
而通过。独立 reader 要求版本标头、明确工况、正确坐标/六分量表、完整冻结
GRID 位移、已知唯一反力和完整结束标志。解析、文件发布与 numerical check
分别持久化，numerical 使用原输入和映射，不能因进程退出或解析成功自动认证。
全部结果查询与核验仍经同一应用服务，求解事实不构成第二份工程模型或历史。

## 替代方案与取舍

直接把 MYSTRAN 声明成 MSC 会破坏厂家版本与结果格式依据；本决定拒绝这一
方案。临运行时重写已冻结 BDF 会改变输入来源并绕过完成清单；本决定使用
原 codec 的规范化导出和额外拒绝门禁。为当前相同梁物理子集建立第二套领域、
事务或 profile 模型会重复已有权威状态；本轮复用平台和共享交换语义。

代价是维护一个明确有界的输入门禁、厂家版本协议及独立结果读取器，并在
运行记录中区分其来源。本轮不会把任意 Nastran 方言、多物理模型、任意 INCLUDE
布局、动态插件或跨求解器转换变成已支持能力。未来若改变共享 BDF/物理规则，
仍需新的不可变 ProfileRef 和显式迁移，不能借本后端热替换旧定义。

## 验证与后续

实现、完整 v2 配置和 opt-in 真实集成命令见
[本地接入说明](../implementation/mystran-local.md)。适配器负例使用显式合成事实，
脱敏真实 F06 仅检验格式；真实进程、数值和 SQLite 集成结果必须单独登记。
本机运行包最低系统版本为 macOS 26.0，实际验证不扩展成其他系统支持承诺。
此前独立运行的三条 INCLUDE WARNING 保留，不能被当作任意 INCLUDE 完整验证。

真实 MYSTRAN 的在途取消/故障恢复、GUI 和 AI 端到端会话、完整数值场景及
分档性能继续作为后续门禁。终态结果重启恢复与真实小模型求解不关闭全部
TST-A08/I06—I10、M4/M5、SK 或 P0 验收。
