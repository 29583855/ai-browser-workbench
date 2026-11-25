#pragma once
namespace awb
{
// 门户资源由tools/embed_admin_portal.py生成；日期：2026-11-05。
const char PortalHtml[] =
    R"AWB0(<!doctype html>
<html lang="zh-CN">
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width,initial-scale=1" /><title>
    Workbench · 管理中心
  </title>
  <style>
    :root {
      font-family:
        Segoe UI,
        Microsoft YaHei,
        sans-serif;
      color: #183149;
      background: #f1f5fa;
    }
    * {
      box-sizing: border-box;
    }
    body {
      margin: 0;
    }
    header {
      background: #142c45;
      color: white;
      padding: 20px 32px;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    header strong {
      font-size: 22px;
    }
    main {
      max-width: 1320px;
      margin: 24px auto;
      padding: 0 24px;
    }
    h2 {
      font-size: 22px;
    }
    h3 {
      font-size: 17px;
    }
    p {
      line-height: 1.7;
    }
    small {
      color: #536b80;
    }
    .card {
      background: white;
      border: 1px solid #dbe4ed;
      border-radius: 14px;
      padding: 22px;
      margin: 18px 0;
      box-shadow: 0 3px 14px #142c4508;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(260px, 1fr));
      gap: 16px;
    }
    .stat b {
      display: block;
      font-size: 30px;
      color: #0b7285;
    }
    nav {
      display: flex;
      gap: 8px;
      flex-wrap: wrap;
    }
    button {
      border: 0;
      border-radius: 7px;
      padding: 10px 16px;
      background: #087f8c;
      color: white;
      cursor: pointer;
      font: inherit;
    }
    button:hover {
      background: #076572;
    }
    button:disabled {
      opacity: 0.5;
      cursor: wait;
    }
    nav button {
      background: #dce7f3;
      color: #183149;
    }
    nav button.active {
      background: #183149;
      color: white;
    }
    label {
      display: block;
      font-size: 13px;
      color: #435e75;
      margin: 10px 0;
    }
    input,
    select {
      display: block;
      margin-top: 5px;
      padding: 10px;
      border: 1px solid #bccbd9;
      border-radius: 7px;
      width: 100%;
      font: inherit;
    }
    form {
      margin: 12px 0;
    }
    form .grid {
      gap: 0 20px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }
    th,
    td {
      padding: 12px 8px;
      border-bottom: 1px solid #e5edf4;
      text-align: left;
      word-break: break-all;
    }
    th {
      color: #536b80;
    }
    .scroll {
      overflow: auto;
      max-height: 470px;
    }
    .id {
      font-family: Consolas, monospace;
      font-size: 12px;
    }
    .actions button {
      padding: 6px 10px;
      margin: 3px;
      font-size: 12px;
    }
    .muted {
      background: #e5edf4;
      color: #183149;
    }
    #notice {
      position: sticky;
      top: 8px;
      padding: 12px 18px;
      background: #e4f4f2;
      border: 1px solid #9ccbc6;
      border-radius: 8px;
      white-space: pre-wrap;
      z-index: 2;
    }
    #notice.error {
      background: #fff0ee;
      border-color: #e6aca3;
      color: #972d22;
    }
    [hidden] {
      display: none !important;
    }
    pre {
      white-space: pre-wrap;
      overflow-wrap: anywhere;
      font-size: 12px;
    }
    footer {
      padding: 30px;
      color: #536b80;
      font-size: 12px;
    }
  </style>
  <header>
    <strong>Workbench <span style="font-weight: 400">/ 管理中心</span></strong
    ><span id="account">账户 · 商业 · 运行管理</span>
  </header>
  <main>
    <div id="notice" role="status" hidden></div>
    <section id="auth">
      <div class="card">
        <h2>登录管理中心</h2>
        <button id="sso-login" type="button" hidden>
          通过企业身份服务登录
        </button>
        <p>
          首次运行请从后台数据目录读取
          bootstrap.key，初始化平台管理员。无默认密码。
        </p>
        <form id="auth-form">
          <div class="grid">
            <label
              >操作<select name="mode">
                <option value="login">登录</option>
                <option value="bootstrap">初始化平台管理员</option>
                <option value="register">注册客户组织</option>
              </select></label
            ><label
              >邮箱<input
                name="email"
                type="email"
                required
                autocomplete="username" /></label
            ><label
              >密码（注册至少12字符）<input
                name="password"
                type="password"
                required
                autocomplete="current-password" /></label
            ><label
              >组织 / 昵称（注册时）<input
                name="name"
                autocomplete="organization" /></label
            ><label
              >初始化密钥（仅首次）<input
                name="key"
                type="password"
                autocomplete="off"
            /></label>
          </div>
          <button>进入管理中心</button>
        </form>
      </div>
    </section>
    <section id="workspace" hidden>
      <div class="card">
        <div class="grid">
          <label>当前组织<select id="tenant"></select></label>
          <div>
            <p id="who"></p>
            <button id="refresh">刷新数据</button>
            <button id="logout" class="muted">退出登录</button>
          </div>
        </div>
      </div>
      <nav id="navigation">
        <button data-page="overview" class="active">概览与套餐</button
        ><button data-page="team">成员与席位</button
        ><button data-page="billing">订单与账务</button
        ><button data-page="runtime">设备与运行</button
        ><button data-page="operator" id="operator-tab" hidden>平台运营</button
        ><button data-page="scheduler">多机自动化</button
        ><button data-page="security">账户安全</button>
      </nav>
      <section data-view="overview">
        <div id="stats" class="grid"></div>
        <div class="card">
          <h2>可购买套餐</h2>
          <p>
            免费档：10个环境、10路并发、1个席位，支持代理协议、RPA、Local
            API和AI测试。付费套餐扩展额度；续费从原到期日接续。
          </p>
          <div id="plans" class="grid"></div>
        </div>
        <div class="card">
          <h3>订阅与续费</h3>
          <div id="grants"></div>
        </div>
      </section>
      <section data-view="team" hidden>
        <div class="card">
          <h2>席位与权限</h2>
          <p id="seat-info"></p>
          <form data-action="member.invite">
            <div class="grid">
              <label>邀请邮箱<input name="email" type="email" required /></label
              ><label
                >角色<select name="role">
                  <option value="member">成员</option>
                  <option value="admin">组织管理员</option>
                  <option value="finance">财务</option>
                  <option value="owner">所有者</option>
                </select></label
              >
            </div>
            <button>邀请并预占席位</button>
          </form>
          <div id="members"></div>
          <h3>待处理邀请</h3>
          <div id="invites"></div>
        </div>
        <div class="card">
          <h3>接受邀请</h3>
          <form data-action="member.accept">
            <label
              >邀请令牌<input
                name="invitationToken"
                required
                autocomplete="off" /></label
            ><button>接受邀请</button>
          </form>
          <pre id="invitation-result"></pre>
        </div>
      </section>
      <section data-view="billing" hidden>
        <div class="card">
          <h2>订单与支付</h2>
          <form id="channel-form">
            <div class="grid">
)AWB0"
    R"AWB1(              <label>订单号<input name="orderId" required /></label>
              <label
                >官方渠道<select name="channel">
                  <option value="alipay">支付宝</option>
                  <option value="wechat">微信支付</option>
                  <option value="stripe">Stripe</option>
                </select></label
              >
              <label
                >操作<select name="verb">
                  <option value="create">创建付款</option>
                  <option value="query">查询付款</option>
                  <option value="refund">执行已申请退款（平台）</option>
                  <option value="refund-query">查询退款</option>
                </select></label
              >
            </div>
            <button>执行官方渠道操作</button>
          </form>
          <pre id="channel-result"></pre>
          <p>
            渠道参数由服务器配置。支付返回结果未确认时，请先查询付款或退款。
          </p>
          <p>
            线下支付由平台核实银行流水后确认。网关未配置时不能发起网关支付。订单支付、退款、回调均核验成交金额。
          </p>
          <div id="orders"></div>
        </div>
        <div class="card">
          <h3>收款与退款分录（人民币分）</h3>
          <div id="ledger"></div>
        </div>
        <div class="card">
          <h3>发票申请</h3>
          <form data-action="invoice.request">
            <div class="grid">
              <label>订单号<input name="orderId" required /></label
              ><label>发票抬头<input name="title" required /></label
              ><label>税号<input name="taxId" required /></label>
            </div>
            <button>提交申请</button>
          </form>
          <div id="invoices"></div>
        </div>
      </section>
      <section data-view="runtime" hidden>
        <div class="card">
          <h2>设备与全局并发</h2>
          <p>
            离线或心跳中断的运行槽继续占用配额。核实浏览器停止后才能人工释放，防止重复运行。
          </p>
          <div id="devices"></div>
          <h3>运行槽</h3>
          <div id="slots"></div>
          <h3>已结束用量（秒）</h3>
          <div id="usage"></div>
        </div>
      </section>
      <section data-view="operator" hidden>
        <div class="card">
          <h2>平台运营</h2>
          <p id="reconciliation"></p>
          <h3>发布套餐版本</h3>
          <form data-action="plan.save">
            <div class="grid">
              <label
                >计费类型<select name="billingMode">
                  <option value="subscription">包时订阅</option>
                  <option value="prepaid">预付秒数</option>
                </select></label
              ><label
                >时长包秒数<input
                  type="number"
                  name="timeSeconds"
                  min="300"
                  value="36000" /></label
              ><label>产品标识<input name="productId" required /></label
              ><label>套餐名称<input name="name" required /></label
              ><label
                >价格（人民币分）<input
                  type="number"
                  name="amountMinor"
                  min="1"
                  max="100000000"
                  required /></label
              ><label
                >有效天数<input
                  type="number"
                  name="days"
                  min="1"
                  max="3650"
                  value="30"
                  required /></label
              ><label
                >环境数<input
                  type="number"
                  name="environmentLimit"
                  min="10"
                  value="50"
                  required /></label
              ><label
                >并发数<input
                  type="number"
                  name="concurrentLimit"
                  min="1"
                  value="20"
                  required /></label
              ><label
                >席位数<input
                  type="number"
                  name="seatLimit"
                  min="1"
                  value="5"
                  required
              /></label>
            </div>
            <button>发布新价格版本</button>
          </form>
          <div id="operator-plans"></div>
        </div>
        <div class="card">
          <h3>线下收款 / 退款确认</h3>
          <form id="settle-form">
            <div class="grid">
              <label
                >操作<select name="mode">
                  <option value="payment.confirm">确认收款</option>
                  <option value="refund.confirm">确认已退款</option>
                </select></label
              ><label>订单号<input name="orderId" required /></label
              ><label
                >核实金额（分）<input
                  type="number"
                  name="amountMinor"
                  min="1"
                  required /></label
              ><label>唯一银行流水号<input name="receipt" required /></label
              ><label>核实说明<input name="reason" required /></label>
            </div>
            <button>确认已核实的资金变动</button>
          </form>
          <div id="operator-orders"></div>
          <h3>关闭订单后的已核实来款</h3>
          <div id="payment-reviews"></div>
          <form data-action="payment.reconcile">
            <div class="grid">
              <label>订单号<input name="orderId" required /></label
              ><label>核对事件号<input name="reviewId" required /></label
              ><label
                >金额（分）<input
                  name="amountMinor"
                  type="number"
                  min="1"
                  required /></label
              ><label>核实流水号<input name="receipt" required /></label
              ><label>核实说明<input name="reason" required /></label>
            </div>
            <button>核实后补记收款</button>
          </form>
        </div>
        <div class="card">
          <h3>组织管理</h3>
          <div id="tenants"></div>
          <h3>账号管理</h3>
          <div id="users"></div>
        </div>
        <div class="card">
          <h3>发票处理</h3>
          <form data-action="invoice.issue">
            <div class="grid">
              <label>申请号<input name="id" required /></label
              ><label
                >实际开具发票号<input name="invoiceNumber" required
              /></label>
            </div>
            <button>登记已开具发票</button>
          </form>
          <div id="operator-invoices"></div>
          <form data-action="invoice.credit">
            <div class="grid">
              <label>退款发票申请号<input name="id" required /></label
              ><label
                >实际红字发票 / 冲红编号<input name="creditNoteNumber" required
              /></label>
            </div>
            <button>登记已完成冲红</button>
          </form>
        </div>
        <div class="card">
          <h3>审计记录</h3>
          <div id="audit"></div>
          <button id="export">导出运营数据</button>
        </div>
      </section>
      <section data-view="scheduler" hidden>
        <div class="card">
          <h2>项目与执行节点</h2>
          <p>
            按组织和项目批准节点。任务固定绑定环境与流程版本；失联任务需要核对业务结果。
          </p>
          <form id="project-form">
            <div class="grid">
              <label>项目名称<input name="name" required /></label
              ><label
                >允许的网站来源（每行一项）<textarea
                  name="origins"
)AWB1"
    R"AWB2(                  required
                ></textarea>
              </label>
            </div>
            <button>创建项目</button>
          </form>
          <div id="remote-projects"></div>
          <form id="enrollment-form">
            <div class="grid">
              <label
                >项目<select
                  name="projectId"
                  id="enrollment-project"
                ></select></label
              ><label
                >节点并发容量<input
                  name="capacity"
                  type="number"
                  min="1"
                  max="64"
                  value="1"
                  required
              /></label>
            </div>
            <button>生成一次性节点注册码</button>
          </form>
          <pre id="enrollment-result"></pre>
          <div id="remote-nodes"></div>
        </div>
        <div class="card">
          <h2>分配环境与发布流程</h2>
          <form id="assignment-form">
            <div class="grid">
              <label
                >项目<select
                  name="projectId"
                  id="assignment-project"
                ></select></label
              ><label
                >节点<select name="nodeId" id="assignment-node"></select></label
              ><label>环境名称<input name="name" required /></label>
            </div>
            <button>分配环境</button>
          </form>
          <div id="remote-assignments"></div>
          <form id="version-form">
            <label
              >项目<select
                name="projectId"
                id="version-project"
              ></select></label
            ><label
              >已验证流程定义 JSON<textarea
                name="definition"
                rows="10"
                required
              ></textarea></label
            ><button>发布不可变流程版本</button>
          </form>
          <div id="remote-versions"></div>
        </div>
        <div class="card">
          <h2>提交与核对任务</h2>
          <form id="remote-run-form">
            <div class="grid">
              <label
                >环境<select
                  name="assignmentId"
                  id="run-assignment"
                ></select></label
              ><label
                >流程版本<select
                  name="versionId"
                  id="run-version"
                ></select></label
              ><label
                >超时秒数<input
                  name="timeoutSeconds"
                  type="number"
                  min="5"
                  max="86400"
                  value="120"
              /></label>
            </div>
            <label>输入 JSON<textarea name="input" rows="3">{}</textarea></label
            ><button>加入执行队列</button>
          </form>
          <div id="remote-runs"></div>
        </div>
      </section>
      <section data-view="security" hidden>
        <div class="card">
          <h2>企业登录与桌面连接</h2>
          <button id="sso-link" type="button">绑定当前企业身份</button
          ><button id="desktop-code" type="button">生成一次性桌面连接码</button
          ><button id="sso-logout" type="button">退出企业身份登录</button>
          <pre id="desktop-code-result"></pre>
          <p>
            原生账号窗口：邮箱填写
            sso，密码栏粘贴连接码。连接码120秒有效且仅能用一次。
          </p>
        </div>
        <div class="card">
          <h2>修改密码</h2>
          <p>修改后立即吊销全部旧会话，并为当前窗口签发新会话。</p>
          <form data-action="password.change">
            <label
              >当前密码<input
                type="password"
                name="oldPassword"
                required
                autocomplete="current-password" /></label
            ><label
              >新密码（至少12字符）<input
                type="password"
                name="password"
                required
                autocomplete="new-password" /></label
            ><button>修改密码</button>
          </form>
        </div>
      </section>
    </section>
    <footer>
      Workbench Control Plane · C++14 / VC++2019 · 独立SQLite事务存储 ·
      网关凭据在服务端配置
    </footer>
  </main>
  <script src="/portal.js"></script>
</html>
)AWB2";
const char PortalJs[] =
    R"AWB0("use strict";
let token = sessionStorage.getItem("awb-session") || "",
  currentTenant = sessionStorage.getItem("awb-tenant") || "",
  operatorData = null;
// 用途：按ID取得门户元素；日期：2026-11-05。
function element(id) {
  return document.getElementById(id);
}
// 用途：安全显示结果或错误，不把业务文本解析成HTML；日期：2026-11-05。
function notice(message, error = false) {
  const box = element("notice");
  box.textContent = message;
  box.classList.toggle("error", error);
  box.hidden = false;
}
// 用途：发起同源JSON调用并统一检查业务错误；日期：2026-11-05。
async function api(action, body = {}) {
  const response = await fetch("/api/" + action, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: "Bearer " + token,
    },
    body: JSON.stringify({ tenantId: currentTenant, ...body }),
  });
  const value = await response.json();
  if (!value.ok) throw new Error(value.error.code + ": " + value.error.message);
  return value.result;
}
// 用途：安全创建带文字的DOM元素；日期：2026-11-05。
function node(tag, text) {
  const item = document.createElement(tag);
  item.textContent = text ?? "";
  return item;
}
// 用途：创建异步操作按钮并防止重复点击；日期：2026-11-05。
function button(label, operation) {
  const item = node("button", label);
  item.type = "button";
  item.onclick = async function () {
    item.disabled = true;
    try {
      await operation();
    } catch (error) {
      notice(error.message, true);
    } finally {
      item.disabled = false;
    }
  };
  return item;
}
// 用途：用textContent渲染表格及操作列，避免存储型脚本注入；日期：2026-11-05。
function table(id, rows, columns, actions) {
  const area = element(id);
  area.replaceChildren();
  if (!rows?.length) {
    area.append(node("p", "暂无记录"));
    return;
  }
  const wrap = node("div");
  wrap.className = "scroll";
  const grid = node("table"),
    head = node("tr");
  for (const col of columns) head.append(node("th", col[1]));
  if (actions) head.append(node("th", "操作"));
  grid.append(head);
  for (const row of rows) {
    const tr = node("tr");
    for (const col of columns) {
      let text = row[col[0]];
      if (col[2]) text = col[2](row);
      if (typeof text === "object") text = JSON.stringify(text);
      const td = node("td", text);
      if (col[0] === "id") td.className = "id";
      tr.append(td);
    }
    if (actions) {
      const td = node("td");
      td.className = "actions";
      for (const item of actions(row)) td.append(item);
      tr.append(td);
    }
    grid.append(tr);
  }
  wrap.append(grid);
  area.append(wrap);
}
// 用途：将秒级UTC转换为本地可读时间；日期：2026-11-05。
function date(seconds) {
  return seconds ? new Date(seconds * 1000).toLocaleString() : "—";
}
// 用途：调用变更动作后刷新快照，不重复自动发送财务变更；日期：2026-11-05。
async function mutate(action, body) {
  const result = await api(action, body);
  notice("操作完成");
  if (result.invitationToken) {
    element("invitation-result").textContent =
      "邀请令牌仅本次显示，请交给对应邮箱的用户：\n" + result.invitationToken;
  }
  await refresh();
  return result;
}
// 用途：保存会话与组织选择，密码永不进入浏览器持久存储；日期：2026-11-05。
function saveSession(result) {
  token = result.token;
  currentTenant = result.tenantId;
  sessionStorage.setItem("awb-session", token);
  sessionStorage.setItem("awb-tenant", currentTenant);
}
// 用途：根据当前组织权限渲染套餐、席位、账务和运行信息；日期：2026-11-05。
async function refresh() {
  const identity = await api("identity");
  const select = element("tenant");
  select.replaceChildren();
  for (const tenant of identity.tenants) {
    const option = node("option", tenant.name + " · " + tenant.role);
    option.value = tenant.id;
    select.append(option);
  }
  if (
    !identity.tenants.some(function (t) {
      return t.id === currentTenant;
    })
  )
    currentTenant = identity.tenants[0]?.id || "";
  select.value = currentTenant;
  sessionStorage.setItem("awb-tenant", currentTenant);
  element("account").textContent = identity.email;
  element("who").textContent =
    "当前角色：" +
    (identity.tenants.find(function (t) {
      return t.id === currentTenant;
    })?.role || "");
  element("operator-tab").hidden = !identity.platform;
  const data = await api("snapshot"),
    e = data.entitlement;
  element("auth").hidden = true;
  element("workspace").hidden = false;
  const stats = element("stats");
  stats.replaceChildren();
  for (const [title, value] of [
    ["环境额度", e.environmentLimit],
    ["全局并发", e.concurrentLimit],
    ["席位", data.occupiedSeats + " / " + e.seatLimit],
    ["到期", date(e.expiresAt)],
    ["预付余额（秒）", e.timeBalanceSeconds],
  ]) {
    const card = node("div");
    card.className = "card stat";
    card.append(node("small", title), node("b", value));
    stats.append(card);
  }
  element("seat-info").textContent =
    "已分配/预占 " +
    data.occupiedSeats +
    " 个，授权 " +
    e.seatLimit +
    " 个。邀请有效期7天。";
  const plans = element("plans");
  plans.replaceChildren();
  for (const plan of data.plans) {
    const card = node("div");
    card.className = "card";
    card.append(
      node("h3", plan.name),
      node(
        "p",
        (plan.amountMinor / 100).toFixed(2) +
          " 元 / " +
          plan.days +
          " 天" +
          (plan.billingMode === "prepaid"
            ? " · " + plan.timeSeconds + " 秒"
            : ""),
      ),
      node(
        "p",
        plan.environmentLimit +
          " 环境 · " +
          plan.concurrentLimit +
          " 并发 · " +
          plan.seatLimit +
          " 席位",
      ),
      button("购买 / 续费", async function () {
        await mutate("order.create", {
          planId: plan.id,
          quantity: 1,
          idempotencyKey: crypto.randomUUID(),
        });
        showPage("billing");
      }),
    );
    plans.append(card);
  }
  table("grants", data.grant, [
    ["id", "授权订单"],
    [
      "start",
      "开始",
      function (r) {
        return date(r.start);
      },
    ],
    [
      "end",
      "结束",
      function (r) {
        return date(r.end);
      },
    ],
    ["state", "状态"],
  ]);
  table(
    "members",
    data.members,
    [
      ["email", "邮箱"],
      ["role", "角色"],
      ["state", "状态"],
    ],
    function (row) {
      return [
        button(
          row.state === "active" ? "停用 / 回收席位" : "恢复席位",
          async function () {
            await mutate("member.update", {
              id: row.id,
              role: row.role,
              state: row.state === "active" ? "disabled" : "active",
            });
          },
        ),
        button("修改角色", async function () {
          const role = prompt("owner / admin / finance / member", row.role);
          if (role)
            await mutate("member.update", {
              id: row.id,
              role,
              state: row.state,
            });
        }),
      ];
    },
  );
  table(
    "invites",
    data.invite,
    [
      ["id", "邀请ID"],
      ["email", "邮箱"],
      ["role", "角色"],
      ["state", "状态"],
    ],
    function (row) {
      return row.state === "pending"
        ? [
            button("撤销", async function () {
              await mutate("invite.cancel", { id: row.id });
            }),
          ]
        : [];
    },
  );
  table(
    "orders",
    data.order,
    [
      ["id", "订单"],
      ["amountMinor", "金额（分）"],
      ["state", "状态"],
      [
        "createdAt",
        "创建",
)AWB0"
    R"AWB1(        function (r) {
          return date(r.createdAt);
        },
      ],
    ],
    function (row) {
      if (row.state === "pending")
        return [
          button("线下支付说明", async function () {
            const result = await api("payment.prepare", {
              orderId: row.id,
              channel: "manual",
            });
            notice(
              "订单 " +
                row.id +
                "，金额 " +
                result.amountMinor +
                " 分。实际付款后由平台核对流水并确认收款。",
            );
          }),
          button("网关支付", async function () {
            const result = await api("payment.prepare", {
              orderId: row.id,
              channel: "gateway",
            });
            const link = node("a", "打开支付页面");
            link.href = result.checkoutUrl;
            link.target = "_blank";
            link.rel = "noreferrer noopener";
            notice("支付链接已生成：");
            element("notice").append(link);
          }),
          button("取消订单", async function () {
            await mutate("order.cancel", { orderId: row.id });
          }),
        ];
      if (row.state === "paid")
        return [
          button("申请全额退款", async function () {
            const reason = prompt("退款原因");
            if (reason)
              await mutate("refund.request", { orderId: row.id, reason });
          }),
        ];
      return [];
    },
  );
  table("ledger", data.ledger, [
    ["orderId", "订单"],
    [
      "at",
      "时间",
      function (r) {
        return date(r.at);
      },
    ],
    ["entries", "平衡分录"],
  ]);
  table("invoices", data.invoice, [
    ["id", "申请号"],
    ["title", "抬头"],
    ["state", "状态"],
    ["invoiceNumber", "发票号"],
  ]);
  table(
    "devices",
    data.devices,
    [
      ["id", "设备记录"],
      ["name", "名称"],
      ["state", "状态"],
      [
        "lastSeenAt",
        "最近连接",
        function (r) {
          return date(r.lastSeenAt);
        },
      ],
    ],
    function (row) {
      return row.state === "active"
        ? [
            button("撤销设备", async function () {
              await mutate("device.revoke", { id: row.id });
            }),
          ]
        : [];
    },
  );
  table(
    "slots",
    data.slots,
    [
      ["id", "运行槽"],
      ["environmentId", "环境"],
      [
        "state",
        "状态",
        function (r) {
          return r.state === "active" && Date.now() / 1000 - r.lastSeenAt > 180
            ? "状态未确认（仍占用）"
            : r.state;
        },
      ],
      [
        "startedAt",
        "开始",
        function (r) {
          return date(r.startedAt);
        },
      ],
    ],
    function (row) {
      return row.state !== "released"
        ? [
            button("核实停止后释放", async function () {
              const confirmation = prompt(
                "核实该浏览器已经停止，然后输入运行槽ID：",
                row.id,
              );
              if (confirmation === row.id)
                await mutate("slot.resolve", {
                  id: row.id,
                  confirmStopped: confirmation,
                  reason: "operator-verified-browser-stopped",
                });
            }),
          ]
        : [];
    },
  );
  table("usage", data.usage, [
    ["id", "运行槽"],
    ["seconds", "运行秒数"],
    ["paid", "付费订阅"],
    ["billingModel", "计费归属"],
  ]);
  if (identity.platform) await refreshOperator();
  await refreshScheduler();
}
// 用途：渲染平台专用表格，不向普通账号请求运营数据；日期：2026-11-05。
async function refreshOperator() {
  const data = (operatorData = await api("platform.snapshot"));
  table("payment-reviews", data.paymentReviews, [
    ["id", "事件"],
    ["orderId", "订单"],
    ["amountMinor", "金额（分）"],
    ["state", "核对状态"],
  ]);
  element("reconciliation").textContent =
    "账本平衡：" +
    data.reconciliation.balanced +
    " · 净收款 " +
    (data.reconciliation.netReceiptsMinor / 100).toFixed(2) +
    " 元";
  table(
    "operator-plans",
    data.plans,
    [
      ["id", "版本ID"],
      ["productId", "产品"],
      ["name", "名称"],
      ["amountMinor", "金额（分）"],
      ["published", "已发布"],
    ],
    function (row) {
      return row.published
        ? [
            button("下架", async function () {
              await mutate("plan.retire", { id: row.id });
            }),
          ]
        : [];
    },
  );
  table("operator-orders", data.orders, [
    ["id", "订单"],
    ["tenantId", "组织"],
    ["amountMinor", "金额（分）"],
    ["state", "状态"],
  ]);
  table(
    "tenants",
    data.tenants,
    [
      ["id", "组织ID"],
      ["name", "名称"],
      ["state", "状态"],
    ],
    function (row) {
      return [
        button(row.state === "active" ? "冻结" : "解冻", async function () {
          await mutate("tenant.state", {
            id: row.id,
            state: row.state === "active" ? "suspended" : "active",
          });
        }),
      ];
    },
  );
  table(
    "users",
    data.users,
    [
      ["id", "账号ID"],
      ["email", "邮箱"],
      ["state", "状态"],
    ],
    function (row) {
      return row.platform
        ? []
        : [
            button(row.state === "active" ? "停用" : "恢复", async function () {
              await mutate("user.state", {
                id: row.id,
                state: row.state === "active" ? "suspended" : "active",
              });
            }),
          ];
    },
  );
  table("operator-invoices", data.invoices, [
    ["id", "申请ID"],
    ["title", "抬头"],
    ["state", "状态"],
    ["invoiceNumber", "发票号"],
  ]);
  table("audit", data.audit.slice(-200).reverse(), [
    [
      "at",
      "时间",
      function (r) {
        return date(r.at);
      },
    ],
    ["actor", "操作人"],
    ["action", "动作"],
    ["tenantId", "组织"],
    ["resourceId", "资源"],
  ]);
}
// 用途：切换导航视图并保留输入中的表单；日期：2026-11-05。
function showPage(page) {
  for (const view of document.querySelectorAll("[data-view]"))
    view.hidden = view.dataset.view !== page;
  for (const item of document.querySelectorAll("[data-page]"))
    item.classList.toggle("active", item.dataset.page === page);
}
// 用途：提交登录或初始化表单并清除密码；日期：2026-11-05。
element("auth-form").onsubmit = async function (event) {
  event.preventDefault();
  const form = event.currentTarget,
    submit = form.querySelector("button");
  submit.disabled = true;
  try {
    const body = Object.fromEntries(new FormData(form));
    const mode = body.mode;
    delete body.mode;
    saveSession(await api(mode, body));
    form.reset();
    await refresh();
    notice("已登录");
  } catch (error) {
    notice(error.message, true);
  } finally {
    submit.disabled = false;
  }
};
// 用途：注册业务表单事件，数值字段保持整数语义；日期：2026-11-05。
for (const form of document.querySelectorAll("form[data-action]"))
  form.onsubmit = async function (event) {
    event.preventDefault();
    const submit = form.querySelector("button");
    submit.disabled = true;
    try {
      const body = Object.fromEntries(new FormData(form));
      for (const input of form.querySelectorAll("input[type=number]"))
        body[input.name] = Number(input.value);
      const result = await api(form.dataset.action, body);
      if (result.token) saveSession({ ...result, tenantId: currentTenant });
      if (result.invitationToken)
        element("invitation-result").textContent =
          "邀请令牌仅本次显示：\n" + result.invitationToken;
)AWB1"
    R"AWB2(      form.reset();
      await refresh();
      notice("操作完成");
    } catch (error) {
      notice(error.message, true);
    } finally {
      submit.disabled = false;
    }
  };
// 用途：仅对已经核实的线下资金变动执行后台确认；日期：2026-11-05。
element("settle-form").onsubmit = async function (event) {
  event.preventDefault();
  const form = event.currentTarget,
    submit = form.querySelector("button");
  submit.disabled = true;
  try {
    const body = Object.fromEntries(new FormData(form));
    const action = body.mode;
    delete body.mode;
    body.amountMinor = Number(body.amountMinor);
    await mutate(action, body);
    form.reset();
  } catch (error) {
    notice(error.message, true);
  } finally {
    submit.disabled = false;
  }
};
// 用途：组织切换后刷新租户快照；日期：2026-11-05。
element("tenant").onchange = async function () {
  currentTenant = this.value;
  try {
    await refresh();
  } catch (error) {
    notice(error.message, true);
  }
};
// 用途：绑定导航，不依赖HTML内联事件；日期：2026-11-05。
for (const item of document.querySelectorAll("[data-page]"))
  item.onclick = function () {
    showPage(this.dataset.page);
  };
// 用途：刷新当前组织和运营数据；日期：2026-11-05。
element("refresh").onclick = async function () {
  try {
    await refresh();
    notice("数据已刷新");
  } catch (error) {
    notice(error.message, true);
  }
};
// 用途：服务器吊销会话后清除窗口令牌；日期：2026-11-05。
element("logout").onclick = async function () {
  try {
    await api("logout");
    token = "";
    sessionStorage.removeItem("awb-session");
    element("auth").hidden = false;
    element("workspace").hidden = true;
    notice("已退出");
  } catch (error) {
    notice(error.message, true);
  }
};
// 用途：将已授权运营快照导出到用户本地文件；日期：2026-11-05。
element("export").onclick = function () {
  const url = URL.createObjectURL(
    new Blob([JSON.stringify(operatorData, null, 2)], {
      type: "application/json",
    }),
  );
  const link = node("a");
  link.href = url;
  link.download = "workbench-operations.json";
  link.click();
  URL.revokeObjectURL(url);
};
// 用途：恢复现有会话，失败时回到登录入口；日期：2026-11-05。
async function initialize() {
  try {
    const status = await api("status");
    if (!status.initialized)
      notice("请先初始化平台管理员，密钥位于后台数据目录 bootstrap.key。");
    element("sso-login").hidden = !status.oidc?.enabled;
    if (location.search === "?sso=success") {
      token = "";
      currentTenant = "";
      sessionStorage.removeItem("awb-session");
      history.replaceState(null, "", "/");
    }
    try {
      await refresh();
    } catch (error) {
      if (token) throw error;
    }
  } catch (error) {
    notice(error.message, true);
    element("auth").hidden = false;
    element("workspace").hidden = true;
  }
}
// 用途：启动受绑定Cookie保护的企业登录或显式账号绑定；日期：2026-11-05。
async function sso(link) {
  try {
    const result = await api("oidc.begin", { link });
    location.assign(result.authorizationUrl);
  } catch (error) {
    notice(error.message, true);
  }
}
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
element("sso-login").onclick = () => sso(false);
element("sso-link").onclick = () => sso(true);
// 用途：生成一次性桌面连接码，仅显示在当前页面；日期：2026-11-05。
element("desktop-code").onclick = async () => {
  try {
    const result = await api("client.code.create");
    element("desktop-code-result").textContent =
      "120秒内使用一次：" + result.code;
  } catch (error) {
    notice(error.message, true);
  }
};
// 用途：先吊销本地会话再跳转受信身份服务注销页；日期：2026-11-05。
element("sso-logout").onclick = async () => {
  try {
    const result = await api("oidc.end-session");
    token = "";
    sessionStorage.removeItem("awb-session");
    location.assign(result.logoutUrl || "/");
  } catch (error) {
    notice(error.message, true);
  }
};
// 用途：使用官方渠道操作并安全显示付款链接或扫码内容；日期：2026-11-05。
element("channel-form").onsubmit = async (event) => {
  event.preventDefault();
  const submit = event.currentTarget.querySelector("button");
  submit.disabled = true;
  try {
    const body = Object.fromEntries(new FormData(event.currentTarget)),
      action = "payment.channel." + body.verb;
    delete body.verb;
    body.idempotencyKey = crypto.randomUUID();
    const result = await api(action, body);
    element("channel-result").textContent = JSON.stringify(result, null, 2);
    if (result.checkoutUrl) {
      const url = new URL(result.checkoutUrl);
      if (url.protocol !== "https:") throw new Error("付款链接必须使用HTTPS");
      const link = node("a", "打开官方付款页面");
      link.href = url.href;
      link.target = "_blank";
      link.rel = "noopener noreferrer";
      element("channel-result").append(link);
    }
    await refresh();
  } catch (error) {
    notice(error.message, true);
  } finally {
    submit.disabled = false;
  }
};
let schedulerData;
// 用途：填充项目、节点、环境、版本下拉选择，保留仍可用的当前选择；日期：2026-11-05。
function options(id, rows, label) {
  const select = element(id),
    old = select.value;
  select.replaceChildren();
  for (const row of rows) {
    const option = node("option", label(row));
    option.value = row.id;
    select.append(option);
  }
  // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
  if (rows.some((row) => row.id === old)) select.value = old;
}
// 用途：显示当前组织项目范围内的调度资源及人工核对入口；日期：2026-11-05。
async function refreshScheduler() {
  const data = (schedulerData = await api("scheduler.snapshot"));
  for (const id of [
    "enrollment-project",
    "assignment-project",
    "version-project",
  ])
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    options(id, data.projects, (row) => row.name);
  options(
    "assignment-node",
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    data.nodes.filter((row) => row.state === "active"),
    (row) => row.name + " · " + (row.online ? "在线" : "离线"),
  );
  options(
    "run-assignment",
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    data.assignments.filter((row) => row.state === "active"),
    (row) => row.configuration.name + " · " + row.id,
  );
  // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
  options("run-version", data.versions, (row) => row.id);
  table("remote-projects", data.projects, [
    ["id", "项目ID"],
    ["name", "名称"],
    ["allowedOrigins", "网站范围"],
  ]);
  table(
    "remote-nodes",
    data.nodes,
    [
      ["id", "节点ID"],
      ["name", "名称"],
      ["state", "状态"],
      ["online", "在线"],
      ["capacity", "容量"],
    ],
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    (row) =>
      row.state === "revoked"
        ? []
        // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
        : ["active", "maintenance", "revoked"].map((state) =>
            button(
              { active: "批准 / 启用", maintenance: "维护", revoked: "撤销" }[
                state
              ],
)AWB2"
    R"AWB3(              // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
              async () => {
                if (
                  state === "revoked" &&
                  !confirm("撤销后需要重新注册，确认继续？")
                )
                  return;
                await api("node.state", { id: row.id, state });
                await refreshScheduler();
              },
            ),
          ),
  );
  table(
    "remote-assignments",
    data.assignments,
    [
      ["id", "环境ID"],
      ["nodeId", "节点"],
      ["state", "状态"],
    ],
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    (row) =>
      row.state === "active"
        ? [
            // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
            button("回收环境", async () => {
              await api("assignment.release", { id: row.id });
              await refreshScheduler();
            }),
          ]
        : [],
  );
  table("remote-versions", data.versions, [
    ["id", "不可变版本ID"],
    ["projectId", "项目"],
    ["definitionHash", "摘要"],
  ]);
  table(
    "remote-runs",
    data.runs,
    [
      ["id", "任务ID"],
      ["state", "状态"],
      ["nodeId", "节点"],
      ["stoppedConfirmed", "确认已停止"],
      ["queueReason", "排队原因"],
    ],
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    (row) => {
      const actions = [];
      if (["queued", "dispatched", "accepted", "running"].includes(row.state))
        actions.push(
          // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
          button("取消", async () => {
            await api("run.cancel", { id: row.id });
            await refreshScheduler();
          }),
        );
      if (row.state === "unknown" && row.stoppedConfirmed)
        actions.push(
          // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
          button("核对业务结果", async () => {
            const reason = prompt(
              "请核实目标网站实际业务结果并填写核对说明（不会重跑）：",
            );
            if (reason) {
              await api("run.resolve", {
                id: row.id,
                confirmRunId: row.id,
                reason,
              });
              await refreshScheduler();
            }
          }),
        );
      return actions;
    },
  );
}
// 用途：提交有界业务表单并保留失败时输入，避免自动重试网页业务；日期：2026-11-05。
function remoteForm(id, action, convert) {
  element(id).onsubmit = async (event) => {
    event.preventDefault();
    const submit = event.currentTarget.querySelector("button");
    submit.disabled = true;
    try {
      const body = convert(
        Object.fromEntries(new FormData(event.currentTarget)),
      );
      const result = await api(action, body);
      if (result.enrollmentToken)
        element("enrollment-result").textContent =
          "5分钟有效，写入节点私有配置后立即启动：" + result.enrollmentToken;
      notice("操作完成");
      await refreshScheduler();
    } catch (error) {
      notice(error.message, true);
    } finally {
      submit.disabled = false;
    }
  };
}
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
remoteForm("project-form", "project.save", (body) => ({
  name: body.name,
  allowedOrigins: body.origins
    .split(/\r?\n/)
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    .map((x) => x.trim())
    .filter(Boolean),
  authorizedUsers: [],
  state: "active",
}));
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
remoteForm("enrollment-form", "node.enrollment.create", (body) => ({
  projectIds: [body.projectId],
  capacity: Number(body.capacity),
}));
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
remoteForm("assignment-form", "assignment.create", (body) => ({
  projectId: body.projectId,
  nodeId: body.nodeId,
  configuration: {
    name: body.name,
    // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
    allowedOrigins: schedulerData.projects.find((x) => x.id === body.projectId)
      .allowedOrigins,
  },
}));
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
remoteForm("version-form", "workflow.remote.publish", (body) => ({
  projectId: body.projectId,
  definition: JSON.parse(body.definition),
}));
// 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
remoteForm("remote-run-form", "run.submit", (body) => ({
  assignmentId: body.assignmentId,
  versionId: body.versionId,
  input: JSON.parse(body.input),
  timeoutSeconds: Number(body.timeoutSeconds),
  idempotencyKey: crypto.randomUUID(),
}));
initialize();
)AWB3";
} // namespace awb
