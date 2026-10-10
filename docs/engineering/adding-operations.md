# 新增操作、字段与实体

扩展QCAE时按本页固定步骤进行。不要编辑生成文件（`records.hpp`、构建目录中的 `operation_inputs.hpp`），不要在功能代码中写ledger计量，不要在消费者中为某个实体写特例分支。

## 新增写操作（作用于已有实体）

1. 生成骨架：

   ```sh
   python3 tools/new_operation.py --feature materials --schema schemas/operations/basic.json \
     --operation material.set_name --type MaterialSetNameInput \
     --field entity_id:entity_id --field name:string --label "Rename material"
   ```

   字段写法为 `名称:类型[:optional][:units=a,b]`，类型可用 `string`、`entity_id`、`entity_id_array`、`quantity`（必须给units）、`finite_number`、`vector3_mm`（units=mm）、`positive_uint32`。工具追加schema条目，在功能头文件声明 `prepare_<操作>`，写入拒绝执行的桩函数，并以 `declare_unimplemented` 登记为不可用：实现前客户端和MCP都看不到它。
2. 实现桩函数，范例是 [material_operations.cpp](../../features/materials/src/material_operations.cpp)：
   - `auto canonical = canonical_copy(input);`
   - 换算单位：`normalize(canonical.young_modulus, parameters::Dimension::pressure, "young_modulus")`，失败时 `return forward_failure<OperationPlan>(normalized);`
   - 新建记录先用 `check_record_fields(records::X{...})` 复用生成的字段规则。
   - `return record_plan(std::move(canonical), [](const Input& canonical, PlanEdit& edit) { ... });`，回调返回 `ChangeSummary{"撤销标签", 受影响实体, 0, 是否新建}`。
   - 在 `PlanEdit` 中用 `put`（新建，ID来自 `edit.allocate()`）或 `update<records::X>(id, ...)`。把字符串等堆数据复制进记录时用 `copy_into`，标量直接赋值。非法修改抛 `RecordError`，整批不提交。
3. 把登记行 `declare_unimplemented<Input>(registry)` 换成 `register_plan(registry, app, prepare_<操作>)`，删除TODO。
4. 在 [model_operation_tests.cpp](../../tests/model_operation_tests.cpp) 或功能已有的测试中经 `registry.invoke` 覆盖：成功、非法输入不提交、同键重试返回原事务、undo/redo。
5. 检查：`cmake --build build-core && ctest --test-dir build-core`、`python3 tools/check_design.py`、`python3 tools/check_cpp_format.py`、`git diff --check`。

由此自动获得：类型化解码与版本检查、`capabilities.list` 与MCP工具、文档/epoch/修订校验、幂等重试、undo/redo、SQLite持久化与恢复、复制计量。只读查询、后台任务或需要额外上下文的写操作（如 `analysis.create` 绑定profile）不适用骨架，参照 [analysis_operations.cpp](../../features/analysis/src/analysis_operations.cpp) 手工登记。

## 给已有实体加可选字段

1. 在 [entities.json](../../schemas/entities/entities.json) 中为实体追加字段（新 `id`、`optional: true`），并把实体 `version` 加1；旧记录读出为空。
2. 运行 `python3 tools/generate_entities.py` 重新生成 `modules/document/include/qcae/records.hpp`（需要clang-format 21）。
3. 需要编辑入口时按上一节新增操作；范例是EXT-01的 `material.set_description`。
4. 测试旧版本记录读出空值、新值保存与恢复、undo/redo。

## 新增实体类型

1. 在 `entities.json` 中新增实体（未用的类型ID、字段与引用目标）并重新生成 `records.hpp`；需要跨字段规则时在 `modules/document/src/records_rules.cpp` 补充。
2. 运行 `ctest --test-dir build-core -R record_consumers`：[record_consumer_tests.cpp](../../tests/record_consumer_tests.cpp) 要求每个消费者为新类型归类。已支持就加入该消费者的类型列表（`analysis_record_types()`、`model_projection_record_types()`），否则加入测试中的拒绝集合。归类之前，分析与导出已会自动拒绝含该类型的文档。
3. 需要显示时，在 `modules/query/src/render_projector.cpp` 的 `default_render_contributions` 中加一条贡献。
4. 创建与编辑操作按第一节新增；范例是EXT-03的Tri3。
