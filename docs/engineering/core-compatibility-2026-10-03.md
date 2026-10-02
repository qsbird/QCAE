# 核心兼容与接口证据补充：2026-10-03

本轮新增声明的 `QCAE-RECORD-PROJECT v1` reader兼容回归，实际执行历史生产者的物理模型迁移，并为20份已实现接口合同绑定既有原始证明。实现改动仅为[新测试](../../tests/record_project_v1_compatibility_tests.cpp)及CMake四行注册；原503行schema测试及其余409份冻结源码字节保持不变。不据此宣布完整SK、C3/C4或P0通过。

## 实际测试

候选412文件的SHA为 `d2b507a1bf1c19336731a2a1014baa554d66210ba269dea09b96793fc427c41e`，基于 `8bcec36874a92c80e273141e42263e4cbf67fd71`。新源码537行、SHA为 `c4a6fdaf8f69ce19c304fccdb88483438399bc0985157a73a71626ff790c4bdb`。实际[四配置执行完成收据](core-compatibility-evidence/2026-10-03/compatibility120/attempt-01/complete.json.gz)及[逐文件冻结](core-compatibility-evidence/2026-10-03/compatibility120/attempt-01/candidate.json.gz)保持原命令和来源。

| 配置 | 实际结果 | 范围 |
|---|---:|---|
| 纯核心Release | 40/40 | Qt/VTK/SQLite关闭的完整注册集合 |
| 本地Qt Debug | 54/54 | SQLite关闭，真实IPC完整注册集合 |
| SQLite headless Release | 49/49 | 完整集合，含两份新增持久状态测试 |
| ASan/UBSan SQLite | 1/1 | 仅本轮v1兼容测试；LSan未测量 |

四组均实际退出0、自有编译警告0、严格C++20。测试执行次数保持配置分母，不按不同配置的重复案例累计为唯一测试数；原六配置378次验证仍属于原source85，不改标本轮412文件候选。

新测试由实际几何操作、后台网格TaskService和应用事务生成完整33条记录的M，明确序列化为合成reader-format1输入。正常打开保留全部实体ID、字段、项目ID和内容状态，创建新DocumentId/Epoch、revision0及空历史。一次真实应用事务验证新ID跳过输入中预设的公开候选ID冲突且不覆盖原记录，Undo恢复完整M；正常保存写当前format2，原输入文件字节不变。

真实SQLite关闭重开及显式恢复保留M与redo历史，更新Epoch。待恢复文档首先拒绝正常打开，单列生命周期guard。另在明确discard后、无活动或待恢复文档的状态中，四类future-format/截断/尾随字节/非法record各同键执行两次，均达到具名解码原因、保留非空持久host/metadata逻辑行及generation且没有成功打开事实；这八次解码拒绝不声称同时保留一个可恢复M。普通与双sanitizer各实际执行一次。

[准备范围](core-compatibility-evidence/2026-10-03/v1-preparation117/ready.json.gz)明确使用当前entity schema且不带v2 owned-row后缀；没有将当前TaskService事实或历史捏造为v1内容。可达Git历史没有发现record-project v1生产encoder，因此该合成输入仅验证现有reader声明，不能计入历史旧程序或原R0六份夹具。

## 真实旧物理模型迁移

本轮在私有目录实际构建历史提交 `62e2b1d804e025636caf8fc1d49cd207ee1f6677`，源码前后相同、构建退出0。重建engine/CLI的SHA分别为原冻结生产者的 `b4f2cca68f2c83abdfd6d404f70f0d638613dc9890a69cd8a00623d5945b9799` / `78f32e39651588b9fd77aefb85e56dfabb82d2c25222c3531a9e8217fc583825`。初始构建收据预填了“新binary预计不同”的字段，该预期不成立；[实际逐项比较](core-compatibility-evidence/2026-10-03/legacy-physical117/actual-binary-comparison.json.gz)明确更正，原收据保留。

实际旧engine经公开IPC预览/提交创建11节点、10梁、材料、截面、载荷、约束、部件、装配、两集合及旧Analysis，保存为真正 `QCAE-PROJECT v1` 并正常重开；[旧生产者结果](core-compatibility-evidence/2026-10-03/legacy-physical117/old-native-02/result.json.gz)保留全部字段、引用、来源及BDF输出。

当前source85实际engine首先拒绝旧profile的普通打开和错误旧摘要，持久行不变；显式 `project.migrate_profile` 后逐项比较185个旧实体顶层字段（非标量叶计数）、109个旧实体间引用及25个来源编号。独立CSV读回新旧BDF卡片，核对11GRID/10CBAR连接、坐标、210000MPa/.3材料、100/833.333/833.333/1400截面、末端(0,-1,0)N和首端六自由度约束。幂等重试、同键不同旧摘要冲突、同一engine内SQLite显式恢复、另存当前格式及正常重开均通过，项目ID保留，原旧输入及原六份golden字节未改。见[实际当前迁移结果](core-compatibility-evidence/2026-10-03/legacy-physical117/current-native-01/result.json.gz)与[只读核对](core-compatibility-evidence/2026-10-03/legacy-physical117/post-run-readonly-review.json.gz)。

旧版本没有GeometryLine或独立LoadCase。迁移只增加文档化的imported Mesh并保留旧直接载荷引用；本次是完整旧物理子集的迁移，不能冒充当前33记录几何/载荷工况图迁移或完整BP15。这份当前迁移没有关闭SQLite连接或重启engine，不能单独证明跨进程恢复。两次actual owned engine均由采集器SIGTERM后回收，literal exit -15，不据此误判业务失败；首次私有旧采集脚本漏传 `project.open mode` 的真实INVALID_INPUT记录保留，修正后的新目录重跑成功。未修改旧生产程序或原R0输入，未提交新运行数据库。

## 能力包关闭的实际观察

本轮新增OFF观察精确绑定原source85和原OFF86生产engine，不能改标本轮412文件候选。新私有消费者实际调用生产贡献callback、规则、操作注册及四类通用显示投影，`qcae.nastran` owner不存在、专用UI调用被拒绝、通用Node validator接受合法坐标且拒绝NaN；Node、梁、Tri3及GeometryLine投影保留，未启动文档或任务。

实际OFFengine使用明确SQLite工作库，24个实际IPC请求中8个专用请求全部UNSUPPORTED_CAPABILITY，declared profiles为空；通用analysis.check仍可调用，建线、版本化显示资源与Undo均通过。QtCore/QtNetwork/stock SQLite运行image已实际核对，owned engine PID97851经SIGTERM -15且回收，无GUI创建。见[原始专项完成记录](core-compatibility-evidence/2026-10-03/off117/results/observe-02/observation-complete.json.gz)、[实际公共调用](core-compatibility-evidence/2026-10-03/off117/results/observe-02/public-observations.json.gz)和[实际IPC观察](core-compatibility-evidence/2026-10-03/off117/results/observe-02/ipc/observations.json.gz)。

首次public peer实际退出1，错误为私有GeometryLine夹具将revision设为0，触发已存在的minimum1校验，engine尚未启动。新successor02只改该值为1、所有断言保留，在新目录构建和观察均退出0。更早observer把最高接受wire版本当成实际packet版本的静态假设已在独立successor01纠正，原输入仍保留；没有声称未运行原observer也出现过实际失败。

这证明关闭包后的可用性和通用服务行为。宿主codec/profile指针、独立Nastran export validator及RenderService registration owner仍没有直接枚举证明，不以三个硬编码false补出六类结果，也不把编译后仍存在的工厂函数调用当成宿主注册证据。原ON六类调用、原OFF73项回归及本轮新增观察分别保留来源。

## 已实现接口的对应关系

[20份接口绑定](core-interface-evidence-bindings-2026-10-03.md)新增有日期的implemented_subset证据，原reference_evidence、L1库存、旧pending值及全部设计接口保留。29个便携proof逐一核对压缩和解压SHA；74公共头消费者、20个类型消费者/106静态断言和有界BP运行各自保持分母、来源与限制。EXT04仅归并已有geometry_mesh双后端功能证明；EXT05材料复制预算已知14/30失败及SDK unknown保留，未做性能优化或把R5分母改为4。

有限源码QG与文档/旧迁移独立审查均未发现提交阻塞，见[新测试审查](core-compatibility-evidence/2026-10-03/v1-review122/review.json.gz)及[文档/旧迁移审查](core-compatibility-evidence/2026-10-03/review121/review.json.gz)。静态审查没有被计为额外产品执行。

## 仍需接续

当前reader-v1声明已有直接合成兼容证明，历史旧物理M已有真实生产者迁移证明；不存在的历史v1 encoder、完整33记录旧图迁移、全设计AP的L2/运行时/ODR义务、能力包六类私有owner直接归属、第二个格式变体的实际编译及其余97项完整SK仍未完成。性能、SDK内部覆盖、真实Nastran数值闭环与完整外部AI场景继续保留后续门禁。当前任务优先完成核心框架，非必要性能优化暂停。

全部本轮原始文本、方法、原失败、配置和测试日志按原始及gzip双SHA归档，见[归档索引](core-compatibility-evidence/2026-10-03/archive-index.json)。没有将旧PASS改绑新源码，也没有提交运行数据库、二进制或SDK凭据。
