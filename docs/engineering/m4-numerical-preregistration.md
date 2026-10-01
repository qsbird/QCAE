# M4 悬臂梁真实数值验收预登记

对应 TST-F07、TST-A08/09/10、TST-I07/08 和 TST-P06。登记发生在真实求解
运行之前；当前没有 Nastran 执行或数值通过证据。输入和固定参考位于
`tests/fixtures/nastran-real-benchmark-v3/`；旧 Nastran/结果夹具不改。

基准为 1000 mm 直梁、20 个均匀 CBAR、21 个 GRID，端点完全约束，另一端
施加全局 -Y 方向 1 N。E=210000 MPa，两个惯性矩均为 833.333 mm⁴，面积
100 mm²。两个惯性矩相同避免截面轴切换带来的歧义；所有节点使用基本坐标。
PBAR K1/K2 留空，根据 [MSC Nastran 2022.1 Quick Reference Guide](https://documentation-be.hexagon.com/bundle/MSC_Nastran_2022.1_Quick_Reference_Guide/raw/resource/enus/MSC_Nastran_2022.1_Quick_Reference_Guide.pdf)
对应零横向剪切柔度，使用 Euler-Bernoulli 参考。该基准不能推断一般几何/大变形。

参考使用声明后的实际数值 I=833.333，不能暗改为未舍入的精确方形惯性矩。
尖端 Y 位移为 -PL³/(3EI) ≈ -1.904762666667 mm，Z 转角为 -PL²/(2EI)
≈ -0.002857144 rad；支座反力 +Y 1 N，支座反力矩 +Z 1000 N·mm。其余分量
为零。输入参数、每节点解析公式、十进制参考、绝对/相对容差及源文件 SHA-256
冻结于 `reference.json`。每分量均使用 `|实际-参考| ≤ 绝对容差+相对容差·|参考|`。
缺分量、非有限数、错方向、错单位、错工况和错误映射必须拒绝；不能只比较范数。

实施计划：先增加受控 F06 位移/单点约束力解析，保留 T1/T2/T3/R1/R2/R3
与不同的平移/转角、力/力矩单位；有限输入、唯一编号和页/工况状态严格校验。
依据 [MSC Nastran 2021 Getting Started Guide 的 F06 输出示例](https://documentation-be.hexagon.com/bundle/MSC_Nastran_2021_Getting_Started_Guide/raw/resource/enus/MSC_Nastran_2021_Getting_Started_Guide.pdf?save_local=true)。
合成输出只用于解析失败与公式检查，不能标记 external_solver 或数值验收完成。
实际运行必须使用 runner 已冻结的输入、配置、运行/产物摘要和导出映射，之后
分别记录执行、解析、数值结论，并测试物理修改、纯组织变化及 undo/redo。

首片实际结果：`qcae_nastran_static_result_tests` Release 编译通过，
`nastran_static_result` CTest 1/1 通过（0.52 秒），日志
`c3-closure-evidence/package/nastran-static-reader-build34.log`。测试是显式合成
解析输入，覆盖分页、D/E指数、六分量单位、冻结稳定ID映射、错工况、错单位、
非有限数、截断/缺失表、重复/未知编号、fatal与NUL拒绝。解析器只返回
`NastranStaticResult`，没有 external_solver 标签或执行/数值认证。生产
`analysis.start` 仍未接真实结果发布，能力保持不可用/部分状态。

独立复核修正：v1 输入没有通过现有受控 codec（CBAR X1 的整数 0 被解释为
G0，且 LABEL 尚不支持），原文件/摘要保留于 v1，不能用于真实求解。v2 移除
LABEL，并使用显式实数方向，case label 使用受控 LC1；数值参数、公式和容差
完全不变。v3 进一步将 Y 弯曲公式的截面惯性矩标签更正为 I1；I1/I2 实际
相等，所以输入字节、参考数值和容差不变，原 v2 也保留。F06 首版保留独立
因果失败；后续 MPC 表、分页/作业结束的 case
来源以及带符号非法 GRID 必须在重验中拒绝，不能继续沿用首片通过结论。

修正版实际验证：Release 目标编译及 `nastran_static_result` 1/1 CTest 通过
（0.46 秒），日志 `c3-closure-evidence/nastran-static-reader-test-build34d.log`。
同一测试通过生产 Nastran codec 导入 v3 四文件，要求 candidate、complete 且
issues 为空。独立 Release 探针复现的 7 个旧误成功全部拒绝，另外 16 个
ASA、分页、行边界样本符合预期；日志分别为
`c3-closure-evidence/package/nastran-static-independent-review-build34d.log`、
`c3-closure-evidence/package/nastran-static-asa-review-build34d.log`。
这些均为合成解析及输入兼容性证据，真实 F06 版本兼容、执行及数值核验仍待实测。

2026-10-01 build38 增加纯 C++ `compare_preregistered_cantilever`。比较器只读取
原 `FrozenAnalysisInput`，核对受信适配器提供的完整注册 ProfileRef，要求物理
参数、21 节点/20 梁拓扑、梁方向、载荷与约束严格匹配 v3；不读取当前模型。
结果需要完整基本坐标 SUBCASE 1、逐 GRID 稳定 ID/原 solver number、六分量
位移及唯一支座六分量反力，检查全部 132 项，返回每项实际值、参考、误差、
容差和匹配结论。零参考仍采用预登记绝对容差，不能只比较尖端或向量范数。

首轮测试误将导出中非阻塞的名称/格式归一化提示视为失败；原日志
`c3-closure-evidence/package/static-validation-tests-build38c.log` 保留。
修正后要求完整产物且无 blocking issue，不改变物理、参考或容差。
独立审查随后发现同 ID/版本但不同定义摘要可进入比较，已增加受信注册三元组
参数与同步修改摘要/签名头的拒绝回归。修正版 Release 1/1（0.60 秒）及
ASan/UBSan 1/1（0.72 秒）通过，见
`c3-closure-evidence/package/static-validation-tests-build38e.log`、
`c3-closure-evidence/package/static-validation-sanitizers-tests-build38e.log`。

独立只读探针用 90 位十进制从原 `reference.json` 和四输入 SHA256 重建
全部参考及容差，实际 517/517 checks 通过。参考值最大差 0 ULP、容差最大差
1 ULP；覆盖全部零参考正负 inclusive/nextafter 边界、全部非零参考相对
容差内外，以及单位、工况、方向、映射、非有限值、溢出和 Profile 摘要拒绝。
日志及独立参考为
`c3-closure-evidence/package/static-reference-independent-517-build38.log`、
`c3-closure-evidence/package/static-reference-independent-decimal132-build38.json`。
这些只证明数学 helper 与受控输入边界，仍没有真实求解器或工程数值验收。
持久数值报告的生产接线正在下一切片中实施，不回写或覆盖旧运行的 `not_run`。
