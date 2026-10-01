# Cocoa 字体后端独立行为探针

状态：源码及独立严格 Release 编译完成。Root 的原始默认 Mac 64/64 案例完成并核验真实后端；原始 FreeType 在六个成功案例后，第七例 `SFNS/14/emoji` 全白而失败，原始 build36 JSON/log 保留。没有改变产品默认字体后端，也没有关闭默认 CoreText 的复制未知项。新增单例诊断的实际运行由 root 顺序执行。

`tests/qt_font_backend_behavior_probe.cpp` 只依赖现有 Qt 6.11.1 的 `Qt6::Gui`、测试私有 `Qt6::GuiPrivate` 和系统 loader 只读 API。Cocoa 原有 `fontengine=freetype` 参数由 `qcocoaintegration.mm` 的 `QCoreTextFontDatabaseEngineFactory<QFontEngineFT>` 分支处理；已安装 Qt 的 FreeType 依赖为 2.14.3。仅依赖存在尚不能证明该分支能加载这些实际字体。

64 个固定案例使用 SFNS.ttf、SFNSMono.ttf、系统 GeneralFont、系统 FixedFont，分别在 14/18 pixel 下绘制材料/修订 ASCII、组合拉丁字符、希腊/CJK、阿拉伯、天城体、emoji 和双向混合字符串。保留字体文件 SHA256、实际每个 glyph run 的字体表 SHA256、Mac/Freetype/其他引擎枚举、glyph IDs、源 cluster、位置、原始未塑形 advance、布局/字体度量以及完整 ARGB32 像素 SHA256。缺失 glyph 不被过滤。

实际引擎来自已经产生的 `QGlyphRun::rawFont()` 的 `QRawFontPrivate::fontEngine->type()`，不是 nominal Multi 引擎或请求参数。该私有读取仅允许 Qt 6.11.1。ASCII 真实 glyph run 必须符合请求后端；Unicode fallback 引擎逐项报告。注册字体或加载后端失败时输出失败 JSON、已完成的案例和错误，返回非零。

比较要求相同 Qt/平台、屏幕参数和实际加载 SDK 文件及 SHA256，基准后端必须是 Mac、当前后端必须是 Freetype。逐案例精确比较全部输入、字体身份/字体表、glyph IDs/位置/cluster、glyph run、度量、像素、glyph 数和缺失数。没有容差、重排或字段忽略。差异保留于 `comparison.cases[].different_fields`；仅该案例所有项相同才有 `exactly_equal=true`。捕获成功但不同是完成的诊断，退出码为零，`comparison.equivalence_passed=false`；不能仅凭退出码声称后端等价。

全白仍是失败、退出码为 1。新增 `failed_case` 保存失败输入、实际字体/引擎/glyph、位置、度量、原全白像素 SHA；失败后才调用的 glyph alpha-map 诊断另存 `failure_only_post_draw_glyph_bitmaps`，不能预热原绘制或修复失败像素，也不是正常路径复制计量。`--case SFNS/14/emoji` 仅运行这一固定案例，便于独立复现；未知 case ID 拒绝。默认/FT 单例可各自保存 JSON 后直接核对失败原因，不能拿原始 64 例基准与单例报告宣称同一案例域的全量等价。

Root 的 build36b 单例实际结果为 Mac 完成、FT 保留全白失败。FT 的 `.SF NS` 与 `Apple Color Emoji` 都是真实 Freetype 引擎，非空 glyph alpha-map 有像素；emoji fallback 的 ascent 297.5/descent 92.96875 将原整行 baseline 推到 draw origin 加上 317.5 个逻辑像素，所有非空 glyph 顶部均大于 306.875，而原画布仅 160 逻辑像素高。默认 Mac 行高为 23，FT 为 391，已经存在实际度量差异。只读源码及真实字体表/FreeType strike 重建：non-scalable `emSquareSize()` 是 y_ppem 20，文件 design UPEM 为 800，HHEA 按 20 换算后再乘 `QFixed(14/26)=34/64`，恰好产生实际巨大 ascent/descent。完整证据为 `font-backend-freetype-emoji-causal-audit.json`。这是裁剪失败的因果解释，不是所有 glyph 栅格化失效；没有更换后端或修改 Qt SDK。

Root 注册的最小目标：

```cmake
add_executable(qcae_qt_font_backend_behavior_probe tests/qt_font_backend_behavior_probe.cpp)
target_link_libraries(qcae_qt_font_backend_behavior_probe PRIVATE Qt6::Gui Qt6::GuiPrivate)
qcae_warnings(qcae_qt_font_backend_behavior_probe)
```

独立编译证据为 `font-backend-probe-independent-compile.json`（`-O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Werror`）。本 agent 没有启动 QApplication/原生 GUI。Root 在同一独立构建、同一字体和屏幕环境顺序执行，保留各自 stderr/log：

```sh
QT_QPA_PLATFORM=cocoa ./qcae_qt_font_backend_behavior_probe \
  --output font-backend-coretext.json --expected-engine Mac
QT_QPA_PLATFORM=cocoa:fontengine=freetype ./qcae_qt_font_backend_behavior_probe \
  --output font-backend-freetype.json --expected-engine Freetype \
  --compare font-backend-coretext.json
```

如需 CTest 注册，这两次应串行并让第二次依赖第一次；不要把行为差异当作复制覆盖成功，也不要与性能/native GUI 采样同时运行。

第三版 SDK 的原 mixed-image 故障独立保留。`qt-tranche3b-loader-preparation.json` 记录从冻结 tranche3 复制的新 prefix，仅 QtGui 删除 `/opt/homebrew/lib` 的 LC_RPATH，保留 `@loader_path/../../..`，没有重新编译、修改原库或改变 157/354-site manifests。此前外部 rpath 先命中 `/opt/homebrew/lib/QtDBus.framework` 的 Cellar symlink，也可能先命中系统 HarfBuzz。只读 loader 探针 `qt-tranche3b-core-gui-widgets-loader.json` 已确认 Core/Gui/DBus/Widgets 全部来自独立 tranche3b，且仅一个本地 observed HarfBuzz；没有构造 QApplication，真实 root mixed-image/source gate 仍待运行。

这组固定案例即使全部像素/布局一致，也只支持其有限输入的行为事实。默认 CoreText 的模型派生 UTF16/glyph 输入内部复制、Qt/HarfBuzz 尚未观测的缓存和间接 owned stores 仍按 `font-copy-remaining-source-inventory.json` 保留未知；该探针不能替代同次真实编辑的端到端复制计量。
