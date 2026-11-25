# 本地 HTTP API 参考

日期：2026-11-05。适用版本：0.1.13-poc。这里只列可调用接口；01–12中的未落地后台、商业及高级流程接口仍按计划状态执行。API与桌面、CLI、MCP共享C++14业务实现。
本轮0.1.13新增官方支付三渠道、OIDC/PKCE与主体绑定、一次性桌面连接码和独立多机执行节点，97工具；配置/操作详见[新增操作部署指南](45_官方支付企业登录与多机自动化操作部署指南.md)，过程与结果见[实施验收](44_官方支付OIDC与多机调度实施验收.md)。外部参数保持空占位，不能把本地协议测试当真实商户或物理多机验收。
上一轮0.1.10修复退出时恢复保存失败仍关闭、恢复文件删除后不重建的问题；完整复盘、测试步骤和结果见[项目功能复盘与回归测试报告](34_项目功能复盘与回归测试报告.md)。


当前0.1.12新增独立后台、客户/运营门户、支付账务、席位、续费计时与浏览器准入，共96个共享工具。详见[后台实现、测试及操作](42_后台支付席位与计时实现测试及操作指南.md)。新测试包目录为dist/AI-Browser-Workbench-0.1.12-win64-commerce-r1；原0.1.11能力和旧版本报告保留为历史。

## 1 启动和鉴权

便携包双击 `Start-API.cmd`，使用独立 `data/api`。源码构建可执行：

```powershell
New-Item -ItemType Directory -Path '.run' -Force
& .\build\native-vs2019\x64\Release\workbench-cli.exe --root "$PWD\.run\api" --browser "$PWD\build\runtime\chrome-win64\chrome.exe" --http 0
```

`--http 0`分配随机端口，或传1–65535固定端口。仅绑定127.0.0.1，不申请管理员权限/URLACL。端口独占，端口被占用报API_BIND_FAILED。同根不能由另一桌面/CLI/MCP/API服务同时打开。

启动stdout输出 `{ready,url,configFile}`，不含令牌。根目录 `local-api.json`含本次 `url/token/pid`；64位十六进制令牌每次启动更换。调用者以当前用户读取配置，不复制到网页或日志：

```powershell
$awbEndpoint = Get-Content '.\data\api\local-api.json' -Raw | ConvertFrom-Json
$awbHeaders = @{ Authorization = 'Bearer ' + $awbEndpoint.token }
Invoke-RestMethod -Uri ($awbEndpoint.url + '/v1/health') -Headers $awbHeaders
```

所有路由（包括健康检查和工具列表）都要求 `Authorization: Bearer TOKEN`。Host必须精确等于 `127.0.0.1:PORT`；拒绝任何Origin或Sec-Fetch-Site，不开放CORS。使用本地程序客户端调用；网页fetch会被拒绝。同一Windows用户可读自身数据，当前不是隔离同一用户下恶意程序的安全边界。

正常退出会删除端点配置并关闭本服务受管浏览器。异常退出残留配置不能证明服务仍活着，应核对进程和连接，重新启动后重新读令牌。

## 2 实际路由

| 方法和路径 | 请求 | result |
| --- | --- | --- |
| GET /v1/health | 无正文 | 实际capabilities及版本 |
| GET /v1/tools | 无正文 | tools数组，每个含name/description/inputSchema |
| GET /v1/profiles | 无正文 | profiles数组，默认active |
| GET /v1/profiles?state=all | 无正文；亦可active/archived/trashed/purging | 指定状态的profiles |
| POST /v1/profiles | name、allowedOrigins；可选proxyServer/runtimeSettings | 新环境详情，revision=1 |
| PATCH /v1/profiles/{id} | If-Match；name/allowedOrigins/proxyServer/runtimeSettings中至少一项 | 修改后的环境 |
| POST /v1/profiles/{id}/archive | If-Match、正文{} | 归档后的环境 |
| POST /v1/profiles/{id}/trash | If-Match、正文{} | 回收后的环境 |
| POST /v1/profiles/{id}/restore | If-Match、正文{} | 原ID及原数据恢复为active |
| POST /v1/profiles/{id}/purge | If-Match、confirmProfileId完整ID | 永久删除结果 |
| POST /v1/tools/{name} | 对应工具inputSchema的参数对象 | 与CLI/MCP共享结果 |
| POST /v1/shutdown | Content-Type必须application/json，正文必须精确{} | stopping=true，随后退出 |

其余写路由Content-Type接受 `application/json` 或 `application/json; charset=utf-8`。正文UTF-8 JSON对象，最大1MiB；头部最大16KiB。拒绝重复头、分块传输、压缩正文、Expect、额外数据/管线和不明确长度。每次响应关闭连接。当前单线程串行处理；长任务、并发调度、事件订阅/重放尚未实现，调用方应设置有界超时。断开连接不撤回已经提交的网站动作。

资源路由修改要求 `If-Match: "REVISION"`，只接受一个带双引号的十进制版本。不接受星号、弱ETag、列表；版本应来自最近列表/修改结果。资源路径/If-Match决定ID和版本，正文不能重复profileId/expectedRevision。工具路由使用工具参数中的expectedRevision，亦执行CAS；没有绕过版本验证的入口。

## 3 创建、修改与回收

```powershell
$awbBody = @{
    name = '本地Web测试'
    allowedOrigins = @('http://127.0.0.1:3000')
    runtimeSettings = @{ languages = @('zh-CN', 'zh'); timezone = 'Asia/Shanghai' }
} | ConvertTo-Json -Depth 8
$awbNew = Invoke-RestMethod -Method Post -Uri ($awbEndpoint.url + '/v1/profiles') `
    -Headers $awbHeaders -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($awbBody))
$awbProfile = $awbNew.result
$awbWriteHeaders = @{ Authorization = $awbHeaders.Authorization; 'If-Match' = '"' + $awbProfile.revision + '"' }
$awbChanged = Invoke-RestMethod -Method Patch -Uri ($awbEndpoint.url + '/v1/profiles/' + $awbProfile.id) `
    -Headers $awbWriteHeaders -ContentType 'application/json; charset=utf-8' `
    -Body ([Text.Encoding]::UTF8.GetBytes('{"name":"测试环境A"}'))
```

修改成功使用返回的新revision继续下一步。冲突时重新读取并核对意图，不盲目把旧变更套到新版本。归档/回收/修改/删除都要求环境停止；引用中的环境不能归档/回收/删除。状态含purging表示清理未完成，关闭占用/消除重解析或多硬链接等问题后，按当前revision重试purge。不会删除数据库索引掩盖残留。

代理支持无认证 `http://host:port` 与 `socks5://host:port`，端口显式1–65535，不接受凭据、路径。更新proxyServer为空字符串清除；runtimeSettings为空对象清除区域配置。UA、languages、timezone支持独立配置；启动后的runtime报告区分requested/effective，scope为primary-target。没有修改Chromium指纹内核，也没有弹窗/Worker全局一致性承诺。

## 4 浏览器调用

50个工具的实际参数见[操作手册工具表](16_功能操作说明.md#4-外部程序调用cli)和运行时GET /v1/tools。调用例子：

```powershell
$awbStartBody = @{ profileId = $awbProfile.id; headless = $true } | ConvertTo-Json
$awbStart = Invoke-RestMethod -Method Post -Uri ($awbEndpoint.url + '/v1/tools/browser_start') `
    -Headers $awbHeaders -ContentType 'application/json' -Body $awbStartBody
$awbLeaseBody = @{ profileId = $awbProfile.id; owner = 'my-web-test'; ttlMs = 300000 } | ConvertTo-Json
$awbLease = (Invoke-RestMethod -Method Post -Uri ($awbEndpoint.url + '/v1/tools/lease_acquire') `
    -Headers $awbHeaders -ContentType 'application/json' -Body $awbLeaseBody).result
```

页面动作正文带 `profileId` 与完整 `lease:{owner,token,generation}`。导航后 `page_locate`取得临时ref，交给fill/click/text；网页业务成功用page_wait_text或服务器状态独立验证。租约/元素ref只属于当前实例/文档，不跨环境复用。停止无需租约或购买。当前API不接收任意原生CDP命令，也不把原生调试端点交给调用方。

## 5 响应与错误

```json
{"ok":true,"requestId":"32位请求ID","result":{}}
```

失败为 `ok:false,requestId,error:{code,message,outcomeUnknown}`；响应头X-Request-Id与正文一致。不是JSON-RPC。不得只看HTTP连接成功判断动作成功。

| 状态 | 主要含义 |
| --- | --- |
| 200 | 已执行并返回result |
| 400 | INVALID_REQUEST/INVALID_ARGUMENTS/非法JSON |
| 401 | 缺少或失效令牌 |
| 403 | Host或浏览器来源拒绝 |
| 404 | 未注册路由/环境不存在 |
| 405 / 415 | 方法/正文类型不支持 |
| 409 | 版本冲突、运行中、状态不符、租约忙或引用保护 |
| 413 / 428 | 请求超预算/缺少If-Match |
| 422 | 其他结构化业务错误，包括审计写入失败 |
| 500 | 非预期内部错误 |

若outcomeUnknown=true、连接断开或超时，动作可能已生效。先只读核对网页/服务器，不自动重复点击、注册、付款等操作。审计接受记录写失败会阻断业务；完成记录写失败保守标结果未知。失败日志也无法写时响应额外含auditRecorded=false。

审计文件 `local-api-audit.jsonl`按requestId写accepted/completed/failed、code和Windows FILETIME，不写请求正文、网页URL、令牌或填写值。单文件约8MiB后轮换，保留3份旧日志，总量约32MiB；这只是本地API审计，不代替未来步骤证据/全入口审计系统。

## 6 验证与边界

`tests/integration_api.py`在真实回环HTTP和Chromium中验证注册1次/登录1次、令牌轮换、Host/Origin、If-Match、端口占用、异常报文、审计占用及退出Job清理。准确结果和二进制哈希见[本轮报告](19_功能增量与测试报告.md)。

本API不包含组织、席位、订单、支付或中央10免费额度；它是本机自动化接口。API工作包独立验收不解除T020全入口日志、G0或发布门。

## 7 0.1.3页面、事件和重连

新增page_list/page_open/page_switch/page_close/browser_events/browser_reconnect，经POST /v1/tools/{name}调用；全部带profileId/lease。参数及游标语义见[文档21](21_多页面事件与显式重连测试报告.md)。pageId只属于当前受管实例，切换/重连失效旧ref。browser_events是轮询，不是SSE/WebSocket订阅；显式重连不重放业务。单个页面主帧可操作，iframe/OOPIF/Shadow深层动作仍未实现。

## 8 0.1.4元素探查和表单选择

新增page_inspect(selector,highlight?)、page_preview(ref,action)、page_clear_highlight和page_select(ref,values)。经POST /v1/tools/{name}调用，正文仍带profileId/lease；action限定click/fill/select，values最多32个不同字符串。探查/预览不回显字段/选项值，正式选择派发事件并验证结果。ACTION_POSTCONDITION_FAILED可能已经运行网页事件，outcomeUnknown=true，不自动重试。最新54次HTTP请求及官方MCP31工具结果见[文档22](22_元素探查与表单操作测试报告.md)。


## 0.1.5 流程工具

POST /v1/tools/workflow_validate接收definition及可选input；workflow_publish接收definition；workflow_list/workflow_runs接收空对象；workflow_get接收workflowId；workflow_run接收workflowId/profileId/lease/input；workflow_status接收runId；workflow_control接收runId/operation，操作枚举pause/resume/cancel。认证、来源检查、消息预算和HTTP审计仍适用。

workflow_run是异步启动，HTTP客户端断开不取消执行。若启动回复丢失先workflow_runs核对，不直接重发workflow_run。流程正在执行时，同Profile交互页面工具拒绝WORKFLOW_PROFILE_BUSY；browser_status/lease_revoke/browser_stop仍可用。终态未知、已取消、服务重启中断均不得resume。详细输入类型和状态契约见[流程报告](23_流程执行器与检查点测试报告.md)。


## 0.1.6 诊断工具

POST /v1/tools/browser_diagnostics_start接收profileId/lease和可选routePaths数组；browser_diagnostics_read接收profileId/lease及可选epoch/after/limit，按返回cursor分页；browser_diagnostics_stop接收profileId/lease。活动流程期间允许只读诊断查询；开始/停止不能改变活动流程的观察设置。所有入口仍校验租约，旧epoch拒绝。共享工具总数42，数据范围及丢失语义见[诊断报告](24_诊断收集与步骤关联测试报告.md)。


## 0.1.7 数据提取与动作批处理

POST /v1/tools/page_extract接收profileId/lease/schema/bindings；POST /v1/tools/page_batch接收profileId/lease/actions及可选onError。当前0.1.11共67工具。提取的valid/partial及批次的逐项state必须由调用方检查，HTTP200和ok=true只说明取得结构化结果，不能判作所有业务成功。未知结果强制停批且禁止自动重发；没有回滚。API审计使用固定validation-failed/partial/outcome-unknown摘要码，不记录字段值或动作输入。

批次同步执行，单Profile锁覆盖整批，每次派发仍校验租约。HTTP监听当前串行处理；批次执行时后到请求排队，不能通过同一HTTP通道立即撤销正在执行的批次。异步任务使用workflow_run/status/control，桌面接管走直接租约门。完整定义、预算和结果见[文档25](25_结构化提取与动作批处理测试报告.md)。

0.1.7-r2中browser_start/browser_status增加storagePathMode=canonical或private-junction-alias；capabilities.privateBrowserStorageAlias=true。该字段说明存储路径兼容方式，不改变ProfileID或额度。参数及当前工具数仍为44，不新增任意文件系统/目录连接管理API。Web项目设计bundle尚无导入/执行接口。

## 9 录制与元素拾取预览

通过POST /v1/tools/{name}调用，共用profileId和租约（page_pick_stop例外）。

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| recorder_start | profileId、lease | 主文档隔离监听，不采输入值 |
| recorder_read | profileId、lease | 返回有界事件、complete/captureIssues、待配置draft |
| recorder_control | profileId、lease、operation=pause/resume/stop | stop解绑，停止后仍可读取当前内存记录 |
| page_pick_start | profileId、lease | 原生Overlay拦截选取点击 |
| page_pick_read | profileId、lease | 唯一CSS候选及探查结果，文档变化拒绝 |
| page_pick_stop | profileId | 无须租约，退出不补发点击 |

录制与流程执行互斥；录制期间切换/打开/关闭/重连页面返回RECORDING_PAGE_LOCKED。草稿须补URL、参数和业务后置条件后workflow_validate/publish/run，不能把complete:true理解成草稿已合法或任意站点适配已完成。能力与限制见运行时capabilities。

## 0.1.11 增量工具及单一契约

新增17个工具：credential_create/list/rotate/delete/audit、page_fill_secret、workflow_debug、model_configure/status/generate_workflow、site_package_preview/import/list/export、page_discover、workflow_repair_draft、doctor。完整参数和工具POST路由见[自动生成手册](contracts/TOOLS_V1.md)、[OpenAPI](contracts/openapi-v1.json)和[SDK手册](../sdk/AGENT_API_V1.md)。

workflow_control增加step/stepOver/stepOut；workflow_run支持debug。模型/修复只返回草稿，知识包导入不执行；doctor明确中央配额未实施。SecretRef使用后实例级截图阻断。预算/超时/未知结果/敏感输出等约束见文档35。

## 11 文件动作、环境库与批次回执（automation-r3）

96个完整参数Schema以`GET /v1/tools`和[生成工具手册](contracts/TOOLS_V1.md)为准。新增浏览器工具仍要求profileId和有效lease：page_upload、page_download_config、page_download_wait、page_assert。上传的path为本机普通文件；download_wait使用fileName和1–10000ms的timeoutMs，等待当前配置之后的唯一建议文件名，完成事件与实际文件核对通过才返回成功。超时不取消、不重试。

环境库工具不需要浏览器租约：profile_group_list/create/update/delete、profile_template_list/create/update/delete/instantiate/history/rollback、profile_export/import、profile_batch/list/get。模板更新和回滚需要expectedRevision；回滚额外传sourceRevision，生成新revision，历史不被覆盖。

`profile_batch`可传可保存的32位小写十六进制requestId。该ID和规范请求摘要在执行前落盘，每项执行前写dispatching、执行后写已确认状态。重复ID/参数仅返回回执，receiptOnly=true、replayed=false；相同ID不同请求返回BATCH_ID_CONFLICT。首次省略ID由服务生成并返回batchId；为处理响应丢失建议由客户端预先持有ID。

```powershell
$awbBatchId = [guid]::NewGuid().ToString('N')
$awbRequest = @{ requestId=$awbBatchId; onError='stop'; operations=@(@{ command='browser_stop'; arguments=@{ profileId=$awbProfileId } }) }
$awbBody = $awbRequest | ConvertTo-Json -Depth 12 -Compress
Invoke-RestMethod -Method Post -Uri ($awbEndpoint.url + '/v1/tools/profile_batch') -Headers $awbHeaders -ContentType 'application/json' -Body $awbBody
$awbGet = @{ batchId=$awbBatchId } | ConvertTo-Json -Compress
Invoke-RestMethod -Method Post -Uri ($awbEndpoint.url + '/v1/tools/profile_batch_get') -Headers $awbHeaders -ContentType 'application/json' -Body $awbGet
```

持久回执只含命令、环境ID、项状态及错误码，不保存生命周期调用的完整结果、网页正文或秘密参数。重启后未派发项变not-run，派发无回执项变outcome-unknown，不自动继续。BATCH_JOURNAL_UNCONFIRMED表示动作已派发但回执无法确认，应重启并按已持有ID核对。批次不是跨操作事务；已有成功动作不会撤销。当前最多1000未归档批次、100000终身身份，list默认active范围/每页100条；使用before与scope分页，归档保留摘要去重。

admin_policy/admin_summary/commerce_entitlements/commerce_quote只提供当前根的本地开发预览。10环境、默认10并发、RPA/Local API及代理不单独收费的政策已统一；中央容量grant、真实商品/订单/时长/续费/支付尚未实现，生产标志保持false。

## 12 批次分页与详情归档（automation-r4）

profile_batch_list接受scope=active/archived/all、limit=1–100和可选before（上一页nextCursor）。返回items、hasMore、nextCursor（末页null）、activeCount、archivedCount。游标记录归档后仍有效；不存在返回BATCH_CURSOR_INVALID。分页不是快照，新增批次需刷新第一页查看。

profile_batch_archive参数为`{"batchId":"32位ID","confirmBatchId":"相同32位ID"}`，仅允许已确认终态；确认不符BATCH_CONFIRMATION_REQUIRED、未结束BATCH_STATE_INVALID、未知项BATCH_RECONCILIATION_REQUIRED。压缩后items为空、itemCount及计数保留，archived/detailsCompacted为true；同ID/相同请求仍仅回执、不同请求BATCH_ID_CONFLICT。确认后详情不可恢复，ID/摘要不物理删除；到终身预算仍可查询旧回执。归档提交结果不明时要求重开核对，不自动重试。参数Schema以[自动生成契约](contracts/TOOLS_V1.md)为准。


### 2026-11-05 后台与商业业务链更新

0.1.12已交付可运行后台及中文门户，账户/组织/角色、线下核实支付与通用签名回调、订阅续费、席位、预付秒数、用量、退款/发票以及真实浏览器准入已落地。操作、参数、测试、修复和仍未交付的原计划范围统一见[文档42](42_后台支付席位与计时实现测试及操作指南.md)。新增启动入口为 `Start-Admin.cmd`，原生入口为“环境库与本地后台 → 后台账号与授权（表单）”。
