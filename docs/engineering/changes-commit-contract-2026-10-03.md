# 预览提交契约与实际验证

实现提交 `88942c03da817110afa66675773f80dfc1bb2499` 已合入主工作区。已有 `changes.commit` 现在通过生成 `ChangesCommitInput` 解码，发现公布同一schema与版本规则。此切片实现完成；完整图形回归、SK和P0验收仍未关闭。本机包仍为 `f2d2e7f`，本轮未刷新。

## 实现与权威状态

`schemas/operations/document_changes.json` 定义版本1、`qcae.operation.changes.commit.v1`、唯一必需非空字符串 `preview_id`。IPC复用已有字符串输入/版本/发现辅助函数，继续调用同一 `MemoryApplication::commit` → `RecordApplication`。没有新registry、业务状态、SDK公开核心类型或依赖。调用者、文档/epoch/revision、幂等键、提交回执和历史路径保留。

省略版本或显式1使用安装合同；非法版本返回 `INVALID_INPUT`、不同合法uint32返回 `SCHEMA_UNSUPPORTED`，字段是 `requested_version`。缺失/空/错误类型/未知输入字段拒绝在应用提交之前。设计符号类型保持，用 `wire_input_type:ChangesCommitInput` / `wire_output_type:ChangeReceipt` 公布实际DTO。MCP沿用实际engine参数schema；本轮补了发现断言，没有声明新增实际AI会话。

## 实际构建与执行

修改前四组既有核心/IPC/MCP测试实际通过。新构建均为严格C++20、零编译警告，未改图形后端或安装依赖。首轮冻结418输入，结果如下：

| 配置 | 实际执行 | 通过 | 失败 |
|---|---:|---:|---:|
| 纯核心Release | 40 | 40 | 0 |
| 本地Debug | 55 | 55 | 0 |
| SQLite/Nastran ON，无桌面 | 86 | 85 | 1 |
| SQLite/Nastran OFF，无桌面 | 77 | 76 | 1 |
| ASan/UBSan typed_host专项 | 1 | 1 | 0 |
| 首轮合计 | 259 | 257 | 2 |

两次失败均是新 `changes_commit_ipc` 在恢复比较时错误要求实体迭代顺序不变。两个原始transcript独立证明相同三个稳定ID、完整query行及暴露字段相等，只是顺序不同；失败时因短路尚未读恢复后的history，不能凭此声明那一步通过。修正测试为完整枚举、唯一ID检查及按稳定ID排序，未改产品或删去字段/历史断言。失败源码/日志和比较证据全部保留。

仅此Python测试在首轮之后变化；同一实际产品包ON/OFF各 `--repeat until-fail:2`，四次实际通过，无完整矩阵重跑。总计263次CTest执行为261通过/2原失败；修改前四组测试和下述原生图形诊断不计入该分母。LSan未测。

四次最终SQLite运行每次完成18次拒绝、4次commit重放、两个成功提交和两个独立engine进程。每次拒绝/重放比较每个SQLite逻辑表、列、BLOB、generation、完整DocumentInfo和history；合法提交只增一次修订/generation，保留历史已应用前缀并只在成功时截断redo尾。撤销后的旧回执不重建实体；省略/版本1互相重放。进程重启显式recover后，稳定ID/字段/history相等，epoch更新，两份持久回执查询及旧epoch拒绝通过。每次两个engine实际exit均为-15；不声称物理WAL字节相等或完整M夹具。

## 图形诊断与仍未解决的风险

前轮 `desktop_selection` 在原180秒限制内两次CTest超时，完整原生诊断也超时；150秒采样定位 `vtkHardwareSelector` → `glReadPixels` → AppleMetalOpenGLRenderer等待，并观察到累计NSAnimation等待线程及线程池饱和。组件owner和驱动根因仍未证明。

本轮166是方法后继诊断，使用保留的151二进制，未编入本轮新提交契约；三个相关图形源码字节未变。实际枚举23函数并逐函数顺序启动，完整69原数据行精确匹配，连同init/cleanup是115条Qt PASS。枚举、24个实际子进程和清理共用180秒预算，耗时约119.97秒，全部exit0；无FAIL/SKIP/BLACKLIST/XFAIL，无删除数据行、硬件拾取或负例，没有新增每函数180秒额度。

这支持fixture进程累计压力方向，但不与原执行拓扑完全等价，不证明驱动修复。生产虽只有一个主窗口，仍有反复创建对话框的生命周期风险。原累计窗口失败和当前完整图形门禁保持未关闭，不能把此次旧二进制诊断当作当前源码完整GUI通过。

## 证据、审查与接续

[机器记录](changes-commit-evidence/2026-10-03/validation.json)、[索引](changes-commit-evidence/2026-10-03/archive-index.json)、[269成员原始归档](changes-commit-evidence/2026-10-03/evidence.tar.gz)包含实际方法、命令/返回、首轮失败、最终四次SQLite数据、代码冻结、图形静态方案/实际诊断及下一批只读计划。归档945205字节，SHA256 `fa250cfda11687a20c8a46efc05cf759d8cd437cc38e8d6e9c4623c3e655e678`；每成员/来源回读相等，不含运行数据库、产品二进制或构建树。

最终418输入字典SHA256 `4b09296e5af3bd4fe1982b9858c22169c96b8d9b1bbdb52e88ea87d3d41acfe6`，算法是排序相对路径→文件SHA256的canonical JSON，非C4-source-v1。168跟进中181个实际产品、原失败数据及源输入前后未变。本说明阶段只改源码输入中的 `adapters/engine_api/README.md` 文档，其他417输入和实际产品保持验证范围；未追加产品重跑。

QG-02独立审查登记真实调用链、完整状态保护和scope，初版不存在的metadata键/未隔离未知字段问题及修正阶段保持。最终没有源码阻塞；审查者没有运行产品。初始格式失败保留，格式修正后的236手写C++、设计及diff门禁通过。

下一核心切片一次处理七个同类空参数入口：五个读取入口和undo/redo，共用合同来源及测试矩阵；`operations.get` 的条件上下文和兼容语义单独保留。28个仍手写解码的入口不代表28个功能缺失。不做非必要性能优化；真实Nastran、完整外部AI、SDK内部复制覆盖、已测原预算、完整SK/P0门禁保持此前范围和未通过状态。
