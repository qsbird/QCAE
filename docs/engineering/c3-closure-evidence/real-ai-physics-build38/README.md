# Build38 真实 AI：物理澄清与共享属性诊断

独立 harness：`tests/real_ai_mcp_physics_scenarios.py`。每个场景有独立临时 SQLite 工作区、真实本地 engine/CLI、真实 Codex app-server ephemeral/read-only 会话及 QCAE stdio MCP。模型固定为 `gpt-6.1-sol`、`xhigh`，每个会话最多可见 18 个 QCAE 工具；收到其他工具动作时 harness 拒绝继续。使用原有登录，不读取、复制或提交凭证。发往模型服务的提示、schema 和模型事实均为合成测试数据。

- `physics-*`：CLI 实际建立 20 梁共用材料/截面。AI 首轮澄清缺失实体身份、全局方向和力值，独立 observer 检查修订、全部实体字段及历史未变；第二轮给定实际 Node 身份和全局 -Y 1 N，验证真实 force/constraint 字段与两项提交。
- `shared_preview-*`：AI 读取实际共享材料/截面引用与全部 20 梁，调用真实共享材料变更预览；输出完整影响范围，保持模型与历史。旧 `changes.preview` 只有 affected material ID，20 梁清单由真实引用/查询获得，不能声称服务端预览本身含完整 impact DTO。
- `shared_unsupported-*`：只选实际 10 梁、要求原子复制/重分配并一次撤销。AI 必须查能力和完整共享范围；当前没有该原子复合操作，应明确 unsupported 并保留模型。不能将多个独立 create/assign 事务称作一次编辑。本诊断不伪造成功复制或撤销。

每目录保存 prompts、完整 client JSONL 与 request JSONL、engine 日志、observer CLI 请求/响应、facts 中完整模型/历史 before/after 和二进制/桥/transport/harness SHA-256。目录 nonce 区分真实会话，旧失败保留。`setup_only=true` 只证明 CLI 夹具，不能计入 AI 会话。

先前失败：`physics-814e241816f4` 的默认 sandbox 拒绝 QLocalServer.listen，engine 日志 `Unknown error 1`；没有 AI 会话。`physics-d29feae8de1d` 在已授权 OS sandbox override 下 CLI setup 通过；仍没有 AI 会话。真实会话运行结果以各 `facts.json` 为准，不能从此说明推断通过。

相机部分仅实际 view.create/update 的 camera_fingerprint 变化与力字段保持，不是原生 GUI 相机旋转。每场景先一真实会话诊断；至少三次独立会话、真实 GUI 部分及其他 AI 验收仍待执行。本记录不宣称完整 TST-I02/I03、M5、P0、真实求解或数值验收。

实际首轮结果：

| 场景 | 真实 session 目录 | MCP 调用 | 用时 | 诊断结果 |
|---|---|---:|---:|---|
| I02 两轮澄清/实际物理字段 | physics-55896e38392b | 15 | 117.28s | 通过；初始 R5 保持，明确后仅两项物理对象到 R7，view API 后仍 R7 |
| I03 共享属性预览 | shared_preview-13f2a93f335d | 8 | 118.02s | 通过；实际 material incoming=1、section incoming=20，真实材料预览220000 MPa，不提交 |
| I03 仅十梁原子编辑不支持 | shared_unsupported-f6c051b687f3 | 17 | 198.14s | 通过；AI 依据真实 capability 明确 unsupported，无物理操作尝试或模型/历史变化 |

合计 40 次完成的实际 QCAE MCP 调用。独立 CLI observer 核对每个初始夹具全部20梁共享同一截面、截面引用同一材料；所有实际 AI 工具返回均为 success，unsupported 是 AI 对实际能力目录的结构化结论，没有伪造一次不存在的后端原子写入。详细汇总见 `summary.json`；`harness-source-build38.py` 和 `transport-source-build38.py` 保留本次调用源码。原失败文件没有覆盖。此表仍是各一真实 session 的受控诊断，不改变前述验收缺项。
