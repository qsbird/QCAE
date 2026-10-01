# R 结果夹具交付与可读性审查

状态：BP-19 / AP-19 的结果夹具读取切片已通过核心与真实 CLI/脚本 IPC 定向验证。F22 在此覆盖指纹、位置、编号映射三个负样本，每个入口各执行 10 次；不代表 F01—F22 全部完成，也不代表完整 SK/P0 或 C4 已验收。结果来源始终为 `fixture`，数值不是实际求解结果。

## 交付与范围

[`R-displacement-v1.json`](../../../../tests/fixtures/results/R-displacement-v1.json) 保存 R 的固定数值与必需元数据。实际 engine 创建 M 并发布 Nastran 输入产物后，测试将模板绑定到该产物的冻结物理签名与原始编号映射；完整绑定数据留在 [CLI R](cli-R-bound.json) 与 [脚本 R](script-R-bound.json)。节点 0 值为 `(0,0,0)`，节点 10 值为 `(0,-1,0)`；quantity=displacement，unit=mm，components=[X,Y,Z]，location=node，coordinate_basis=global，case=LC1，frame=0，source_kind=fixture。

只支持该静力节点位移夹具读取。没有求解器执行、完整后处理界面或真实 AI 接入声明。冻结范围与当前导出范围一致，包含所有节点、材料、截面、梁及所选载荷和约束；因此对未使用物理记录的改变也可能保守地判为陈旧。

## QG-02 调用链与权威状态

入口由 [`results.json`](../../../../schemas/operations/results.json) 生成 typed DTO。`results.read_fixture` 要求 document/epoch/expected_revision/idempotency_key，副作用为 `auxiliary_write`；`results.get` 只读取已保存事实。

[`result_api.cpp`](../../../../adapters/engine_api/src/result_api.cpp) 只解析有限大小的 JSON、转换纯 C++ DTO、调用服务并序列化响应。[`FixtureResultService`](../../../../features/results/src/result_service.cpp) 通过 caller-aware resolver 获得已发布产物的冻结输入，再调用 [`FixtureResultReader`](../../../../features/results/src/result_fixture.cpp)。产物 resolver 核对调用者、published 状态、磁盘完成 manifest 与文件摘要。core 公共接口没有 Qt、SQLite 或 VTK 类型。

权威模型仍在统一 `RecordApplication` 的记录、事务和历史中；结果服务只通过已有 `update_owned_rows` 副作用口保存 owner=`qcae.results.fixture` 的不可变事实，handler 在恢复/打开时验证二进制载荷。读取结果不会创建模型事务、增加模型修订或历史。产物任务使用显式 `TaskArtifactReceipt`，保存产物 ID、冻结输入修订和 manifest 摘要，不伪造模型事务凭据。

## 主要不变量与错误处理

- [`FrozenAnalysisInput`](../../../../features/analysis/include/qcae/analysis_input.hpp) 保留分析实体 ID、工况实体 ID、目标 profile、原始 document/epoch/revision、完整 ExportIdentifier 映射和确定性物理签名。实体 ID、solver number 和 Task ID 分开。
- 物理签名使用既有 length-delimited binary codec，不宣称是密码学散列；网络用可逆 hex，产物 `input_sha256` 是精确二进制签名的独立 SHA256，文件/manifest SHA256 各自独立。没有 binary→UTF-8→binary 往返。
- fingerprint、完整编号映射、quantity/unit/components/location/basis/case/frame/source_kind 必须匹配。未知或重复 GRID 编号、非有限数值、缺失元数据和来源重标记均拒绝，失败不发布结果事实。
- 映射必须覆盖签名中的精确实体和 namespace。编号仅用于解析，返回值定位稳定实体 ID；不同 solver 编号排列不会改变物理签名。
- current/stale 比较当前文档身份和物理签名；节点、材料、载荷或边界改变使旧结果 stale。组织名称/成员和显示状态不参与签名。undo 恢复相同物理输入可恢复 current，原始输入版本始终保留；正常打开产生新文档身份时旧结果保持 stale。
- 事实 ID 由 caller/document/key 确定，创建使用 absence CAS。完全相同请求重试返回原事实；同 key 改值报 `IDEMPOTENCY_KEY_CONFLICT`，新 key 携旧修订报 `REVISION_CONFLICT`。旧结果不会覆盖新版本，调用者隔离在产物 resolver 和结果查询两处保留。

## QG-03 行为证据

最终统一 Release desktop 构建完成后，核心 `analysis_checks`、`result_fixture` 为 **2/2 PASS**，见 [core-tests.log](core-tests.log)。独立核心构建的实体/操作生成器、生成产物、分析与结果定向检查为 **6/6 PASS**；optional reference vector/array 的实际生成编译与 schema 反例已验证，旧数组默认非空约束保持。

真实 Qt local engine 使用显式 SQLite workspace。运行命令：

```sh
python3 tests/result_fixture_ipc_tests.py \
  --engine build-c3-closure-desktop/qcae-engine \
  --cli build-c3-closure-desktop/qcae-cli \
  --evidence-dir docs/engineering/c3-closure-evidence/results
```

[result-fixture-report.json](result-fixture-report.json) 记录 **2/2 PASS**：每模式 154 次请求，两个节点值，三个负样本共 30 次拒绝，11 个必需字段缺失拒绝；覆盖幂等重试/冲突、陈旧修订、组织/显隐不陈旧、物理改变/undo/redo、保存、进程终止、明确恢复、另存和正常打开。原始调用见 [CLI transcript](cli-result-transcript.json) 与 [脚本 transcript](script-result-transcript.json)，冻结发布信息见 [CLI manifest](cli-input-manifest.json) 与 [脚本 manifest](script-input-manifest.json)。

真实 IPC 使用原生本地 socket 测试权限。此前分析 IPC 的受限沙箱监听失败与原生测试结果分开；本结果最终记录来自成功运行的真实 engine，不将沙箱能力限制计为产品缺陷或通过证据。

QG-01 本切片手写 C++ 已使用 clang-format 21 并通过定向格式检查；设计检查和 `git diff --check` 已通过。全仓格式门禁与完整集成回归由主执行者统一归档，本文件不代替该门禁。

审查结论：入口、权威模型、持久结果事实、外部文件验证边界可以定位；主要不变量有正负测试，未引入新依赖或独立参数库。后续真实求解结果读取需要独立来源适配和工程数值验证，不能重标记本夹具获得通过。
