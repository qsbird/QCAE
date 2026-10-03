# entity201 只读归档审查

状态：**READ_ONLY_ARCHIVE_REVIEW_NO_BLOCKERS**，findings为空。本代理仅用标准库只读元数据、原始日志、归档/原origin及源/产品字节哈希，没有执行产品、生成器、检查、构建、CTest或Git，没有修改源码、归档或197目录。

290个唯一常规UTF-8归档成员逐项匹配index，size/SHA256与290个原origin回读字节全部相等。归档955917字节，SHA256 `f920e94cdb39daf627ca459b0486fa3294dab9da2ede824bce914c7ac713eb3e`。JSON包含逐成员明细；没有数据库、二进制或构建树成员。

422个最终测试输入canonical字典 `7cca4e73fe40063582588ca9b301055b2b6ca7617b0fa91b3afc5a92824f55f8` 与repair-02候选一致；两次测试修复都只改变typed_host源码，最终hash `467c883d9c126d9e6fb8e6bf80a5652985ce9d908219ca8b5e553501efd02fbc`。当前421输入不变，唯一冻结输入文档差异是已声明的engine_api README；没有为文档重跑产品。

| 实际轮次 | 执行 | 通过 | 失败 |
|---|---:|---:|---:|
| matrix/attempt-02 | 261 | 257 | 4 |
| test-only-repair/attempt-01 | 4 | 0 | 4 |
| test-only-repair/attempt-02 | 4 | 4 | 0 |
| 累计 | 269 | 261 | 8 |

计数由13份原始LastTest.log逐测试块读取，与tests/ctest元数据逐名称/count/actual exit code匹配。初轮40/40、54/55、86/87、77/78、0/1；原四signature失败的实际文本为“Prepared operation signature does not match normalized input”。第一次修复后四个目标均Unknown operation失败，第二次local/ON/OFF/sanitized四目标actual exit0。八次历史失败保留，当前四独立目标无未解决失败；取消构建0 CTest不是产品失败。

17项产品当前size/hash相符，仅四个typed_host测试二进制较初轮改变，13项不变，原257通过保持其产品绑定。修复后没有新261/261全量矩阵。修改前六基线均actual exit0，单列不进入269或初轮261。13份完成compile summary均strict C++20/零warnings；ASan/UBSan仅typed_host，LSan未执行。

SQLite ON/OFF契约分别92/88请求：各geometry53观察、35省略/v1对＋18单版本请求、26成功/58失败/4冲突；ON organization另两对、4成功。逐响应request_id、参数/operation/version、实际exit与transcript一致，省略/v1完整envelope只差request_id，成功内外revision相同。所有before/after authority digest相等；初始SQLite record_state/store_rows/document/history的canonical digest与首观察一致，saved_content_state非空等于current，dirty=false/durable=true。ON/OFF各一组1003完整incoming refs保持唯一from及一致to，匹配实际响应。未重新打开live数据库或宣称physical WAL一致。

本地和SQLite ON/OFF raw日志含两个实际MCP stdio与CLI共享fixture通过记录，trusted local-user范围未冒充跨principal或真实外部AI。SQLite非空、多内部页引用与MCP共享Material保留各自证据范围。

报告/validation/index/执行状态当前与raw证据一致。审查初发现execution-status继承旧lookup全通过计数，root仅修正外部current块为初轮261/257/4、累计269/261/8、unresolved0/full_matrix_passed=false、freshfull NOT RUN，并留下status-correction.json；该文件与原origin相等，放在immutable290成员之外，归档hash未变。已修正观察单列，不留未解决finding。

197静态审查阶段pending文本原样保留，最后四通过由repair-02 raw日志承载。后续view plan文件可回读且NOT IMPLEMENTED/NOT RUN。旧图形累计失败、完整图形门禁、旧f2d包未刷新、真实solver/完整AI、性能、未知SDK及完整REQ-16/SK-04/C3/C4/P0仍开放；本审查不关闭这些范围。
