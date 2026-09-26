# R0 旧格式夹具冻结

本记录对应 [SK-04 旧格式夹具分母](../baseline/skeleton-acceptance.md)。已冻结六份由重构前真实 engine 经 CLI/IPC 生成的 SQLite 文件；这里只验证旧程序自身的正常打开、明确恢复和 redo 行为，**没有验证 R2 迁移，也不宣称骨架验收完成**。

这些文件是用户明确授权的不可变 golden 测试数据，属于 AGENTS.md 禁止提交运行数据库规则的明确例外。普通工作库、WAL/SHM、锁文件、构建产物和实际运行数据仍不提交。捕获只新增脚本、夹具和本文，未修改生产代码。

## 冻结来源

源基线为 `62e2b1d`，其 `src/`、`include/`、既有 IPC 测试辅助和 Nastran 输入与 `afb9829` 相同。捕获前使用现有 `build-desktop/` 中的旧 engine/CLI，并复制到临时目录固定二进制，未重新构建。源提交关联与二进制散列分别记录；没有把源提交信息伪装成嵌入二进制的构建证明。

| 程序 | SHA-256 |
|---|---|
| qcae-engine | `b4f2cca68f2c83abdfd6d404f70f0d638613dc9890a69cd8a00623d5945b9799` |
| qcae-cli | `78f32e39651588b9fd77aefb85e56dfabb82d2c25222c3531a9e8217fc583825` |

API 为 `1.1`。旧能力包为 `qcae.nastran.linear-static` / `0.1.0`，定义摘要为 `sha256:bb856be32336514d1aebf67b524ed0403664291cb4ba9e473e922ae7bb71b662`。未配置求解器；导出是内存预览，不是产物发布或工程求解验证。

清单位于 [manifest.json](../../tests/fixtures/legacy/manifest.json)，冻结每份数据库、预期 JSON、IPC 记录和四份 BDF 输入的散列、字节数、格式版本及 ProfileRef。该清单只声明 SQLite 封装版本 1、project 状态版本 1、workspace 状态版本 2 和 model 记录布局版本 1；没有增加迁移支持范围。当前实体记录没有逐条独立版本号，model 布局版本按旧 codec 记录。

## 六份输入

| fixture_id | 类型 | 文件字节 | 实体数 | 特征 |
|---|---|---:|---:|---|
| saved-empty | project | 8192 | 0 | 空模型，但工程身份和 SQLite 状态负载非空 |
| saved-beam | project | 12288 | 12 | 2 节点、1 梁、材料/截面/载荷/约束/分析、4 INCLUDE |
| saved-organization | project | 12288 | 15 | 梁模型增加 part、assembly、set，保留来源和 INCLUDE 父子图 |
| unsaved-workspace | workspace | 32768 | 12 | 从未保存；导入后材料修改至 200000 MPa；2 项已应用历史 |
| redo-workspace | workspace | 32768 | 12 | 从未保存；材料修改后 undo，当前 210000 MPa；cursor=1，2 项历史中第 2 项未应用 |
| all-supported-entities | project | 12288 | 15 | 11 类当前支持实体均非空，来源标识 7 项 |

11 类为 node、beam、material、section、part、assembly、set、include、force、constraint、analysis。组织模型与全类别模型采用同一冻结梁输入，但分别通过独立 engine 会话创建、保存，有独立身份与原始文件散列；两个条目分别满足不同最低覆盖条件。输入含 root→mesh/nodes→mesh/beams 与 root→properties 两条 INCLUDE 路径。

每份 `*.expected.json` 包含提交后的文档身份/修订/保存状态、实体全部公共字段、源编号、INCLUDE 成员/父子关系、全部出向引用、历史及 BDF 预览。redo 夹具另存首次修改后的 `after_redo` 快照。实体与引用按精确 ID 比较，不用 ID 双射掩盖持久身份变化。

## 一致性和路径策略

捕获先通过 IPC 执行 `project.close(policy=keep_recovery)` 完成文档关闭事务，再终止 owned engine 并等待进程退出。旧 engine 没有宿主 shutdown 操作或 SIGTERM 处理器，因此不称为 C++ 析构意义上的优雅退出，也不把这一步描述为故障注入测试。

只有写进程退出后才执行 SQLite `wal_checkpoint(TRUNCATE)`；要求返回 busy=0。随后用 SQLite backup API 生成临时一致副本，对比封装元数据、负载长度和负载 SHA-256，验证两份 `integrity_check=ok`。最终 golden 保留 checkpoint 后旧程序输出文件的精确字节，不用新编码器重写负载，也不直接复制仍有写进程的数据库。临时 backup 不进入夹具清单。

工程快照不依赖捕获时的保存目录。预期文档状态及 IPC 记录中的绝对临时保存路径仅保留为来源记录；正常打开时使用副本的新路径，并按产品语义生成新的 DocumentId/Epoch、revision=0、空历史。模型字段、实体身份、来源、引用和导出资源保持全等；导出响应的 revision 只作为会话元数据排除于模型语义比较。

两个恢复夹具从未执行 save/save-as，`saved_path` 为空，避免外部保存文件依赖。BDF 的路径为相对资源路径，实际内容已经保存在负载与冻结输入中。恢复保留 DocumentId/修订/历史，生成新 Epoch。

旧 SQLite 适配器在建立 writer 前用 READONLY 连接检查文件。将 WAL 模式工作库移到无 WAL/SHM 的新目录时，旧程序会报 `invalid SQLite metadata`。验证器先在**临时副本**上建立 SQLite 读写连接并完成读取，保持该连接到 engine 启动完成，以创建必要的空 WAL/SHM；随后关闭该连接。没有改 golden 的 journal mode、字节或内嵌路径。检查 golden 时使用 `mode=ro&immutable=1`，避免旁生运行 sidecar；这些文件已经完整 checkpoint，immutable 读取不遗漏 WAL 状态。

## 执行和结果

[capture_legacy_fixtures.py](../../tools/capture_legacy_fixtures.py) 固定检查上述二进制 SHA-256、临时复制二进制，并复用既有 CLI 帧辅助。真实 Unix 本地套接字运行需要允许本机 IPC 的执行权限。原捕获命令为：

```sh
python3 tools/capture_legacy_fixtures.py \
  --engine /private/tmp/qcae-r0-legacy-binaries/qcae-engine \
  --cli /private/tmp/qcae-r0-legacy-binaries/qcae-cli
```

再次验证已冻结的文件：

```sh
python3 tools/capture_legacy_fixtures.py \
  --engine /private/tmp/qcae-r0-legacy-binaries/qcae-engine \
  --cli /private/tmp/qcae-r0-legacy-binaries/qcae-cli \
  --verify-only
```

临时目录中的二进制不提交；换机器验证需提供符合散列的旧二进制及其可运行环境，不能拿重构后新二进制冒充。若要重放捕获，可用 `--output <新的目录>`；脚本复制冻结输入且拒绝覆盖已有 manifest。旧 engine 生成随机身份，重放复现操作和语义覆盖，不承诺新运行的数据库 SHA-256 与原 golden 相同。

[verification.json](../../tests/fixtures/legacy/verification.json) 记录六份夹具全部通过：数据库完整性、原始散列与字节数、旧程序正常打开/明确恢复、公开实体/引用/源编号/导出内容全等、恢复历史全等以及 redo 回放。每份验证前后 golden SHA-256 不变。project 适用分母 4，workspace 适用分母 2；这些是旧程序自回归结果，未来迁移仍需以全部六份冻结输入独立执行并记录。

另外在新临时目录完整重放捕获脚本，六份新生成数据均通过同一验证；原 golden 没有重新生成。`python3 -m py_compile tools/capture_legacy_fixtures.py`、设计检查、C++ 格式检查和 `git diff --check` 均通过。本切片不改 C++，未以重构后的构建结果证明旧格式来源。

可读性审查（QG-02）：脚本入口固定旧二进制并选择捕获或验证；`Session.call/commit` 只经既有 CLI/IPC 修改旧 engine 权威文档；`capture_closed_database` 负责退出后的 SQLite 备份验证与精确字节保留；`verify` 只在临时副本上打开、恢复及 redo。关键不变量是固定 producer SHA-256、至少六个输入且 project/workspace 分母非空、无持久写入绕过 IPC、golden 散列不变以及恢复历史保留。IPC 状态错误、busy checkpoint、散列/模式/语义不匹配均抛出失败；已有 manifest 拒绝被捕获覆盖。审查未发现混淆权威状态或未记录副作用的路径。
