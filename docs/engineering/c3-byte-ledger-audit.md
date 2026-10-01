# C3 同次操作字节账本：边界与源码审计

状态：测量设施和局部优化正在验证；本文件不宣告 SK-12 或 C3 完整通过。以
`docs/baseline/skeleton-acceptance.md` 的 SK-12、固定 B/R/K/H 与 60 个样本为准，
节点 589824 字节、材料 69632 字节、事务元数据 65536 字节的门槛未变。

新增 VTK 坐标 VBO 子路径审计见
`docs/engineering/c3-closure-evidence/vtk-coordinate-source-audit.json`。实际 RenderWindow
Start/End 观察公共 VBO upload timestamp，对共享 `vertexMC` VBO 按身份去重；double
坐标转 float3 的 PackedVBO resize/转换写入按 `2 * tuples * stride` 保守计入库内
拥有型 payload。上传参数长度另报，不等同 GPU 实际流量。该子路径不覆盖 IBO、
范围/shift-scale 临时值或其他 SDK 模型缓冲，完整管线复制覆盖仍为 unknown。

## 同次操作边界

`tests/c3_end_to_end_bench.cpp` 启动真实本地 socket engine、SQLite 工作库和 Qt/VTK
桌面。预热和模型创建在样本外完成；每个样本独立记录 run_id、document_id、
document_epoch、base_revision。`test.ledger.begin` 还声明固定的目标 operation，
只能是 node.move 或 material.set_young_modulus。engine 的 pending ledger 在
该目标请求的正常 readAll 之前激活；arm 等待期的相机/轮询请求仍在样本外。
桌面在 begin 响应后再次等待真实显示空闲，然后在发送目标请求前激活同一标识。
激活期间所有正常 GUI 请求、事件和 trailing 帧都计入，不能按 opcode 过滤。

引擎只在测试账本 pending 时 peek/解析有界完整帧以定位采集起点；这个观察不会
消费帧，业务解析、校验和权限判定仍走原路径。窥视临时副本是观测控制开销。
若目标帧已部分进入接收 buffer、与 arm 外帧混在同次读取，或 peek/readAll 间又
到达未观察前缀，相关计数标为 unknown 并保留失败的实际 trace；不填零或补差值。

完成边界为实际 VTK installedVersion 与模型树 treeRevision 都达到业务 receipt 的
修订、文档/epoch 一致且待处理请求为零。VTK 消费者在 Render 和 WaitForCompletion
之后记录 vtk_complete。事件、资源读取/释放和局部树回包属于同次操作，不能在
业务 receipt 到达时提前结束。`test.ledger.end` 请求仍计入两端实际字节，响应在
关闭观测后生成。账本 begin/read/end 响应和账本自身分配是观测控制开销，不是
模型路径。业务 GUI 的正常轮询和查询仍计入。

每端记录实际 socket write 接受的字节和 readAll 返回的字节；报告验证 engine
发送等于 desktop 接收、desktop 发送等于 engine 接收。网络上限只把每次发送
计一次，不把同一流量的两端相加。收到一个 event 或 trailing tree 响应也不能
只取业务 nonce 的大小。

两端还在实际成功发送和接收解码的位置保留 FrameTrace：stage、实际帧字节数、
frame type、operation、request_id 和 event。响应通过相同 request_id 关联已经
收到/发送的 operation。只记录这些固定协议字段，不记录任意业务参数。每端的
trace 帧长度之和必须等于相应 socket 字节；超过有界记录容量或观察分配失败时
trace 标为不完整。账本自身 trace 字符串/容器是观测开销，不是模型传输路径。

默认执行 1000/10000/100000 节点三个模型，每档十次节点移动与十次材料修改。
指定单个 N 只产生 20 个诊断样本，不能替代 60/60。每次恢复语义状态的请求在
样本外。仅 seed 使用独立 180 秒 setup 超时，局部业务请求仍为 30 秒。

## 指标归属与重复计算

`OperationLedger` 使用固定 stage/metric 数组，无 SDK 类型或新依赖。未覆盖值为
null（传输为 unmeasured）；经过源码证明的无复制边界可以记录已知 0。unknown
是粘性的；仅有阶段 cover 不代表所有复制已知。报告列出具体缺失的指标，完整
覆盖判定必须拒绝必要指标的 unknown。

每个样本的 `complete_coverage` 只检查指定计数器有值且没有 unknown，不证明
整个 SDK 已完成审计。`whole_pipeline_owned_copy_coverage` 当前明确为 unknown，
顶层 `complete_sk12_passed` 仍为 false。VBO 子路径新增的 GPU upload argument
指标要求节点修改大于零、材料空 delta 为零；它不能替代未覆盖的 CPU 模型缓冲。

| 指标 | 定义与上限归属 |
| --- | --- |
| model_copy_bytes | 应用、显示、资源、帧、数组中的真实或保守拥有型 payload 写入/复制 |
| metadata_copy_bytes | 文档、epoch、修订、事务、历史、事实、清单等真实事务元数据；计入 H 和总复制上限 |
| library_internal_copy_bytes | Qt 库内拥有型字符串、编码/解析/读缓冲和经源码证明的 SHA 工作复制上界；计入复制上限 |
| json_object_copy_bytes | JSON 对象/数组建造、COW 变更及显式字符串转换的拥有型 payload 上界；计入复制上限 |
| reference_descriptor_copy_bytes | 经源码证明的共享容器指针、偏移/长度索引和排序代理，不是拥有型模型字节，也不是事务 H；独立保留 |
| driver_internal_copy_bytes | 仪器 SQLite 的全部显式、编译器聚合和 realloc 搬移上界；当前全部计入合同复制量，尚未按函数豁免 |
| driver_bind_copy_bytes | 适配器 SQLITE_TRANSIENT 输入副本的单独核对值；仪器 driver_internal 已包含它，不能再次相加 |
| driver_explicit/aggregate/allocator_copy_bytes | driver_internal 的分类子集，allocator 是 explicit 的子集；保留原始值，不再次相加 |
| encoded_bytes | 全部实际编码输出，包括 SQLite OP_MakeRecord、额外 event 大小统计编码、wire、Base64、JSON |
| batch_payload/key/delete | 完整 StoreBatch/RowMutation、键、after payload、删除项和元数据；不能只取模型记录 after |
| physical_*_write_bytes | 成功 SQLite VFS xWrite 的请求字节，按 WAL/数据库分开；不是磁盘设备物理写放大 |
| scanned_records/sql_* | 校验/索引扫描、SQLite fullscan/VM 步数；另报，不据此声称整体 O(1) |

报告同时保留 `raw_conservative_copy_bytes` 与 `contract_copy_bytes`。前者把已证明
的共享描述符保守字节也纳入；后者按需求的拥有型模型数据与事务 H 计算。旧失败
报告不改写。原 raw 上界超过门槛证明该上界无法支持通过，不能直接证明实际拥有
型复制必然超标；它含潜在而非必然的容器搬移、未优化前的聚合复制、allocator
padding 和借用描述符。任何分类必须有源码/类型证据，不能凭函数名或为通过门槛
把数字实体值、owned strings、record/history payload 移走。

终端 JSON toJson 输出由调用者计一次；Qt writer 观察不再把同一终端输出计入。
Base64 的输出与 Latin-1→UTF-16 是独立边界，资源对象观察另计 QCbor 的拥有副本；
不再用旧 resourceBase64 常数与新 helper 同时计算同一转换。nested QJsonObject/
QJsonArray 的不可变共享引用在其 builder 建造一次，父对象不递归再次计 payload。

## Qt 6.11.1 审计范围

安装的 Qt 为 6.11.1；helper 编译版本与 qVersion() 都必须匹配。审计依据官方
[qjsonwriter.cpp](https://github.com/qt/qtbase/blob/v6.11.1/src/corelib/serialization/qjsonwriter.cpp)、
[qjsonparser.cpp](https://github.com/qt/qtbase/blob/v6.11.1/src/corelib/serialization/qjsonparser.cpp)、
[qjsonobject.cpp](https://github.com/qt/qtbase/blob/v6.11.1/src/corelib/serialization/qjsonobject.cpp)、
[qcborvalue.cpp](https://github.com/qt/qtbase/blob/v6.11.1/src/corelib/serialization/qcborvalue.cpp)
及安装版本的 qcborvalue_p.h、QArrayData/QList 实现。Socket 依据官方 Unix
QLocalSocket、QIODevice 和 QRingBuffer 实现；整套推导不适用于其他版本/后端。

`json_ledger.hpp` 以空 shadow QByteArray/QList 取得安装库的容量增长，没有把业务
数据复制进观测缓冲。ASCII 字符串计 UTF-16/UTF-8 拥有副本及已初始化前缀搬移；
输出 terminator 和分配器空闲区不冒充模型字节。CBor 元素的数字值仍按 8 字节、
bool 按 4 字节计拥有型 scalar；其余 ptr/offset/type 部分作为 reference descriptor
独立记录。ObjectCopies 必须按 builder 源码的插入顺序调用，不能按最终排序键
猜建造路径。retained 只适用于新鲜 factory DTO 被借用后的 COW 变更，计直接拥有
前缀与可能的后续搬移；对删除/反复替换产生的 dead byte data 不能套用。

JSON parser 接受 ASCII、无转义、严格递增且唯一的对象键。libc++ 的已排序
stable_sort/相邻比较代理按源码提供保守描述符界；超过 128 个键、重复/乱序键、
非 ASCII、转义或未审计路径均 unknown。该限制作用于观测完整性，不改变业务
JSON 接受能力。字符串方法的返回 QString 若实际拥有，必须计 UTF-16 副本；
toStdString 的 UTF-8 中间 QByteArray 与最终 std::string 是两个不同副本。

引擎的 operation 和 request_id 只从解析对象取一次 QString；ASCII request_id
的 UTF-8 长度检查借用字符视图，不再为了检查复制 QByteArray。应用输入字段的
空白检查和合法字段比较借用 QAnyStringView/keyView；ASCII 值直接构造所需的
std::string，避免旧的临时 QString→QByteArray→std::string 路径。真实拥有的最终
std::string 与 SSO move 上界仍计入；Unicode 保持原转换/校验语义且未审计路径
仍 unknown。这些是实际分配减少，不是改变 JSON 接受能力。

SHA-256 使用官方
[qcryptographichash.cpp](https://github.com/qt/qtbase/blob/v6.11.1/src/corelib/tools/qcryptographichash.cpp)、
[sha224-256.c](https://github.com/qt/qtbase/blob/v6.11.1/src/3rdparty/rfc6234/sha224-256.c)
与 sha.h。安装的私有 config 的 QT_FEATURE_openssl_hash 为 -1；测量构建显式使用
`QCAE_C3_QT_RFC6234_SHA256=1`。没有这个后端证明或版本不匹配时仍 unknown。
输入 N 字节复制至 Message_Block；finalize 聚合复制 SHA256Context；每个消息块
计首 16 字的 64 字节、A..H 的 32 字节及 64 轮各六项 4 字节 scalar 赋值，另计
32 字节 digest、64 字节 hex 与 64 字节 std::string。块数为 floor(N/64) 加一，
末段至少 56 字节时加二。保守工作 scalar 上界保留；纯数学计算生成值不伪称
输入记录的 memcpy。资源和客户端两次实际 hash 分别计数。

QTreeWidget/QTextDocument、VTK 相机/内部路径等未完成的 SDK 审计必须独立列出；
不能因 writer/parser 已有界就宣称整个 Qt/VTK 路径无未知。GUI 基准和所有库边界
须与最终构建对应的证据重新验证。

文本追加的独立源码边界见
`docs/engineering/c3-closure-evidence/qt-text-source-audit.json`，保留 Qt 6.11.1 官方
源码哈希与函数位置。`PlainTextAppendCopies` 仅计 piece-table UTF-16 QString
追加的保守上界：非空文档可能先追加段落分隔符，再追加输入；每次追加分别计
已初始化旧前缀与新输入的可能拥有型写入。调用者的输入 QString 构造另计，
不能再次重复。观察实例跟随最初空文档的全部追加，包括样本外预热；公开
characterCount 前后值、控制字符及版本不符时保持 unknown。独立 smoke 已验证
预热前缀与失步/多行拒绝规则。`tests/qt_text_ledger_probe.cpp` 又在 2026-10-01
通过真实 Qt 6.11.1 offscreen QPlainTextEdit/QTextDocument 的预热、非空/空/4096
字符追加、clear 失步和多行 unknown 边界；公开运行形状吻合仍不能证明字体、
布局、绘制内部副本或完整 QTextDocument 路径已覆盖。

源码还确认 QTextEngine::validate 会取得 QTextBlock::text，后者 reserve 后逐
fragment 复制为新的 QString；QTextLayout 默认不缓存 glyph，layout/paint 可重复
发生。这些 CPU 拥有型文本和布局缓冲没有实际调用次数观察，仍在完整管线未知
列表中。不能把图形驱动 opaque 排除当作这些 Qt CPU 副本已覆盖的理由。

## 同快照刷新合并的计量

`ipc::info_json(const DocumentInfo&)` 与 `entity_rows_json(const RecordSnapshot&, ids,
row_limit, encoded_byte_limit)` 复用原文档摘要与实体行 DTO。行默认上限为 1000，
控制编码保守预算为 65536 字节；借用 Qt 字符视图遍历预算，不为了计量再构造
一次编码。超数量/字节、缺失或非 query-kind 记录返回 complete=false 且行为空，
非法/重复 ID 或无效预算返回结构化错误。空 ID 集合直接返回完整空数组。同一
不可变快照的行不随稍后模型修订改变；正常 query_entities 的实际全记录扫描和
sources 表扫描另报，不把少量返回行冒称没有扫描。

事件明确选择 include_document_summary 后，只在真实当前修订的最后一条
DocumentChanged 携带这次 current_document 的共享序列化结果；此前 journal
事件和 HistoryChanged 不冒称历史摘要。旧读者/订阅者移除扩展字段，实际 COW
副本仍计量，gap 保留订阅选择。三个新能力分别协商，显示重绑定和完整行安装
未具备时不广告。摘要、行 JSON 建造/编码/解析及实际套接字仍计入同次操作。

独立 /private/tmp 回归已验证累计 edit/undo/redo 六事件的顺序与唯一末条摘要、
旧 wire 等价、read/subscriber/gap/metadata、同快照字段/sources 与行预算拒绝；
新 DesktopClient 能力版本、独立选择、旧握手和断线重置 7/7 通过。该验证使用
当前已构建核心静态库与新 adapter/client 源码，不代替最终统一构建、真实桌面
安装或 60 样本负载验收。既有失败报告继续保留，完整 SDK 覆盖仍 unknown。

## SQLite 3.51.0 测量构建

官方源码下载自
[SQLite 3.51.0 amalgamation](https://www.sqlite.org/2025/sqlite-amalgamation-3510000.zip)，
sqlite3.c 的 SHA-256 为
`dc58f0b5b74e8416cc29b49163a00d6b8bf08a24dd4127652beaaae307bd1839`。
源码、LLVM IR、object 全部留在 `/private/tmp`，不加入产品仓库。生产默认仍链接
系统 SQLite；测试专用 observed build 通过
`QCAE_C3_SQLITE_OBSERVED_OBJECT=/private/tmp/qcae-c3-sqlite-observed/sqlite3-observed.o`
选择同版本仪器对象。test engine 还必须使用 `QCAE_C3_SQLITE_INSTRUMENTED=1`。

`tests/prepare_c3_sqlite_ledger.py` 把源码 memcpy/memmove 包装为 C bridge 并携带
`__func__`。前端 `-O0 -Xclang -disable-O0-optnone -fno-builtin` 生成 LLVM IR，
重写全部 71 个 aggregate llvm.memcpy，再以 `-O2 -fno-builtin -c -x ir` 生成
object。编译参数包含 SQLITE_THREADSAFE=1、SQLITE_ENABLE_COLUMN_METADATA=1、
NDEBUG=1。nm 检查不允许剩余 libc memcpy/memmove/realloc 的未知调用。OP_MakeRecord
在成功编码后记录实际 nByte。新增 marker 与 sqlite3_libversion_number 双重核对，
普通同版本 SQLite 不能冒充已插桩。

realloc 包装在旧指针有效时读取旧 usable size；成功搬移地址时计 min(old usable,
new request)。macOS malloc_size/Linux malloc_usable_size 包含 padding，所以是
保守上界，不是实际 owned payload。原 explicit/aggregate/profile 数据全部保留。
结构赋值被优化掉前已经加入观测，包装也改变优化机会，因此这个构建不是生产
Release 的耗时环境；不能把它的耗时写入生产图形 P95。

独立 bridge smoke 实际核验：24 字节 memcpy/重叠 memmove 的精确计数与输出、
SQLite allocator struct assignment 在 O2 后仍被观察、SQLITE_TRANSIENT 的
4096 字节 BLOB 精确落库、SQLite 记录编码输出。Qt helper smoke 核验嵌套 1MiB
共享、不漏 numeric payload、COW 原对象不变、未审计路径 unknown，以及 SHA 的
0/1/55/56/63/64/65/1024 字节 padding 边界 digest 不变。它们不是完整端到端
SK-12 证据。

SQLite 适配器已实际复用 prepared statements，reset/clear_bindings 释放每次借用
的游标与 TRANSIENT 输入，析构先 finalize 再 close；schema/旧格式初始化保留原
路径。独立原 record_store_tests 已通过含故障/旧格式/不确定提交回归。缓存后的
同次操作 profile 和必要故障回归仍须由共享构建重新运行，再决定残余优化。

新建 row_store 改用 SQLite rowid 表和相同 `(space, identity)` 主键。旧的 WITHOUT
ROWID 表把完整 value BLOB 放在索引键中，SQLite saveCursorPosition/copyPayload
在另一个键的局部提交期间仍可能复制旧 seed history 的溢出 BLOB；prepared
statement 缓存无法移除这个真实模型历史复制。rowid 布局把 value 留在表数据中、
键留在独立主键索引，schema user_version、列、逻辑 row codec 和持久化 payload
字节保持兼容。旧 WITHOUT ROWID 文件仍能读取/提交，不自动迁移或改写源文件，
其性能不归为新布局诊断的通过证据。

独立同版本仪器 `record_store_tests` 实际保留 2MiB 旧历史后提交一个节点/事实更新，
观察 driver 内部复制为 1308 字节；旧 WITHOUT ROWID 布局兼容回归和原故障回归
同时通过。该测试不经过 GUI/socket，不能替代完整同次操作 60 样本重新测量。

## 新请求帧与事件缓存的实际复制边界

DesktopClient 请求从空 QJsonObject 按源键顺序合并借用的 context 和四个协议字段，
仍由协议字段覆盖同名 context，原 context 不改动。真正有序追加时不计不存在的
中间插入标量搬移；元素增长、新字符串、拥有型值和终端编码仍计。Unicode key
保留原转换语义并按未审计路径报告 unknown，不能把 ASCII 优化推广为全 SDK 覆盖。
无符号十进制字段直接扫描借用字符视图；空串、符号、空白、非十进制和 overflow
保持原失败语义，不为了读取数字再复制 QString/UTF-8 字符串。

EventStream 首次真实生成事件时缓存同一 compact JSON 加换行的 QByteArray。只有
保留序号和整个不可变事件对象都一致时 host 借用这份已编码 wire；修改过的
事件、旧读者移除摘要、gap 和已过保留期事件仍正常编码。共享 wire 没有再次编码
或复制模型缓冲，实际 socket 接受的完整字节仍每次计量。事件第一次编码和库内
复制仍保留，缓存的逻辑事件大小不含换行，与原 ledger 定义一致。

独立最新 event_stream 回归已验证共享 constData、调用者 detach 不影响缓存、
原逻辑字节统计、旧 wire/扩展 wire、过期/gap/修改拒绝；DesktopClient Qt 回归
实际 8/8 通过，包含保留字段覆盖和 Unicode context。纯 observer 和真实文本
探针也通过。它们不代替统一 build30、真实同次操作和三档 60 样本字节验收。

后续接收路径改用公开 `QJsonValue::fromJson(QByteArrayView)`，在输入变更前完成
解析和解码观测；对象拥有自己的字符串，之后才删除接收前缀。客户端和引擎
继续拒绝顶层非对象，原 INVALID_JSON/断线语义不变。Qt 6.11.1 的
QByteArray::remove 与 QArrayDataOps<char>::erase 对容量大于零且唯一拥有的
readAll/append 缓冲仅推进指针或减小 size；共享 chunk 和 fromRawData 仍复制
实际保留后缀。`remove_prefix` 分别记录真实 0 或后缀长度，其他 Qt 版本保持
unknown。不能把所有 isDetached 或所有前缀删除无条件当成零复制。

host 首次产生的原大帧立即计编码/库内/终端写入，即使稍后以 RESOURCE_LIMIT
替换或因 queued-byte 限额中止。错误替代帧使用其真实 QJsonObject size 为
writer reserve 观测输入，实际 socket trace 也记录替代对象。未送出原大帧不
计 socket_bytes，已产生编码不因 abort 消失，原限额和错误字段不变。

`tests/c3_host_frame_ledger_tests.py` 通过 test-only ledger engine 的固定 2MiB
oversize/512KiB large payload 实际核验完整首次编码加错误替代编码、仅错误帧
写入/trace、queued abort 的已生成字节、原对象错误 wire 与模型修订不变。
这两个固定传输夹具只在测试贡献中，不能替代 C3 的三档模型样本。独立最新
Qt client 回归 10/10、JSON observer smoke 均通过，包含真实分片/批次、Unicode、
转义、重复 key、输入寿命、共享 read chunk 和 raw-data 复制边界。统一 build31
随后实际通过 data-targeted 6/6、原业务 IPC 11/11，以及同一 host_frame_ledger
真实套接字探针；原始日志在 `c3-closure-evidence/package/*build31.log`。这些
结果只验证行为和指定计量边界，真实桌面三档模型负载尚待重新采集，完整 SDK
覆盖继续 unknown。

## 纯 C++ RecordVersion 按值复制的独立缺口

只读核对确认 `modules/document/src/document_view.cpp:70` 的
`DocumentView::version()` 直接按值返回 `version_`，包含两个 std::string 拥有型
document/epoch 字段和 revision。这不是 SDK 内部未知，也不是不可变索引描述符。
方法本身没有 ledger。下列调用方已核对，不能把其他对象的 sizeof 上界作为这些
另行产生的字符串副本已覆盖的证明：

| 调用位置 | 已有边界与判断 |
|---|---|
| `modules/document/src/edit_session.cpp:77` | prepare 返回的 base version；当前 RecordStats/返回路径没有对应 version 字符串复制项。 |
| `modules/application/src/core.cpp:358`、`:482` | prepare/commit 各检查一次 current.records.version；相邻 Prepared/Data/receipt H 项各计其自己的拥有型对象，没有这两份临时 version 的对应项。 |
| `modules/document/src/document_view.cpp:161` | candidate 校验每次产生 base/candidate 两份 version；末尾只计 seen RecordKey，未计 version 临时值。正常直接编辑在 prepare 和 commit 各校验一次，合计四份。 |
| `adapters/engine_api/src/render_service.cpp:155` | 当前修订校验产生一份；snapshot DocumentInfo 的复制不等于这份 DocumentView version 的复制。 |
| `modules/query/src/query.cpp:548`、`:568`、`:570` | inspect/update_view 的 version 临时值；render_service 的 observe_view_copy 只计实际返回 ViewSession 和按值 hidden/camera，未计这些另行产生的 version 字符串。正常 rebase 外部 inspect 一次、update 内 inspect 一次和 revision 检查/赋值各一次。 |
| `modules/query/src/entity_queries.cpp:149` → `adapters/engine_api/src/ipc_model.cpp:260` | EntityPage 的一份 version 已由 query_page 的显式 sizeof(RecordVersion)+document/epoch 长度计入；不能再次加一次。ReferencePage 等其他调用没有据此获得覆盖。 |
| `modules/query/src/render_projector.cpp:213`、`:216` | 正常局部成功分支现有 :240 的四份 document 字符串上界足以容纳两份 guard version、delta.document 和 retained ViewSession.document。若以后在 getter 统一计量，需一起调整这个上界避免重计；提前失败分支没有走到 :240。 |

因此当前普通局部编辑至少有七份 application/document 返回 version，以及正常
render rebase 路径五份 service/selection version，尚未找到各自的对应复制项。
这只是静态源码计数，不把推算值补入旧样本或填成测量的零；旧 raw 总量继续保留，
纯 C++ 自有字段覆盖也明确不完整。DocumentView 的隐式复制、其他 version 调用、
错误分支和其他纯 C++ 拥有型字段还需独立核对，不能宣称纯核心已全面覆盖。
以上描述 build31 插桩前的具体缺口，未追补旧样本；build32 的源头插桩及
EntityPage/projection 去重如下，门槛和字段归属不变。

## build32 借用 ASCII 编码与 version 计量实施边界

最新 build31 同次 N1000 诊断保留原始失败：材料 84719—88501 B，10 个样本均超
69632 B；节点 222391—235863 B 的指定观测上界通过，完整 SDK 仍 unknown。
下一轮编码仅处理整帧无转义 ASCII 键/字符串、null、bool 和有限安全整数。
整数先与 Qt scalar compact 编码逐字节证明相等，实际 probe 的 owned 输出、
库内临时值和本地数字缓冲仍计量。Unicode、转义、非整数、非有限数、超深对象
或任何不等价值都使整帧回退原 Qt 编码；预检不改变对象、字段顺序或接受范围。
精确 reserve 后直接写借用 ASCII 字符，不创建临时 QString/UTF-8 字符串；请求、
响应及首次缓存事件共用这一编码边界。原限额、oversize 替代和 queued abort
仍在真实编码之后判定，生成而未送出的编码不能消失。

DocumentView::version 源头按每次实际成功产生的值计 records H：
sizeof(RecordVersion)+document.id/epoch 字符串长度，不改变返回值或归属。
ipc_model::query_page 已有 EntityPage.version 的同一项完整扣除；projection
局部成功上界仅扣两个 getter guard 已计的 document 字符串副本，保留 delta、
retained view、view ID 和原数值上界。其它 ViewSession、DocumentInfo、receipt
都是不同副本，不能扣除。新计量不会追补或改写旧 raw，也不宣称其他纯 C++
隐式复制或 SDK/font/layout/paint 已全面覆盖。

实现位于既有 `adapters/transport_local/include/qcae/json_ledger.hpp` 的
compact_frame：预检和输出只借用 const 容器、keyView/toStringView，未修改
QJsonObject 或其共享源。数字先限制在 ±(2^53−1)，再将 std::to_chars 结果与
该值的 Qt scalar toJson 原始输出比较；只有全帧都通过才写一个精确长度的
未初始化 QByteArray。ASCII 字符、分隔符和末尾换行全部写入，该终端输出按
model_copy_bytes 计一次。本地数字栈写入和拥有型 QJsonValue 数值临时值另计，
Qt probe 的 terminal 输出与内部数字格式化另计 library_internal_copy_bytes。
encoded_bytes 延续完整协议帧输出定义；probe 与 Qt 内部 escapedString/数字
formatter 一样是已计复制的编码内部临时值，不伪造 socket 帧。失败预检已发生
的临时复制保留，再完整计原 Qt 回退编码；首次事件 wire 缓存复用仍只编码一次。

独立 `/private/tmp` 实际编译/运行通过：JSON writer 逐字节兼容与真实 probe/
失败预检计数、record_document（源头两份 owned version exact H 和原 3 档
record 层 locality）、event_stream 原共享缓存/旧读者回归、Qt 客户端 11/11，
以及真实 host_frame_ledger 固定 oversize/queued-abort socket 探针。客户端新增
真实 socket raw-wire 对照涵盖 ASCII、嵌套 null/bool/整数、转义、Unicode、
浮点和 exponent 来源整数；原 context 覆盖回归同时检查完整 Qt wire。
这些独立测试不是桌面三档 60 样本，不据此改变材料失败结论或完整覆盖状态。

## build33 坐标 range 的窄观测计划

VTK 9.7.0 的 vtkPoints::ComputeBounds 直接调用 ComputeScalarRange，不会填充
GetRange 的 PER_COMPONENT 缓存。VBO 的默认 AUTO_SHIFT_SCALE 另调用三次
GetRange，启用自动 shift/scale 时再调用三次；缓存命中仍构造 2*C 个 double
的临时向量。因此不从一次 VBO 上传猜测一次扫描，不预先调用 GetRange。

仅给现有私有 double 坐标数组增加子类，包裹原 ComputeRange/ComputeScalarRange
虚调用并接同一个 operation tracker；数值、AOS 布局、bounds 和缓存策略保持
原实现。当前上界只支持实际 VTK 9.7.0、C=3、无 ghosts、非负分量、运行时
Sequential backend；其他实际调用和计数溢出保留具体 unknown。每次 range
调用计输出初始化、临时 2*C double value-initialization 和输出两标量；真实
cache miss 另计三份两标量缓存。每次 scalar scan 独立计 range 初始化、
归并缓冲、Sequential TLS 创建/赋值/初始化、归并输出，以及按实际 N 点数
限制的标量读取和至多两次 accumulator 写入。非空扫描保守上界为
7*(2*C*sizeof(double)) + 3*N*C*sizeof(double)，空扫描为
2*C*sizeof(double)；这些是源码逐项逻辑写入上界，不是 malloc 或 GPU 字节。
range 调用另外计 (2*C+4)*sizeof(double)，只有真实成功的冷缓存增加
2*C*sizeof(double)。shared tuple/array 指针不复制整个坐标缓冲。

独立无 GUI 探针已实际验证冷/热缓存、Modified 失效、AUTO 近/远点的 3/6 次
调用和 DISABLE 的零调用。实施后再跑私有 header 的真实 VTK 库回归。自动
shift/scale 的临时/保留向量与 mapper CPU transform 仍须独立观察，不能因
range 入口已覆盖就把整段 CPU 模型缓冲改成已知；GPU upload 参数计数保持
原边界，未重计。

## build34 纯 C++ 拥有型边界补齐计划

先把 commit 的 optional::value_or eager fallback 改成条件分支，只在没有
direct_signature 时构造原 commit_signature。结果字节、幂等比较和原提交
路径保持不变；实际消除的字符串不再计，仍产生的字符串按真实 size 和
capacity 增长逐 append 计事务 H，包含数字临时输出和原初始化/复制。

DocumentView 显式 copy constructor/assignment 继续共享 immutable state，
分别在真实 version_ 复制成功后计 sizeof(RecordVersion)+doc/epoch 字符串
长度为 records H；失败保持原异常并标该具体边界 unknown。move 保持默认
noexcept 所有权转移。copy_data 的原 sizeof(Data) 恰扣这一份
sizeof(RecordVersion)，原 DocumentInfo/preview/pending 拥有型字段保留，
不能把它们当作 RecordVersion 的重复。update_document/初始激活构造的新
RecordVersion DocRef 副本单独计；与旧 DocumentView 的隐式复制是不同对象。
RenderProjector::version 返回值在自己的源头计 projection H，不由已计的
ResourceVersion 或另一 DocumentView::version 代替。

EditSession 的 put 本地 RecordKey、成功插入的 map key 和 change.key 各
计自己的 H（身份字符串仍是实际拥有型负载）；find 的查询临时字符串单独
计。prepare 初始 RecordChange 拷贝仍只计一次；field vector 每次真实写入
按 field 大小计，capacity 改变时另计其旧 initialized prefix。apply 的
seen key 插入另计，不能借用 validate 的另一个 set 的记录。旧 immutable
Record/shared_ptr 没有深复制模型缓冲，不把真实字符串换成索引描述符。

signature/ID 构造改用借用 string_view 的逐段 append，按真实新段写入和
capacity 变化计 H。Prepared/preview response、初始 receipt/fact 中不同
对象的拥有型字段也逐项核对，persist 的 encoded=o 已计克隆保留；它不
抵消较早的 receipt/signature 构造。新增测试保护独立 version 值、self
copy/move、原 candidate 信任验证/故障原子性/幂等和 signature 旧字节。
所有固定门槛保持原值，新 H 如导致真实样本超限则继续修实际复制。

build34 同轮扩到材料 feature 与共用 canonical builder：typed canonical input
复制、normalizer 的新 Quantity、generated fresh-emplace Value 投影和 quantity
initializer-list 的额外节点副本按模型字段计；字段键/规范签名按事务 H 计。
输入移入 callback，builder 终端字符串移入 OperationPlan，保留 callback
签名和 RecordPreparedOperation 输出的真正克隆。该输出与 State::prepare
的 Prepared/ChangePreview 是不同对象，分别计，不用后者抵消前者。
record probe/创建临时值与 RecordRegistry descriptor.encode 的 typed→field
投影也是不同复制。builder 只借用源 Value，不重复计输入树；初始 QCV1、
数字临时字符串、浮点栈缓冲和 append 的 initialized prefix 扩容均按实际
size/capacity 计。旧 QCV1 编码、有限数/深度拒绝、单位归一、幂等和提交
路径保持不变，失败前已发生的复制保留，不追补旧 raw 结果。

共用 Value 成功构造/复制在源头计其真实 scalar/string payload；Array/Object
移动只转移存储，copy 的 child Value 递归计，Object 的新 key 副本独立计 H，
不再加第二个全树总量。libc++ allocator-free std::string move 的 short rep
在实际 capacity 守卫下计 inline 字符，long rep 只转移地址；其它 STL 保留
这个具体边界 unknown。UTF8 helper 已计终端 std::string，新 Value(string&&)
只计后续 SSO transfer，Qt 的新 QString 编码另属不同对象。receipt DTO 在
局部 observer scope 内保持事务 H，其 scope 不影响任何模型 Value 的字段。
生成器只添加 aggregate-copy/key 观测，不改字段/schema/校验与 wire 输出；
wire 的 owned 返回字段与生成器最终 typed aggregate 克隆分别计。共享纯只读
Value 源和 moved 容器不按整树深复制计。fresh receipt/Quantity emplace 去掉
原 initializer-list 的真实克隆，同时保留所有新建字段/key 的源计量。

build34 独立验证（没有写共享 build，也没有启动 GUI）：实际 root Release
flags 编译九个本轮数据拥有型 TU 及五个相关测试 TU；仅在 /private/tmp 复制
当前 archives 并替换这些 owned objects，实际运行 record_document、
record_application、model_operation、operation_registry、event_stream 共
5/5 通过。新增回归验证 DocumentView copy/self-copy/move 的独立版本字段、
put/replace/find 的不同 owned key，Value 的两次递归深复制与容器移动，
receipt 的 H source scope 恢复，以及 canonical 的旧 tag/排序/int64/NUL
逐字节输出和 callback 输入独立性。原 record-layer 三档 60 次回归仍运行；
它不是同次真实 IPC/桌面 SK-12 六十样本。生成器实际 CMake 增删 schema、
参数空对象与非法 allow_empty 回归 3/3 通过，不修改 solver schema 字段。

Value 源头与既有计数去重核对：typed_host decode 的 utf8 终端 std::string
仍属较早输出，新 Value 接管只计后续 SSO inline move；typed_values 没有
旧纯 C++ Value tree-copy 公式，initializer-list 的真实 child 克隆现在由
中央 copy hook 逐项记录；from_utf8/Qt ObjectCopies 是之后新建的 QString/
CBor 字段，没有扣除。canonical 不重计源 Value，材料不再对 generated
Value 添加第二个全树配方。State 的 Prepared/receipt/fact 和 persist
encoded=o 均是不同阶段实际拥有型副本，原计数保留。DocumentView header
唯一 dedup 是 copy_data 的 sizeof(RecordVersion)，EntityPage/projection
先前 build32 的 guard 去重不再扣第二次。

root 初次 Release 编译新的 VTK range test 发现 assert 被 NDEBUG 去除，
该失败日志保留；测试已改成所有配置都执行的 check，并独立按真实 root
Release/NDEBUG/Werror flags 编译、链接、运行通过。没有修改 VTK 产品。
本轮仍不宣称完整纯 C++/SDK/字体/paint 或正式三档六十样本通过；其他 owner
的 runtime/task/solver source 不由这一组局部测试获得覆盖。

## build35 材料提交的有限复制削减计划

build34 同次实测材料的已知复制与真实 H 合计仍超过 69632 B，且 SDK 仍有
具体 unknown。本片只重开 application 的原数据 owner 文件，保留旧失败报告、
所有上限和计量定义；不修改 Qt、桌面、solver、生成 schema 或公共 wire。

持久化 Writer 根据本次已有不可变字段与 history transaction ID 借用计算
相同 wire 的最终长度，在写入前一次 reserve。每段的新字节写入和之后任何
真实 capacity 增长计数继续保留；不把输出或 history payload 改成零。新
operation fact 在插入 immutable 表前完成原格式编码，pending 使用同一个
encoded shared string，从而去掉 persist 的 encoded=o 深克隆与第二次 AVL
replace。其他 unencoded fact/host fact 保留原 fallback，旧恢复格式不变。
execute 向同锁下同步的 private commit 借用它已生成的 scoped key；公共
commit 仍自己构造，借用不越过调用生命周期，也不替代输入或权限检查。

验证先保护旧 wire 逐字节、恢复、幂等重放、候选信任和前/后持久化故障；
独立 /private/tmp 严格 Release 编译与现有测试，不写共享 build 或运行 GUI。
实际去掉的 prefix/fact/key 副本从其源头消失；仍存在的字段复制继续计原
model/H，不通过调低配方或转为 descriptor 获得预算。

同片补充私有 preview 的不可变共享：Prepared 已在全部校验之后建成，插入
后只有只读消费与 map erase，没有字段修改。将 Data::previews 的值从拥有型
Prepared 改为 shared_ptr<const Prepared>；candidate Data 仍复制自己的 map key
和 entry header，初次 Prepared/context/signature/label、公共 ChangePreview 与
新的 RecordVersion 都继续按真实源计。预构造 shared payload 再 emplace，
next_id/statistics 仍只在强保证插入成功后发布，失败不消耗 ID 或留下 preview。
这消除一次候选 Data 的旧字符串深复制，不把这些字符串重标为索引，也不把
其它 DocumentInfo、pending key 或新签名计数扣掉。

DocumentView 的 guard 只比较现存 version 字段，不需要返回拥有型 RecordVersion。
新增 boolean matches_version 借用自身字段，core 的 base 检查使用它；friend
candidate 校验直接借用两侧私有 version，不生成随后销毁的字符串。公开
version() 按值合同与真实源计保持不变。with_version 通过私有共享 state 的
构造器接管新 version，去掉“先复制旧 version，再立即替换”的真实冗余；
新 RecordVersion 的已有源头构造/复制仍计，页/Record 的共享与版本独立性
保持不变。此处削减来自源代码不再产生旧副本，不扣其他 getter/copy ctor。

build35 独立严格 Release checkpoint 已实际通过 record_application、
record_document、model_operation 和 fresh event_stream 共 4/4；原 record-layer
三档 60 次回归保留。新增检查逐字节保护 v1 fact/metadata（包括 Unicode、
NUL 签名、32 条历史和 save-intent 分支），并确认 reserved Writer 对所有
输出字节保持真实 H。版本 guard 测试初次把 empty-set 的显式 known 0 误当
“没有 counter”；失败保留，已修断言为 known 0，没有改产品计数。

独立同层 material execute 的前/后各 30 次探针，后 10 次每次 application H
减少 4890 B、records H 减少 938 B，model 314 B 保持不变，合计真实减少
5828 B。初始三项优化单独减少 4480 B；这是原代码不再复制旧 prefix、
fact、key、preview 和 guard version 的结果。它不包含 IPC、SQLite/Qt SDK
或 GPU，不替代同次桌面门禁，也没有达到“额外 10 KiB margin”的目标；
仍需 root 新鲜统一构建与真实样本核算剩余已知/未知项。

首次手工链接 event_stream 的旧 test TU 和新 runtime 时实际 SIGBUS；系统
崩溃栈进入 TaskService::State::run 的 completion 调用。旧弱 RecordTaskPayload
vtable 只有 32 B（无新 completion slot），fresh test TU 的表为 40 B，重新
编译后实际通过。旧 crash/失败日志保留，不能把混 ABI 的结果归为业务通过；
正式验证仍由 root 统一重编所有派生 TU。独立编译、运行及前/后探针日志在
`/private/tmp/qcae-build35-owned/`，design/clang-format 21/diffcheck 实际通过。
