# AI 调用手册 v1

日期：2026-11-05。程序：0.1.11-poc。完整工具名和输入 Schema 见 `python/contract_v1.json`；67 个工具来自 C++ 注册表。契约版本 1 与工作流 DSL 1.0/1.1 分开编号。

1. 以独立 `--root` 启动 `workbench-cli.exe --http 0`，从该根的 `local-api.json` 读取本次短期令牌。桌面、HTTP 服务、MCP 不能同时占用同一个数据根。
2. Python 使用 `Client.from_file(...).connect()`；TS 使用 `new Client(url, token).connect()`。连接核对完整工具集合及参数 Schema。令牌不得放入模型上下文、仓库或测试报告。
3. 查询 `capabilities`、`doctor`、`profile_list`。`doctor.quota.enforced=false` 表示中央权益尚未实现，不能据此出售额度。模型未配置时手工编排和现有工具仍可用。
4. 创建环境时明确 `allowedOrigins`。启动后取 `lease_acquire`，每条页面命令携带相同 `profileId/lease`。受管页面和控件引用仅在当前页面、文档及服务生命周期内有效。
5. `page_discover` 接受功能名称和可选角色，返回可见主文档候选、得分、enabled、唯一 CSS 与截断标志。名称来自受限标签启发式；多个候选必须继续核对对象，不能把得分视作业务授权。
6. `page_locate` → `page_preview` → `page_fill/page_select/page_click` → `page_wait_text`。定位成功和点击已派发均不代表业务完成。SecretRef 使用 `credential_create` 和 `page_fill_secret`，不要把网站密码写进流程常量或模型提示词。
7. 需要复用时 `workflow_validate` → `workflow_publish` → `workflow_run`，保存 `workflowId/runId/sha256`。DSL1.1 增加类型变量、只读输出、列表循环、表达式和精确断点。运行后用 `workflow_status` 查询，不能因连接超时再次运行同一发布动作。
8. `workflow_run.debug.pauseOnStart=true` 进入调试；`workflow_debug` 设置 `{flow:"main",stepId:"..."}` 断点；`workflow_control` 支持 step/stepOver/stepOut/resume/cancel。仅暂停时允许读取脱敏变量；暂停/取消不回滚页面已经执行的操作。
9. 定位失效时保留原失败记录，使用 `workflow_repair_draft` 提供完整原定义、原 `expectedSha256`、指定节点和新唯一 CSS。返回只改一处的草稿与观察信息；审阅后另行发布，原流程和原运行不会被替换。
10. `site_package_preview/import/list/export` 管理不含脚本/附件/本机秘密的数据包。导入使用 `expectedLatestVersion`，发布版本不可变；知识包不会自动运行工作流。
11. `model_configure` 使用 OpenAI 兼容完整 chat/completions 端点和模型用途凭据 ID；`model_generate_workflow` 仅返回通过类型/图/来源检查的草稿。一次调用先预留预算，失败与未知均不自动退回或重试。
12. `outcomeUnknown=true`：立即停止相关后续写入，读取页面/后台回执/步骤状态核对。所有 SDK 都只发送一次请求；客户端超时只结束等待，不能承诺取消服务器任务。

当前边界：主文档普通控件；跨 frame/OOPIF/闭合 Shadow、高级控件、中央身份/计费/席位/远端节点与商业许可仍在开发。SecretRef 使用后，本浏览器实例所有后续截图被阻断；停止并重启实例才解除。DPAPI 库绑定原 Windows 用户，复制包不等于迁移网站凭据。

SDK 仅连接 `http://127.0.0.1:端口`，禁自动重定向和业务重试；响应最多 16MiB。TS 需要 Node 22+（本机实测 23.9），Python 需要 3.12+（本机实测 3.12.3）。产品运行本身不需要 Node、Python 或 VS。

TS 开发校验：`npm.cmd install --prefix build/sdk-tools --ignore-scripts --no-audit --no-fund --save-exact typescript@5.9.3`，然后 `node build/sdk-tools/node_modules/typescript/bin/tsc sdk/typescript/client.ts sdk/typescript/contract_v1.ts --target ES2022 --module commonjs --strict --outDir build/sdk-compiled`。生成契约运行 `python tools/generate_contract.py --executable build/native-vs2019/x64/Release/workbench-cli.exe`；CI 加 `--check` 拒绝漂移。

三个可运行示例位于 `sdk/examples/`，参数均支持 `--help`：01 发现页面功能；02 首节点暂停后单步；03 知识包差异/CAS导入/固定版本导出。示例 02 会执行流程的第一个节点，使用前核对流程文件及站点范围。

登录示例`examples/04_login.py`及`examples/04_login.cjs`使用相同场景JSON：endpointFile、profileId、url、username、credentialId、usernameSelector、passwordSelector、submitSelector、successSelector、successText。先在工作台建立已绑定来源/环境的browser用途凭据并启动浏览器，配置中只填credentialId，不填口令。运行Python：`python sdk/examples/04_login.py --scenario 私有场景.json`；TS：`node sdk/examples/04_login.cjs 编译后client.js的绝对路径 私有场景.json`。示例各提交一次，必须匹配业务成功文本，不支持多因素/跨frame登录；未知结果停止并核对服务器身份。双端已在两个全新Profile实测并核对服务器两次登录/两个身份会话，见integration_sdk报告。
