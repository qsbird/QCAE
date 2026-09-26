# 骨架验收报告校验器

`python3 tools/check_skeleton_acceptance.py`仅检查冻结目标的结构及算术，不会将任何产品检查点标为通过。

`--report <json> --artifact-root <directory>`检查报告对当前Git提交、规范哈希、证据文件哈希、原始P95采样、220次故障记录、20个代表案例、旧版本夹具分母和扩展基线/diff的绑定；缺失、类型错误、篡改、越界路径、失败或阈值超限均返回非零退出码。它校验提交的证据结构和数值一致性，不能独立证明采集器没有伪造数据；采集器/测试源码与日志仍需审查。

单测 `python3 tests/test_acceptance_checker.py` 使用临时目录内明确标为checker self-test的合成数据，仅验证校验器拒绝错误报告的行为。该合成报告不保存为产品完成证据。

报告的fixture_path指向本轮迁移执行清单（需包括原始夹具路径/hash、executed、passed和versions）；R0的原始fixture manifest保持不可变，不能直接把旧引擎验证结果当作新引擎迁移通过。
