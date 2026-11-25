# T015：SQLite schema、迁移和事务访问层验收

日期：2026-11-03。验收范围为本机 C++14/v142 的元数据基础层；阶段门不随本工作包自动关闭。

实现：[catalog.hpp](../../include/awb/catalog.hpp)、[catalog.cpp](../../src/core/catalog.cpp)、[统一服务](../../src/core/service.cpp)。取舍及未覆盖内容见 [ADR-002](../adr/002_SQLite索引迁移与独立验收.md)。

## 可核验标准

| 类别 | 真实执行与断言 | 对应测试 |
| --- | --- | --- |
| 创建及身份 | 中文路径/名称、emoji 和 SQL 特殊字符完整保存；新库应用 v1/v2、application_id、mode、迁移记录 | storage/core |
| 环境原子性 | 写完环境和首来源后第二来源触发失败，核对实际 SQL 行数不留下半条 | storage |
| 旧 JSON | 重复 ID 整批拒绝；保存原字节及只读副本；多环境第二条写入失败整批回滚；去除故障后重试 | storage |
| 旧清单检查错误 | 实际Windows删除挂起路径报检查失败，不能静默当作文件不存在 | storage |
| 自动迁移 | 通过 Service 打开旧受管根，保留 ID；迁移后创建新环境，再次重开不导入残留旧清单 | core |
| 数据库升级 | 独立构造完整 v1 关系表；升级前生成仍为 v1 的快照；核对迁移记录和版本原子更新 | storage |
| 升级故障 | 源库写入返回 SQLITE_FULL，v1 字段/版本/数据和迁移记录保持原状；随后重试成功 | storage |
| 并发/争用 | 4 个线程共写 32 环境无丢失；另一连接持有写锁时 2 秒后 STORAGE_BUSY | storage |
| 磁盘满 | 测试 VFS 返回 SQLITE_FULL，真实 SQLite 回滚，前序环境保留，没有假成功 | storage |
| 提交 I/O | 实际 fsync 后注入回执失败，标记结果不明，后续访问 STORAGE_REOPEN_REQUIRED；重开后已确认环境仍在且无半条来源 | storage |
| 在线快照 | 创建快照后继续增加环境；恢复仍得到快照时的一条记录，证明不是引用当前源状态 | storage |
| 恢复保护 | 恢复到新文件；已有目标拒绝覆盖；损坏源拒绝且不创建空目标 | storage |
| 文件边界 | 实际只读文件报错且原字节不变；实际硬链接拒绝 | storage |
| 版本/损坏 | 未来版本不修改；改写迁移记录拒绝；非 SQLite 文件拒绝，不回退空库 | storage |
| WAL 恢复 | 子进程未提交 5001 条记录生成 WAL 后直接退出，重开不可见；另一子进程提交后直接退出，重开仍保留 | storage |
| 共享入口回归 | 两种配置的原生窗口/撤销、122 条真实浏览器命令、HTTP 代理、官方 MCP SDK 互操作 | CTest/browser/proxy/MCP |

运行方式：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Configuration Both -WithBrowserTests -WithMcpTests -McpPython E:\project\webexploer_codex\build\test-venv\Scripts\python.exe -Browser E:\project\webexploer_codex\build\runtime\chrome-win64\chrome.exe
```

构建脚本运行三组 CTest，将真实 storage-report 保存到每次独立证据目录，再执行浏览器/代理/MCP 回归及自有源码审计。测试 VFS 不链接入工作台、CLI、MCP 或 awb_core。

## 通过证据与剩余范围

本轮最终双配置结果保存在 [SQLite 证据目录](../execution/evidence/2026-11-03-sqlite-final/summary.json)，包含两个 storage-report、构建/CTest 日志、浏览器/代理/MCP 报告、源码哈希和依赖配置证明。全部结果已通过；T015标为完成，原T014/G0前置门继续保留。

T016 仍缺修改、删除、回收站、运行中删除保护、流程引用保护及完整路径边界；revision 存储列不算这些能力。T055 仍缺会话/凭据/加密包/跨机器完整恢复和产品入口。后台数据、免费额度、计费、长稳、断电与干净机均未由 T015 验收覆盖。
