"use strict";
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
        function (r) {
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
      form.reset();
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
              // 用途：映射当前资源或处理此控件操作，实际权限由后台核验；日期：2026-11-05。
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
