# AI Browser Workbench · AI 浏览器自动化工作台

让人工、脚本和 AI 在独立浏览器环境中完成 Web 测试与业务自动化。

支持从“打开网站、输入帐号密码、点击登录”开始，把文章发布、搜索提取和指定内容互动编排为可复用流程；同时提供组织后台、商业权益和 Windows 多节点执行能力。

**当前版本：0.1.13-poc · Windows x64 · VC++2019 / C++14 · 开发测试阶段**

[下载 Windows x64 完整便携包](https://github.com/29583855/ai-browser-workbench/releases/download/v0.1.13-poc/AI-Browser-Workbench-0.1.13-win64-20261106.zip) · [Release 与 SHA-256](https://github.com/29583855/ai-browser-workbench/releases/tag/v0.1.13-poc) · [发布与下载说明](docs/52_GitHub发布与下载说明.md)

GitHub 首次发布：2026-11-25。本次发布沿用 2026-11-06 已构建的 Release 便携包；产品源码与包内快照一致，仓库补充了发布入口和说明。

[功能说明](docs/47_软件功能说明.md) · [使用与部署](docs/48_软件使用与运行环境配置指南.md) · [源码编译](docs/49_源码编译环境搭建指南.md) · [测试证据](docs/44_官方支付OIDC与多机调度实施验收.md)

## 可以用它做什么

- **测试自己的 Web 系统**：注册、登录、填写表单、点击业务按钮，读取结果并执行断言。
- **重复执行网页业务**：登录后发布准备好的文章，搜索并提取结果，对明确选定的文章执行点赞、评论和回复。
- **搭建可视化流程**：录制主文档操作、拖拽编排、配置变量和分支、调试并发布固定版本。
- **让 AI 操作浏览器**：通过 MCP、Local HTTP API、JSONL CLI 或 Python/TypeScript SDK 接入。
- **管理多个浏览器环境**：独立用户目录与登录态，分组、模板、配置导入导出和生命周期管理。
- **管理组织与执行资源**：成员、席位、设备、套餐、订单、计时用量，以及指定 Windows 节点上的任务执行。

典型流程：

```text
打开网站 → 填写帐号密码 → 登录 → 核对身份
→ 填写文章 → 发布一次 → 核对结果
→ 搜索关键词 → 提取列表 → 操作指定目标 → 核对业务状态
```

当前包含功能候选发现和定位辅助；实际网站仍需配置来源、参数、唯一定位与成功判断。点击返回成功表示动作已派发，业务成功需通过页面断言或服务器记录确认。

## 功能概览

| 模块 | 当前能力 |
| --- | --- |
| 环境管理 | 独立 Profile、分组、模板及历史回滚、配置导入导出、持久批次、归档/回收/恢复 |
| 网页操作 | 主文档语义快照、唯一 CSS 定位、填写/点击/原生选择框、文本读取、断言、截图 |
| 操作辅助 | 探查高亮、只读动作预览、鼠标拾取、功能候选发现、多页面与诊断 |
| 文件与数据 | 单文件上传、下载完成核对、Schema 提取、字符串数组、有界动作批次 |
| RPA | 参数化录制、原生拖拽画布、DSL 1.0/1.1、分支/循环/子流程、不可变发布、暂停/恢复/取消 |
| 调试与凭据 | 断点、单步/stepOver/stepOut、脱敏变量、SecretRef、Windows 当前用户 DPAPI |
| AI 与知识包 | 兼容 chat/completions 的模型配置及预算、受校验流程草稿、定位修复草稿、数据型站点知识包 |
| 程序接口 | 97 个共享工具；MCP stdio、JSONL CLI、鉴权 Local HTTP API、双语言薄 SDK |
| 组织后台 | 本地账号、组织角色、席位邀请、设备、全局运行槽、用量、审计 |
| 商业能力 | 套餐价格快照、订单、线下收款核实、续费、预付秒数、全额退款、发票/红票登记 |
| 外部集成 | 支付宝 RSA2、微信支付 V3、Stripe Checkout；OIDC/PKCE、主体绑定、桌面连接码 |
| 多节点 | Windows 出站节点、注册审批、环境固定归属、持久队列、回执、取消与未知结果核对 |

**浏览器运行时采用独立的 Chrome for Testing 156.0.8078.80。** 原生工作台通过 CDP 管理浏览器进程和用户目录。当前没有自研指纹内核；代理仅支持实验性的无认证 HTTP/SOCKS5，其他代理协议及完整网络泄漏矩阵尚未交付。

## 快速开始

### 方式一：完整便携测试包

取得完整便携包后：

1. 解压到本地可写目录，保留 EXE、DLL、`browser`、`portable.mode` 和脚本的相对位置。
2. 运行 `Self-Test.cmd`，确认报告通过。
3. 运行 `Start-Workbench.cmd`，输入站点地址和环境名称，新建并启动环境。
4. 定位帐号框和密码框，填写后点击登录，再读取身份或结果元素。
5. 需要组织后台时运行 `Start-Admin.cmd`。

普通运行无需安装 VS、Python、Node.js 或外部数据库。Python 只在运行附带练习站和开发脚本时需要。当前已有 Windows 11 x64 本机与复制部署证据；Windows 10 和第二台干净物理机仍待验收。

`dist/` 和大型浏览器运行时不随 Git 源码提交。完整运行包及校验文件通过 [GitHub Releases](https://github.com/29583855/ai-browser-workbench/releases/tag/v0.1.13-poc) 下载；便携包目录、参数和校验方式见[运行配置指南](docs/48_软件使用与运行环境配置指南.md)。

### 方式二：从源码构建

安装 VS2019 16.11 的“使用 C++ 的桌面开发”、v142 `14.29.30133` 和 Windows SDK `10.0.19041.0`。在仓库根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build-vs2019.ps1 -Configuration Release
```

脚本自动查找 VS2019；非默认安装可加 `-VsRoot '你的VS2019安装目录'`。输出位于 `build/native-vs2019/x64/Release`，默认同时运行核心、存储和窗口 smoke 测试。

准备完整 Chrome for Testing 运行时，修改下面的浏览器路径后启动：

```powershell
$awbBrowser = 'D:\Runtimes\chrome-win64\chrome.exe'
New-Item -ItemType Directory -Path .run -Force | Out-Null
& .\build\native-vs2019\x64\Release\workbench.exe `
    --root "$PWD\.run\desktop" --browser $awbBrowser
```

也可直接打开 [AIBrowserWorkbench.sln](AIBrowserWorkbench.sln)，选择 `Debug/Release | x64` 构建。解决方案包含 9 个项目。浏览器归档来源、SHA-256、CMake、集成测试和打包步骤见[编译指南](docs/49_源码编译环境搭建指南.md)。

## 跑一个完整示例

### 注册、登录和退出

额外安装 Python 3.12，在源码根的另一个终端启动练习站：

```powershell
python -X utf8 tests/fixtures/web_app.py --port 3000
```

在工作台中创建允许 `http://127.0.0.1:3000` 的环境并打开 `/login`。依次操作：

| 动作 | 选择器 | 核对结果 |
| --- | --- | --- |
| 填写新测试帐号/密码 | `#username`、`#password` | 对应输入框内容 |
| 注册 | `#register` | `#result` 为 `registered` |
| 登录 | `#login` | `#result` 为 `logged-in`，`#identity` 为测试帐号 |
| 退出 | `#logout` | `#result` 为 `logged-out` |

可直接校验、发布并运行 [注册登录流程](tests/fixtures/workflow-register-login.json)，输入对象包含 `user` 和 `password`。该示例最后退出登录，最终匿名状态是预期行为。完整逐步操作见[使用指南](docs/48_软件使用与运行环境配置指南.md)。

### 文章发布、搜索和互动

[Content Lab](examples/content-lab/README.md) 提供本地内容网站和通过产品浏览器工具执行的脚本：

```powershell
python -X utf8 examples/content-lab/server.py --port 3010
```

保持服务运行，在另一个已设置 `$awbBrowser` 的源码根 PowerShell 中执行：

```powershell
$awbDemoOutput = 'build/content-demo-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
python -X utf8 examples/content-lab/runner.py `
    --executable build/native-vs2019/x64/Release/workbench-cli.exe `
    --browser $awbBrowser --output $awbDemoOutput --headed
```

脚本完成登录、发布、搜索分页及对指定文章的互动，输出报告和截图。19 步 JSON 负责登录、发布和第一页搜索，Python runner 继续执行分页和互动。输出目录须为新目录，示例不是对任意搜索结果批量互动的通用脚本。

## 接入 AI 和外部程序

### MCP

以下示例用于已完整解压的便携包，目录按实际位置替换；从源码运行时还需显式传入 `--browser`。

```json
{
  "mcpServers": {
    "ai-browser-workbench": {
      "command": "D:/Apps/AI-Browser-Workbench-0.1.13-win64-integrations-r1/workbench-mcp.exe",
      "args": [
        "--root",
        "D:/Apps/AI-Browser-Workbench-0.1.13-win64-integrations-r1/data/mcp"
      ]
    }
  }
}
```

由兼容 MCP stdio 的客户端启动并初始化服务。当前工具注册表共 97 项。连接 MCP 服务本身无需模型密钥；AI 客户端使用的模型由客户端配置。

### JSONL、Local API 和 SDK

启动便携包 `Start-CLI.cmd` 后，一行发送一个 UTF-8 JSON 请求：

```json
{"id":1,"command":"capabilities","arguments":{}}
```

`Start-API.cmd` 启动本机 HTTP API。读取 `data/api/local-api.json` 中本次生成的地址和 Bearer 令牌，通过 `POST /v1/tools/{name}` 调用；健康检查也需要鉴权。Local API 仅绑定回环，不开放浏览器跨域调用。

调用顺序通常为：创建环境、启动浏览器、取得租约、导航、定位、填写或点击、断言、停止。环境 ID、租约和元素引用必须来自实际结果，导航/切页后重新定位。

- [工具参数契约](docs/contracts/tools-v1.json)
- [Local HTTP API 参考](docs/20_本地HTTP_API参考.md)
- [Python SDK](sdk/python/awb_client.py)
- [TypeScript SDK](sdk/typescript/client.ts)

桌面、CLI、MCP 和 API 启动脚本默认使用独立数据根；同根不能由多个进程并发打开。需要共享环境时复用单一服务进程。

## 后台与多节点

后台由 `workbench-admin.exe` 提供中文 Web 门户。便携包运行 `Start-Admin.cmd`；源码构建后可执行：

```powershell
New-Item -ItemType Directory -Path .run -Force | Out-Null
& .\build\native-vs2019\x64\Release\workbench-admin.exe `
    --root "$PWD\.run\admin" --port 17861
```

首次从对应数据根的 `bootstrap.key` 初始化平台管理员，没有默认密码。普通客户通过“注册客户组织”进入免费档。

多机部署在门户创建项目、登记来源、生成注册码；节点填写私有配置，运行 `workbench-node.exe --config ...`，经审批后分配环境和固定流程版本。每台节点主动出站连接中央后台。浏览器用户目录保留在所属节点，跨机不自动搬迁。

官方支付与 OIDC 参数默认为空且关闭。正式部署需要实际商户、IdP 和 HTTPS 入口；参数模板、回调、桌面 SSO 与可选 mTLS 见[操作部署指南](docs/45_官方支付企业登录与多机自动化操作部署指南.md)。

## 免费使用规则

| 项目 | 当前中央后台默认规则 |
| --- | --- |
| 独立环境容量 | 每组织 10 个，跨项目、设备和节点合计 |
| 同时运行槽 | 默认 10 个，与环境容量分别计算 |
| 命名席位 | 默认 1 个 |
| 核心 RPA、Local API、AI 调用、Web 测试 | 工作台核心功能免费 |
| 用户自有节点 | 不因使用节点本身强制购买 |
| 扩展收费 | 超出环境/并发容量，或明确购买额外资源与服务 |

第三方模型、代理及托管等资源费用由相应服务另行确定。免费政策允许配置代理，但不代表所有代理协议都已经支持。离线引擎试算不等于中央配额强制执行或防篡改许可证。

## 当前状态与已知限制

已有本机 Debug/Release、真实 Chrome 业务、CLI/MCP/API、支付协议、OIDC 浏览器链路、同机两个真实节点及复制部署/包内源码重建证据，详见[集成测试记录](docs/44_官方支付OIDC与多机调度实施验收.md)和[部署包复核](docs/46_集成版独立部署包复核记录.md)。

- 真实商户、企业 IdP、实际 TLS/mTLS 握手和第二台物理 Windows 尚待外部环境验收。
- 任意网站完全自动适配、完整跨 iframe/OOPIF/封闭 Shadow 操作与录制尚未交付。
- 当前为单持久协调器，多协调器 HA、跨节点浏览器数据迁移和完整生产发行体系尚未交付。
- 支付当前限 CNY、全额退款和明确下单续费；不含部分退款、自动代扣和拒付争议处理。
- 网络超时或 `outcomeUnknown:true` 时，先核对目标网站/官方交易；不自动重放已经可能提交的业务。

完整计划仍在推进，当前版本不是完成全部规划的商业发行版。

## 文档

| 文档 | 内容 |
| --- | --- |
| [软件功能说明](docs/47_软件功能说明.md) | 当前能力、工具索引与边界 |
| [使用与运行环境](docs/48_软件使用与运行环境配置指南.md) | 从解压到网页操作、后台和节点配置 |
| [源码编译环境](docs/49_源码编译环境搭建指南.md) | 固定工具链、双配置、回归与新包生成 |
| [开发与验收台账](docs/13_执行记录与验收台账.md) | 任务状态及证据 |
| [微信公众号宣传稿](docs/51_微信公众号宣传文案.md) | 场景介绍与发布文案 |
| [原首页历史归档](docs/50_项目README历史记录.md) | 历次计划、版本增量和历史文档索引 |

## 开发约定与问题反馈

自有 C++ 保持 C++14，使用 VS2019/v142；新增或修改函数添加中文用途和日期，按 `.clang-format` 格式化，并保留原文件编码、BOM 与换行。修改接口后同步生成契约及 SDK，提交前运行相关检查。第三方源码保留上游字节。

反馈问题请提供版本、入口、复现步骤、预期/实际结果和脱敏错误码；不要包含密码、令牌、Cookie、支付密钥或私有数据库。

## 许可与第三方声明

当前仓库尚未提供覆盖项目自身代码的 `LICENSE`，不能据第三方库的 MIT 许可推断整个项目采用 MIT。项目自身的使用、修改和分发授权需由维护者明确。

第三方依赖、浏览器与 VC 运行库来源及条款见 [NOTICE.md](NOTICE.md) 和[依赖锁](third_party/dependencies.lock.json)。便携包当前用于开发测试，正式商业分发许可和发行审查尚未完成。
