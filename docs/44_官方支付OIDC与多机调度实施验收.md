# 官方支付、OIDC与多机调度实施和验收

日期：2026-11-05。工程要求：VC++2019 v142、x64、C++14；保留原文件编码和换行，新增函数包含中文用途和日期。

用户已授权先实现功能，真实商户、身份服务和节点部署参数留空。本轮实现支付宝开放平台RSA2、微信支付API V3、Stripe Checkout及退款查询；OIDC授权码/PKCE及RS256校验；单协调器管理多个真实执行节点的持久任务调度。外部联调结果与本地协议验收分别记录。

## 执行清单

- [x] X001 有界HTTPS客户端、表单编解码、Windows原生RSA/JWK/AES-GCM能力及参数模板。
- [x] X002 支付宝预创建、查单、全额退款、退款查询、原始响应及通知验签。
- [x] X003 微信Native下单、查单、退款、退款查询、RSA签名/响应验签及AES-GCM通知解密。
- [x] X004 Stripe Checkout、查单、退款、退款查询、原文Webhook验签。
- [x] X005 渠道操作持久幂等意图、未知结果核对、到账/授权/账本原子更新与门户操作。
- [x] X006 OIDC发现、授权码+S256 PKCE、state/nonce/浏览器绑定、RS256/JWK/issuer/audience/期限校验。
- [x] X007 OIDC主体绑定、防邮箱自动合并、HttpOnly会话、注销、桌面一次性连接码。
- [x] X008 项目、节点一次性注册/审批、独立密钥身份、签名出站协议、维护/撤销及能力心跳。
- [x] X009 环境分配、流程不可变版本、任务幂等入队、事务Outbox和节点持久Inbox。
- [x] X010 多节点容量/全局免费配额、中央租约和本地发送前fencing、取消、失联unknown及恢复核对。
- [x] X011 节点程序、启动/停止/配置、公共HTTPS入口配置和门户管理。
- [x] X012 官方协议本地测试服务、真实两节点浏览器业务执行、故障/重复/越权测试、双配置编译。
- [x] X013 更新原计划与操作/环境文档，生成新独立部署包及复制运行验收。

X001–X013为本轮实现范围，已按下述代码、候选ZIP解压/复制源码重建及真实浏览器证据验收。最终独立包为dist/AI-Browser-Workbench-0.1.13-win64-integrations-r1；文档更新后重新冻结，最终ZIP哈希以同级.zip.sha256.json为准。包内文档是冻结时快照，工作区文档记录最终制品复验。真实外部服务验收和原234条完整产品计划继续分别追踪。

## 交付内容及入口

版本0.1.13-poc，97个共享工具；新增独立 `workbench-node.exe`，VS2019解决方案共9个项目。支付、OIDC、调度复用现有C++后台、中文门户、账本和原子权益准入；节点复用真实Chromium、DSL校验与执行器，没有把网页业务改成模拟结果。

| 模块 | 主要源码 | 使用入口 |
| --- | --- | --- |
| 协议/原生密码能力 | src/admin/backend_protocol.cpp | WinHTTP、CNG、Crypt32、证书库；严格默认端口Origin规范化 |
| 官方支付 | src/admin/official_payments.cpp | 门户“订单与账务”的官方渠道创建/查询/退款/退款查询，三条通知地址 |
| 企业身份 | src/admin/oidc_identity.cpp | 企业登录、显式绑定、企业退出、账户安全桌面连接码 |
| 中央节点调度 | src/admin/node_scheduler.cpp | 门户“多机自动化”的项目、节点、环境、版本、队列、取消及核对 |
| 节点执行 | src/node/main.cpp、src/node/node_agent.cpp | --config / --stop --config，便携包Start-Node.ps1和Stop-Node.ps1 |
| 契约 | include/awb/backend_extensions.hpp、docs/contracts、sdk | 97工具、account_connect_code、账户动作与节点消息版本握手 |

实际参数模板由后台首次运行生成，默认关闭外部集成；示例在examples/deployment。详细配置、操作、编译和异常恢复见[操作部署指南](45_官方支付企业登录与多机自动化操作部署指南.md)。

## 测试方法与结果

所有测试在隔离目录创建本次账号、数据库、临时密钥及节点身份。Python夹具独立实现支付/IdP的RSA、AES、HMAC和JWK校验，C++产品使用Windows原生密码API；本地测试例外只允许显式配置的127.0.0.1。测试没有连接真实商户，也没有把本地IdP当作企业生产身份服务。

证据目录：[2026-11-05-integrations-r1](execution/evidence/2026-11-05-integrations-r1/summary.json)。HTTP计数包含等待状态的轮询，不能解释成互不重叠的测试用例数。

| 运行 | 过程及断言 | 实际结果/证据 |
| --- | --- | --- |
| 原生VS2019 Debug/Release | 9项目、v142/C++14、核心/流程、SQLite故障与窗口自检 | 两配置0错误0警告；Debug-build.log、Release-build.log及storage JSON |
| 新功能Debug | 三渠道下单/查询/通知/退款、真实Chromium SSO、两个节点各发布一次 | 通过，132次HTTP；debug-main.json |
| 新功能Release | 同上，并核对重复提交只对应一个命令和一次业务动作 | 通过，133次HTTP；release-main.json |
| 边界Debug | 篡改/serial/AES tag/过期签名、退款冻结、OIDC绑定/CSRF/换钥/停用；独立节点签名协议、unknown、强杀协调器 | 通过，104次HTTP；debug-edges.json |
| 边界Release | 同上，正常stop、Host白名单、节点入口证明、Secure Cookie | 通过，104次HTTP；release-edges.json |
| 旧商业业务回归 | 账户、组织、账本、续费、席位、10免费槽与预付计时，真实门户与设备撤销停止浏览器 | 通过，6组、105次HTTP；commerce-regression.json |
| 旧商业故障/原生账号回归 | 收款冲突、迟到核对、退款/红票、欠额与充值、原生窗口密码掩码/登录/注销 | 通过，6组、131次HTTP及7项原生检查；commerce-edges-regression.json |
| Python/TypeScript SDK | 新契约完整比对、CRUD、3个示例、两独立环境真实登录、拒绝重定向/超体积且不重发 | 通过，7组；sdk-regression.json |
| Local API | 实际登录/注册、令牌轮换、端口归属、审计故障阻断和未知写结果不自动重试 | 通过；api-regression.json |
| CMake Release | 重新生成包含节点与ncrypt的新工程，构建并运行CTest | 通过，3/3；cmake-configure/build/ctest.log |
| 契约/源码审计 | 97个工具无漂移，UTF-8无BOM/LF、C++格式、中文日期注释、固定依赖哈希及未完成标记 | 通过；contract-check.json、source-audit.json |
| 候选ZIP复制部署 | 完整逐文件SHA256、中文空格路径、无关工作目录、真实GUI正常启动/关闭、桌面运行时并行自检不改用户库、CLI/MCP/API/Admin启动/重启/停止、文档篡改/CRT缺失拒绝 | 通过，10组；candidate-deployment.json |
| 包内源码独立重建 | 中文空格路径的source/Build-VS2019.cmd，9项目Release及原生测试，包含新增node工程 | 通过，0错误0警告；copied-source-build.log |
| 复制包业务复验 | 使用解压包内全部EXE/浏览器运行三渠道、SSO和真实两个节点网页发布、取消/崩溃恢复 | 通过，131次HTTP；candidate-deployed-main.json |
| 节点脚本 | 系统PowerShell5.1，无关工作目录，中文空格配置，真实注册/审批/心跳、正常stop及相对root拒绝 | 通过，5项；node-launchers.json |

两个节点都是本机不同私有目录、密钥、进程、Inbox和浏览器，真实向夹具网站提交发布。测试独立核对服务端计数恰为2；重复run.submit不增加次数。测试包括排队取消、运行中取消、节点强杀重启、跨组织拒绝、节点身份撤销及旧用户令牌冒充节点拒绝。

协调器强杀实验中，已运行的浏览器在Debug约3.657秒、Release约3.797秒停止；重启提高epoch，原任务保留unknown，不重放业务。该观测是一次本机故障实验，不作为公网停止延迟SLA。中央租约20秒，节点本地单调保护18秒；每次实际CDP发送都经过远程资格及本地门检查。未收到真实进程及Job停止证明前，运行槽保持占用，核对接口不能跳过此条件。

正式入口测试覆盖正常HTTPS默认443的Origin形式、Host白名单、伪造Forwarded不生效、节点私有入口证明、六个协议入口拒绝及门户管理可用性与Secure/HttpOnly Cookie。测试通过HTTP连接到本机代理后端模拟受信入口，**realTlsHandshakeTested=false**；没有声称真实TLS/mTLS证书握手或网关轮换已验收。

真实浏览器门户截图：[SSO后多机自动化页面](execution/evidence/2026-11-05-integrations-r1/sso-portal.png)。仅包含本次测试项目，不含身份服务令牌或私钥。

## 发现的错误与修复

| 编号 | 发现的问题 | 修复及验证 |
| --- | --- | --- |
| D001 | 节点DPAPI调用参数与原生接口不匹配 | 更正CryptProtectData参数；双配置编译与持久状态/重启通过 |
| D002 | 支付宝退款查询金额字段用错，微信退款响应错误要求支付字段 | 按官方退款结构使用refund_amount，分别校验退款/支付字段；三渠道退款查询通过 |
| D003 | 微信退款查询和通知可能被当成两笔不同经济事实 | 固定订单退款身份，重复事实核验后幂等；账本仅一笔退款 |
| D004 | HTTP解析收到JSON数组时调用object.value导致错误 | 先判断对象类型；异常正文不进入对象字段处理 |
| D005 | OIDC成功跳转/?sso=success被门户路由拒绝 | 支持固定登录回跳查询参数；真实Chromium完成SSO并创建项目 |
| D006 | 节点首次执行未创建assignments父目录 | 创建受控目录后启动环境；失败期间保持未确认槽，恢复不盲重派 |
| D007 | Outbox/签名协议缓存可能明文持久保存任务输入 | 对持久载荷及缓存使用DPAPI；节点Inbox同样保护输入，不复制私有运行数据入包 |
| D008 | 内部准入失败后可能保留部分事务写入 | 内部SAVEPOINT失败回滚；容量/配额负向与重启测试通过 |
| D009 | 官方预付包退款请求期间仍可能消费可退款秒数 | 发出渠道指令前冻结可退秒数；未知结果保留冻结，确认不二次扣减；已消费时发送前拒绝 |
| D010 | 默认HTTPS 443端口与内部Origin表示不一致，正常生产回调地址被拒绝 | 新增ProtocolOrigin规范化默认80/443，保持内部浏览器模型不变；生产形式Host/回调/Cookie检查通过 |
| D011 | 引入浏览器公共头后私有InternetHandle重名 | 后台私有句柄类改为ProtocolInternetHandle；双配置及CMake通过 |
| D012 | SDK回归加载了本机旧版TS编译文件，触发契约漂移拒绝 | 按固定TypeScript5.9.3重编译新契约，两个SDK及独立登录通过；产品拒绝旧契约的行为符合预期 |
| D013 | mTLS示例和后台按node.*前缀限制接口，误拦截门户注册码/审批/维护管理动作 | 代理及后台统一只保护enroll/open/poll/ack/complete/event六个执行协议接口；门户管理继续按账号授权，新增启用mTLS策略时无节点证书的管理正向和六接口拒绝负向回归 |
| D014 | 新节点脚本测试用capture_output等待后台节点继承的管道，启动器返回后测试仍等到超时 | 测试改普通日志文件接收启动器输出并用进程句柄独立跟踪真实节点；重新完成中文空格路径的5项启动/停止验收。此修复不改产品协议或执行行为 |

测试脚本还修正了精确文本等待的期望值/最大10000ms预算，并对导航切换期间的只读元素定位做有界重新获取。点击、发布、支付、退款均不因定位/网络错误而自动重做。

## 参数占位与未验收边界

- 官方商户appId/merchantId、私钥/平台公钥、API V3 key、Stripe API/Webhook key、回调域名待配置。代码已实现三渠道协议，本地签名夹具通过；真实沙箱、商户许可与资金验收尚未运行。
- OIDC issuer/clientId/clientSecret、回调与退出域名待配置；真实企业IdP/MFA/目录及恢复流程待联调。RP退出先吊销本地会话，再生成client_id退出地址，不保存id_token_hint；IdP可能要求确认或额外策略。
- 多机为**单持久协调器+多Windows执行节点**。真实第二台机器、不同Windows用户、NAT、证书链/mTLS握手及轮换、断网长稳尚未验收。本地两进程测试不能替代这些结果。
- 多协调器HA、PostgreSQL、跨地域弹性、Windows服务跨会话桌面代理、浏览器登录态跨节点迁移、复杂公平调度/DST定时，不在已实现范围；原ADM/COM/BR完整父包保留具体剩余项。
- 当前仅CNY、全额退款及用户明确下单续费；部分退款、拒付/争议和自动代扣未实现，不以参数占位冒充已完成。

原234条基线不变，进行中不折算已完成工时。X001–X013为本轮细分验收，不与原父包重复计权。源码没有未完成标记不等同原产品全部完成。实际商业发布、签名安装器、干净机和长稳验收继续开放。

## 最终独立制品复验

最终ZIP和目录为dist/AI-Browser-Workbench-0.1.13-win64-integrations-r1，233849876字节，SHA256 `61ac436995d2f630be5d1fbd44dae616a5d474ad8a2bd91e4e42ee01d667c826`。从最终ZIP再次解压核对2147文件，9组部署检查、131次新增业务HTTP检查和5项节点脚本检查均通过；7个EXE与已独立重建验收候选完全一致。dist原始data保持为空。

冻结后过程与结果见[独立部署包复核](46_集成版独立部署包复核记录.md)及本轮目录final-*.json。原始包文档保持冻结快照，最新工作区文档记录最终制品结果，不回写改变已冻结ZIP。

## 协议资料

本轮直接核对官方文档和官方SDK，使用Windows CNG/Crypt32/WinHTTP实现既有标准协议，不引入自定义签名算法。

- Stripe：[Checkout](https://docs.stripe.com/api/checkout/sessions/create)、[Webhook](https://docs.stripe.com/webhooks/signature)、[退款](https://docs.stripe.com/api/refunds/create)。
- 微信支付：[官方Go SDK Native接口](https://github.com/wechatpay-apiv3/wechatpay-go/blob/main/services/payments/native/api_native.go)、[AES-GCM实现](https://github.com/wechatpay-apiv3/wechatpay-go/blob/main/utils/aes.go)。
- 支付宝：[官方Python SDK客户端](https://github.com/alipay/alipay-sdk-python-all/blob/master/alipay/aop/api/DefaultAlipayClient.py)。
- OIDC：[Core 1.0](https://openid.net/specs/openid-connect-core-1_0.html)、[RFC7636](https://www.rfc-editor.org/rfc/rfc7636)。

配置中的本地测试开关仅允许127.0.0.1明文夹具；正式远程渠道、IdP及节点控制地址必须HTTPS，不禁用证书核验。真实参数未提供不影响协议代码和本机节点验收，不能把本地测试记为真实商户或第二台物理Windows验收。
