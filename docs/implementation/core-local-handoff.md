# 本机包运行说明

本机包由 `tools/stage_macos_package.py` 生成（见[M6本机包](m6-local-package.md)）。最近一次实际验证的包对应 `f2d2e7f`：265项适用回归及重定位后的SQLite、双MCP、真实桌面验证通过；该包尚未随后续源码刷新。完整SK、真实求解/完整AI、性能和P0发布仍待验收。

下文用两个变量表示本机位置，按自己的环境设置；真实路径、数据库和运行目录不提交Git：

```sh
qcae_bundle="<包目录>/QCAE.app"
qcae_run="<仓库>/runtime/core-local"   # runtime/ 已被 .gitignore 忽略
```

## 启动同一持久化engine

在一个终端以前台方式启动，并明确提供SQLite工作库；若该目录已有工程，换一个目录：

```sh
mkdir -p "$qcae_run" && chmod 700 "$qcae_run"
"$qcae_bundle/Contents/MacOS/qcae-engine" \
  --socket "$qcae_run/engine.sock" \
  --workspace "$qcae_run/workspace.sqlite"
```

每个工作库及端点只使用一个engine；GUI、CLI和MCP连接该端点。省略 `--workspace` 时为内存模式，不能声明持久化。

## GUI与CLI

```sh
"$qcae_bundle/Contents/MacOS/qcae-desktop" \
  --socket "$qcae_run/engine.sock" --workspace "$qcae_run/workspace.sqlite"
```

工作区有几何建线、线网格、材料/截面、赋属性、节点移动、分析检查、文件发布及fixture结果工具。成功导出BDF或显示fixture结果不代表实际求解通过。

CLI每次从stdin读取一个API 1.1请求并输出一个JSON响应；`--no-start` 保证连接已启动的engine：

```sh
printf '%s\n' '{"api_version":"1.1","request_id":"handoff-capabilities","operation":"capabilities.list","parameters":{}}' | \
  "$qcae_bundle/Contents/MacOS/qcae-cli" --socket "$qcae_run/engine.sock" --no-start
```

首次创建工程：

```json
{"api_version":"1.1","request_id":"handoff-create","operation":"project.create","requested_version":1,"idempotency_key":"handoff-create-1","parameters":{"name":"Core local"}}
```

读取响应中的 `document_id`、`document_epoch` 和修订，后续文档请求显式携带；写入需要 `expected_revision` 和幂等键。一次逻辑动作重试保留相同键/参数，不因网络超时猜测提交结果。输入和版本规则见[实际wire合同](../../adapters/engine_api/README.md)。

## MCP连接

stdio桥需要本机已有的Python≥3.10，解释器不在包内：

```sh
"$qcae_bundle/Contents/MacOS/qcae-mcp" \
  --python "<Python 3.10+ 解释器>" \
  --endpoint "$qcae_run/engine.sock"
```

桥不创建第二份业务状态，工具列表由engine的 `capabilities.list` 动态生成。实际外部AI客户端、模型及完整编排场景尚未验收。

## 保存、关闭与恢复

`project.save` / `project.save_as` 可省略或使用空字符串路径，保留原应用的位置选择及错误行为；首次使用应显式给出绝对工程文件路径。正常打开使用 `{mode:"normal",path:"绝对工程路径"}`；工作库重启后的明确恢复使用 `{mode:"recover"}` 且不携带path。

`project.close` 明确选择 `keep_recovery` 或 `discard`。正常打开生成新的文档身份和空历史；恢复保留身份、更新epoch并保留有限历史，旧epoch请求被拒绝。恢复不等同于重新打开最后保存的快照。

## 平台范围

包内非系统依赖已闭合，重定位及本地adhoc签名验证通过；只实测过arm64/macOS 26.6.2，旧系统、其他架构及公证发布未验证。
