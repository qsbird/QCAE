# 核心生命周期、生产装配与本地交付验证（2026-10-03）

实现提交 `f2d2e7f25eb2b6218f233c1612a0b16a7705f06a` 已完成本轮适用回归和本地打包运行验证。五个已有工程生命周期入口公布生成输入及版本合同；生产宿主从实际贡献选择 codec、Profile 与显示服务。核心40、本地54、桌面/SQLite94、能力包关闭75、专项ASan/UBSan2，共265次实际CTest执行通过。完整SK、C3/C4、真实求解/完整AI、性能及P0发布仍待验收。

## 已有入口的输入合同

`project.create/open/save/save_as/close` 在进入原应用服务前解析生成DTO和可选正uint32 `requested_version`，版本1以外的有效版本返回 `SCHEMA_UNSUPPORTED`，非法参数/版本返回 `INVALID_INPUT` 及字段。拒绝先于生命周期、文档上下文及宿主幂等登记。发现信息公布实际 wire 类型、schema、版本及封闭参数对象；其他legacy入口保持原范围。

| 原宿主操作 | 生成输入 | 参数规则 |
|---|---|---|
| `project.create` | `ProjectCreateInput` | 必需非空字符串 `name`；应用原有1024 UTF-8字节上限保留 |
| `project.open` | `ProjectOpenInput` | `normal` 必需非空 `path`；`recover` 禁止 `path` |
| `project.save` | `ProjectSaveInput` | 可省略或使用空字符串 `path`，继续由原应用解释保存位置 |
| `project.save_as` | `ProjectSaveAsInput` | 可省略或使用空字符串 `path`，保留原应用的成功/拒绝行为 |
| `project.close` | `ProjectCloseInput` | 必需 `policy`，发现枚举为 `discard` / `keep_recovery` |

这些仍是原宿主适配路径，继续使用 `MemoryApplication`、原调用者/宿主键命名空间、操作事实查询和有限事务历史，未向 `OperationRegistry` 重复注册业务handler。省略与空保存路径保留原归一及重放关系，显式路径与原始空路径的同键冲突也保留。公开 `allow_empty` 只接受字符串与实体ID数组，现有默认拒绝空值的使用不变。MCP从实际发现获取五个输入，仍使用原有有界schema处理。

## 实际贡献决定生产服务

`EngineAssembly` 保存贡献提供的拥有式 `IModelCodec` / `IProfileProvider` 绑定及显示工厂。Nastran贡献引用协调器实际使用的同一codec与State；移除协调器包装后，装配保留的绑定仍能实际编码/解码。生产宿主把这些实际端口传给IPC和Profile支持判断，并使用冻结的显示贡献；编译开关只决定默认贡献列表。

装配拒绝重复贡献ID、空端口、无所有权或不同所有权的绑定、无效Profile定义及重复codec/显示工厂。P0保持单codec绑定、单显示工厂的静态启动范围，缺显示工厂时使用既有通用投影。拒绝发生在装配返回和TypedHost发布之前；本地socket监听及部分工作库初始化更早，不能声明拒绝先于一切端点/存储副作用。

新增测试入口在能力包编译为ON时只传入通用模型贡献，并调用同一生产宿主：实际发现不公布Nastran Profile、导入/导出或专有UI；缺codec调用明确拒绝，通用创建、几何、查询与undo仍能运行。它是受控测试入口，未增加产品进程或动态插件框架。六类贡献的完整发现元数据及其余legacy契约仍是接续工作。

## 同一候选的实际回归

本轮冻结414份源码、测试和构建输入，规范排序的路径→文件SHA256 JSON摘要为 `38ec95e3d66aee93fd127cb5d47c64170d99363d59ce2ba37befcec4be191907`；这不是 `C4-source-v1` 摘要算法。全部运行前后字节一致，实现提交之后再次核对相同。实际环境为macOS arm64、Apple clang 21、Qt 6.11.1、SQLite 3.53.4、已有Qt支持VTK及Python 3.14.0，无新增依赖。Python版本来自本轮CMake实际配置日志及同路径解释器复核，不沿用早期环境记录的3.14.7。

| 实际配置 | 范围 | 结果 |
|---|---|---|
| Core Release | IPC/SQLite/桌面关闭 | 40/40 |
| Local Debug | Qt本地IPC，SQLite/桌面关闭 | 54/54 |
| Desktop Release | Qt/VTK、SQLite、Nastran包开启 | 94/94 |
| Package OFF Release | Qt IPC/SQLite，Nastran包关闭 | 75/75 |
| Sanitized Release | ASan+UBSan，`typed_host` 与 `engine_assembly` | 2/2 |

五配置重新配置和构建，编译命令均严格C++20、编译警告0。专项消毒器只运行表中两项，`detect_leaks=0`，LSan未测。桌面回归的 `c3_desktop_workflow` 有两项实际AI场景因未配置 `QCAE_AI_UNITS_SNAPSHOT` 跳过；94项CTest通过不能把内部跳过改为AI验收通过。设计检查、235份手写C++格式及diff检查均通过；文档接续另执行同样门禁。

QG-02分别审查生命周期及装配源码，最终哈希与冻结候选一致，无阻塞发现。生命周期审查记录一个非阻塞文案问题：允许空保存路径时，错误类型提示仍写“non-empty string”；实际错误码、字段及schema正确，未为此增加性能或重构范围。

首轮本地/桌面构建因新增测试误比较 `DocumentRef` 而失败，未运行该轮CTest；只修正测试的ID/epoch比较和重复查询。后继使用本轮新建、首次失败的缓存重新配置/构建并实际运行，保留原失败候选及日志，不称为每次均删除缓存重建。更早的预检曾增量使用 `/private/tmp/qcae-project-open-checks126-successor02-builds/local`，该路径的旧binary已被覆盖；旧收据/LastTest与272成员归档保持原样。本次265项使用独立141目录，不能把旧路径当前binary当作旧哈希证据。

## 本地包及重定位

本机包为 `/Users/qs/Documents/ChatGPT/QCAE/out/local-delivery138-02/QCAE.app`，实际复制到 `/private/tmp/qcae-relocated138-02/QCAE.app` 后验证。包内419个普通文件、145个Mach-O，925条非系统依赖边均解析到重定位包内；原包与重定位包的文件哈希及符号链接目标一致，deep/strict本地adhoc签名验证退出0。五份原构建产物前后未改动。

staging manifest SHA256为 `8d5fdd2df0a77aefa4fdc06ef53ae6ceb2155e65c1065745792efc787920af64`。manifest的 `source_dirty=true` 对应主工作区原有AGENTS.md修改；提交及414份实际构建输入另行逐项核对。本地包不提交Git，完整发布/公证未完成；arm64、声明最低macOS26，仅在本机macOS26.6.2验证。MCP需要已有Python≥3.10，未捆绑解释器。运行方法见[交接说明](../implementation/core-local-handoff.md)。

| 包内实际流程 | 证据与结果 | 有界范围 |
|---|---|---|
| SQLite生命周期 | 39次进程命令；创建/编辑到修订2、save/save_as、keep_recovery、独立engine重启恢复；8次参数/版本拒绝的16份全表前后原始行相等，同键正确请求随后成功 | 1个材料，保留文档身份及历史、更新epoch并拒绝旧epoch；不是完整M/全故障测试 |
| 双MCP | 5次进程命令；两个独立stdio客户端各发现同一68工具，13次JSON-RPC请求及2次通知；跨客户端同键重放同一事务，CLI读取同一材料与修订 | 本地标准库桥，无外部AI模型调用 |
| 真实桌面 | 9次进程命令；包内Cocoa Qt/VTK加载实际BDF夹具，13实体/2节点/1梁；真实PNG经人工查看，GUI退出0且engine继续可查询、修订不变 | `--smoke`，不是正式图形选择/帧率/容量验收 |

上述运行总计55次进程命令，包括打包/签名2次；CLI实际46次。四个自有engine进程均SIGTERM退出-15并回收、无强杀，两个MCP客户端EOF退出0，桌面退出0。每个流程使用独立socket/工作库，源码、包及原构建产物的前后保护均通过，不将并行耗时当作性能测量。

包的前两次失败分别保留：第一次在产品命令前因Qt6_DIR目录别名身份不一致拒绝，后继改为已封存同一 `Qt6Config.cmake` 文件身份核对；第二次staging因PDF图像插件的QtPdf搜索路径缺失退出1，最终配置增加已有Qt聚合库目录后退出0。没有安装库或降低依赖闭合检查。失败partial包保留在原路径，与最终包分列。

## 验收对应、归档及接续

本轮对应REQ-08/10的工程生命周期及恢复、REQ-15的身份/epoch/幂等事实、REQ-16的可发现输入合同；TST-F08、TST-A05、TST-A12及SK-04/06均只登记上述适用子集。实际SQLite/MCP/桌面运行使用正常包内程序，测试用平台贡献入口和先前仪表证据分别标注，不能代替全部正式验收。

[544成员归档索引](core-local-delivery-evidence/2026-10-03/archive-index.json)及[终态登记](core-local-delivery-evidence/2026-10-03/validation.json)保存方法、源摘要、原始命令/退出码、CTest日志、前后原始行、MCP逐行请求响应、GUI图像及失败记录。归档逐成员独立读回，并再次读取原件核对一致；不包含运行数据库、工程快照、socket、产品binary或整套生成build目录。归档SHA256为 `eda62fa31a0487bb0f407d2791f2a992cce7339c4f7662729606090db3c4e541`。

测试后的文档提交只改变414输入中的 `adapters/engine_api/README.md`，其他413份冻结输入保持相同；文档更新不冒充重新运行265次测试。早先378次统一验证、177次project.open验证及本轮265次保持各自源码与分母。

接下来优先收敛实际六类贡献的发现信息及剩余legacy合同，沿用原回调和应用提交链。真实Nastran配置/数值闭环、完整外部AI、完整SK及P0仍待完成。SDK内部复制覆盖保持unknown/null，仅从C4启动前置项暂移；已测约束不变，旧材料30次中14项预算失败未在本轮重测。本轮遵循核心框架质量优先，未进行非必要性能优化。
