# operations.get 归档独立核对 191

结论：**本轮归档、报告和 execution-status 的来源、计数及范围一致；没有发现阻塞项。** 只读检查使用 `/private/tmp/qcae-framework-delivery80`，未运行产品、构建、测试或 Git，未修改源码或归档。本审查仅写入本目录两份审查文件，不代表重新执行验收。

归档 `docs/engineering/operation-lookup-evidence/2026-10-03/evidence.tar.gz` 实测851451字节、138个唯一普通文件，SHA256为 `f8389d90c77b40a0b2ddde7b028795ca75e15f80c6850892cbe235d6f3544bbb`。成员集合与 archive-index 完全相等；138项成员大小/hash及138项索引原来源文件回读均相等，没有缺失、额外或重复成员。矩阵仅有 `attempt-01`，没有把基线或旧矩阵并入本轮分母。

| 本轮配置 | 原始 LastTest 执行/通过 | 失败 | CTest退出码 |
|---|---:|---:|---:|
| core | 40/40 | 0 | 0 |
| local | 55/55 | 0 | 0 |
| sqlite-on | 87/87 | 0 | 0 |
| sqlite-off | 78/78 | 0 | 0 |
| sanitized | 1/1 | 0 | 0 |
| 合计 | 261/261 | 0 | — |

五份 LastTest 原始测试名逐项等于 complete.json 的 executed_test_names，stdout 的最终 CTest汇总与上述数量一致。sanitized 的实际命令只选 `^typed_host$`，配置库存55项没有冒充执行55项；ASAN_OPTIONS明确关闭泄漏检测。五配置均配置 `QCAE_BUILD_DESKTOP=OFF`；五份编译摘要均 strict_cpp20=true、警告列表为空。报告及 current_core_framework 正确限定为四份完整非图形配置加一项ASan/UBSan专项。

candidate.json 的输入字典实际421项，独立重算canonical排序相对路径→SHA256 JSON得到 `c81d0e14a4b42bd7b86a2eb417a4e4531bf3a6e90604e428434a52c511960db4`，与报告、validation.json及execution-status一致；不是C4-source-v1。freeze.json的421项与候选字典完全相等，记录实现提交 `cf2f4c447800ffecffa31263007fe21cce548b20`。未通过Git重新证明工作区HEAD。文档阶段README差异在报告及机器记录中单列，未冒充产品重跑。

两份实际SQLite观察文件各含35条lookup_reads（全部success）、31条lookup_refusals（30 failed、1 conflict），全部66条 complete_state_unchanged=true。每条请求及完整响应均能在该场景两进程原始transcripts中找到完全相等的配对；跨进程重复request_id没有被当作全局唯一键。原101条拒绝、5组读取、8次历史写及6次重放保持单列。两场景的恢复保留document_id与history、更换epoch；discard后normal-open产生新document_id/epoch，内容状态与保存模型相等。两份记录均保留engine退出码[-15,-15]；无新的强杀、物理WAL字节相等或真实solver/AI主张。

baseline的IPC、MCP、durable MCP、协议及typed_host五项分别有退出码0、PASS原始stdout及空stderr，baseline/complete和validation明确不计入261。基线复制桥hash为 `586c2ed9ab01b8b3eb0001f954209aebaa4046edbab147cdd5f1120c171c8fa8`；本轮四个复制桥记录均为 `6b3f94d2605b4aae81b3bc4c6e84e61262401a8532ba1631e70de426ff6da257`，与421候选及当前桥源码相等，旧基线与本轮产品来源没有混用。

归档包含产品/C++/MCP来源审查和最终独立QG-02审查。最终独立记录保留REVIEW186-01变量覆盖、REVIEW186-02生命周期顺序两项测试问题及运行前修复事实，unresolved_blockers为空；其执行边界仍是静态审查，没有充当运行证据。

报告、validation与execution-status current_core_framework均保留：原图形180秒累计失败和门禁未关闭、旧f2d本机包未刷新、旧包证据仅属旧来源、真实solver/完整外部AI/性能/未知SDK覆盖及已测预算失败未关闭，完整REQ-16、SK-04、C3/C4、SK/P0没有通过。历史七入口261结果仍留在独立历史对象中，未增加本轮261计数。未发现将本切片范围扩大为完整验收的声明。

收尾新增next-plan.md/json位于138成员归档之外；其开头及执行记录明确entity.query/entity.references计划188尚未实施。execution-status的下一入口及计划路径与此一致，没有把计划算作本轮实现或测试通过。
