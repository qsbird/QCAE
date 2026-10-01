# BP-01—20 具名代表核对

2026-09-30。本表逐条列出合同断言及其具体测试，不把一个 CTest 总通过数拆成 20 项。当前所列运行均为 `mutable-diagnostic`；正式判定必须由主代理在同一冻结源码上重跑并绑定来源。C4 扩展、GUI 和包关闭构建的最终证据由对应负责人补入。

完整 M 固定为 11 节点、10 梁、E=210000MPa、ν=.3、Area=100mm²、I1=I2=833.333mm⁴、J=1400mm⁴、端力(0,-1,0)N、固定123456、部件/装配/两集合、一工况和一分析。最新 `M-second/M-golden.json` 保存全部33个原始实体记录，其IPC代表套件5/5实际通过；早期 `M-first/` 四项诊断继续保留。工作流比较通过显式 ID 双射保留每一个字段、连接顺序及引用；名字、单位和物性均不被忽略。

| ID | 具体断言/测试 | 当前证据与仍需完成的部分 |
|---|---|---|
| BP-01 | `skeleton_representative_ipc_tests.py`：完整M保存关闭、正常打开DocId变化、真实进程kill后恢复DocId不变/Epoch变化、全部实体相等。 | `M-first/` 已实际通过；`skeleton_workflow_ipc_tests.py` 的三轮完整CLI也各覆盖。需冻结重跑。 |
| BP-02 | 可选字段扩展应验证生成、编辑、查询、undo/redo、保存恢复及旧schema默认。 | 属于EXT-01实验及最终集成后重跑；原有optional字段测试不能代替新增字段实验。当前未登记完整通过。 |
| BP-03 | `skeleton_representative_tests.cpp` 调生产 `evaluate_line`，u=0/.5/1分别0/500/1000mm，真实.qcae重开保留GeometryId。CLI工作流也调用生产 `geometry.evaluate_line`。 | 最新 `mechanisms-fourth/` 具名记录已实际通过。 |
| BP-04 | `line_mesh_regeneration_tests.cpp`：endpoint1000→1200、binding stale、同10段更新坐标间距120及绑定revision；不同segments的unmapped外部引用拒绝。F21另以实际SQLite/后台任务覆盖set/constraint/INCLUDE/source。 | 产品公开再生入口已存在。具名测试须列入最终回归；F21诊断10/10已通过。不得用此前“新建第二份网格”的路径代替再生。 |
| BP-05 | 完整M由真实后台离散器生成，11节点/10梁逐坐标/连接比对。`skeleton_representative_tests.cpp` 对实际已生成worker candidate分别注入empty和self-loop，拒绝且model/history/revision不变。 | 正向 `M-first/` 与最新负向 `mechanisms-fourth/` 合起来覆盖；单独正向记录不代表完整BP-05。 |
| BP-06 | `skeleton_representative_tests.cpp`：独立固定seed6101/6102/6103打乱EntityId、solverNumber、containerIndex；真实query和BDF输出/读回，通过namespace/number显式双射比对每条有序连接、属性/载荷/约束引用及全部固定物性。native记录另保留LoadCase→Analysis及Geometry/Mesh关联。 | 最新 `mechanisms-fourth/` 已实际通过；`BP-06-export/` 保留实际输出文件。 |
| BP-07 | 同时平移node4/5各(0,1,0)，只2条记录、1个commit、1次undo完整恢复。 | 属于EXT-02批量命令。现有连续调用两次 `node.move` 不能计此项通过。当前未登记完整通过。 |
| BP-08 | 完整M part/assembly查询原11node；Fixed/Loaded两set精确引用端点；修改Loaded后undo全部33记录恢复，无重复node。 | `M-first/` 已实际通过。 |
| BP-09 | `skeleton_representative_tests.cpp` 与 `parameter_tests.cpp`：1m=1000mm、210GPa=210000MPa、身份basis+origin(10,20,30)的(1,2,3)→(11,22,33)、表格x=.5→1.5。 | 最新 `mechanisms-fourth/` 记录具体数值，已实际通过。 |
| BP-10 | 活跃包需core/operations/codecs/validation/ui/render六种实际贡献；关闭包构建并启动，包操作不可用且公共工程操作可执行。 | `engine_assembly_tests.cpp`、`nastran_package_tests.cpp`、`nastran_disabled_ipc_tests.py` 等须和同源关闭包构建日志共同绑定。当前表不凭活跃包结果宣称关闭包通过。 |
| BP-11 | `analysis_check_ipc_tests.py`在完整M验证LoadCase/Analysis正确引用、删除被引用force拒绝、AnalysisId不等于TaskId。 | 具名实际CLI/IPC断言已存在；最终冻结回归由主代理归档。 |
| BP-12 | `skeleton_representative_ipc_tests.py`：完整M node5 y+=1；100轮每次undo和redo比较全部33记录；总revision增量精确201。 | `M-first/` 已实际通过。原geometry-only历史夹具作为额外回归保留。 |
| BP-13 | `c3_tool_lifecycle_tests.cpp` GUI enter/pick/preview/cancel/leave→enter/pick/preview/apply/leave，30次，cancel0修订/apply1修订。 | GUI负责人归档30个实际运行；纯CLI preview dispose/cancel不能顶替GUI工具生命周期。 |
| BP-14 | 完整M移除工况force引用，分析check精确1条missing-load；Issue定位该Analysis；修复后0，旧Issue stale。 | `analysis_check_ipc_tests.py` 与 `skeleton_workflow_ipc_tests.py`三轮真实CLI均覆盖。`workflow-cli-first/`已实际3/3。 |
| BP-15 | 完整M保存/正常打开；旧格式迁移≥6冻结夹具；BDF真实输出/读回物性和连接；duplicate numbering原模型/修订不变。 | M保存打开由本轮工作流、M BDF/readback由BP-06覆盖。`BP15-duplicate-first/` 真实当前完整M先导出已验证产物，再以新增INCLUDE重复原GRID编号；`changes.preview(command=model.import)` 返回IMPORT_REJECTED及duplicate_number，完整33记录/历史/DocId/Epoch/revision全不变。valid输入另实际返回empty-model conflict，明确当前public importer只准空模型。六冻结迁移夹具的具体最终日志仍须绑定；不把局部证据宣称整项完成。 |
| BP-16 | 点/线/Tri3各4项PICK，实际backend visible及through、ID集合全等。 | 点/线原图形测试保留；Tri3属于EXT-03后最终集成。当前未登记12/12。 |
| BP-17 | 三面板固定布局尺寸、快捷键改绑、重启恢复、序列化golden相等、model revision0增量。 | 最新layout测试已含真实shortcut preference改绑及重启恢复、配置字节/模型/历史相等。固定三面板尺寸的显式设置及golden核对仍待GUI负责人补齐并跑最新实际日志；当前未以旧layout-only通过代替完整BP-17。 |
| BP-18 | 同文档GUI/CLI/薄脚本交替，3模式×3轮，同一权威模型，ID双射以外0语义差异。 | 纯CLI完整流程 `workflow-cli-second/` 3/3，每轮8个raw及canonical检查点，跨run0差异；此前 `workflow-cli-first/` 仅7点，保留作早期诊断。`skeleton_workflow_compare.py` 严格核对九轮及八阶段，每字段/有序引用全比对；`workflow-comparison-partial.json` 明确GUI/MIX未齐，不能当完整验收。 |
| BP-19 | 实际M产物绑定R，数值/位置/单位/来源/映射逐项比对；改变对应物理输入后state stale，fixture标记保留。 | 原 `../results/` CLI/script2轮及本轮F22/三轮完整CLI均实际覆盖；始终不计真实求解。 |
| BP-20 | `skeleton_representative_tests.cpp` actual child真实2workers/8queued，第9RESOURCE_LIMIT；保存真实state/event观测后 `_Exit(86)`；父RecordApplication恢复10任务interrupted，event序号严格增加，0模型事务。 | 最新 `mechanisms-fourth/` 实际通过。`mechanisms-third/` 保留前次测试失败：真实任务断言已通过，最后误用跨registry的diff抛异常；修正后精确记录字节及完整历史事实相等。原runtime手工running恢复fixture不能替代原始进程运行。 |

CLI工作流的 `workflow.json` 保存每轮所有断言、ID双射和8个完整raw/canonical语义检查点，`transcript.json` 保存实际每条CLI请求/响应。`M_baseline`、`selection_undone`、`repaired_M`、`normal_open_M`、`recovered_M`、`published_M`应相等；`selection_applied`仅node5 y=1；`stale_result_input_M`仅E变为200000MPa。用于GUI/混合比较时必须比较相同阶段。comparer只允许对检查点名称 `check_repaired`→`repaired_M` 建别名；不移除任何模型字段。`comparer-self-test.json` 的9条纯算法自检包含全ID改名/储存顺序重排的正例和8种字段/连接/引用/布尔类型篡改负例，明确贡献0次真实工作流运行。

这些夹具不运行真实Nastran求解器，也不连接实际外部AI。`.qcae`/SQLite runtime文件在临时目录中创建并清理，证据目录只保存输入、请求响应、结果和断言。
