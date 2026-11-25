# ADR-001：C++14 技术验证工程与原生 UI

日期：2026-11-03。状态：已采纳，用于开发验证版；不表示 G0 或商业发布通过。

自有目标使用 Visual Studio 2019 16.11 Enterprise、v142 14.29.30133、MSVC 19.29、x64、SDK 10.0.19041.0、C++14。CMake 在配置阶段拒绝其他工具集、架构和 SDK。Debug 使用 MDd，Release 使用 MD；所有自有 C++ 目标开启 W4/WX、permissive-、Zc:__cplusplus、UTF-8、ASLR/DEP。核心测试实际检查编译器与语言宏。干净机重建、签名与安装包尚未验收。

新增自有文本为 UTF-8 无 BOM、LF；本轮已有文档也是此格式，修改保留原字节编码。禁止全项目转码。`.gitattributes` 使用 `-text` 禁止 Git 自动改换行。C++ 函数、构造/析构及有逻辑的回调附中文用途和日期；自有 JS/Python 同样注释。采用固定 clang-format 12、Microsoft 风格、C++14、四空格、Allman 大括号。第三方源码保持上游内容，不为通过自有检查改写版权、编码或格式。

UI 选择 Windows SDK 原生 Win32 控件。Qt 5.15 可作为候选，但公开维护周期、LGPL/GPL/商业模块、插件与分发义务需要逐项锁定；“能用 v142 编译”不等于当前维护合格。Qt 6 不符合本轮自有 C++14 基线。当前 POC 不链接 Qt、QtNodeEditor 或 WebView2，因而无需为建立壳而引入未准入组件。日后画布方案仍须 T077，不能把 Win32 壳验收当作画布验收。

工作台、JSONL CLI、MCP stdio 共用 `awb::Service`。UI 组合定位和动作时仍逐条调用该服务；不直连原生 CDP。窗口线程不执行长时间浏览器读操作；忙碌时允许撤销租约，停止请求在当前操作返回后关闭自有 Job。此行为不是全产品急停状态机的完成证明；尚无 Space、跨客户端排空、持久化命令或中央撤销。

浏览器为独立 Chromium 运行时，使用全新私有 user-data-dir、临时回环 CDP 端口及 kill-on-close Job。运行时通过显式路径选择；不得扫描、接管或结束普通用户浏览器。开发测试使用官方 Chrome for Testing 156.0.8078.80，其来源和归档校验见 `build/runtime/runtime-origin.json`。它不是自有内核，也未进入产品分发包。本机 Edge 启动验证失败的记录保留，不修改用户安装及策略。

初次 POC 实际链接的第三方源码是 MIT 的 nlohmann/json 3.11.3；本轮按 [ADR-002](002_SQLite索引迁移与独立验收.md) 增加官方公共领域 SQLite 3.54.1 的 C 源目标。文件哈希见 [依赖锁](../../third_party/dependencies.lock.json)。SQLite 目标保留上游字节及适用告警配置，与自有 C++ 目标区分；两者仍使用同一 v142 和匹配 CRT。Python MCP SDK 与固定依赖只用于互操作测试。其他候选仍按来源准入门分别处理。

初次 POC 使用原子 JSON 清单；现已由 ADR-002 的 SQLite 索引取代并提供保留源字节的迁移。独占开发验证根目录和非商业模式保持不变。标准化错误、Unicode、命令行引用、租约竞态、协议预算和持久身份由真实单元测试验证；没有为未实现的业务返回假成功。

T004 的接受依据是本 ADR、真实 Win32 窗口/控件创建和忙碌撤销 smoke 测试。完整 UI 交互、DPI/键盘、产品导航、可访问性和用户演练仍归对应工作包。
