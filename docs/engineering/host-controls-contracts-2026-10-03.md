# 七个读取与历史入口的生成契约

实现提交 `89241496fa4f3b80886cd6a8560019686d5c6d4e` 已合入主工作区。五个读取入口和undo/redo共用生成版本1输入、闭合空参数schema与版本规则；相关五配置实际261/261通过。完整图形、SK和P0验收仍未关闭，本机包仍为 `f2d2e7f`，未刷新。

## 实现与权威路径

| 入口 | 实际输入DTO | 已有结果标签 | 最小上下文 |
|---|---|---|---|
| capabilities.list | CapabilitiesListInput | CapabilityCatalog | 无文档上下文 |
| project.current | ProjectCurrentInput | DocumentInfo | 无文档上下文 |
| project.status | ProjectStatusInput | ProjectStatus | document/epoch |
| model.summary | ModelSummaryInput | ModelSummary | document/epoch |
| history.list | HistoryListInput | HistorySnapshot | document/epoch |
| history.undo | HistoryUndoInput | ChangeReceipt | document/epoch/revision/key |
| history.redo | HistoryRedoInput | ChangeReceipt | document/epoch/revision/key |

`host_reads.json` 定义五个读取，`document_changes.json` 追加两个历史输入。所有schema为 `qcae.operation.<operation>.v1`，字段为空。IPC复用已有输入/版本/发现函数，省略或整数1选择已安装合同；非法版本为 `INVALID_INPUT`，其他合法uint32为 `SCHEMA_UNSUPPORTED`，字段均为 `requested_version`。额外字符串、数字、null输入拒绝在原应用调用之前。

输入验证后仍调用原 `MemoryApplication` → 单一 `RecordApplication`；undo/redo仍使用同一caller、WriteContext、幂等命名空间与persist-before-swap协调器。读取不要求写修订或变更键；历史写各提交一次，旧请求重放只报告原事实。没有修改应用、TypedHost、registry、存储或桥实现，也没有增加依赖、SDK公开核心类型或性能机制。

发现保留设计符号类型与query/model_write效果，用wire标签描述实际DTO/既有结果；生成内部效果read_only/document_write分别保留。结果标签不是新增的生成输出schema。status/summary原来已有正确手写空schema，此次统一来源与版本；其他五个入口补齐实际接受的闭合参数发现。`operations.get` 的条件上下文、忽略的original_mode及原返回路径继续保留。

## 实际验证

| 配置 | 实际执行/通过 |
|---|---:|
| 纯核心Release | 40/40 |
| 本地Debug | 55/55 |
| SQLite/Nastran ON，无桌面 | 87/87 |
| SQLite/Nastran OFF，无桌面 | 78/78 |
| ASan/UBSan typed_host专项 | 1/1 |
| 合计 | 261/261 |

同一冻结420输入完成所有配置，严格C++20、零编译警告；生成及公共头消费者在对应构建/CTest中检查。LSan未测。修改前四项既有C++/IPC/MCP基线通过；最初沙箱内C++通过、IPC因本地socket监听限制失败，原记录保留，随后同源同产品的已授权基线四项通过。这六次前置执行不计入261次CTest分母。

C++共用一个实际应用/假存储场景，比较完整record view/version、存储行、保存快照、metadata、history、generation和计数器；精确检查七个定义与发现、读取的完整既有结果、版本/形状/上下文拒绝、caller/key/replay及排除的lookup兼容。假存储不充当SQLite证据。最终独立源码审查无阻塞，审查者未运行产品。

两个真实SQLite场景各完成101次拒绝、5个读取的省略/显式版本对比、8次历史写、6次原commit/undo/redo重放。每次读取/拒绝/重放比较全部逻辑表、列、BLOB、generation、DocumentInfo和history。保存后的两记录模型在undo U → redo R → retry U中保持重做后的内容与游标；非法输入及不可重做拒绝后的同键可成功执行。旧签名、旧revision、unknown key和旧epoch拒绝，原三份结果仍可查询。每次独立进程recover后稳定ID/完整暴露字段/history相等、epoch更新、原事实保留；两个engine均SIGTERM退出-15，无物理WAL字节相等主张。

实际双MCP/CLI共享流程覆盖七个入口的完整engine元数据到工具schema/说明、参数和版本转发，以及历史原结果重放。可选requested_version仍是正uint32范围schema，安装版本为1；未人为加const或在桥内解码业务。既有save/open/close及其他回归在对应完整配置保留，不为七个入口复制生命周期。

## 证据与剩余工作

[机器记录](host-controls-evidence/2026-10-03/validation.json)、[成员索引](host-controls-evidence/2026-10-03/archive-index.json)和[原始归档](host-controls-evidence/2026-10-03/evidence.tar.gz)保留方法、源码/产品hash、实际命令/日志、SQLite观察和transcript、初始沙箱失败、格式前后源码、静态审查及后续只读方案。归档154成员、920290字节，SHA256 `216218dcb314ad62605e8d5c446e309b85a7370c2eed728278a6f2e960b228f0`；每成员和来源已回读相等，不含数据库、二进制或构建树。

冻结420输入字典SHA256 `fd726316a1bc0be0f282aaf0ed4dd8e2b82784e1d64e58c2921b74ae6272a3ce`，算法为排序相对路径→文件SHA256的canonical JSON，非C4-source-v1。说明阶段只有冻结输入中的engine_api README文档变化，其他419输入保留测试范围；没有为文档重跑产品。设计/格式/diff门禁与最终归档独立核对在收尾记录登记。

原累计图形180秒失败及旧二进制69数据行隔离诊断仍按[原证据](changes-commit-contract-2026-10-03.md)保留，当前源码没有新增图形执行或根因修复。下一步为 `operations.get` 的条件schema、兼容投影与MCP上下文发现，使用已有生成器并保留原应用路径；这是未执行方案，不能据此宣布完成。更广目录、真实Nastran/完整外部AI、性能、未知SDK覆盖、已测原预算及完整SK/P0仍保持此前范围。继续以核心框架质量为目标，不追加非必要性能优化。
