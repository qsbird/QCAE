# C2 横向扩展审查

审查对象：`8dba9e5c05c23cf5b75e158fbd0cc848eaa53afe`（`checkpoint/c2`）。本次修改生产代码文件数为 0；新增审查报告和隔离实验材料。审查分工为 GPT 6 Sol 极高检查操作/约束链路、Sol 中检查查询/显示链路、主协调者检查记录机制并运行实验。

## 结论

通用记录、提交、历史和 SQLite 持久化可以承载新的记录类型及静态不变量。当前生产宿主、IPC、拓扑语义、显示和生命周期策略仍有具体类型分支。因此：**已有类型上的局部操作可以低改动扩展；新增类型的完整产品功能尚不能承诺低改动。** C2 回归通过不等于 SK-13 扩展成本验收通过。

| 扩展情形 | 当前判断 | 改动边界 |
| --- | --- | --- |
| 已有节点批量平移 | 可以复用原事务/历史/存储；未实际实现此操作 | 静态预计修改操作 schema、mesh_editing handler 两个产品文件；公开 plan 工厂时再改头文件 |
| 新关系记录的提交、字段/引用读取、历史、持久化 | 本次实验通过 | 实验新增类型/descriptor/规则和独立装配；既有生产文件改动 0 |
| 新实体的通用 IPC 查询、树组织、来源映射 | 尚不能自动贯通 | IPC whitelist/序列化、组织关系允许目标、旧 Model 投影需要处理 |
| 新 Tri3/Tet4 单元或曲线/曲面 | 尚不能低改动贯通 | 数据与规则之外，还需要拓扑语义、空间查询、显示/picking、属性和 profile 适配 |
| 静态几何范围/引用/唯一性规则 | 底层能注入，生产宿主尚未开放统一贡献入口 | 注册启动前的 RecordRule 或 RecordApplicationOptions.validate |
| CAE 新边界条件，如强制位移/MPC/接触 | 属于新业务模型，不能只加校验函数 | 实体/引用、分析引用目标、诊断、profile 映射及操作实现 |
| 与调用者、操作类型、外部状态相关的规则 | 缺少统一上下文与一致性合同 | 当前 callback 只有候选视图，没有 before/changes/caller/operation/version |
| 删除、合并、重网格与引用重映射 | 防悬空已实现；通用策略未实现 | feature 必须准备完整复合变更，默认不能静默移除引用 |

## 本次运行证据

隔离 C++ 实验直接链接 C2 已构建的 application/document/query/SQLite 库，新增一个 `ProbeRelation`（两个 Node 引用），注册额外 `x <= 1000` 规则。它不经过生产 engine 装配，不增加一套生产权威状态，不将实验类型加入产品 schema。

10 个检查全部通过：新类型提交、类型/字段查询、入引用查询、规则拒绝且修订不变、被引用节点删除拒绝、复合删除、undo、redo、SQLite 重启恢复、工程保存与正常打开。保存/恢复保留关系和节点 ID，正常打开产生新 DocumentId，故障恢复保留 DocumentId 并更新 epoch。

源代码与结果见 [probe.cpp](evidence/extensibility-c2/probe.cpp)、[probe.log](evidence/extensibility-c2/probe.log)、[证据摘要](evidence/extensibility-c2/manifest.json)。这只证明公开底层机制可组合，不证明 production IPC/GUI/求解器包或独立执行者扩展成本。

另一次真实 engine/CLI 实验复现了现有入口不一致：同一工程有一条 GeometryLine，`entity.query {kind: geometry}` 返回 1；`entity.query {}` 返回 0；`entity.query {kind: geometry, name_contains: ""}` 返回 `INVALID_INPUT / Unknown entity kind`。见 [原始请求/响应](evidence/extensibility-c2/ipc-transcript.json)、[复现说明](evidence/extensibility-c2/ipc-probe.log)。这三个结果是缺口证据，不记为查询功能验收通过。

## 已有机制可保留

- [RecordRegistry](../../modules/document/include/qcae/record_registry.hpp:113)承载类型/字段 ID、codec、引用 visitor 和校验；启动后 freeze。[EditSession](../../modules/document/include/qcae/edit_session.hpp:12)构造候选与差量，`prepare()` 校验后才进入应用服务。
- [RecordApplication.execute](../../modules/application/src/core.cpp:430)统一文档/epoch/revision、幂等与发布。预览、提交和历史重放重新校验；SQLite 按稳定 RecordKey 发布，无新增实体专用表分支的要求。
- [entity_queries](../../modules/query/src/entity_queries.cpp:32)的枚举、字段、入/出引用使用 descriptor；kind/id/name/逻辑查询也主要由 descriptor 驱动。新的非空 `query_kind` 能获得这些底层能力。
- [删除校验](../../modules/document/src/document_view.cpp:126)遍历声明引用并拒绝错误目标/悬空。复合变更可以同时删除关系和实体，历史前后镜像能恢复它们。
- [几何终点修改](../../features/geometry/src/geometry_features.cpp:29)已有 geometry revision 与关联 mesh stale 的复合更新；任务底层有 snapshot、取消和过期提交保护。这些应该复用，不需要重新做一套框架。

## 需要补强的具体位置

| 编号 | 代码事实与后果 | 建议 |
| --- | --- | --- |
| E01 生产装配 | [legacy settings](../../features/legacy_api/src/core.cpp:126)固定 `make_record_registry()` 和 `validate_records`；[TypedHost](../../adapters/engine_api/src/typed_host.cpp:185)直接注册具体功能。底层注入接口存在，功能包不能直接贡献到生产宿主。 | 在明确的启动装配入口收集实体、规则、操作贡献，再 freeze；兼容 facade 使用已装配的同一 application。无需动态插件或大型 DI 框架。 |
| E02 IPC 与旧投影 | [entity.query](../../adapters/engine_api/src/ipc_model.cpp:266)仅六个 kind 且无过滤时走 records；其它分支先创建旧 Model。[model_from_records](../../modules/document/src/record_model_bridge.cpp:103)只投影旧实体，遗漏 geometry/mesh/新类型与 mesh ownership。 | 通用查询和编辑走 RecordSnapshot；旧投影仅留在已明确格式边界。声明无法投影的新类型，不能用旧投影定义全模型。 |
| E03 拓扑与组织绑定 | [Mesh.geometry](../../schemas/entities/entities.json:509)只允许 GeometryLine，[规则](../../modules/document/src/records_rules.cpp:82)也直接取 GeometryLine。Part/Set/Include/Analysis 的引用目标枚举固定。 | 将几何来源、单元拓扑和业务引用角色显式化；允许目标由受控能力组/功能贡献解析，不能简单放宽成任何 EntityId。 |
| E04 查询与显示 | [属性关系](../../modules/query/src/query.cpp:230)限定 BeamSection/Beam；[空间查询](../../modules/query/src/query.cpp:305)只有 Node/Beam/GeometryLine；[RenderPacket](../../modules/contracts/include/qcae/render_packet.hpp:7)只有 points/beams。GeometryLine 已可查询却不出现在显示包中。 | 用一个真实 Tri3 切片建立类型对应的拓扑、空间几何、属性解析和显示转换接口；包输出与 VTK/picking 采用明确可支持的 cell/primitive 类型。 |
| E05 约束与生命周期 | [规则上下文](../../modules/application/include/qcae/record_application.hpp:100)不能判断具体操作/调用者。旧 [delete](../../features/legacy_api/src/core.cpp:393)只有 Include 清理和 SourceIdentifier 删除两个例外。 | 分清永久领域不变量、操作前置规则、分析就绪检查；提供版本固定的纯校验上下文。删除/合并/替换用策略生成可预览复合变更；无法映射引用时整笔拒绝。 |
| E06 契约与生成 | [输入生成器](../../tools/generate_operation_contracts.py:16)只有七种输入形式且拒绝无参数操作；[CMake](../../modules/operations/CMakeLists.txt:5)只依赖 basic.json。registry 支持 expected_profile，但 [host](../../adapters/engine_api/src/typed_host.cpp:124)和顶层字段校验不接收；调用未带操作请求版本。 | 先补齐 schema 文件集合的构建依赖、常用输入形式、操作版本/profile 完整传递及能力发现；再冻结通用生成器/分派保护区。 |
| E07 演进与成本 | [实体生成器](../../tools/generate_entities.py:73)单位白名单固定，optional 默认只能 null；[输入校验](../../modules/document/src/record_registry.cpp:86)仍要求新 schema 的全部必填字段，没有类型版本迁移 hook。复杂 CAD 的数组/变换/资源数据也未由现有字段种类覆盖。 | 将单位与字段形式纳入明确受控契约；对既有类型新增必填字段提供显式迁移。新类型注册不等于任意 CAD 数据已能表达。 |

E02 已通过运行复现；其它表项为源代码审查结论，没有把尚未实施的修复标为通过。

还有两个创建/删除负载方面的静态边界，应在扩展阶段加入针对验证：[DocumentView.validate](../../modules/document/src/document_view.cpp:112)每次扫描全部记录、引用和规则；因此记录复制局部化不等于校验 CPU 工作局部化。[位置分配](../../modules/document/src/document_view.cpp:239)为新 ID 追加 slot，删除保留空槽；配额检查使用累计 slot 数，反复创建/删除会在活动实体数量较小时仍触碰配额。需要明确整理时机、历史保留边界和拒绝行为；本次没有压力测量。

## 建议的下一阶段出口

先完成 E01/E02/E06 的入口补强，再以 Tri3 的完整切片落实 E03/E04/E05；不先堆批量业务功能。只有类型对应的空间/渲染/profile 实现可以随类型增加，通用提交、历史、存储和分派算法应保持不变。

沿用 [SK-13](../baseline/skeleton-acceptance.md:36) 的正式量化标准：EXT-01—05 为 5/5；字段、命令、Tri3 三项扩展各自保护区触碰文件数为 0；手写产品文件分别不超过 6/6/10；至少一次独立执行者扩展，初始上下文不超过 8 文件/64KiB，骨架作者代写产品补丁数为 0。三项都从同一冻结基线开始，记录真实 diff，不能用本次孤立 10/10 实验替代。

此外，通用入口至少应针对「已有 Node、GeometryLine、新 Tri3、新关系记录」验证：无 kind 枚举、kind 筛选、ID/名称筛选、字段读取、入/出引用结果一致；支持项错误 0；未支持的条件返回显式错误。三项模型扩展均执行提交、undo/redo、save/open、SQLite recovery，语义差异 0。新规则的正常与拒绝输入、复合删除引用保留/拒绝、重映射失败整笔拒绝必须有原始记录。

## 实验复现

以下命令使用已有 C2 Release 构建库；先按 C2 handoff 构建 `build-c2-desktop`。每次实验使用新临时目录，不操作用户工程。

```sh
qcae_audit_tmp="$(mktemp -d /private/tmp/qcae-ext-XXXXXX)"
/usr/bin/c++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2 \
  docs/engineering/evidence/extensibility-c2/probe.cpp \
  -I modules/foundation/include -I modules/contracts/include \
  -I modules/document/include -I modules/application/include \
  -I modules/query/include -I adapters/storage_sqlite/include \
  build-c2-desktop/modules/query/libqcae_query.a \
  build-c2-desktop/modules/application/libqcae_application.a \
  build-c2-desktop/modules/document/libqcae_document.a \
  build-c2-desktop/adapters/storage_sqlite/libqcae_sqlite.a \
  -lsqlite3 -o "$qcae_audit_tmp/probe"
"$qcae_audit_tmp/probe" "$qcae_audit_tmp/runtime"
python3 docs/engineering/evidence/extensibility-c2/ipc-probe.py \
  "$PWD" "$qcae_audit_tmp/ipc-runtime"
```

本次环境为 C2 验证的 macOS/AppleClang；IPC 测试需要允许本地域套接字绑定。没有重跑完整产品测试，生产文件及其已归档 C2 源摘要保持不变。
