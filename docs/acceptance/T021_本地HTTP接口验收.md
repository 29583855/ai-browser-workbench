# T021 本地鉴权HTTP接口与CLI验收

日期：2026-11-04。结论：本地REST/CLI工作包独立验收通过；T020完整步骤证据/全入口审计和G0仍未关闭。

新增Winsock回环HTTP/1.1服务，CLI使用--http PORT显式开启；保留默认JSONL。仅127.0.0.1/独占端口，随机令牌启动轮换、私有配置、Host精确校验、拒绝Origin/Sec-Fetch-Site及CORS。请求大小、接收预算、正文长度严格限制。工具注册表、参数和处理入口与CLI/MCP/UI共用，资源修改使用If-Match/CAS。

实际路由、21工具参数、状态码、结果未知语义、正常退出、日志轮换及调用样例见[API参考](../20_本地HTTP_API参考.md)。不提供任意CDP转发或中央账号/计费路由。HTTP当前串行，不声明并发调度/长任务事件协议已完成。

[Debug](../execution/evidence/2026-11-04-implementation/Debug-api/report.json)与[Release](../execution/evidence/2026-11-04-implementation/Release-api/report.json)各42次实际HTTP请求及5种原始错误报文通过，注册/登录服务器各确认1次；覆盖鉴权、浏览器来源/Host、内容类型/版本、端口独占、令牌轮换、无秘密日志及退出Job清理。

文件占用故障分别证明：接受记录无法刷盘时不创建环境；真实按钮提交1次后完成日志无法打开时返回AUDIT_WRITE_FAILED/outcomeUnknown=true/auditRecorded=false，调用方只读核对而不重发。不能把日志失败视为动作没有发生。

前置T020尚有完整证据系统缺口；本项在共享Service上独立验证接口，沿用ADR-002子项验收原则，不改变整体集成/发布依赖门。
