Qt SDK8 候选独立限定审查完成。冻结提交 b740b374c934f3b4624a21247b9a5deeb2ca6bf4 的实际 Qt 6.11.1 macOS 候选及有限行为证据没有显示回归；但发现通用 collector 采用前应修复的 P2 manifest 语义验证缺口，具体见下一段。审查者只读根源码、旧 SDK 和候选 SDK，所有新文件仅写入本目录；自行编译共两次，自行执行 GUI/native 进程为零。

具体缺口位于 `collector-optional-status.hpp:121` 的 fence 条件和 `:294` 的完整性结果：guard 只检查 version、failure_is_unknown、callback_abi_unchanged 和三符号名，不检查 noexcept_callback_boundary、original_C_declarations_and_scope_layout_unchanged 或 balanced_admission_stack_capacity。若一份 exact embedded manifest、外部 JSON、image SHA 全部互相一致，但 v1 fence 声明 noexcept_callback_boundary=false，现有逻辑仍会设置 failure_fence_v1_=true，之后 status=0/depth=0/exhausted=false 时报告 exception_boundary_complete=true，与 manifest 自身声明矛盾。旧 C 声明或 scope 布局声明 false 同样不被拒绝；callback ABI 布尔不能覆盖这些维度。当前实际候选两布尔=true、容量=1024，因此此前实际 GREEN 证据不失效，但不能宣称矛盾 fence 门禁已完整验证。

建议 guard 严格要求 noexcept_callback_boundary==true 及 original_C_declarations_and_scope_layout_unchanged==true；当前 v1 的硬编码 1024 admission 实现和实际容量探针对应要求 balanced_admission_stack_capacity==1024，并拒绝 missing、0、负值、非整数/字符串。若以后允许协商容量，先定义新合同及允许范围。为每个声明的 false/missing 和容量异常增加 exact-marker、有效 sidecar/image 的明确合成负面夹具，要求构造拒绝且不生成完整 snapshot。这里是可从源码条件与结果表达式直接复算的静态发现；这些新增矛盾 manifest 夹具尚未编译/实际运行，也不冒充实际 Qt failure 行为证据。

`collector-optional-status.hpp` 保留旧 `Callback` / `Install` C ABI。新状态、loss、depth 是 manifest v1 授权后的可选符号；SDK7 正常采集仍可使用，而 failure status、scope balance 为 null，exception boundary 为 false。没有 fence 却暴露部分扩展符号时，扩展不被调用且完整性降为未知。HarfBuzz 不被赋予 Qt 状态。

初始化先比对实际嵌入 manifest 与冻结 JSON，再核对外部 manifest SHA、实际 canonical Core 路径及 image SHA。macOS `dladdr` 显式核对 install/status/loss/depth 来源，随后检查已加载 Qt framework 前缀。实际候选 qglobal.cpp 的嵌入 macro 也与外部 manifest 完全相等。此结论覆盖该冻结候选；未扩大为任意外部 manifest provider 的通用认证承诺。

collector 把分配失败、非法站点/metadata 和计数溢出转为本地 exhausted 及 v1 sticky loss，而不返回已知零。run ID 和当前 ledger 筛选保持原路径；原始 source observations 继续排除在 contract total 外。`begin` 清空本次 facts 但不清除 process-sticky status。析构卸载 callback 的位置仍在 QLibrary 成员析构前，异常落入无分配的 loss 路径；FT Scope 的 active 指针恢复、失败 admission 的平衡 leave、callback guard 恢复和两线程退出得到相应有限实际探针支持。

实际无窗口证据按原 argv 独立重放：15 项实际 Qt 行为、7 项实际 image/manifest 绑定错误、5 项明确标注的合成 C ABI 协商夹具，全部 exit=0；27 份重放输出与保留日志逐字节全等，103 项记录 binary/log/image/manifest SHA 核验匹配。5 项合成夹具只证明协商分支，不证明 Qt 的实际 callback、exception 或数据行为。另独立只读核验根代理的 41b 实际混装负面输入：独特 install-id 的旧 QtGui 实际拷贝确已加载，实际第二 framework 被拒绝；第一次被 loader 重定向的准备 RED 保留且没有当成通过证据。

原始 `tests/qt_freetype_hash_probe.cpp` 1000-key 探针由冻结 Python 3.14 驱动编译及执行一次，`QT_HASH_SEED=0` 与旧实际执行一致。严格 compiler/runtime exit=0，实际 dyld 输出仅使用新候选 QtCore/QtGui。完整 parsed JSON、全部旧顶层字段和 33 个 actual_sites 的每个 entry/copy/write/unsupported 值，与 SDK7、旧实际 SDK8b 和旧归档 SDK8b 三份完整报告全等。保留原始报告、完整命令、stdout/stderr、binary SHA 和完整差异报告，不以 `passed` 字段代替比较。

第二次编译直接使用未改动 `tests/qt_font_backend_behavior_probe.cpp`，source SHA 与旧独立 review pin 相同；binary、qt.conf、原 argv、候选六个加载 image、字体文件和两份旧完整 64-case 报告均绑定。根代理随后独占执行 native64，exit=0、64 cases，actual report SHA 为 d290894be85d65e80969afbedf9aac714eb6d67b6753d322f8c6a8bac7a0855b。审查者只读其已完成证据并独立重算完整双射。

native64 主比较仅隔离 run_id、started_utc、qt_libraries_path、loaded_sdk_images 四个出处字段，保留全部工程字体、文字、backend、metrics、pixels、bounds、glyph ID、position、cluster、raw advance 及所有相关数组的原值和原序。原序完整相等为 false：对 SDK7 有 25 个 case、2977 处原序叶值差异；对 SDK8b 有 29 个 case、3459 处差异。所有差异严格只在 actual_engines/font_identities/glyph_ids/glyph_runs/positions/string_indexes 六个相关数组，且64个case每个完整 bundle 均能用同一个共享外层双射逐值对应；其余 case 字段及非出处顶层字段全等。因此补充完整 bundle 语义相等为 true，而 raw 原序相等仍为 false。没有按字段分别排序、删掉 glyph/position/font 值或用主序相等掩盖差异。

`whole_pipeline_owned_copy_coverage=unknown`，完整 C3=false，完整 SK12=false。并发 install/uninstall 或 callback 进行时销毁 context、任意无界计数流、FreeType 内部 stores、默认 CoreText gap、全部 Qt/HB stores、contract-total 去重以及非 macOS image origin 不在已验证范围。上述有限 failure-fence 与行为证据没有关闭这些边界；实际同次端到端账本由根代理后续串行执行。

主要证据：

- `full-behavior-compare.json`：完整原始 1000-key 比较及全部前后输入散列。
- `independent-evidence-audit.json`：15/7/5 原 argv 独立重放及103项SHA核验。
- `independent-native64-review.json`：根实际64项完整差异及独立完整bundle共享双射审查。
- `independent-mixed-prefix-review.json`：实际第二QtGui输入、install-id、原RED和最终拒绝日志审查。
- `native64-prepared/`：原源码第二次编译、qt.conf、根实际运行、原始完整报告、原序主比较和共享bundle补充。
