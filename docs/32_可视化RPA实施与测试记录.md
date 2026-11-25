# 可视化RPA实施与测试记录

日期：2026-11-04。版本：0.1.9-poc。对应[专项计划31](31_可视化录制拖拽编排与网站自动适配开发计划.md)。本报告记录真实实现、失败、修复和复验；预览版尚不具备完整跨站点RPA能力，不代表28项全部完成。

## 1 实施范围与逐项状态

本轮新增原生画布、主文档录制、原生元素拾取、草稿持久化/恢复，以及桌面录制到发布回放闭环。自有代码保持C++14、VC++2019/v142；中文用途/日期注释、UTF-8无BOM/LF及clang-format沿用工程规范。没有改动固定第三方依赖字节。

| 项目 | 结果 | 证据与剩余范围 |
| --- | --- | --- |
| VE001 范围与契约 | 完成 | [冻结契约与工时归集](acceptance/VE001_范围契约与工时归集.md) |
| VE002 画布技术验证 | 完成 | [Direct2D/DirectWrite选型与实测](adr/003_原生流程画布选型与技术验证.md) |
| VE009 基础画布交互 | 完成 | [拖拽、框选、平移、缩放和键盘](acceptance/VE009_节点画布基础交互.md) |
| VE011 编辑历史与恢复 | 完成 | [历史、剪贴板、恢复和并发revision](acceptance/VE011_编辑历史剪贴板与恢复验收.md) |
| VE003–008 | 进行中 | 主文档/会话、录制、输入合并与拾取预览；跨frame、复杂控件、实时工具条和凭据库未完成 |
| VE010、012 | 进行中 | 顺序/分支连线、计数loop/call、校验及v1无损模型已实现；图错误导航、属性向导和新版本迁移未完成 |
| VE025、027、028 | 进行中 | 本机集成、性能、部署和文档推进；完整VR矩阵、实体多屏/可访问性及独立新用户/第二台机器未完成 |
| VE013–024、026 | 未开始 | 类型化变量、断言扩展、断点/单步、派发阶段、SecretRef、模型、知识包、自动适配和跨站点基准等待实施 |

当前专项4项完成、11项进行中、13项未开始。完成标记与[专项机器台账](execution/visual-rpa-tasks.json)同步。146人日归集到已有父包的剩余范围，没有直接加到317人日；原父包不能因一个专项子项通过就全部关闭。

## 2 实际可操作能力

画布支持9类DSL1.0节点：navigate、fill、click、select、assertText、condition、loop、call、end。支持主/子流程选择、next/then/else连线、设置入口、原始JSON属性编辑、统一校验、不可变发布和导出。非法环、递归、缺失后置条件和类型/预算错误由现有校验器拒绝。200节点用于整体草稿/多子流程性能测试，不改变单flow128步等执行限制。

录制器在新文档预注册隔离世界监听，只观察动作元数据，所有输入值均不采集；普通输入、密码和选择值生成显式必填参数槽。输入按区间合并，处理Chromium IME和Enter/click/submit重叠。点击仍需用户补齐业务后置条件，URL需明确配置；录制草稿不是可直接运行的完整流程。跨文档需审阅，frame等不支持范围明确返回不完整。队列1000事件/8MiB；溢出或草稿超出DSL预算时不截断伪造成功。

元素拾取使用Chromium原生Overlay选取，拦截拾取点击并返回唯一ID/testId/结构CSS候选。退出不补发点击；撤销租约后也可退出。只验收当前主文档，候选不能被解释为任意网站自动适配。

新增6个工具，与原工具共50个，共用Service及租约校验：recorder_start/read/control、page_pick_start/read/stop。录制与workflow_run互斥；录制时切换/打开/关闭/重连页面被拒绝。CLI、MCP、HTTP注册表共享，但新增工具完整三协议业务往返矩阵尚未验收。

画布发布后回填打开画布时选中环境的workflowId；之后即使切换环境，发布仍绑定原环境。主窗口“运行流程”可直接运行发布版本。重启后的自动重新绑定尚未提供，需重新导入/发布。

自动保存每5秒保存已应用模型、布局及当前子流程到数据根flow-drafts，每窗口独立文件，最多保留50份、单份1MiB。未应用的JSON文字不保存；异常退出最多损失最近5秒的已应用修改。恢复不会执行节点，必须另存；显式确认关闭编辑器删除本窗口恢复文件。正常退出主工作台时则尝试保留打开编辑器的最后快照。系统剪贴板使用专用节点格式，复制不包含运行参数和整个项目；节点中用户手填的静态属性仍会复制。

## 3 测试环境与过程

环境为本机Windows11 x64（10.0.26200）、VS2019 16.11.53、v142 14.29.30133、SDK10.0.19041.0，固定Chrome for Testing156.0.8078.80。Python3.12用于测试，Node仅用于测试专用Chromium输入/拾取手势；产品运行不依赖这两个运行时。

测试以随机回环端口站点为对象，独立DOM计数核对真实提交/点击；使用临时Profile和数据根，不访问用户生产账号。流程为：先故障复现→保留失败报告→修复→双配置构建→正/反功能复验→打包和复制部署。测试报告中的passed仅表示声明的范围，清理失败也计为整轮失败。

| 测试层 | 测试方法 | 结果与证据 |
| --- | --- | --- |
| 初轮三轮重复 | Debug/Release各3轮，画布19项、录制11组 | 114项画布、66组录制检查通过；[初轮汇总](execution/evidence/2026-11-04-visual-rpa/three-rounds.json)；此时尚未包含后补的发布回填和恢复功能 |
| 最新三轮回归 | 双配置各3轮，画布、真实恢复、桌面闭环、浏览器录制/拾取顺序运行 | [最终逐轮报告](execution/evidence/2026-11-04-visual-rpa/final-regression-r2/report.json)，逐项包含二进制SHA256；24轮共240项通过（画布114、恢复36、桌面24、录制66）；以报告实际result为准 |
| 草稿/历史核心 | 模型与布局隔离、15种DPI/缩放、环拒绝、复制ID、100项/8MiB共享历史、只读兼容、坏文件回滚、过期revision | 纳入双配置awb-core-tests；原生与CMake检查均运行 |
| 画布UI/性能 | 真实Win32窗口消息、Direct2D渲染、200节点、命中、拖拽/框选/平移/缩放、属性中文 | 每轮19项；最新六轮p95=17.1312–17.4361ms，目标50ms；完整数值见各canvas JSON |
| 录制/拾取 | 普通输入/密码/select、IME/Enter、暂停/恢复、停止解绑、导航、撤销、队列溢出及frame能力拒绝 | 每轮11组；真实DOM计数确认没有重复提交，输入canary不进入事件/草稿 |
| 桌面闭环 | 菜单录制→停止→打开草稿→配置URL/后置条件→校验→切环境→发布→原环境运行 | 每轮4组；真实页面最终提交数=1；输入控件运行后清空 |
| 崩溃恢复 | 原生复制/粘贴/剪切/撤销→等待真实计时器→强制结束→重启→系统文件对话框恢复→确认关闭 | 每轮6组；节点/布局一致、必须另存、本窗口恢复文件单独清理、对话框期间关闭保护 |
| 旧桌面回归 | 既有环境/登录/注册/页面/提取/流程操作 | [Release桌面报告](execution/evidence/2026-11-04-visual-rpa/prior-Release-desktop.json)：109步、11组 |
| 旧流程回归 | 顺序/条件/循环/子流程、发布、检查点及失败 | [Release流程报告](execution/evidence/2026-11-04-visual-rpa/prior-Release-workflow.json)：20正向流程、34定义反例、13检查 |
| 旧HTTP/协议回归 | API鉴权/资源/工具及CLI/MCP信封 | [API](execution/evidence/2026-11-04-visual-rpa/prior-Release-api.json)88请求、[协议](execution/evidence/2026-11-04-visual-rpa/prior-Release-protocol.json)24检查；该轮在恢复补丁前，服务调用契约未变 |

物理窗口实际DPI为96；144DPI渲染目标及缩放测试不能替代真实多显示器迁移。Chromium真实Input.imeSetComposition验证不能替代系统输入法、候选窗和多屏的完整人工矩阵。上述未测范围仍归VE027。

## 4 已发现问题、解决办法及复验

失败记录留在[专项证据目录](execution/evidence/2026-11-04-visual-rpa)，不覆盖成成功。中间报告代表对应旧源码/二进制，不与最终证据混用。

| 编号 | 发现与影响 | 修复或处理 | 复验 |
| --- | --- | --- | --- |
| V01 | 控件ID直接转HMENU触发x64 C4312 | 经INT_PTR转换 | 双配置编译无该警告 |
| V02 | 录制分支初次放进WorkflowStep，接口不在正确入口 | 移到Service::Execute并统一租约 | 工具调用和录制互斥通过 |
| V03 | ValidateWorkflow返回valid:false而非抛异常，导出可能误判 | 显式检查valid后导出 | 核心/画布负向检查 |
| V04 | 草稿使用assert/predicate、navigate输入引用、select输入上限等与DSL1.0不符 | 改postcondition/when、URL留待配置、选择输入上限4096 | Debug-recording-r1保留；补齐草稿后校验和回放通过 |
| V05 | 修改属性/入口时Import清空历史和布局 | 增加Metadata/Entry独立命令 | 核心撤销和布局隔离通过 |
| V06 | 撤销子流程创建后当前视图仍指向已删子流程 | 历史保存当前flow并恢复 | 子流程撤销/重做通过 |
| V07 | ListBox内部拖选吞掉节点库拖入 | 显式命中、捕获和坐标转换 | Debug-canvas-r2失败保留，后续真实拖放通过 |
| V08 | Overlay退出缺少highlightConfig导致picker-stop失败 | none模式显式传空对象 | Debug-recording-r4失败保留；退出和租约撤销后退出通过 |
| V09 | Enter测试只发键码，未提供回车文本 | 测试补text/unmodifiedText回车 | 真实提交一次；属于测试夹具修正 |
| V10 | 浏览器未完全退出就清理临时目录，WinError32 | finally显式停止，检查Job为空，再释放CLI和目录；cleanup错误将result置failed | final-1改为failed保留；最终各轮清理通过 |
| V11 | 画布发布workflowId未回填主窗口，无法直接运行 | 发布回调绑定打开时Profile | 桌面闭环测试切环境后发布、原环境回放通过 |
| V12 | 历史两条链缺少统一字节裁剪 | undo/redo共享100项/8MiB预算 | 大模型历史及反向恢复测试通过 |
| V13 | 文件对话框刚出现时测试过早找输入框；失败清理在模态框期间关主窗口触发异常退出 | 测试等待控件；主工作台检测模态编辑器并阻止关闭；清理先取消对话框 | Debug-recovery-r1保留，r3及最新三轮复验 |
| V14 | 大历史测试恰好把name填满128KiB，连同JSON字段超出属性预算 | 缩小测试元数据且保留超过8MiB历史压力 | 核心测试通过；未放宽产品预算 |
| V15 | 构建数次LNK1104，目标EXE短暂无法打开 | 检查无残留工作台进程后重试，保留失败构建日志；未确证占用来源 | 后续完整双配置结果单独记录 |
| V17 | 桌面重复测试假定环境列表按创建顺序，同毫秒创建会按其他稳定键排序，Profile断言偶发失败 | 测试按真实ID匹配列表，避免依赖索引；生产发布回调正确 | final-regression失败保留；r2六轮桌面均通过 |
| V16 | 新桌面测试首次路径指向不存在的build/native | 改原生实际输出native-vs2019/x64 | 后续桌面集成通过；属于测试命令修正 |

## 5 复现命令

在工程根执行；VS路径按本机调整，输出目录必须尚不存在，避免覆盖既有失败证据。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-vs2019.ps1 -Configuration Both -VsRoot 'D:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise'
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build.ps1 -Configuration Both -VsRoot 'D:/Program Files (x86)/Microsoft Visual Studio/2019/Enterprise'
python -X utf8 tools/run_visual_rpa_tests.py --binaries build/native-vs2019/x64 --browser build/runtime/chrome-win64/chrome.exe --output build/evidence/visual-rpa-retest --rounds 3
python -X utf8 tests/integration_visual_protocols.py --package dist/AI-Browser-Workbench-0.1.9-win64-visual-rpa-r2 --output build/evidence/visual-protocol-retest
python -X utf8 tools/validate_visual_rpa_plan.py --output build/evidence/visual-plan-retest.json
python -X utf8 tools/verify_progress.py --output build/evidence/progress-retest.json
```

专项自动化脚本顺序操作系统剪贴板和桌面窗口；运行期间保持工作台可用。原生方案与CMake分别验证，不能用其中之一替代另一个。源审计扫描未完成标记、格式、编码、注释和依赖hash；扫描无标记不等于所有功能已实现，真实剩余范围以台账为准。

## 6 部署与未完成边界

新便携测试包目标为dist/AI-Browser-Workbench-0.1.9-win64-visual-rpa-r2。包含当前程序、匹配Release运行库、固定浏览器、完整源码/示例和操作文档；原0.1.8冻结包保持原字节。包内录制与画布不需要Python/Node；源码集成测试及启动Python练习站才需要。

不把本机复制/解压测试称作第二台干净机验收。支付、OIDC、签名证书、模型服务及第二台Windows目前均没有实接资源；相应门保留未验收。跨frame/Shadow录制、变量与调试、凭据库、知识包、站点发现/修复和全量内容可视化流程仍需开发；不承诺“任意网站自动适配”。

下一阶段按依赖推进VE003–008和VE010/012，再进入变量/断言/调试、秘密与站点适配；每次增量均保留失败及修复证据，再更新逐项状态。使用步骤见[录制与画布预览操作指南](33_可视化录制与画布预览操作指南.md)。

## 7 便携候选后验收与最终快照

0.1.9-r1候选ZIP通过[9项部署检查](execution/evidence/2026-11-04-visual-rpa/package-deployment-r1.json)，包含中文/空格路径、桌面打开时自检、哈希损坏和缺CRT拒绝、复制源码Release重建及测试。解压副本另通过[录制11组、恢复6组、桌面闭环4组、协议24项及官方MCP50工具](execution/evidence/2026-11-04-visual-rpa/package-behavior-r1.json)。新工具在实际CLI/MCP/鉴权HTTP上完成[12项同一业务检查](execution/evidence/2026-11-04-visual-rpa/package-protocols-r1.json)，包含拾取不提交、录制输入不回显、pause/resume/stop、未知参数及撤销租约拒绝；这仍不代表新DSL1.1等VE024完整交付。

原生双配置日志、CMake双配置/CTest及源审计均通过；最新自有源码70文件、未完成标记0。扫描无标记不能替代计划验收。修正文档当前状态后生成r2最终快照，程序二进制与通过240项回归的版本保持相同，r1候选及其失败记录保留。最终ZIP与后验收结果见[最终制品清单](execution/evidence/2026-11-04-visual-rpa/final-delivery.json)。包内source是打包时快照，最终ZIP哈希/后验收仅追加到主工程，不反写冻结ZIP。


最终r2已通过9项部署检查、12项CLI/MCP/HTTP一致性检查，解压副本录制11组/恢复6组/桌面4组/协议24项与官方MCP50工具互操作均通过。原始分发目录1627个清单文件重新核对一致，无额外文件，data为空、无test-results。[下载最终便携ZIP](../dist/AI-Browser-Workbench-0.1.9-win64-visual-rpa-r2.zip)，大小228022976字节，SHA-256：`63d5a449a6d1e4cab4d0d6f858dd410103f6428692ebf13a224f7acd647598bd`。完整后验收范围与真实未完成项见[最终制品清单](execution/evidence/2026-11-04-visual-rpa/final-delivery.json)。原生/CMake双配置及三轮回归的程序与r2中二进制哈希相同；第二台Windows和全产品仍未验收。

2026-11-04复盘增量：0.1.10-poc修复恢复保存失败仍退出及已删除恢复文件不重建，两配置重新编译并复测。完整范围、有效失败、测试脚本修正和复验见[项目复盘报告](34_项目功能复盘与回归测试报告.md)。本地21完成/30进行中/59未开始、41/317及专项4完成/11进行中/13未开始保持不变；后台/商业尚未实现。
