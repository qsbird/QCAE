# 已实现接口的证据对应关系：2026-10-03

本记录把已有实际证据绑定到20份AP合同的 `implemented_subset`，只新增有日期的状态。原 `reference_evidence`、L1库存、旧pending字段、设计接口和阈值保持原样；旧字段记录原文状态，新字段说明本次已实现子集的实际来源。机器可读对应关系和逐项SHA见[绑定清单](core-interface-evidence-bindings-2026-10-03.json)。这次整理没有运行编译、CTest、engine或GUI，也不提升完整L2、97项SK指标或P0验收状态。

统一接口与运行证据来自 `ef0df3bdd8c1b27330d71a596f16098cb6b6410a`，原410文件SHA为 `15d20f20fef88ed6d1b8637b97edf77143887728cb9c42427620b25429e65c03`。实际86六组完整回归为40/47/47/80/73/91，共378次测试执行；74消费者是独立编译链接数量，注册CTest为0，不加入378。原[统一结果](core-framework-evidence/2026-10-02/validation.json)和[归档索引](core-framework-evidence/2026-10-02/archive-index.json)保留原源码、命令和结果。

当前文档基于 `8bcec36874a92c80e273141e42263e4cbf67fd71`；它新增一份SQLite schema测试及CMake四行注册，其余409份原源码字节与source85相同。新增候选的纯核心40、本地54、SQLite48及双sanitizer schema单项结果保留在[后续验证](core-acceptance-evidence/2026-10-03/validation.json)，不能把旧378次执行改标为当前411文件候选。本文不把新schema测试扩大解释为所有原始record均可经IPC注入。

## 编译证据和运行证据

原74个消费者使用GNU C++20，独立编译链接公共头及其使用要求。20份AP映射其中18个不同的公共头消费者，AP04/05及AP01/15各共享一个。另有20个严格C++20类型消费者实际编译106项静态断言：88项函数类型、18项数据成员类型，警告0。类型断言处于未求值上下文，不能证明每个函数均被ODR使用、执行或通过全部运行时合同。

下表只概括BP98已核对的有界机制；完整原始日志和指向具体断言的行号由JSON逐项绑定。106项类型断言与这些运行场景保持不同分母。

| AP | 实际owner/公共头入口 | 类型断言：函数/成员 | 有界运行证据 |
|---|---|---:|---|
| [AP-01](module-contracts/AP-01.json) | application / RecordApplication | 7/0 | BP01正常打开与真实终止后恢复；90单例5项断言 |
| [AP-02](module-contracts/AP-02.json) | document/schema / RecordRegistry | 5/2 | 可空description的生成、编辑、重试、历史、保存与恢复；旧字段默认值 |
| [AP-03](module-contracts/AP-03.json) | geometry / geometry_features | 3/1 | BP03解析直线0/.5/1求值及保存后GeometryId；5项机制断言 |
| [AP-04](module-contracts/AP-04.json) | mesh/association / line_mesh_task | 1/1 | BP04几何替换、过期绑定、120mm重生成及reject_unmapped原子拒绝；90单例23项断言 |
| [AP-05](module-contracts/AP-05.json) | meshing_adapter / line_mesh_task | 1/2 | BP05实际后台离散生成M的11节点/10梁，坏候选不发布；90单例24项断言 |
| [AP-06](module-contracts/AP-06.json) | mesh/document / DocumentView | 5/1 | BP06稳定ID、容器顺序与solver编号分离，实际BDF语义映射；78项机制断言 |
| [AP-07](module-contracts/AP-07.json) | mesh/editing / mesh_editing_operations | 5/1 | BP07选定节点的批量平移、取消、单事务提交、Undo/Redo及重试 |
| [AP-08](module-contracts/AP-08.json) | organization/query / query | 5/1 | BP08部件/装配/集合返回原11节点ID及Undo；90单例6项断言 |
| [AP-09](module-contracts/AP-09.json) | physics/parameters / quantities | 4/0 | BP09单位、局部坐标及插值；3项机制断言与parameters原测试 |
| [AP-10](module-contracts/AP-10.json) | extensions/capability_package / engine_contributions | 9/0 | 90被动ON消费者枚举并调用core/operations/codecs/validation/ui/render六类；OFF公共服务原回归单列 |
| [AP-11](module-contracts/AP-11.json) | analysis / analysis_input | 4/1 | BP11精确LoadCase引用、引用删除拒绝、TaskId不冒充AnalysisId |
| [AP-12](module-contracts/AP-12.json) | operations/history / OperationRegistry | 7/0 | BP12完整M编辑及100轮Undo/Redo，修订增量201；90单例202项断言 |
| [AP-13](module-contracts/AP-13.json) | interaction / VtkView | 4/0 | 99实际配对10步；104新增30轮完整节点编辑生命周期，分开保留 |
| [AP-14](module-contracts/AP-14.json) | validation / analysis_features | 4/2 | BP14缺载荷检查的位置/版本、修复后零问题、旧检查过期 |
| [AP-15](module-contracts/AP-15.json) | project/exchange / RecordApplication | 4/0 | BP15完整M重复编号导入拒绝；90单例9项断言；旧六份迁移夹具另列 |
| [AP-16](module-contracts/AP-16.json) | visualization / RenderProjector | 6/2 | BP16实际VTK拾取的12个point/line/Tri3具名PICK场景 |
| [AP-17](module-contracts/AP-17.json) | ui / desktop | 3/0 | 96两个独立GUI进程，精确偏好/窗口状态/快捷键及文档中性 |
| [AP-18](module-contracts/AP-18.json) | clients/extensions / DesktopClient | 4/1 | 88的CLI3/GUI3/MIX3共9流程、72检查点、语义差异0；非真实外部AI |
| [AP-19](module-contracts/AP-19.json) | results / ResultService | 2/1 | BP19显式结果fixture的值、单位、来源及修改后过期/Undo后当前 |
| [AP-20](module-contracts/AP-20.json) | runtime/platform_support / TaskService | 5/2 | BP20实际2worker/8排队/第9拒绝、child86、10任务恢复且不重跑；45项机制断言 |

90原六例为BP01/04/05/08/12/15，各自原断言5/23/24/6/202/9，共269；BP04另有1项可选pending轮询断言。其来源字段原本就是410文件SHA。86的机制记录原来源标签 `mutable-diagnostic` 原样保留，来源绑定来自实际86源码、构建与原始完整CTest日志，不改写为冻结标签。

## 图形补证的边界

[96比较](core-framework-evidence/2026-10-02/layout96/native/comparison.json)实际GUI PID34865/34885分别退出0；两个侧面板宽度为280/360、历史面板高度170，窗口状态、几何、原始偏好、Ctrl+U及文档/历史均精确比较。使用显式私有Ini设置路径，修订0→0。86原窗口重建是同一QApplication中的支持证据，不能代替该独立进程证明。

[99比较](core-framework-evidence/2026-10-02/move99/native02/lifecycle-comparison.json.gz)实际完成cancel与apply各五步：进入、拾取、预览、取消或提交、离开。取消修订增量0，提交1、独有事务1；2次真实拾取、2次空白离开，离开后额外Apply写入0、只读观察器写入0。

[104比较](core-acceptance-evidence/2026-10-03/lifecycle30/native/lifecycle-comparison.json.gz)是新的固定后继脚本：同一GUI/engine实际30轮、300个有序步骤、60次真实拾取、60次空白离开及30次GUI Undo，30个新事务，修订13→73。每轮所有实体字段恢复、原13条历史前缀精确、新提交按cursor截断旧redo；最终cursor13、items14。原建线30次夹具及99记录不替换。离开限定为空白视口选择清空，dock关闭、Escape、通用ToolSession和完整SK10均不由此推导。独占engine由所有者SIGTERM收尾的实际-15是明确的清理状态，不写成所有进程退出0。

## 扩展试验与未完成范围

EXT01/02/03保留原独立作者、共同b740基线和6/4/10个产品文件变更范围，不把当前类型消费者视为三次扩展试验重跑。EXT04的[原双后端报告](c3-closure-evidence/package/c4-independent41/EXT04/report.json)保留不变：同一 `geometry_mesh` fixture经实际RecordApplication/EditSession/任务发布，memory和SQLite各10次成功、10次提交前取消、10次过期候选，以及100轮Undo/Redo、语义差异0、修订增量201。86原core与SQLite完整日志提供当前相同fixture的功能再验证。该夹具不是完整物理M；memory只重建应用并保留堆内store，只有SQLite真正关闭重开持久库。

EXT05仍为已知失败及未知覆盖。source49实际60样本中节点30项已测复制预算失败0，材料30项失败14，最大90501B高于原69632B；这不是source85重测。完整SDK内部复制保持unknown/null，原阶段阈值不改。R5分母仍为五个试验，不能排除EXT05后写4/4，也不能宣布5/5。

参考库存6113条保持5854条自有参考及259条SDK边界的原L1分类。全部设计接口的L2认定、完整运行时/ODR义务、OFF六类直接归属、格式变体的第二次实际编译、旧record-project v1生产者证明、真实Nastran/外部AI和完整97项SK/P0仍未完成。此次只把已实际发生的有限证明放到可以查找的位置。
