# 能力包六类贡献：实现与验证边界

2026-10-03，实现提交 `6eab0ca2bd3d6fa7fb76ae148e73e555f7c0b388` 已将 Nastran 六类贡献的发现信息接到生产宿主，并验证实际绑定与调用。初始五配置270项中269通过、1项桌面选择超时；原上限复验仍失败。完整图形回归、SK及P0继续未通过。本轮优先核心框架质量，没有增加依赖或非必要性能优化。

## 实际注册决定发现信息

生产 `capabilities.list` 增加 `package_contributions_version:1` 和 `package_contributions`。目录来自同一装配、冻结记录表和实际注册的 OperationDescriptor；不重新维护另一套操作目录，也不增加业务提交链。单一 `qcae.nastran` 在实际SQLite宿主中的六类入口如下：

| 类别 | 实际发现 | 验证的调用 |
|---|---|---|
| core | 1个具名规则 `qcae.nastran.controlled_subset`；公共记录由 `qcae.model` 贡献 | C++通过目录索引直接调用规则，有效模型接受、不支持分析拒绝 |
| operations | 7个实际新增descriptor，逐项匹配全局name/version/schema/available | 同一TypedHost及实际IPC的UI、export、artifact调用 |
| codecs | 1个已选择的codec/Profile绑定 | 实际BDF导入、编码和已发布BDF重新导入 |
| validation | 1个拥有式 `qcae.nastran.export` v1回调 | 原 `model.export` 实际调用传入回调；错误分析在任务/文件副作用前拒绝 |
| ui | 1个自身注册且可用、只读的 `nastran.ui` v1 | IPC响应指向同一export/Profile/单位，全表/历史/文档前后相等 |
| render | 3个工厂实际投影：point、line2、geometry_line | C++直接调用及IPC二进制显示资源核对实体ID、坐标、连接与版本 |

规则名称必须对应实际追加数量与索引；重复/空ID、无效validator、重复绑定/工厂及未知/外来/写入型UI引用拒绝。操作归属从各贡献者实际新增descriptor获取；受信任旧匿名规则仍可注册，但不冒充具名发现条目。目录只覆盖静态贡献的实际注册，宿主自身的生命周期/传输等入口仍通过全局操作目录发现。

codec/Profile和validator保留协调器同一State。释放协调器包装后，拥有的端口和回调仍可使用；释放最后拥有者后State可释放，无新增强引用循环。回调被同一export handler实际使用，保留原context/Profile/replay/revision检查及冻结输入、任务和文件发布顺序。注册及UI校验未完成时不能读取目录；生产宿主仅在TypedHost成功构造后对外公布目录。这不宣称装配拒绝先于原socket监听或工作库初始化。

## 有界实际证据

新C++用例通过实际registry注入拒绝validator，两次同键调用保留精确错误、完整存储行、代次、模型和历史，且未访问TaskService或发布文件；其中Store是测试替身。独立SQLite IPC用例有69条逐请求/响应记录，检查所有逻辑表和blob、只读UI、实际BDF导入、三类显示、validator拒绝、异步导出最终manifest/文件哈希及七类物理字段/引用重新导入相等。它没有公共入口的直接core-rule负例；该负例在C++中直接调用真实注册规则验证。

默认Nastran包OFF，以及编译ON但只选择公共贡献的生产测试宿主，都未发现 `qcae.nastran`、Profile或专有服务；导入/导出缺codec时明确拒绝，公共创建、几何、查询与undo仍运行。OFF不意味着所有Nastran源文件均未编译，直接包适配单元测试与默认宿主选择是不同范围。

| 初始配置 | 实际执行 | 结果 |
|---|---|---|
| Core Release，无IPC/存储/桌面 | 40 | 40通过 |
| Local Debug，Qt IPC | 55 | 55通过 |
| Desktop Release，SQLite/Qt/VTK | 96 | 95通过、`desktop_selection`超时180.03秒 |
| Package OFF Release，SQLite/Qt IPC | 76 | 76通过 |
| Sanitized Release，三个适用适配测试 | 3 | 3通过，ASan/UBSan；LSan未测 |

五配置构建均严格C++20、最终编译警告0。设计、236份手写C++格式及diff检查通过；独立QG-02审查无未解决源码阻塞。首编译因误引用另一源文件的局部 `qs` 失败，修正为既有json_ledger UTF-8转换并重新编译；保留第一次候选和日志，后继复用本轮首次失败的新建151缓存，没有改写更早已完成的构建目录。

审查发现固定证据目录使第二次运行必然失败，随后只改Python测试的证据目录为独立子目录。同一已编译产品连续两次CTest通过，旧flat证据保持字节相同，两个新子目录互不覆盖。初始270项与最终416输入的唯一差异是该Python文件；不能说最终416字节又跑了一次完整矩阵。初始结果加一次选择失败复验、两次IPC重复，总计273次CTest执行：271通过、2超时；原生诊断另列，不加入此分母。

## 当前图形阻塞

原 `desktop_selection` 在相同源码、原180秒上限复验再次超时。相关五份GUI/测试/VTK源码与上次265项候选相同，但旧120.83秒通过不能代替本次通过。单独的 `latestTreeSelectionSurvivesViewRefreshAndEnablesMeshTools` 原生诊断约1.8秒退出0；整套原生诊断在180秒终止，未使用强杀。

整套时间记录显示约107.95秒进入该用例；150秒自有进程采样确认主线程在 `vtkHardwareSelector::SavePixelBuffer → glReadPixels → AppleMetalOpenGLRenderer` 等待。更早采样在正常Qt等待/事件处理；这是实际停顿位置，没有证明完整根因，不能把单slot通过写成整套修复。后续继续定位图形上下文/窗口释放路径，保留原选择语义与原测试上限。桌面工作流另有两项真实AI场景因缺 `QCAE_AI_UNITS_SNAPSHOT` 跳过，不能记为AI通过。

## 冻结、归档与接续

初始416输入规范路径→SHA256 JSON摘要 `b05c22c2c04e191aa5e8f4638d482b9f05e5d4d33a6335c7e8a1ba8df7b07d7a`；最终仅测试目录修正后的摘要 `36e23a74ebe7fde0c7d51f784613a1c0a2e3e2cf814b5599048fef86ddcb6fbb`。两者都不是C4-source-v1算法。实际环境为macOS26.6.2/arm64、Apple clang21、Qt6.11.1、SQLite3.53.4、已有VTK9.7及Python3.14.0，无新增依赖。

[202成员索引](package-contributions-evidence/2026-10-03/archive-index.json)和[终态登记](package-contributions-evidence/2026-10-03/validation.json)保留源字典、十份修改源码/原始Python、方法、命令/退出码、完整CTest日志、SQLite前后原始行、显示资源、发布BDF、重复运行、首编译失败、全部图形超时及调用栈。逐成员读回及原件再读相等；[独立归档审查](package-contributions-evidence/2026-10-03/archive-review.md)未发现虚假通过或来源混用，审查者未运行产品/测试。不含DB、产品binary、socket或整套build目录。归档SHA256 `44c387e1b2d2322ec54aa3f4c8f81d21fc4f42177c4f451ec19f52dd5895ca1f`。

本轮对应SK-11/BP-10的六类注册/调用及关闭包相关子集，不关闭完整SK-11语义指标、其他SK或P0。已有本机包仍对应 `f2d2e7f`，见[交接](../implementation/core-local-handoff.md)，本次没有刷新包。旧265/378等证据保留其原来源。本轮文档随后只改变416输入中的adapter README，另做文档门禁，不冒充重跑产品。

下一核心接口切片集中 `changes.commit` 的输入发现与版本拒绝，保留原提交/幂等/历史链；图形回归阻塞继续登记并推进。真实Nastran/完整外部AI、性能、完整SK/C3/C4/P0仍待验收。SDK内部复制覆盖保持unknown/null，仅从C4启动暂移；已测约束及旧材料14/30预算失败保留，未以本轮小模型推断性能。
