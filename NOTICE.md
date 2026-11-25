# 第三方来源与当前复用范围

日期：2026-11-04。

当前 C++ 目标包含 nlohmann/json 3.11.3 官方单头文件，许可 MIT。完整版权与许可保留在 [LICENSE.MIT](third_party/nlohmann/LICENSE.MIT)，版本、URL 和 SHA-256 见 [dependencies.lock.json](third_party/dependencies.lock.json)。

SQLite 3.54.1 官方 amalgamation 的 sqlite3.c/sqlite3.h 也已进入目标，按上游公共领域声明复用，保持原始字节；声明网页快照见 [copyright.html](third_party/sqlite/copyright.html)。下载包已核对官网 SHA3-256，源文件 SHA-256 和 URL 同样进入依赖锁；C 源由 v142 使用同配置 CRT 单独编译。参见 [存储 ADR](docs/adr/002_SQLite索引迁移与独立验收.md)。

Windows SDK / MSVC 为构建工具；Chromium/Chrome/Edge 通过独立进程通信。2026-11-04便携开发测试包包含匹配VS2019安装目录的官方VC142 x64 Release运行库，附带安装目录中的Redist说明；适用条款由该Visual Studio许可及[官方可再分发清单](https://aka.ms/vs/16/redist.txt)约束，不包含Debug CRT、编译器或Windows SDK。

为用户要求的复制开发测试，便携包包含未经修改的Chrome for Testing 156.0.8078.80官方win64归档内容；保留ABOUT及浏览器的chrome://terms、chrome://credits。原始下载URL、归档大小及SHA-256在包内browser/runtime-origin.json；源码仓库的build/runtime仍为本机验证输入，不提交大运行时。便携包是本地开发测试制品，未完成商业分发许可/SBOM/内核维护准入，也不声称提供该上游浏览器的完整对应源码或自有内核。

`build/research-sources` 中 ego-lite、OpenBrowser、fingerprint-chromium 的固定源码归档仅供来源检查，未进入 CMake 目标。OpenBrowser 的根 MIT 不覆盖其另有 AGPL 声明的输入模块；fingerprint-chromium 固定提交没有对应完整内核源码；ego-lite 不包含独立原生浏览器运行时。详见 [来源检查](docs/research/source_gate_execution_2026-11-03.json)。

Python MCP SDK 与其依赖仅在独立测试虚拟环境使用，不打包进 C++ 程序；测试环境固定版本见 [测试锁文件](tests/mcp-sdk-requirements.lock.txt)。最终发行仍需正式 SBOM、传递依赖、制品许可、签名及更新维护验收；本文件不代表 T065 发布审查完成。
