# GitHub 发布与下载说明

更新日期：2026-11-25。适用版本：0.1.13-poc，Windows x64 开发预览。

## 1 仓库与下载地址

- 公开源码仓库：[https://github.com/29583855/ai-browser-workbench](https://github.com/29583855/ai-browser-workbench)，默认分支 `main`。
- 本次 Release：[v0.1.13-poc](https://github.com/29583855/ai-browser-workbench/releases/tag/v0.1.13-poc)，标记为开发预览（Pre-release）。
- [Windows x64 完整便携包](https://github.com/29583855/ai-browser-workbench/releases/download/v0.1.13-poc/AI-Browser-Workbench-0.1.13-win64-20261106.zip)。
- [SHA-256 文本校验文件](https://github.com/29583855/ai-browser-workbench/releases/download/v0.1.13-poc/AI-Browser-Workbench-0.1.13-win64-20261106.zip.sha256)。
- [SHA-256 JSON 校验文件](https://github.com/29583855/ai-browser-workbench/releases/download/v0.1.13-poc/AI-Browser-Workbench-0.1.13-win64-20261106.zip.sha256.json)。

GitHub 自动附加的 Source code ZIP/tar.gz 仅用于取源码。需要直接运行程序时，应下载上方完整便携包。

## 2 校验、解压与启动

包文件名：`AI-Browser-Workbench-0.1.13-win64-20261106.zip`；大小：233874807 字节；包构建日期：2026-11-06；GitHub 发布日期：2026-11-25。

```text
424d36c4d5cab0eea2d5f7717e56f6d7b5dd160019b0dad3fae59d03ec91d59b
```

下载后在 PowerShell 执行，路径替换为实际下载文件：

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath "$env:USERPROFILE\Downloads\AI-Browser-Workbench-0.1.13-win64-20261106.zip"
```

输出 Hash 应与上方值一致。完整解压到本地可写目录，按顺序操作：

1. 双击 `Self-Test.cmd`，等待自检完成并查看生成的 `test-results` 报告。
2. 双击 `Start-Workbench.cmd`，创建浏览器环境，填写目标网站地址并启动。
3. 按[使用与运行环境指南](48_软件使用与运行环境配置指南.md)完成登录、网页操作、流程录制与执行。
4. 需要组织后台时运行 `Start-Admin.cmd`，按指南读取本机生成的 `data/admin/bootstrap.key` 进行首次初始化。
5. 将整个解压目录复制到其他 Windows x64 机器后，在目标机器重新执行自检。

普通桌面、CLI、MCP、Local API 和本机后台均不要求安装 VS、Python、Node.js 或外部数据库。附带练习网站使用 Python 3.12。包内包含 Chrome for Testing 和匹配的 VC142 Release DLL，应保持全部文件及相对路径。

功能、免费额度和集成边界见[功能说明](47_软件功能说明.md)。支付/OIDC/跨机 HTTPS 需要有效外部参数；占位配置不能用于真实收款或企业登录。当前 Windows 11 x64 本机已测，Windows 10 与第二台干净物理机仍待验收。

## 3 获取源码与编译

```powershell
git clone https://github.com/29583855/ai-browser-workbench.git
Set-Location -LiteralPath .\ai-browser-workbench
git checkout v0.1.13-poc
.\Build-VS2019.cmd
```

先安装 VS2019 16.11 的 C++ 桌面开发工作负载、v142 14.29.30133 和 Windows SDK 10.0.19041.0。默认构建与参数见[源码编译指南](49_源码编译环境搭建指南.md)，需要明确 Release 时使用：

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File .\tools\build-vs2019.ps1 -Configuration Release
```

源代码及工程放在 Git 仓库中；`build/`、`dist/`、`.run/`、大型浏览器运行时与用户数据不进入 Git 提交。完整运行包通过 Release 附件提供。

## 4 本次发布复核

- [x] 从 `E:\project\dukto` 确认 GitHub 账号与远程仓库配置，使用本机 Git 凭据管理器完成身份验证。
- [x] 新建公开仓库 `29583855/ai-browser-workbench`，保留 C++14 / VC++2019 工程和现有文件编码。
- [x] 发布前扫描源码的敏感模式；命中项均为测试用例、协议格式标记或公开上游示例，没有发现待上传的实际 GitHub 令牌、私钥或客户数据库。
- [x] 核对冻结 ZIP：全部条目 CRC 通过，全部 2157 个清单文件 SHA-256 通过，初始 `data` 为空。
- [x] 发布文档更新前，1813 个源码文件与 ZIP 内 `source` 快照逐字节一致；本次仅更新在线 README、文档入口及发布说明。

- [x] 2026-11-25 将 ZIP 重新解压后执行完整 `Self-Test.ps1`，核心/存储、后台订单与席位、原生画布、真实浏览器填写/点击、MCP、便携路径及用户数据隔离检查全部通过。

发布前复核证据见[JSON 报告](execution/evidence/2026-11-25-github-release/prepublication-report.json)。

发布验证结果和 Release 说明见[当前版本发布记录](releases/v0.1.13-poc.md)。源码编译与包内源码独立重建的历史验收日期是 2026-11-06，本次发布复用该不可变 ZIP。

## 5 后续更新约定

后续版本使用新标签和新 Release，保留现有冻结包的字节与校验值。修改程序后重新构建、执行相关测试，生成全新的便携目录和 ZIP，再发布对应校验文件。Git 提交及公开文档不要包含个人访问令牌、密码、Cookie、商户私钥或用户数据。

当前项目尚未提供覆盖自有代码的 `LICENSE`，本次公开上传没有擅自授予 MIT 或其他开源许可。第三方来源及条款见 [NOTICE.md](../NOTICE.md)。
