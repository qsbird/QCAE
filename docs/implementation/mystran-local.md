# MYSTRAN 19.0.0 本地运行接入

状态：本轮已实现受控生产运行接入；实际构建、进程运行和数值验证结果见
[本轮验证记录](../engineering/mystran-local-validation-2026-10-03.md)。本文描述代码
及配置合同，不凭测试脚本中的预期值声明验收通过。对应 REQ-11/13/15/19/20、
TST-F07、TST-A08/09/11、TST-P06/07 和 QG-02 的有关范围。

MYSTRAN 是使用 Nastran BDF 输入的独立第三方求解器，不是 MSC Nastran。
本轮保留已有 MSC 2022.1/2024.1 和显式 test-only 路径，增加 MYSTRAN 19.0.0
本地执行配置及独立 F06 reader；架构依据见 [ADR-09](../architecture/mystran-backend.md)。
本轮不把完整 M4、SK 或 P0 验收标为完成。

## 输入和配置边界

共享 `qcae.nastran.linear-static` ProfileRef、现有 BDF codec、定义摘要及导出规则
保持原语义。MYSTRAN 门禁在生产 codec 导出和语义回读之后检查已发布的原字节，
只接受可直接运行的子集，不重写实数字段、编号、INCLUDE 或冻结输入。
原 `nastran-real-benchmark-v3` 手写夹具包含没有小数点的实数字段，不能直接作为
MYSTRAN 运行输入；集成脚本通过既有 `model.import`、`model.export` 和完成清单
发布链得到生产 codec 的规范化输入，再运行该已冻结产物。

当前额外门禁要求：

- root 和附属资源都是同一工作目录中的顶层小写 `.bdf` 文件；不接受路径跳转、
  子目录 INCLUDE 或隐式从其他目录寻找文件。
- 控制区限定 `SOL 101`、`CEND`、`SUBCASE 1`、SPC/LOAD、
  `DISPLACEMENT = ALL`、`SPCFORCES = ALL` 和完整 `BEGIN BULK`/`ENDDATA`。
- bulk 仅接受 GRID、CBAR、MAT1、PBAR、FORCE、SPC1，使用单行逗号自由格式；
  INCLUDE 使用受控的单引号文件名，且必须指向已核验资源。
- 非空实数字段必须带小数点、能精确读回有限数值，单字段最多 8 字符；ID 保持
  正整数。GRID/载荷坐标限定基本坐标，额外物理字段和不支持的卡片明确拒绝。
- 输出限定 SUBCASE 1、`basic` 坐标、`mm-N-MPa` 单位。

本机配置是显式可信资源，不能由普通业务请求替换 executable、argv 或版本依据。
以下为完整 v2 示例；路径按本机实际部署位置填写。`run_root` 必须事先存在，
可执行文件和运行根目录必须使用绝对规范路径。

```json
{
  "schema_version": "qcae.local-solver-config.v2",
  "run_config_id": "mystran-19-local",
  "executable": "/Users/qs/Documents/ChatGPT/QCAE/runtime/solvers/mystran-19.0.0/bin/mystran",
  "argv": ["{input_root}"],
  "solver_family": "MYSTRAN",
  "dialect": "MYSTRAN",
  "solver_version": "19.0.0",
  "version_evidence": "Declaration only; engine must probe the installed binary",
  "expected_outputs": ["work/cantilever.F06", "work/cantilever.ERR"],
  "run_root": "/Users/qs/Documents/ChatGPT/QCAE/runtime/mystran-local/runs",
  "test_only": false,
  "max_wall_time_ms": "30000",
  "cancel_grace_ms": "200",
  "max_output_bytes": "16777216",
  "result_reader": {
    "reader_version": "qcae.mystran.static-f06.v1",
    "resource": "work/cantilever.F06",
    "subcase": "1",
    "unit_system": "mm-N-MPa",
    "coordinate_basis": "basic"
  }
}
```

`argv` 只能包含这一项占位符，启动时展开为已冻结的 root 文件名。F06/ERR 的
文件 stem 必须与 root 一致；例如 `cantilever.bdf` 对应上述两个输出。
`version_evidence` 是声明文本，不是已经验证的事实。加载配置时只运行固定
`--version`，使用 `mystran.version.v1` 协议、C locale、私有探测目录、有界时间及
输出捕获。只有唯一的 MYSTRAN 19.0.0 厂家版本标头、正常零退出、非 synthetic
观察及原可执行文件身份匹配，才允许生产执行。探测证据及二进制身份摘要纳入
配置摘要；原 MSC 配置继续使用固定 `help` 和 `msc.help.v1`。

## 执行、解析和持久化路径

`analysis.start` 从原 artifact/task 行解析冻结分析，检查 document/epoch/revision、
ProfileRef、物理输入、导出编号映射及源完成清单。通过现有 codec 的导出与语义
回读、MYSTRAN 额外门禁后，唯一 engine 通过 SQLite/CAS 保存 queued task/run，
再由本地 runner 保存 startup intent 和 OS 进程创建身份。子进程经过已有闸门
等待身份持久化，随后使用固定 argv 直接 `execve`，不执行 shell。

每个 run 使用新的私有目录：

```text
run-<task_id>/
  input/           原已发布 artifact 的逐字节副本及清单
  work/            第二份相同的输入副本；实际 cwd 和 F06/ERR 所在目录
  tmp/             本 run 的绝对 TMPDIR
  runner.stdout
  runner.stderr
  .qcae-result/    后续已解析结果的独立发布目录
```

原 artifact 保持冻结；`input` 和 `work` 通过同一 LocalArtifactStore 完成清单与
SHA256 核验，退出收集阶段再次核验两份副本。MYSTRAN 使用同目录资源处理
INCLUDE，`TMPDIR` 指向私有 `tmp` 的绝对路径；环境只提供固定 LANG/LC_ALL 和
这一 TMPDIR，不继承用户环境。运行目录权限、墙钟、聚合文件数/字节和单文件
大小继续受现有 runner 约束。临时副本也计入 MYSTRAN 的运行输出空间预算。

执行、解析和工程核验是不同事实。MYSTRAN 输入错误可能仍返回退出码 0，
因此正常退出仅能进入原始产物收集。完整 F06/ERR/stdout/stderr 必须保留冻结
长度和 SHA256；ERR 及日志中的 ERROR/FATAL 诊断会阻止解析。正常的
`EPSILON ERROR ESTIMATE` 信息和已记录 WARNING 不被误判为致命错误。

独立 `read_mystran_static_f06` 要求 MYSTRAN 19.0.0 标头、明确
`OUTPUT FOR SUBCASE 1`、正确的全局坐标说明和 `GRID COORD T1 T2 T3 R1 R2 R3`
加 SYS 标头、每行坐标号 0、六个有限分量和完整 `END OF JOB`。位移必须覆盖
整个冻结 GRID 映射；反力必须映射到已知且不重复的 GRID。重复、缺失、截断、
错误工况、局部坐标、未知表头或无效数值不会成为已解析结果。位移/转角单位
分别为 mm/rad，反力/反力矩分别为 N/N*mm。

解析成功后，原执行快照、reader 版本、映射、各原始资源摘要、解析字段与
最终 manifest 通过现有 owned-row 和文件发布链保存；SQLite 意图、文件发布和
发布确认仍是独立步骤。`analysis.get_run`/`analysis.get_result` 可查询来源与
当前性；`analysis.validate_result` 另行使用原冻结输入执行悬臂梁数值比较，
持久化独立 numerical check。解析成功时 numerical 仍为 `not_run`，不能靠
Task succeeded 或零退出声称工程验证通过。求解及结果核验不增加模型修订或
undo 历史；旧结果不使用当前编号映射重新解释。

## opt-in 真实集成验证

默认测试不会自动搜索或启动本机求解器。显式提供真实二进制后，可配置本地
IPC、SQLite 和 POSIX runner 构建，并注册 `mystran_real_integration`：

```sh
cmake -S . -B build-mystran-local -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_SOLVER_LOCAL=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DQCAE_MYSTRAN_EXECUTABLE="$PWD/runtime/solvers/mystran-19.0.0/bin/mystran"
cmake --build build-mystran-local
ctest --test-dir build-mystran-local --output-on-failure -R '^mystran_real_integration$'
```

也可以对已有二进制手动运行同一脚本：

```sh
python3 tests/mystran_real_integration.py \
  --engine "$PWD/build-mystran-local/qcae-engine" \
  --cli "$PWD/build-mystran-local/qcae-cli" \
  --solver "$PWD/runtime/solvers/mystran-19.0.0/bin/mystran" \
  --evidence-dir "$PWD/runtime/mystran-local/acceptance"
```

脚本创建独立 v2 本机配置、SQLite 工作库与运行目录，通过生产 engine/CLI
执行导入、原子提交、实际导出、运行、结果发布与数值核验。它还检查幂等重试、
旧物理输入/undo、产物损坏拒绝、终态结果的引擎重启恢复和 CLI 同源读取。
日志、完整工具往返、真实产物、配置及报告保留在指定 evidence 目录，不提交
本机配置、数据库、生成构建目录或 solver runs。通过数量及失败证据仅在
[本轮验证记录](../engineering/mystran-local-validation-2026-10-03.md)登记。

## 本机包和仍未覆盖的验收

已检查的 macOS arm64 包位于
`runtime/solvers/mystran-19.0.0/downloads/mystran-19.0.0-macos-arm64.tar.gz`，
SHA256 为 `3d8740e23de60ce96499a5c3ad353039088a226365987a44237795938db9e928`。
其主程序 Mach-O 最低系统版本为 macOS 26.0；本机运行情况以实际验证记录为准，
不据此承诺较旧 macOS、其他架构或 Windows 可用。部署时保持包内 bin/lib 相对
关系及第三方许可证，不能只复制主程序。

此前独立悬臂梁输出包含三条 INCLUDE WARNING，原始证据保留在 runtime 中。
它们不会自动成为错误，也不能解释为任意 INCLUDE 布局均已正确验证。本轮仅
接受上述顶层同目录资源和语义回读范围；实际运行中的警告及资源摘要继续保留。

真实 MYSTRAN 进程的取消、在途 engine 故障/进程组恢复、GUI 与 AI 端到端流程、
完整数值场景及分档性能仍需要独立证据。既有 test-only runner 故障测试、终态
结果重启恢复、脱敏真实 F06 参考和本机独立求解证据不能替代这些门禁。
