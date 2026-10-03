# 核心框架本机交接

本机已生成并实际验证 `/Users/qs/Documents/ChatGPT/QCAE/out/local-delivery138-02/QCAE.app`。对应实现提交 `f2d2e7f`，265项适用回归及重定位后的SQLite、双MCP、真实桌面验证通过，范围见[验证记录](../engineering/core-local-delivery-2026-10-03.md)。当前是本地核心框架交付切片，完整SK、真实求解/完整AI、性能和P0发布仍待验收。

## 启动同一持久化engine

在一个终端以前台方式启动，并明确提供SQLite工作库。选择自己的运行目录，下面示例会在仓库忽略的 `runtime/core-local` 中创建新工作库；若该目录已有工程，使用其他目录。

```sh
qcae_bundle="/Users/qs/Documents/ChatGPT/QCAE/out/local-delivery138-02/QCAE.app"
qcae_run="/Users/qs/Documents/ChatGPT/QCAE/runtime/core-local"
mkdir -p "$qcae_run"
chmod 700 "$qcae_run"
"$qcae_bundle/Contents/MacOS/qcae-engine" \
  --socket "$qcae_run/engine.sock" \
  --workspace "$qcae_run/workspace.sqlite"
```

每个工作库及端点只使用一个engine；GUI、CLI和MCP连接该端点。独立engine省略 `--workspace` 时仍为内存模式，不能声明持久化。运行锁、数据库/WAL和工程快照属于本地数据，不能提交Git。

## GUI与CLI

在另一个终端连接已启动的engine：

```sh
qcae_bundle="/Users/qs/Documents/ChatGPT/QCAE/out/local-delivery138-02/QCAE.app"
qcae_run="/Users/qs/Documents/ChatGPT/QCAE/runtime/core-local"
"$qcae_bundle/Contents/MacOS/qcae-desktop" \
  --socket "$qcae_run/engine.sock" --workspace "$qcae_run/workspace.sqlite"
```

工作区已有几何建线、线网格、材料/截面、赋属性、节点移动、分析检查、文件发布及fixture结果工具。真实求解器仍需独立配置和数值验收；成功导出BDF或显示fixture结果不代表实际求解通过。

CLI每次从stdin读取一个API 1.1请求并输出一个JSON响应。`--no-start` 保证示例连接上面的同一engine：

```sh
printf '%s\n' '{"api_version":"1.1","request_id":"handoff-capabilities","operation":"capabilities.list","parameters":{}}' | \
  "$qcae_bundle/Contents/MacOS/qcae-cli" --socket "$qcae_run/engine.sock" --no-start
```

首次创建工程可发送：

```json
{"api_version":"1.1","request_id":"handoff-create","operation":"project.create","requested_version":1,"idempotency_key":"handoff-create-1","parameters":{"name":"Core local"}}
```

读取实际响应的 `document_id`、`document_epoch` 和修订，后续文档请求显式携带对应值；写入需要原合同要求的 `expected_revision` 和幂等键。一次逻辑动作重试保留相同键/参数，不因网络超时猜测提交结果。输入和版本规则见[实际wire合同](../../adapters/engine_api/README.md)。

## MCP连接

stdio桥要求已有Python≥3.10，解释器不在包内。本机验证使用以下明确路径；换机器时指向该机器已有的解释器：

```sh
"$qcae_bundle/Contents/MacOS/qcae-mcp" \
  --python /Library/Frameworks/Python.framework/Versions/3.14/bin/python3.14 \
  --endpoint "$qcae_run/engine.sock"
```

外部客户端应以这条命令建立MCP stdio会话。桥本身不启动另一份业务状态；本轮两个本地stdio客户端发现68个可用工具并共享同一engine。实际外部AI客户端、模型及完整编排场景尚未验收。

## 保存、关闭与恢复

`project.save` / `project.save_as` 调用原工程保存服务；可省略或使用空字符串路径，继续保留原应用的位置选择及错误行为。首次使用应显式给出自己的绝对工程文件路径。正常打开使用 `{mode:"normal",path:"绝对工程路径"}`；工作库重启后的明确恢复使用 `{mode:"recover"}` 且不携带path，两者行为不同。

`project.close` 明确选择 `keep_recovery` 或 `discard`。保留恢复后可以结束自己启动的前台engine；重启仍指向同一SQLite工作库，再按发现合同请求recover。正常打开生成新的文档身份和空历史；恢复保留身份、更新epoch并保留有限历史，旧epoch请求会被拒绝。恢复步骤不等同于重新打开最后保存快照。

## 环境与接续范围

包内非系统依赖已闭合，重定位及本地adhoc签名验证通过；仍只实测本机arm64/macOS26.6.2，声明最低macOS26，旧系统、其他架构及公证发布未验证。包、manifest和私有重定位副本留在本机；Git只提交源码、说明和文本/图像证据。

最新源码 `88942c0` 已补齐 `changes.commit` 的生成契约及真实SQLite拒绝/重放/恢复，见[本轮证据](../engineering/changes-commit-contract-2026-10-03.md)；此前六类贡献见[原来源范围](../engineering/package-contributions-2026-10-03.md)。本页本机包仍为 `f2d2e7f`，尚未刷新，不能据此使用新版提交发现元数据。保留旧二进制的图形隔离诊断69原数据行通过，原累计压力失败和完整图形门禁仍未关闭。后续成组补齐七个读取/历史入口契约，继续图形生命周期调查与核心正式验收。真实Nastran/完整外部AI和性能验收另行接续；SDK复制未知和既有已测预算失败仍保留原约束，不把本地交接标为完整P0。
