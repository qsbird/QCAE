# M4/M5/M6 真实接入环境核查

以下是初次只读核查快照，不作为当前实现状态。2026-10-01 接续已完成
真实 MCP 薄桥/单次实际 AI 调用、本机 staging 运行，以及明确 test-only 的
本地 solver 执行适配。当前实际证据分别见
[M5](../implementation/m5-mcp-bridge.md)、
[M6](../implementation/m6-local-package.md)、
[M4](../implementation/m4-local-runner.md)和
[数值预登记](m4-numerical-preregistration.md)。真实 Nastran 程序仍未登记，
完整真实求解/数值及 AI 30 场景会话门槛未完成；原搜索结果不改写。

2026-10-01，只读核查。依据 baseline 1.1 的 REQ-13/15/19、ENV-02/05/06、
TST-F07、TST-A08/09/10、TST-I01—10 和 M6 退出条件。本轮仅创建本文；
不运行真实求解、不启动模型推理、不安装依赖、不修改外部仓库或全局配置。
未读取认证文件、许可证内容或 token，也未输出环境变量值。

## 结论与证据范围

| 项目 | 实际核查 | 当前结论 |
| --- | --- | --- |
| 真实 Nastran | 工作区源码与配置名称、标准安装目录的有限名称搜索、常见安装根和指定环境变量名称存在性 | 检查范围内未发现程序或已登记配置；无法登记厂商/方言/可执行版本，ENV-05 未完成。 |
| 实际外部 AI 客户端 | ChatGPT.app 内置 Codex CLI `--version`、本地帮助和经过脱敏的 `login status` | CLI 0.159.2 可运行，状态命令 exit 0、认证模式 ChatGPT；可复用已有缓存登录，暂不需要用户提供 API key。未验证在线模型调用。 |
| 项目 MCP | 核查开始时源码无 MCP 适配桥；用户配置仅有 node_repl/cua_repl，项目与已查父级无 `.codex/config.toml` | root 已认领 stdlib Python stdio 薄桥；仍需协议、真实工具调用和人机共享状态证据。 |
| 本地打包 | 当前 Release 产品的 `file`、`otool -L/-l` 和构建源码 | 已有 arm64 开发二进制；存在机器依赖路径，尚无可迁移安装包或跨系统支持证据。 |

`login status` 由 CLI 内部读取登录状态，核查程序仅发布 exit code 和认证模式，
原始输出未发布。它证明缓存凭据存在，不证明服务可达、模型权限或实际会话成功。
本机非敏感配置的模型为 `gpt-6.1-sol`、reasoning effort `xhigh`。

## Nastran 搜索与实现缺口

名称搜索范围包括 `/Applications`、用户 Applications、`/opt`、`/usr/local`、
`/Users/Shared`（最多七层），以及系统程序目录和 MSC/Siemens/Hexagon 常见
Application Support/安装根。未发现 `nastran`、`nastran.exe`、MSC/NX/Simcenter
Nastran 程序或对应安装包。指定 NASTRAN 路径类与 MSC/SPLM 许可证变量名称均
未出现在当前进程环境；没有读取其值。受系统权限保护的 Library 目录未越权访问。
这不是全磁盘或其他机器调查，不能排除未提供路径的安装。

当前 `NastranCodec` 注册 `qcae.nastran.linear-static` 0.2.0，
`configured=false`、`validated=false`。语义来源是仓库已记录的 MSC Nastran
2022.1/2024.1 文档，见 [受控矩阵](../implementation/nastran-subset.md)：
SOL 101、单个 SUBCASE、GRID/CBAR/PBAR/MAT1/FORCE/SPC1、基本全局坐标、
向量梁方向及受限 free-field/INCLUDE，单位显式 `mm-N-MPa`。文档版本不是
已安装求解器版本，codec 成功和 semantic readback 也不是真实数值验证。

源码入口定位：

- `profiles/nastran/src/nastran_codec.cpp::NastranCodec` 声明文本能力与未配置状态；
  `nastran_package.cpp::validate_nastran_export/verify_nastran_readback` 保护发布输入。
- `adapters/engine_api/src/nastran_contribution.cpp` 通过冻结输入、实际文件和最终
  manifest 发布产物；持久任务/产物回执可复用，不包含 solver 进程执行事实。
- `docs/contracts/operations.json` 声明 `analysis.start/analysis.results`，但核查时
  生产 dispatch/typed contributions 没有它们的真实 solver 实现。现有 QProcess
  调用只负责启动 sibling engine，不能作为 solver runner 证据。
- `features/results/include/qcae/result_fixture.hpp` 的 IResultReader 输入仍是
  ResultFixture，FixtureResultReader 和 `results.read_fixture` 只接受显式夹具。
  不能将外部文件伪装成 fixture 后改标签就算真实解析。
- `modules/runtime/include/qcae/task_service.hpp` 已有有界队列、取消/中断/未明状态
  和模型/产物二选一回执，可作为执行生命周期基础；还需真实进程身份、运行配置
  和执行/解析/数值核验分别落事实。

不依赖缺失程序、可继续实现的最小范围：

1. 为单一受控 Nastran 增加本地 RunConfiguration 的非敏感登记模板、确切版本
   探测合同和不匹配拒绝；核心只见通用值/端口，Qt process 留在 adapter。
2. 对冻结且已验证的 artifact 启动受限 argv 进程；隔离运行目录，记录程序身份、
   配置摘要、PID/启动身份、原 input/profile/map、退出事实和输出 manifest。
   不拼 shell、不因崩溃/未知结果自动重跑；取消与退出竞态沿现有 TaskService。
3. 针对选定输出格式增加真实 displacement/SPCFORCES reader；使用运行冻结映射，
   验证完整输出、工况、数量/单位/坐标基、有限数值和来源。受损/缺失输出不得成功。
4. 预登记悬臂梁方向、截面、假设、位移/反力/力矩参考与容差；增加取消、进程退出
   成功但输出损坏、旧模型结果、重启与 PID 复用的明确 test-only 替身回归。
5. 完成以上实现后，真实 Nastran 门禁仍需真实程序与数值运行；替身覆盖不替代
   TST-F07/A08/A09/A10，也不将 profile `validated` 提前设为 true。

确需用户提供的最小非敏感信息：**可调用 Nastran 的绝对程序路径、厂商/方言与
实际版本、运行 OS，以及必要启动参数/位移与反力输出格式说明或公开样例**。
许可应在本地可用；不要求发送许可证串、账号、密码或 API key。收到确切路径后
才能区分实际兼容版本与当前 MSC 语义矩阵；不能静默换成另一 solver。

## Codex CLI 与项目 MCP 的可执行接续

两个安装入口已区分：

- `/opt/homebrew/bin/codex` 指向 npm 0.120.0 包，但其 darwin-arm64 vendor 程序
  不存在，`--version/--help` 实际 ENOENT；本轮没有修复或重装它。
- `/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex` 的实际版本为
  0.159.2；ChatGPT.app 非敏感 bundle 版本为 26.928.21956 (12404)。

本地 `exec --help` 明示 `--ignore-user-config` 只忽略用户 config，认证仍使用
原 CODEX_HOME；`--ephemeral` 不保存 session rollout，`--json` 输出 JSONL，
`-c` 接收运行时 TOML 值。CLI 支持 stdio MCP 的 command/args。以上也与
[OpenAI Docs 命令说明](https://learn.chatgpt.com/docs/developer-commands?surface=cli)、
[MCP 说明](https://learn.chatgpt.com/docs/extend/mcp?surface=cli)一致。
[官方认证说明](https://learn.chatgpt.com/docs/auth)区分 ChatGPT 缓存登录与 API-key
计费方式；本次只探测状态，没有触发推理。

以下是薄桥完成后的命令模板，路径仍需替换；本轮仅在尾部加 `--help` 验证参数
顺序能被本地 CLI 接受，未执行 PROMPT，也未证明 MCP 握手成功：

```sh
'/Applications/ChatGPT.app/Contents/Resources/codex-cli/bin/codex' \
  --disable plugins --disable apps --disable remote_plugin --disable hooks \
  --ask-for-approval never \
  exec --ignore-user-config --ephemeral --json --sandbox read-only \
  --model gpt-6.1-sol -c 'model_reasoning_effort="xhigh"' \
  -c 'mcp_servers.qcae.command="python3"' \
  -c 'mcp_servers.qcae.args=["/ABS/PATH/bridge.py","--endpoint","/ABS/PATH/engine.sock"]' \
  -C '/ABS/PATH/QCAE' - < '/ABS/PATH/session-prompt.txt'
```

四个 disable feature 的本地 `features list` 探测均为 stable/false、exit 0。
这是单次配置，不改全局文件。保留原 HOME/CODEX_HOME，不复制认证文件，不默认
使用 `--ignore-rules` 或绕过 sandbox。核查时无项目级 config；若以后加入它，
要重新检查所有已配置 MCP 和 plugin 来源，不能只凭此模板断言工具隔离。
Native 工具与 MCP server 不是同一隔离边界；任务提示和执行记录须证明业务操作
实际走项目桥，不能用 shell/直接 SQLite 写入充数。

薄桥应连接显式、已存在的 engine socket，不启动另一个 engine；从真实
`capabilities.list` 生成已实现 tools 和上下文约束，传 document/epoch/revision/
idempotency/profile 进入原提交链。stdin/stdout 只承载 MCP，diagnostics 走 stderr；
不记录环境变量或凭证。可先验证真实 AI 的建模/单位澄清/预览/undo/保存及冲突，
这些不依赖 Nastran。完整 M5 仍要求 TST-I01—10 各三次独立会话，以及至少一次
真实 AI→MCP→engine→Nastran→带单位结果核验链；实际工具调用和确定性状态事实
是证据，AI 文字回答不是。

数据去向需随会话登记：本机 stdio/Unix socket 留在本机；选择发送的提示、工具
schema/结果会进入实际 OpenAI 模型服务。首轮使用公开/合成悬臂梁，记录客户端、
模型、源码版本、原始 JSONL、工具与 engine 事实；不把私有工作库或凭证纳入证据。

## M6 支持矩阵与可逆打包准备

| 系统/能力 | 已有事实 | 发布口径 |
| --- | --- | --- |
| macOS 26.6.2 arm64 | 既有本机 Qt/VTK/SQLite 开发验证；本轮读取三产品 arm64 Mach-O | 当前开发验证系统，尚非完整 P0 发布通过。 |
| macOS 26.0—26.6.1 等未测小版本 | 当前 LC_BUILD_VERSION minos 26.0、SDK 26.5 | 二进制最低 target 不是 runtime 支持验证；不能承诺较旧 macOS。 |
| macOS x86_64 | 无对应本轮二进制/联合回归 | 未验证。 |
| Linux | 存储/发布使用 POSIX 路径；无 Linux 联合构建与图形证据 | 未验证，不从 macOS 推断。 |
| Windows 持久化产品 | sqlite_store.cpp 明确要求 POSIX file locks/atomic rename；产物也用 POSIX API | 当前没有 Windows adapter/安装包保证；`.exe` 分支不等于支持。 |
| headless engine | 当前 engine 链接 QtCore/Network 与系统 SQLite，不链接 VTK | 可独立准备数字业务包；真实求解仍缺运行适配/环境。 |

`build-c3-closure-desktop` 当前缓存为 Release，测试专用 SQLite observer 和 Qt SHA
配置均为空。其 engine/CLI 直接链接 Homebrew Qt 绝对 framework 路径；desktop
另有 `@rpath` VTK 和工作树 `build-vtk-deps/install/lib` RPATH。VTK 宏与 package
version 为 9.7.0，实际加载文件名为 `*-9.7.1.dylib`；打包闭包必须依实际 Mach-O
依赖枚举，不能仅凭版本标签猜文件名。源码暂无 install()/CPack/MACOSX_BUNDLE
规则，拷贝三个 executable 不足以得到可迁移产品。

可不等待真实 solver 的打包准备包括：在被忽略的 `out/` 下新建可删除 staging；
共置 desktop/engine/CLI，复制并校验实际 Qt/VTK framework/dylib 闭包和 Qt platform
资源；仅在副本改相对 loader paths/RPATH；登记许可证/版本清单及非敏感启动模板；
在隔离工作目录做重定位启动、同一 engine 连接、保存/重开/恢复 smoke。签名与
notarization 状态要如实登记，不能把本地临时签名当作公开发行通过。

包内不含真实 workspace、恢复库、认证、solver 许可、.omx、开发数据库或测试
observed engine。实际机器路径和私有运行配置留在忽略目录。正式 M6 仍依赖所有
P0 验收、真实 M4/M5、固定支持矩阵、数据迁移/恢复和性能分档/联合回归；当前 C3
材料复制门槛与 whole SDK unknown 不因可启动安装包而变为通过。
