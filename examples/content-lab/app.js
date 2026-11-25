"use strict";

let csrf = "";
let currentArticle = null;
let searchPage = 1;

// 用途：查找固定ID控件；日期：2026-11-04。
function byId(id) {
  return document.getElementById(id);
}

// 用途：发送同源JSON业务请求，不自动重试写请求；日期：2026-11-04。
async function api(path, data) {
  const options =
    data === undefined
      ? {}
      : {
          method: "POST",
          headers: { "Content-Type": "application/json", "X-CSRF-Token": csrf },
          body: JSON.stringify(data),
        };
  let response;
  try {
    response = await fetch(path, options);
  } catch (_error) {
    throw new Error("RECEIPT_UNKNOWN");
  }
  const result = await response.json();
  if (!response.ok) {
    throw new Error(result.error || "HTTP_ERROR");
  }
  return result;
}

// 用途：为动作设置忙碌与最终业务状态，失败不重复点击；日期：2026-11-04。
async function perform(statusId, operation) {
  byId(statusId).textContent = "busy";
  try {
    await operation();
  } catch (error) {
    byId(statusId).textContent = error.message;
  }
}

// 用途：刷新当前帐号和受保护工作区，清理页面中的旧身份；日期：2026-11-04。
async function refreshSession() {
  const session = await api("/api/session");
  csrf = session.csrf;
  byId("identity").textContent = session.username;
  byId("role").textContent = session.role;
  byId("workspace").hidden = session.username === "anonymous";
}

// 用途：从实际文章结果渲染正文、本人点赞状态及绑定父级的评论；日期：2026-11-04。
function renderArticle(article) {
  currentArticle = article;
  byId("detail").hidden = false;
  byId("article-id").textContent = article.id;
  byId("article-author").textContent = article.author;
  byId("article-category").textContent = article.category;
  byId("article-state").textContent = article.state;
  byId("detail-title").textContent = article.title;
  byId("detail-body").textContent = article.body;
  byId("like-state").textContent = article.liked ? "liked" : "not-liked";
  byId("like").textContent = article.liked ? "已点赞" : "点赞";
  byId("like").disabled = article.liked;
  byId("reply-parent").textContent = "none";
  byId("comments").replaceChildren();
  for (const comment of article.comments) {
    const row = document.createElement("div");
    row.className = "comment";
    row.dataset.commentId = comment.id;
    const meta = document.createElement("small");
    meta.textContent =
      comment.id +
      " · " +
      comment.author +
      " · parent=" +
      (comment.parent || "none");
    const body = document.createElement("p");
    body.textContent = comment.body;
    const reply = document.createElement("button");
    reply.className = "secondary";
    reply.textContent = "回复";
    // 用途：绑定当前评论ID，避免同名按钮误选父级；日期：2026-11-04。
    reply.onclick = function () {
      byId("reply-parent").textContent = comment.id;
    };
    row.append(meta, body, reply);
    byId("comments").append(row);
  }
  byId("detail-status").textContent = "opened:" + article.id;
}

// 用途：读取文章详情，失败时不沿用旧文章作为新目标；日期：2026-11-04。
async function openArticle(identity) {
  currentArticle = null;
  byId("detail").hidden = true;
  renderArticle(await api("/api/article?id=" + encodeURIComponent(identity)));
}

// 用途：按实际搜索分页展示可逐行读取的文章标识与详情链接；日期：2026-11-04。
async function search() {
  const result = await api(
    "/api/search?q=" +
      encodeURIComponent(byId("search-query").value) +
      "&page=" +
      searchPage,
  );
  byId("results").replaceChildren();
  for (const item of result.items) {
    const row = document.createElement("tr");
    row.className = "result";
    for (const [name, text] of [
      ["id", item.id],
      ["title", item.title],
      ["author", item.author],
      ["url", location.origin + "/?article=" + encodeURIComponent(item.id)],
    ]) {
      const cell = document.createElement("td");
      cell.className = "result-" + name;
      cell.textContent = text;
      row.append(cell);
    }
    const cell = document.createElement("td");
    const open = document.createElement("button");
    open.textContent = "打开";
    // 用途：从该行的稳定文章标识打开详情；日期：2026-11-04。
    open.onclick = function () {
      perform("detail-status", async function () {
        await openArticle(item.id);
      });
    };
    cell.append(open);
    row.append(cell);
    byId("results").append(row);
  }
  byId("search-page").textContent = String(result.page);
  byId("search-total").textContent = String(result.total);
  byId("has-next").textContent = String(result.hasNext);
  byId("next-page").disabled = !result.hasNext;
  byId("search-status").textContent =
    "results:" + result.total + ":page:" + result.page;
}

// 用途：提交准备稿件，仅在响应确认后显示文章业务状态；日期：2026-11-04。
async function publish(state) {
  const result = await api("/api/publish", {
    title: byId("article-title").value,
    body: byId("article-body").value,
    category: byId("category").value,
    taskKey: byId("publish-key").value,
    state: state,
  });
  renderArticle(result);
  byId("publish-status").textContent = result.state;
}

// 用途：提交评论或回复并重新读取数据库对应文章，绑定最终评论标识；日期：2026-11-04。
async function sendComment(reply) {
  if (!currentArticle) {
    throw new Error("ARTICLE_REQUIRED");
  }
  const prefix = reply ? "reply" : "comment";
  const parent = reply ? byId("reply-parent").textContent : "";
  if (reply && parent === "none") {
    throw new Error("PARENT_REQUIRED");
  }
  const identity = currentArticle.id;
  const result = await api("/api/comment", {
    articleId: identity,
    parentId: parent,
    body: byId(prefix + "-body").value,
    taskKey: byId(prefix + "-key").value,
  });
  await openArticle(identity);
  byId(prefix + "-status").textContent = "submitted";
}

// 用途：绑定登录、发布、查询和互动事件，所有写入仅派发一次；日期：2026-11-04。
function initialize() {
  // 用途：注册专用练习帐号；日期：2026-11-04。
  byId("register").onclick = function () {
    perform("auth-status", async function () {
      const result = await api("/api/register", {
        username: byId("username").value,
        password: byId("password").value,
      });
      byId("auth-status").textContent = result.state;
    });
  };
  // 用途：完成登录并核对服务端返回的当前身份；日期：2026-11-04。
  byId("login").onclick = function () {
    perform("auth-status", async function () {
      const result = await api("/api/login", {
        username: byId("username").value,
        password: byId("password").value,
      });
      byId("password").value = "";
      await refreshSession();
      byId("auth-status").textContent = result.state;
    });
  };
  // 用途：退出服务器会话并清空旧工作区内容；日期：2026-11-04。
  byId("logout").onclick = function () {
    perform("auth-status", async function () {
      await api("/api/logout", {});
      location.reload();
    });
  };
  // 用途：打开新稿编辑表单；日期：2026-11-04。
  byId("create-article").onclick = function () {
    byId("editor").hidden = false;
    byId("editor-status").textContent = "ready";
  };
  // 用途：执行正式发布；日期：2026-11-04。
  byId("article-publish").onclick = function () {
    perform("publish-status", async function () {
      await publish("published");
    });
  };
  // 用途：保存独立草稿；日期：2026-11-04。
  byId("article-draft").onclick = function () {
    perform("publish-status", async function () {
      await publish("draft");
    });
  };
  // 用途：提交为待审核状态；日期：2026-11-04。
  byId("article-review").onclick = function () {
    perform("publish-status", async function () {
      await publish("pending-review");
    });
  };
  // 用途：从第一页开始新的搜索；日期：2026-11-04。
  byId("search").onclick = function () {
    searchPage = 1;
    perform("search-status", search);
  };
  // 用途：继续当前搜索的下一页；日期：2026-11-04。
  byId("next-page").onclick = function () {
    searchPage += 1;
    perform("search-status", search);
  };
  // 用途：按用户给定文章标识核对实际记录；日期：2026-11-04。
  byId("open-article").onclick = function () {
    perform("detail-status", async function () {
      await openArticle(byId("open-id").value);
    });
  };
  // 用途：设置为已赞，成功后重读本人状态；日期：2026-11-04。
  byId("like").onclick = function () {
    perform("like-state", async function () {
      if (!currentArticle) {
        throw new Error("ARTICLE_REQUIRED");
      }
      const identity = currentArticle.id;
      await api("/api/like", { articleId: identity });
      await openArticle(identity);
    });
  };
  // 用途：提交文章评论；日期：2026-11-04。
  byId("comment-send").onclick = function () {
    perform("comment-status", async function () {
      await sendComment(false);
    });
  };
  // 用途：提交指定父评论的回复；日期：2026-11-04。
  byId("reply-send").onclick = function () {
    perform("reply-status", async function () {
      await sendComment(true);
    });
  };
  // 用途：提供同名但不同业务的按钮作为误定位反例；日期：2026-11-04。
  byId("settings-publish").onclick = function () {
    byId("settings-status").textContent = "settings-only";
  };
  // 用途：页面刷新时恢复真实会话，已登录详情链接可直接核对；日期：2026-11-04。
  perform("auth-status", async function () {
    await refreshSession();
    byId("auth-status").textContent = "ready";
    const identity = new URLSearchParams(location.search).get("article");
    if (identity && byId("identity").textContent !== "anonymous") {
      await openArticle(identity);
    }
  });
}

initialize();
