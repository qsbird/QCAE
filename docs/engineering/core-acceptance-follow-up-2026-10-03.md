# 核心框架验收补证：2026-10-03

本轮继续按用户确认的核心框架质量目标补齐SK-03/BP-13的直接断言；不启动非必要性能优化。此前统一回归仍保留原来源和范围，见[2026-10-02统一验证记录](core-framework-validation-2026-10-02.md)。候选基于 `1e0197e`，仅新增一份SQLite测试和CMake四行测试注册，其余409份冻结源码字节与 `ef0df3b` 相同。本轮不把旧378项回归重标为新提交运行，也不宣布完整SK、C3/C4或P0验收完成。

新112构建和实际CTest结果：纯核心40/40，本地Qt入口54/54，SQLite完整48/48，新增schema双sanitizer测试1/1，首方编译警告0、严格C++20。完整命令、候选411文件摘要及每项原始日志见[结构化结果](core-acceptance-evidence/2026-10-03/validation.json)。

## 当前源码的保护区AST

根代理实际执行既有 `tools/check_protected_entity_ast.py`，使用 `ef0df3b` 的冻结源码和对应86桌面Ninja编译数据库。17个翻译单元、当前16种持久实体类型，通用保护区具体实体类型直接引用0，诊断错误0，实际退出0。未保存的违规 `records::Node` 注入被成功检出；原源码未改。

旧32/39报告的部分TU已与当前源码不同，其零引用结果不能直接导入。本次[实际报告](core-acceptance-evidence/2026-10-03/protected-ast/attempt-01/ast-report.json.gz)记录每个当前TU、实体schema、保护清单及libclang的SHA；[完整命令收据](core-acceptance-evidence/2026-10-03/protected-ast/attempt-01/complete.json.gz)保留工具、编译数据库及410源码的前后检查。

## 原生GUI的30轮完整节点编辑流程

104私有消费者在同一实际Cocoa窗口和engine上运行30轮，每轮依次完成进入、拾取、预览、取消、离开，以及再次进入、拾取、预览、提交、离开。使用实际QVTK鼠标输入、平台 `SelectAll` 键盘快捷键和文本输入；单步等待仍为30000ms。

实际结果为30/30轮、300个有序步骤、60次节点拾取、60次空白视口离开及30次生产GUI Undo。取消每轮修订增量0；提交增量1且各产生一个独有持久事务；Undo增量1。初始修订13，最终73。每轮完整M的所有实体字段均恢复；13条原历史前缀完整，旧redo在新提交时截断，最终保留一个未应用redo。离开后额外Apply写入0，观察器写入0、实际只读请求15160。

该流程使用当前生产 `node.move` 属性工具。离开具体绑定为空白视口拾取→engine空选择→属性草稿清空；dock关闭、Escape和通用ToolSession未由此测量。104固定脚本是明确的新补证，原冻结建线30次夹具及99单次配对证据保持原字节和范围。

实际GUI PID43032退出0；独占engine PID42912由所有者SIGTERM收尾，实际退出-15、已回收、无强杀。新消费者75个严格C++20编译条目，首方警告0。原生运行的前后源码、工具、SDK和原86构建事实一致。

[逐轮独立比较](core-acceptance-evidence/2026-10-03/lifecycle30/native/lifecycle-comparison.json.gz)、[实际原始观察](core-acceptance-evidence/2026-10-03/lifecycle30/native/SK10-BP13-node-move-lifecycle30.json.gz)、[执行完成收据](core-acceptance-evidence/2026-10-03/lifecycle30/native/action-complete.json.gz)和[固定脚本](core-acceptance-evidence/2026-10-03/lifecycle30/method/fixed-lifecycle30.json.gz)保留在仓库。早期私有静态检查把诊断文本误识别为代码调用的实际非零记录仍保留，不归为产品失败。

## Schema拒绝的应用与持久状态

原八类反例已实际执行，但 `record_document` 的该段断言只检查不可变视图，未直接检查真实应用历史和SQLite。本轮新增 [schema_rejection_sqlite_tests.cpp](../../tests/schema_rejection_sqlite_tests.cpp)，通过已有 `RecordApplication::execute`、真实编码的record、`Registry`和`EditSession`检查八类错误。完整33条记录的M由实际几何操作、后台网格任务和应用事务生成；保留4条历史、cursor3及真实撤销后的redo分支。

八类各执行同键两次拒绝，关闭并重开SQLite、显式恢复后再拒绝一次；每次比较全DocumentInfo、记录版本、完整模型编码、全部历史及SQLite逻辑行字节和generation，并检查不存在成功幂等事实。同一epoch两次失败没有消耗候选ID。每类还单独用恢复前的旧epoch发出应用请求，均在planner前拒绝且完整状态不变。

普通Release与ASan/UBSan的新测试均实际退出0；各为8类、24次schema拒绝及8次额外旧epoch检查。恢复本身产生新epoch和合法宿主元数据写入，不计为失败修改；LSan未测量。这是实际应用/SQLite断言，未声称八类原始record都可从IPC直接注入。[普通SQLite完整48项回归](core-acceptance-evidence/2026-10-03/schema/current112/attempt-01/sqlite-complete.json.gz)及[双sanitizer单项收据](core-acceptance-evidence/2026-10-03/schema/current112/attempt-01/sanitized-schema-complete.json.gz)保持各自范围。

首次111构建实际退出1：新测试缺少 `mkdtemp` 的声明头，测试尚未运行。仅补 `<unistd.h>`后在新112目录重新构建，八类拒绝断言及门槛未改；[原失败日志](core-acceptance-evidence/2026-10-03/schema/failed111/attempt-01/sqlite-build.stdout.log.gz)和原候选均保留。最终有限QG独立审查未发现阻塞问题，见[审查记录](core-acceptance-evidence/2026-10-03/quality108/final-candidate-review.json.gz)；静态审查与实际编译/测试分别记录。

正常 `project.open` 在内部仍有待恢复文档时首先拒绝 `DOCUMENT_ALREADY_OPEN`。因此不能用recovery-only入口的失败冒充八类schema解码失败；本轮不改变正常打开、恢复和丢弃的既有语义，也不新增通用record注入API。

## 仍需保留的范围

- 真实旧六份夹具已有运行证据。本轮Git历史核对未找到 `QCAE-RECORD-PROJECT v1` 生产encoder；首次该magic的实现已经写v2。reader仍接受v1/v2，因此v1兼容声明的直接证明缺口保持未完成，不能改当前文件版本号后声称来自旧程序。
- 真实旧生产者 `62e2b1d` 写 `QCAE-PROJECT v1`，可表达11节点/10梁及材料、截面、组织和直接力约束分析的物理M子集；无法表达当前解析几何、独立LoadCase和几何来源网格的完整图。当前迁移保留旧直接载荷引用，未生成LoadCase。原六份输入及分母保持不变。
- AP20均有实际owner/header和有界代表机制；参考库存仍是L1。全部设计接口的L2认定、OFF六类贡献的直接归属证明，以及格式变体的实际编译范围仍需分别核对，不能从74个公开消费者推导全部完成。
- EXT01/02/03原独立范围已有记录；EXT04已有双后端功能证据，待归并限定范围；EXT05已有已知复制预算失败与未测边界，仍不记5/5。本轮没有启动性能优化。
- 真实Nastran数值闭环、完整外部AI场景及其余正式SK/P0门禁保持后续范围。

只读核对来源见[102范围审计](core-acceptance-evidence/2026-10-03/audit102/missing-and-covered.json.gz)、[103接口与扩展审计](core-acceptance-evidence/2026-10-03/audit103/facts.json.gz)和[107旧生产者审计](core-acceptance-evidence/2026-10-03/legacy107/facts.json.gz)。107纠正102关于必有旧record-project v1生产者的先前假设；两份原记录分别保留，不覆盖旧证据。

证据按原始字节与gzip字节分别记录SHA，见[归档索引](core-acceptance-evidence/2026-10-03/archive-index.json)。598个归档文件均已逐项解压比对原始SHA；索引仅登记实际文件，缺少的结果不以目标值代填。

新测试可在普通SQLite构建或双sanitizer构建中独立重跑，无需GUI或IPC：

```sh
cmake -S . -B build-schema -G Ninja -DCMAKE_BUILD_TYPE=Release -DQCAE_BUILD_IPC=OFF -DQCAE_BUILD_STORAGE=ON
cmake --build build-schema --target qcae_schema_rejection_sqlite_tests
ctest --test-dir build-schema -R '^schema_rejection_sqlite$' --output-on-failure
```

Sanitizer复现另用新构建目录并设置 `-DQCAE_ENABLE_SANITIZERS=ON`；实际112运行使用 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。原生GUI补证的源、固定脚本、完整argv与封存输入保留在归档，依赖原冻结来源及本机Cocoa/Qt/VTK环境，不能改绑当前版本后继续沿用原PASS。
