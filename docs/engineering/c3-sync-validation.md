# C3 资源、事件与局部显示切片

本轮从 `393f52e` 的 Release 44/44 基线开始，按[实施计划](c3-sync-plan.md)并行推进资源、事件、局部显示与验证。没有增加依赖；仍使用同一个本地引擎、记录模型、事务及持久历史。外源几何/网格后端和 Tri3 未在本轮加入，完整 C3、SK 与 P0 不在此宣布验收。

## 交付行为

- 引擎提供版本化显示资源，清单绑定文档、epoch、模型/视图修订、长度、SHA-256 和媒体类型。128 KiB 原始块经明确的 base64 编码进入现有 JSON 通道，整帧不超过 256 KiB；16 MiB 单资源、64 MiB/32 项缓存和60秒租约限制内存。共享客户端仅在完整校验后发布字节，取消、切换上下文和迟到回包会释放租约。没有新增任意文件读取或上传接口。
- 真实 commit、undo/redo 和任务发布进入有界只读变更日志。事件含引擎实例和递增序号；保留256项、最多64个订阅。事件用于失效通知，权威文档和任务仍从应用查询。重连、缺口、存储不确定或过期游标触发重同步，不自动重放业务写入。
- `RenderProjector` 通过静态贡献投影已有 Node/Line2/GeometryLine。非视觉字段修改只推进版本，局部坐标修改生成差量；首次加载、拓扑/显隐改变及基线丢失使用完整场景。核心公开接口不包含 Qt/VTK 类型。
- VTK 按1024个实体分块，节点更新只改节点块及相邻梁块，几何线只改对应块；高亮更新复用块坐标，不重建整个场景。差量先完整验证版本、索引、ID、有限坐标与重复项，再改变显示缓存。树默认按改变的 ID 更新，组织视图保留查询刷新；全历史只在首次加载或明确重同步时读取。
- Qt 桌面优先使用事件及资源，保留旧引擎轮询/JSON兼容路径，并以两秒权威查询兜底。过期画面不能参与视口选择。停靠面板/工具条布局保存在本地 Qt 设置中，不增加模型修订和历史。

协议字段及限制见[引擎接口](../../adapters/engine_api/README.md#c3-versioned-display-resources-and-events)。

## 验证范围和测量边界

4 MiB 不透明资源在真实本地套接字中精确往返：32个块、长度4194304、SHA-256 `c2f428c967f45af9722ab0fddd02217150938b2bdad2adaac2d997b242976384`。缺块、破坏、乱序、偏移/长度、文档/epoch/revision/视图、资源ID和base64负样本均不发布部分数据。生产引擎另验证完整几何包、拓扑改变、节点差量、材料空差量、六项版本/范围拒绝、事件保留缺口及权威全量重同步；不把测试服务器等同于生产引擎。

局部编辑夹具使用 N=1000/10000/100000 **节点**与 N−1 梁，材料/截面另计；每档10次内部节点移动与10次材料修改，共60次。节点度为2，不在包围盒极值，无几何绑定；每次采样前恢复相同语义状态并在测量外追平投影。三档固定测试实体配额250000，使最大夹具的200001条记录可进入同一应用；生产默认总实体配额仍为100000，未借此宣布默认桌面支持该最大夹具。

字节报告分别记录应用记录工作、增量投影、二进制差量及实际 SQLite StoreBatch 的非空 after 行值字节，并检查已插桩复制/编码通道之和。SQLite 该计数不含键、删除项及驱动/页开销，不能当作完整批次或物理写入量。元数据上限64 KiB、节点576 KiB、材料68 KiB沿用原合同。全模型序列化/物化次数从插桩读取；共享不可变引用不计模型字节复制。

**这不是完整 SK-12 端到端字节账本。** 未覆盖 Qt/容器分配器的所有内部复制、解码到视口的全程复制、SQLite 驱动内部复制和全部实际套接字流量；物理 WAL/页及扫描工作明确未测，报告中使用 null。视图服务还会复制隐藏 ID 元数据，非默认组织树会重新查询。独立 VTK 60次编辑测试记录保留坐标写入和被标记为变化的有界数组字节；它不等于实际 GPU 上传字节。节点关联梁的工作量随度数变化，不能推导整个流程 O(1)。

固定图形夹具使用10000节点/9999梁，点/梁开启，文字/阴影关闭。实际 framebuffer 为1280×720，设备像素比2；每条相机轨迹预热60帧后采样300帧，连续完成同步点间隔包括 Qt 队列、相机更新和 GPU 等待。输入反馈预热5次、采样50次，路径是视口可见拾取至高亮完成，不含引擎往返。真实 IPC 任务确认另外测量50次 queued/cancel_requested，并等待最终取消状态验证；不把请求已确认说成任务已取消。

## 审查和修正

QG-02 路径：`RecordApplication → snapshot/journal → RenderProjector → render_wire → ResourceStore → ResourceClient → decode → VtkView`。权威状态只在应用，显示及事件都是可丢弃缓存；变更日志分配在持久提交之前完成，发布后不新增可失败的业务步骤。Astra/Sol/Luna 极高分工，主代理集成，另一代理交叉检查采样口径。

回归和审查发现并修复：

1. 资源清单响应超时会让已前移的模型修订与旧场景长期失配；现在明确要求完整重同步并重试。迟到且未使用的清单也释放资源，防止连续相机变动耗尽租约。
2. 订阅确认丢失后事件可能一直被等待重同步的客户端抑制；增加带连接代号保护的限频重试。视图更新确认丢失同样通过重新查询/视图冲突恢复，不重放业务修改。真实套接字＋桌面测试冻结这些时序。
3. 增量投影不再复制已验证相同的隐藏集合，只推进版本。
4. 冻结点/线 PICK 矩阵发现穿透框选错误包含隐藏点；现在穿透只忽略遮挡，隐藏点仍排除。点/线8项通过；Tri3的4项保留未实施，不能称为12/12。
5. 普通模型变更通知立即阻止旧选择、旧资源及旧查询回包重新启用画面；权威查询和新场景安装完成后才恢复选择。跨文档视图更新使用独立代号，旧文档回包不能清除新文档的等待状态。
6. macOS SQLite测试偶发在fork子进程初始化系统日志时崩溃；测试改为启动独立可执行进程，保留路径/硬链接/符号链接锁及精确提交/发布故障窗口，不修改产品存储逻辑。
7. 中间验证包含格式尚未整理、套接字沙箱权限及测试夹具/计数口径错误。最大模型没有缩减为十万总记录；恢复原 N 节点定义，明确固定测试配额。累计预览/提交工作次数与真实改变的单条记录分开报告，不降低原字节门槛。

## 最终结果与归档

最终Release在冻结源码上通过52/52（111.18秒）；原44项基线均保留。纯核心28/28（41.59秒），仅IPC Debug 35/35（111.45秒），公共头51/51，架构检查47个生产单元/27个目标通过。纯核心/仅IPC测试后仅修改桌面时序保护和SQLite测试进程，未触及其产品目标；具体范围见[源码变动说明](evidence/c3-sync/validation-source-changes.json)。自动格式覆盖138个C++文件。

| 验证 | 最终实测 | 原始证据 |
|---|---|---|
| 4 MiB资源 | 32块；最大完整JSON帧175284 B；12项负样本均拒绝；部分发布0 | [完整CTest日志](evidence/c3-sync/release-detailed.log.gz) |
| 生产IPC | 666次请求；版本/范围拒绝、拓扑全量、节点/材料差量和缺口重同步通过 | [摘要](evidence/c3-sync/c3-sync-ipc-summary.json)、[逐请求记录](evidence/c3-sync/c3-sync-ipc-transcript.json.gz) |
| 工具生命周期 | 30/30；取消不改修订/历史；重复Apply仅一次提交；每次Undo回到空模型 | [逐次结果](evidence/c3-sync/tool-lifecycle.json) |
| 拾取/布局/时序 | 点线8/8，漏选/多选均0；布局往返通过；旧选择、超时、跨文档迟到回包通过 | [完整CTest日志](evidence/c3-sync/release-detailed.log.gz) |
| 局部编辑 | 60/60；已插桩整模型序列化/物化0次，投影全量重建0次 | [逐次数据](evidence/c3-sync/locality-bench.json) |
| 旋转/缩放帧时间P95 | 8.796 / 9.700 ms，均≤33 ms | [300+300帧](evidence/c3-sync/graphics.json)、[实际画面](evidence/c3-sync/graphics.json.png) |
| 视口输入反馈P95 | 20.118 ms，≤100 ms；不含引擎往返 | [50次采样](evidence/c3-sync/graphics.json) |
| 任务启动/取消确认P95 | 1.943 / 1.512 ms，均≤100 ms | [真实IPC 50次采样](evidence/c3-sync/c3-sync-ipc-summary.json) |

局部字节最大值（每种操作30次，保持前述口径）：

| 操作 | 应用＋投影模型复制 | 记录＋差量编码 | 元数据复制 | SQLite非空after行值 |
|---|---:|---:|---:|---:|
| 内部节点移动 | 887 B | 596 B | 37304 B | 1296 B |
| 材料修改 | 1173 B | 659 B | 34520 B | 1451 B |

VTK独立60次编辑验证每次内部节点移动只触及1个点块和2个梁块，保留坐标写入96 B、标记变化的坐标数组122880 B；这些值不能与上表相加声称完整端到端账本。

[汇总](evidence/c3-sync/summary.json)、[冻结源码哈希](evidence/c3-sync/source-manifest.json)、[证据清单](evidence/c3-sync/evidence-manifest.json)保留可核对关系。核心、本地、基线和公共头构建日志在同一目录；中间失败日志也保留。SQLite故障进程修正另通过Debug/Release各30次连续回归，Debug/Release重复记录及系统崩溃栈已归档。新增视图夹具先前未完成前一次选择请求，触发预期的选择重试；现在完成该无关请求后再断言视图等待归属，负向选择阻断仍只执行一次点击。

## 复现与环境

环境为 Apple M5、16 GiB、macOS 26.6.2、Apple Clang 21、Qt 6.11.1、VTK 9.7、SQLite 3.51.0；构建并行数2，应用提交线程1，图形基准固定VTK线程1。环境JSON的`qt_thread_count`字段来自`QThread::idealThreadCount()`，不是实际Qt线程数。图形测试需要本机显示与本地套接字权限；没有Windows/Linux或跨GPU结论。公共头边界编译51个独立消费者。

```sh
cmake -S . -B build-c3-sync-desktop -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DQCAE_BUILD_IPC=ON -DQCAE_BUILD_STORAGE=ON -DQCAE_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt \
  -DVTK_DIR="$PWD/build-vtk-deps/install/lib/cmake/vtk-9.7"
cmake --build build-c3-sync-desktop -j2
ctest --test-dir build-c3-sync-desktop --output-on-failure -j2
# 在图形回归及其他性能测试结束后单独采样。
./build-c3-sync-desktop/qcae_c3_graphics_bench graphics.json
./build-c3-sync-desktop/qcae_c3_locality_bench > locality-bench.json
python3 tools/check_design.py
python3 tools/check_cpp_format.py
python3 tools/check_architecture.py --build-dir build-c3-sync-desktop
git diff --check
```

纯核心Release与仅IPC的Debug按[M0命令](../implementation/m0.md)并显式关闭其他可选适配器构建；公共头按[消费者说明](../../tests/public_api_consumers/README.md)构建。本机已有Qt/VTK安装路径仅用于复现示例，没有新增依赖。

## 接续

本轮完成可运行的数据/同步/显示链。后续先补端到端传输和显示复制的同次操作账本、隐藏及组织视图的局部刷新，再冻结 C4 扩展实验基线。Tri3拾取必须随独立扩展补验，不预埋后用空 diff 验收。产物发布、检查/分析引用、结果来源、完整故障矩阵、真实 Nastran 和外部 AI 的原门禁仍保留；外源几何/网格库另从明确的实际用例接入。
