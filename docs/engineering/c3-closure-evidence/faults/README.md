# 220次故障测试原始证据

`tests/skeleton_fault_tests.cpp`执行F01—15（F05除外）和F18—21各10次，共180次，使用真实SQLite RecordApplication提交、历史和幂等事实。F09/F10通过 `posix_spawn`/exec同binary child，在确定性 `after_db_commit`/`after_project_publish` hook `_Exit(86)`；父进程用完整RecordApplication恢复，检查实际模型、历史、原key事实，以及文件/保存intent token。F21通过真实生成及公开再生任务注入unmapped外部引用。

`tests/skeleton_fault_client_tests.cpp`执行F16/F17各10次，通过真实本机Qt socket的native DesktopClient注入event gap/reconnect及reply/event/gap callback同步销毁+buffered/late回包。F17另在实际ASan/UBSan构建重跑，sanitizer记录独立于正式220条，不通过重复计数扩大故障run数量。

`tests/skeleton_fault_ipc_tests.py`执行F05和F22各10次，共20次。F05向真实engine发送写请求而不读取其成功回复，独立observer确认唯一transaction后关闭写connection，再用原key/context/parameters重试，核对原transaction及全部model/history/revision不变。F22每次在真实M→Nastran已发布产物→R reader中分别注入错误fingerprint/location/number map及source_kind，并比较document、全history和原结果不变。结果始终为fixture。

`tests/skeleton_fault_report.py`校验实际180+20+20条，逐F必须恰10条通过、run_id唯一、source tree一致、必需input/expected/actual/passed字段完整。检查实际F05 wire loss、child exit86、save token一致、R三类负样本，并要求F17独立10次的ASan/UBSan instrumentation和清洁日志。mutable诊断明确不满足冻结产品验收。

诊断日志保留失败，不重写为通过。`diagnostic/application-first.*`为首次测试夹具失败和JSON编码错误的原始字节记录，不能作为可解析220条最终证据；`application-second.*`保留原epoch执行重试/签名不匹配两个测试前提错误；`application-third.*`190/190通过。`client-first.*`20/20、`client-sanitizer-first.*`实际ASan/UBSan20/20、`F22-second/`10/10。`F22-first.log`保留第一次错误binary路径的启动失败。

`diagnostic/report.json`是增加实际F05 wire loss前的旧220次诊断。旧F05仅丢普通API返回值，不能顶替丢网络响应的正式验收；`diagnostic/ipc-wire-first/`另有实际F05/F22共20/20。当前严格checker要求实际wire loss，旧190条C++报告不能与新20条IPC简单相加。正式证据必须在同一冻结源码与构建配置上重新运行，不从这些不同诊断快照拼接。新的每次调用会使用独立nonce，失败和后续重跑run_id不会重复。JSON中持久化encoded字节使用Latin-1逐字节Unicode转义，比较的是原始精确字节而非裸SQL统计行。

`diagnostic/application-fourth.jsonl` 为最新180/180应用测试。`diagnostic/report-wire.json` 使用它及真实IPC20/native客户端20核对220个唯一run_id、每项10/10及独立ASan/UBSan F17，共22项运行断言通过；checker唯一拒绝原因为 `mutable-diagnostic`，所以不标记冻结验收完成。
