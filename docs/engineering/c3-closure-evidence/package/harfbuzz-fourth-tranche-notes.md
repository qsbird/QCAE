# HarfBuzz 第四片：23 个受控别名写入

`tests/prepare_c3_harfbuzz_alias_tranche.py` 在新的 ignored `build-c3-qt-observed/tranche4/` 重建原 HarfBuzz 14.4.0 Release 配置，最多两线程，不修改旧 Qt/HB prefix。准备前逐项核对第三片源码 SHA 和已归档的 23 项清单。每个实际赋值原语句执行一次，随后记录精确目标 lvalue 的 `sizeof` 写入上界；`sizeof` 不执行 getter、索引或递增。扩展覆盖 buffer add 的 codepoint/mask/cluster、unicode/lig/glyph 属性 mutator 和 normalizer getter 返回字段的赋值。

源清单由 354 增至 377 点。独立编译/安装成功；15 个默认/OT/CoreText × ASCII/CJK/阿拉伯/天城体/emoji 的原库对照案例，glyph ID、cluster、mask、advance、offset 和塑形 success 全部相等。新增 23 点中夹具实际触发 7 点，共 720 次调用和 2160 字节 typed-write 上界；没有将未触发 16 点描述为已发生写入。完整摘要 `harfbuzz-fourth-tranche-alias-report.json` 含每点原语句、目标字段、实际调用/字节及源清单。原始报告和构建日志在 tranche4 目录。

独立因果夹具直接调用 buffer add 输入 `Aé🙂`：三个 scalar 对应的 codepoint/mask/cluster 赋值各恰执行一次，共 9 次/36 字节；原库与观测库全部五个 glyph record 字段完全一致，UTF8 cluster 为 0/1/3。证据 `harfbuzz-fourth-tranche-bufferadd-causal.json`。没有构造 QApplication 或占用原生 GUI。

同次 Qt/HB 验证使用独立 `tranche4/qt-prefix`，从冻结 tranche3b 复制。12 个 Qt framework Mach-O SHA 完全相同，仅复制后的 CMake/pkg-config/prl 路径重绑和本地 HarfBuzz 文件替换为第四片。Qt 源 marker 仍是原 157 点，HB marker 为 377 点。纯 loader 实际核验 Core/Gui/DBus/Widgets 全来自第四片 prefix，且只有该 prefix 中一个 observed HarfBuzz；marker 与对应 manifest 精确相同。证据 `harfbuzz-fourth-tranche-qt-loader.json` 和 tranche4 的 `qt-hb-binding.json`。实际 QApplication/同次编辑采样仍待 root，不能继承原第三片的采样结论。

15 次 shape-entry placeholder 保留；未枚举 OT/generated 赋值和递增、scratch/cache 的初始化写入及非平凡构造/赋值/搬迁、后端模型派生输入的所有权仍未闭合，`coverage_complete=false`。这些源观测事实不会直接与旧 public 保守上界相加造成同一复制双计，也不会以本片局部成功关闭整体字体未知项。
