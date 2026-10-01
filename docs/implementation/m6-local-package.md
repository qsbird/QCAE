# M6 本机 macOS 运行包准备

对应 REQ-19 和 M6 部署/恢复退出条件。实施计划先保护既有开发二进制：仅在
忽略的 `out/` 中创建新 `.app` 副本，拒绝覆盖已有目录；部署 Qt framework/QPA
资源、实际 Mach-O 依赖闭包与 VTK 许可；只修改副本 loader paths，再核查动态
依赖、临时签名和重定位 CLI/engine 生命周期。原安装、全局设置和实际工作库
不修改，observed 引擎与凭证不打包。

Qt 部署使用已安装同版本 `macdeployqt`，依据
[Qt 官方部署说明](https://doc.qt.io/qt-6/macos-deployment.html)。额外实际 VTK/第三方
动态库按 `otool` 依赖解析，副本改为相对 loader paths；验证未闭合即失败。
manifest 记录实际二进制/资源摘要及源码是否可变；临时本地签名不代表公证发行。

`qcae-mcp` 运行依赖已声明的 Python 3.10+。本切片不安装 Python 或复制用户
site-packages/认证；可通过启动器 `--python` 明确已有解释器。该前置条件须登记，
不能把包含 Python 薄桥文件的 `.app` 描述为包含 Python 运行时的完整包。

本机 staging 已实际验证（build33e）：依赖闭包 144 个 Mach-O 文件，418 个
文件摘要及本地签名校验保存在 `out/package-build33e/QCAE-manifest.json`。实际
包内 IPC 回归、M2 保存/另存/正常打开/强杀恢复、双 MCP 客户端与 CLI 共用
SQLite 全部通过。原生 Qt/VTK GUI 导入两节点一梁，取得真实显示包和截图，
退出 GUI 后 engine 保持存活，CLI 状态仍为两节点一梁。日志、GUI 二进制摘要和
截图保存在 `docs/engineering/c3-closure-evidence/package/mac-gui-smoke-build33e/`
及同目录 `mac-staging-*-build33e*.log`。GUI `--smoke` 不写用户布局。

原始失败均保留：macdeployqt 成功后缺 framework、重复库身份判定、自身 dylib
ID 误认为外部依赖，以及 Python 文件置于 MacOS 目录导致签名失败。现桥文件
部署至 Resources，launcher 支持该位置；SDK 原安装未修改。另保留沙箱禁止
本机 socket 的失败，与通过的本机运行日志分别记录。

这是持续变化源码的开发包；最终冻结源码仍需重打包和统一验收。C3/SK/C4、真实
Nastran/AI 全验收和固定支持矩阵完成
后才能作 M6/P0 发布结论；当前只验证本机 macOS arm64，其他系统未验证。
