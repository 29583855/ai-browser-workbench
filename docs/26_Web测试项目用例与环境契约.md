# Web测试项目、用例与环境数据契约

日期：2026-11-04。契约版本：1.0。对应T067“项目/用例/环境Schema、原24与新增24用例目录、增量选型记录”。本项完成的是可验证的设计契约，测试项目管理、执行包装、SecretRef保管和报告界面仍由T049/T072–T075实现。桌面程序目前不导入此bundle，也不会执行其中的工作流引用。

## 1 文件与校验入口

全部文件在[web-testing-v1](contracts/web-testing-v1/README.md)。5份独立draft-07 Schema分别定义项目、环境、用例、数据集及完整bundle，不解析网络或外部引用；它们与当前运行时的有界workflow输入方言及page_extract方言分开。

| 文件 | 约束对象 |
| --- | --- |
| [project.schema.json](contracts/web-testing-v1/project.schema.json) | 项目身份、revision、环境/用例/数据集/角色与场景引用 |
| [environment.schema.json](contracts/web-testing-v1/environment.schema.json) | 开发/测试环境、规范来源、相对页面、变量与资源要求 |
| [case.schema.json](contracts/web-testing-v1/case.schema.json) | 用例版本、工作流、准备/清理、身份隔离、执行与断言要求 |
| [dataset.schema.json](contracts/web-testing-v1/dataset.schema.json) | 参数类型、敏感标记、数据行与值来源 |
| [bundle.schema.json](contracts/web-testing-v1/bundle.schema.json) | 上述资源的独立可交换快照，结构完整后另作语义校验 |
| [example-bundle.json](contracts/web-testing-v1/example-bundle.json) | 无明文密码的登录测试项目设计示例 |
| [case-catalog.json](contracts/web-testing-v1/case-catalog.json) | WT01–WT48的准备、输入、预期、具体反例、清理及实现任务 |
| [action-contract.json](contracts/web-testing-v1/action-contract.json) | 4场景/12动作的实际工具、未实现边界和未知结果约定 |

在已安装锁定测试依赖的Python环境执行：

```powershell
build/test-venv/Scripts/python.exe tools/validate_web_test_contracts.py docs/contracts/web-testing-v1/example-bundle.json
build/test-venv/Scripts/python.exe tests/test_web_test_contracts.py --output build/evidence/web-contracts-new-run
```

校验器只读取设计文件，不连接目标网站、读取凭据或执行工作流。退出0表示设计预检通过，退出1表示拒绝。返回固定错误码和字段路径，不回显输入值。jsonschema 4.26.0等依赖已在[测试依赖锁](../tests/mcp-sdk-requirements.lock.txt)，不增加桌面运行时依赖；离线复现需提前准备对应wheel或已安装的测试环境。

## 2 对象关系与版本

项目、环境、用例和数据集的ID为1–64位ASCII小写字母开头的字母/数字/下划线/短横线；revision为1–1000000整数，布尔值不能冒充整数。名称可中文，最多128码点。所有对象拒绝未知字段，未来字段/版本不能静默忽略。

项目列出的环境、用例、数据集ID必须与bundle中实际资源集合完全一致，不能遗漏或夹带资源；每类ID唯一。子对象的projectId必须等于当前项目。默认环境、用例环境、角色、数据集与场景引用均须存在。当前bundle上限为每类128资源；数据集最多256行、每行64个参数，设计文件最多1MiB、深度16。

用例通过workflow的id/version引用已发布的不可变流程，setupWorkflow与cleanup.workflow同样固定版本。本设计预检不证明流程已存在、输入类型匹配或具备执行权限；运行实现须在T072/T073检查这些事实，并调用现有workflow引擎。不能新增另一套绕过租约的点击执行器。发布采用revision比较、运行绑定快照、历史attempt追加等运行机制仍需实现，契约不代表其已落地。

## 3 环境与来源

1. 当前契约明确用于development/test，不隐式运行用户项目启动命令。生产业务自动化使用既有流程及单独批准的环境策略；未来新增环境种类须显式升版。
2. baseUrl为HTTP(S)规范origin，支持localhost、自定义端口、测试域名和IPv6；拒绝用户名/口令、路径、查询、fragment、控制符及反斜线。主机大小写、默认端口和IDN使用规范形式，避免同一来源多种拼写。
3. baseUrl必须在allowedOrigins内。defaultPage为同源绝对路径，可包含查询与SPA hash；拒绝网络路径、跨源URL、编码反斜线和控制符。示例保留`/login?view=test#form`。
4. 所有后续页面/重定向/弹窗仍由实际Profile的允许来源、页面归属及租约检查。设计文件预检不能替代运行时门禁，也不自动放行OIDC、邮件链接或清理API。
5. independentProfiles与parallelRuns独立声明，真正并行不得超过独立身份数；声明可以超过免费10槽以表达需求，预检不会扣费或自动购买。实际容量/运行槽授权、排队与回落仍由后台及调度实现。HTTP/HTTPS/SOCKS5/SSH的枚举表示需求，必须与当前实际能力协商，不能据此声称四协议已验收。

## 4 数据与秘密

每个数据参数都有type和sensitive。每行bindings必须恰好覆盖参数集合，数据行ID唯一，不允许缺字段或加入其他参数。

| 来源 | 用法 | 边界 |
| --- | --- | --- |
| value | 明确公开的字符串/数值/布尔/null | 必须匹配参数类型；字符串最多4096码点，禁止非有限JSON数字 |
| secretRef | `secret://项目ID/凭据名` | 必须属于当前项目；敏感参数只允许此来源且类型为string，不读取实际秘密 |
| generator | case-attempt-unique和显式prefix | 仅非敏感string；由未来执行层结合run/case/attempt生成，当前校验器不生成资源 |

一个绑定只能使用一种来源。将敏感参数改为常量或生成器会被拒绝；不能在错误、快照、录制或报告中保存密码。身份状态引用也绑定当前项目。运行时还必须校验账号/租户/环境/存储版本与权限，T049/T072尚未完成，示例SecretRef不表示已创建可用凭据。

new-identity模式不允许复用身份状态；login-each-case通过准备流程登录，不允许偷偷携带复用状态；reuse-verified必须同时声明状态secretRef与验证工作流。认证验证失败必须按用例定义失败/受阻，不能静默换账号。

## 5 用例执行契约与固定目录

沿用[首发四场景](acceptance/首发四场景与POC验收.md)：SC01登录注册、SC02登录后报表/文件、SC03多环境/代理、SC04 AI/外部软件调用。WT01–WT24为原基线，WT25–WT48为追加；目录保留原编号、每项具体反例、准备和仅回收本attempt资源的清理要求。WT18独立建立前置账号，不依赖WT11恰好先运行；WT24的500、业务失败、脚本异常须分参数结果。

所有目录项executionStatus为not-run、evidence为空。已有POC功能测试不自动换算成48用例×10轮验收。T068承担首批24夹具，T099承担补充夹具；T076最终汇总480次执行，并保留参数变体、blocked与真实失败分母。

每个用例固定以下策略：独立Profile；后端case/attempt命名空间；显式timeout；写入never retry；未知结果require-reconciliation；清理必需且只处理本次创建资源，清理失败block-dependent-cases；历史append-only。这些是将来执行层必须落实的行为，当前设计校验不产生运行结果。

断言字符串在契约中记录业务标准，不接受脚本代码，也不由校验器解释。现有文本/标量Schema可实际调用；URL/数量/响应组合、可视化断言编辑及动作前观察由T071实现。12动作清单明确将双击、右键、悬停、按键、勾选和独立滚动列为planned，不能用现有click自动假装实现。

## 6 选型和验收

采用独立draft-07设计文件和Python jsonschema作为开发期离线契约检查，复用现有工作流/租约/来源/C++14服务；不引入第二套浏览器驱动。C++运行时继续按已验证的有界方言执行，通用draft-07与AST/双向画布由T089承担。此项没有新引入运行时库、浏览器内核或需另行核查的分发许可；测试库沿用现有锁定环境。

[测试报告](execution/evidence/2026-11-04-web-contracts/report.json)记录61项设计检查：5份独立Schema、合法示例/来源、跨项目和缺引用、重复资源、参数类型/敏感标记、身份复用、未知结果与清理规则、来源绕过、重复JSON键/非有限数/深度/大小、48目录及12动作。全部通过仅关闭T067设计契约，不关闭T068/T070–T076或完整产品验收。
