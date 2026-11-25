# 本地内容自动化练习站

日期：2026-11-04。适用浏览器工作台：0.1.8-poc。Windows上需要Python 3.10或以上，无第三方Python依赖；浏览器执行使用现有VC2019/C++14程序。

本目录包含真实可运行的网站、SQLite数据库服务、DSL1.0脚本、运行参数示例及浏览器调用脚本。运行后实际在网页中登录、发布准备稿件、搜索并列出文章，再对指定的seed-a文章点赞、评论，并回复comment-a。所有业务写入均由浏览器操作页面完成。

## 1 先启动网站

在本目录打开终端执行，或双击Start-Site.cmd：

```powershell
python -X utf8 server.py --port 3010
```

访问`http://127.0.0.1:3010/`。只接受这个地址，不用localhost代替。端口被占用时先检查是否有自己的旧实例，不结束其他程序。要换端口，需同时修改桌面环境/流程来源，或给runner.py传同一`--origin`。

初次数据库有4篇种子文章和各1条示例评论，无demo-author帐号。脚本先通过网页注册demo-author，再登录。演示密码见inputs.example.json，仅用于本机练习；另有demo-reader帐号（同一演示密码），可验证阅读者不能发布。

数据库在`data/content.sqlite3`；正常关闭网站后数据仍在。Ctrl+C停止服务。如需全新练习，给server.py传一个未用过的`--database`文件路径，不必删除已有数据。

## 2 再运行完整自动化

第二个终端执行，程序目录必须是已完整解压的便携包目录：

```powershell
.\Start-Automation.cmd "D:\AI-Browser-Workbench-0.1.8-win64-ui-r1"
```

此入口显示真实浏览器，可观察自动输入和点击；结束后关闭该脚本创建的浏览器，结果在`runs/run-随机编号/report.json`及browser.png。私有浏览器资料也在该次目录中，只分享报告和确认过内容的截图，不要分发整个runs目录。

也可以在工程根目录执行：

```powershell
python -X utf8 examples/content-lab/runner.py --output build/evidence/my-content-demo --headed
```

源码方式默认使用build/native-vs2019/x64/Release/workbench-cli.exe和build/runtime/chrome-win64/chrome.exe。其他位置用`--executable`和`--browser`传完整路径。每次`--output`必须是新目录，以保留上一次证据。

预期：19步工作流succeeded；两页共4条搜索结果；seed-a为liked，评论submitted，回复parent=comment-a。报告中的结果列表可以直接查看。搜索结果里的临时端口链接只在该网站实例运行期间有效。

## 3 修改稿件与目标

复制inputs.example.json为自己的参数文件，通过`--inputs`传入。user/password是网站帐号；taskKey是本次稿件的稳定标识；title/body/category是实际稿件；keyword是搜索词。评论和回复当前演示文字、seed-a/comment-a目标在runner.py的interact中明确配置，换目标须同步修改适配与核对，不会对所有搜索结果批量互动。

expectedSearchStatus是确定数据集下的精确后置断言。例如初始3篇匹配种子文章加自己的1篇，为`results:4:page:1`；新增其他匹配稿件后应改为真实预期数量。声明错误时流程会报告结果未知，应先查看搜索/文章结果，不能据此假定发布失败。

DSL示例正文上限16384字符，源自当前精确文本断言工具的上限；网站textarea自身上限20000。不同稿件使用新taskKey；同taskKey同内容返回原文章，同标识不同内容拒绝TASK_CONTENT_CONFLICT。评论/回复同样有测试站点自己的任务去重。这是本网站能力，不能推断任意第三方网站也支持相同去重。

## 4 在工作台中导入JSON脚本

1. 启动网站，网页注册准备使用的帐号。
2. 打开workbench.exe，创建来源`http://127.0.0.1:3010`的环境并启动。
3. 切到“流程与数据”，JSON文件填workflow-publish-search.json的完整路径。
4. 点“校验流程”“发布流程”，把inputs.example.json的完整JSON内容粘贴到“流程参数”。
5. 点“运行流程”，查询状态直到succeeded。它执行登录、发布、核对全文和搜索第一页，共19步。
6. 完整的分页采集、已赞跳过及评论/回复由runner.py编排；当前不能把Python文件导入JSON流程，也没有录制画布。

JSON中的“发布流程”保存脚本，网页里的“发布”才提交文章。点击成功、待审核、已公开和结果未知分别核对。

## 5 文件与实现范围

| 文件 | 作用 |
| --- | --- |
| server.py / index.html / app.js / style.css | 本机网站、数据库与页面 |
| workflow-publish-search.json | 已由真实C++执行器验证的DSL1.0流程 |
| inputs.example.json | 专用测试帐号和准备稿件，不是生产凭据 |
| runner.py | JSONL调用现有CLI；采集列表、固定对象互动、报告与截图 |
| Start-Site.cmd / Start-Automation.cmd | Windows启动入口 |

这实现了“脚本驱动浏览器完成业务”的练习链路，思路与RPA产品相同。通用录制器、可视化拖拽画布、内置AI自动识别任意网站、富文本及上传仍按原项目计划开发。网站仅用于本机自动化练习，不作为生产内容平台部署。
