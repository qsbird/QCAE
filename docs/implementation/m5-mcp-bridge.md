# M5 本地 MCP 薄桥实施切片

2026-10-01。对应 REQ-02/15/16/17、TST-A01/02/12、TST-I01/04/05/10。
本切片只建立真实 stdio MCP 到现有本地 engine 的通道；完整 M5 仍需真实 AI
独立会话以及真实 Nastran 数值闭环，不能从协议测试推定通过。

## 实施计划与行为边界

1. Python 标准库实现 JSON-RPC stdio 生命周期，QtCore 启动器提供构建产物
   `qcae-mcp`。不增加依赖，不创建业务模型、不启动第二个 engine。
2. 必须显式传入已存在的绝对 `--endpoint`。每次工具调用握手同一 engine，
   从其实际 `capabilities.list` 生成工具说明和参数 schema；只发布当前 transport
   可用的操作。连接绑定的 `events.subscribe` 不发布，事件游标查询用 `events.read`。
3. 原样传递业务参数和用户提供的 document/epoch/revision/idempotency/profile。
   引擎继续校验单位、歧义、版本、提交与历史。桥不自动填物理量、不重试写操作。
4. JSON-RPC 协议错误与业务失败分开。业务失败保留引擎结构化结果，并返回
   MCP `isError`；断线后报告执行结果可能未知，保留原幂等键供查询/重试。
5. 真实 engine + 两个桥 + CLI 验证发现、单位澄清、共享修订/历史、冲突、重试、
   同键异参和协议负例；再用已登录实际客户端验证真实模型工具调用。

协议实现依据 MCP [stdio transports](https://modelcontextprotocol.io/specification/2025-11-25/basic/transports)、
[lifecycle](https://modelcontextprotocol.io/specification/2025-11-25/basic/lifecycle) 与
[tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools)。显式支持
2025-11-25、2025-06-18、2025-03-26、2024-11-05；不宣称实现其他版本所有能力。
stdout 只输出单行 UTF-8 JSON-RPC，诊断走 stderr。没有后台订阅、GUI 模拟点击或
取消推理能力；业务 `tasks.cancel` 仍按实际引擎操作契约工作。

## 运行

在已有 engine 运行且端点可达时：

```sh
build-c3-closure-desktop/qcae-mcp --endpoint /absolute/path/engine.sock
```

stdio 生命周期需先 initialize、notifications/initialized，随后 tools/list 或
tools/call。`tools/call.arguments` 使用 `parameters` 对象及同级上下文；省略
必要上下文由引擎返回其原错误。单次 IPC 帧上限保持 1 MiB，超时默认 30 秒。
桥断开或退出不终止 engine。认证和真实求解配置不进入桥或版本库。

## 当前实际验证

- build32 新 `qcae-mcp` 启动器及 Python 薄桥通过真实 memory/SQLite engine、两个
  MCP stdio 客户端与 CLI 测试：capability/schema、缺单位不提交、版本/epoch 冲突、
  同键异参、原事务重试、撤销后不重建、redo、EOF、Unicode/非有限/无效 id。
  相关 9 个数据/MCP/widget 回归 9/9，既有 IPC 加两个 MCP 回归 13/13。
  原始日志位于 `docs/engineering/c3-closure-evidence/package/` 的 build32 文件。
- 实际 Codex CLI 0.159.2、gpt-6.1-sol/xhigh、ChatGPT 缓存登录完成一次合成会话：
  11 次完成 MCP 调用、0 次失败，无 shell 或文件动作。CLI 独立复核 revision=3、
  一个 Steel、E=210000 MPa。JSONL、提示、事实与二进制/桥摘要在
  `docs/engineering/c3-closure-evidence/real-ai-mcp-smoke2-build31/`。引擎为 build31，
  源码树可变，不能冒充同源码最终发布或完整 M5 通过。
- 第一次实际会话的工具调用被单次 CLI 默认审批设置阻止，未创建文档；原日志在
  `real-ai-mcp-smoke-build31/` 保留。后续仅对八个已授权合成测试工具设置运行时
  `default_tools_approval_mode="approve"`，不修改全局配置。配置字段依据
  [OpenAI Docs MCP](https://learn.chatgpt.com/docs/extend/mcp)。

仍需真实 AI 人机交替、各验收场景三次独立会话、真实求解器和最终同源码验证。
