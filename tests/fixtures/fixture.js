"use strict";

// 用途：取得固定夹具控件，避免用户输入构造脚本；日期：2026-11-03。
function byId(id) {
    return document.getElementById(id);
}

// 用途：打开真实IndexedDB并创建隔离标记表；日期：2026-11-03。
function database() {
    // 用途：把数据库回调转换为可等待的测试状态；日期：2026-11-03。
    return new Promise((resolve, reject) => {
        const request = indexedDB.open("awb-fixture", 1);
        // 用途：仅在首次创建数据库时建立表；日期：2026-11-03。
        request.onupgradeneeded = () => request.result.createObjectStore("markers");
        // 用途：向调用者交付已打开的数据库；日期：2026-11-03。
        request.onsuccess = () => resolve(request.result);
        // 用途：传播数据库错误，禁止伪造成功状态；日期：2026-11-03。
        request.onerror = () => reject(request.error);
    });
}

// 用途：在事务完成后读取或写入环境隔离标记；日期：2026-11-03。
async function marker(value) {
    const connection = await database();
    // 用途：等待真实提交，避免请求成功而事务尚未落地；日期：2026-11-03。
    return new Promise((resolve, reject) => {
        const transaction = connection.transaction("markers", value === undefined ? "readonly" : "readwrite");
        const store = transaction.objectStore("markers");
        const request = value === undefined ? store.get("identity") : store.put(value, "identity");
        let answer = null;
        // 用途：暂存请求结果，仍须等待事务完成；日期：2026-11-03。
        request.onsuccess = () => { answer = request.result; };
        // 用途：事务提交后关闭连接并公布结果；日期：2026-11-03。
        transaction.oncomplete = () => { connection.close(); resolve(answer); };
        // 用途：事务失败时关闭连接并报告失败；日期：2026-11-03。
        transaction.onerror = () => { connection.close(); reject(transaction.error); };
    });
}

let refreshGeneration = 0;

// 用途：显示服务器身份和存储事实，拒绝旧异步请求覆盖新身份；日期：2026-11-04。
async function refresh() {
    const generation = ++refreshGeneration;
    const response = await fetch("/api/me");
    const me = await response.json();
    if (generation !== refreshGeneration) return;
    byId("identity").textContent = me.username || "anonymous";
    // 用途：只保留本夹具的CacheStorage标记；日期：2026-11-03。
    const cacheNames = globalThis.caches ? (await caches.keys()).filter(name => name.startsWith("awb-")).sort() : [];
    byId("storage").textContent = JSON.stringify({
        local: localStorage.getItem("identity"), indexed: (await marker()) ?? null, cache: cacheNames
    });
}

// 用途：提交一次真实业务并记录固定故障阶段，不输出口令或异常正文；日期：2026-11-04。
async function submit(path) {
    let stage = "request";
    try {
        const response = await fetch("/api/" + path, {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ username: byId("username").value, password: byId("password").value })
        });
        stage = "response-json";
        const data = await response.json();
        if (path === "login" && response.ok) {
            stage = "local-storage";
            localStorage.setItem("identity", data.username);
            stage = "indexed-db-write";
            await marker(data.username);
            stage = "cache-open";
            if (globalThis.caches) await caches.open("awb-" + data.username);
        }
        stage = "identity-refresh";
        await refresh();
        byId("result").textContent = data.error || (path === "register" ? "registered" : path === "login" ? "logged-in" : "logged-out");
        byId("fixture-diagnostic").textContent = "completed";
    } catch (error) {
        byId("result").textContent = "network-error";
        const names = ["AbortError", "ConstraintError", "DataError", "InvalidStateError", "NetworkError", "NotFoundError", "QuotaExceededError", "SecurityError", "TypeError", "UnknownError", "VersionError"];
        byId("fixture-diagnostic").textContent = stage + ":" + (names.includes(error.name) ? error.name : "other-error");
        console.error(error);
    }
}

// 用途：注册按钮只提交一次注册；日期：2026-11-03。
byId("register").onclick = () => submit("register");
// 用途：登录按钮只提交一次登录；日期：2026-11-03。
byId("login").onclick = () => submit("login");
// 用途：注销按钮只提交一次注销；日期：2026-11-03。
byId("logout").onclick = () => submit("logout");
// 用途：读取受登录保护的报表，区分业务成功与鉴权失败；日期：2026-11-03。
byId("filter").onclick = async () => {
    const response = await fetch("/api/report");
    const data = await response.json();
    byId("report").textContent = response.ok ? "rows:" + data.rows.length : data.error;
};
// 用途：替换实际DOM，供失效元素引用测试；日期：2026-11-03。
byId("replace").onclick = () => {
    byId("replace-zone").innerHTML = '<button id="old-node">新节点</button>';
};
byId("open-shadow").attachShadow({ mode: "open" }).innerHTML = "<button>Shadow按钮</button>";
byId("closed-shadow").attachShadow({ mode: "closed" }).innerHTML = "<button>ClosedShadow按钮</button>";
// 用途：只在本地故障测试中提交一次后阻塞渲染线程，验证结果未知不能重试；日期：2026-11-03。
byId("hang-submit").onmousedown = () => {
    navigator.sendBeacon("/api/fault-commit", "fixture-only");
    const deadline = performance.now() + 12000;
    while (performance.now() < deadline) {
        // 故意阻塞本测试页面，浏览器端8秒截止必须先返回未知结果。
    }
};

// 用途：登记真实ServiceWorker并报告固定错误类别，避免伪造隔离结果；日期：2026-11-04。
async function prepareWorker() {
    if (!navigator.serviceWorker) {
        byId("sw").textContent = "secure-context-required";
        return;
    }
    try {
        await navigator.serviceWorker.register("/sw.js");
        await navigator.serviceWorker.ready;
        byId("sw").textContent = "sw-ready";
    } catch (error) {
        const name = ["AbortError", "InvalidStateError", "SecurityError", "TypeError", "UnknownError"].includes(error.name) ? error.name : "other-error";
        byId("sw").textContent = "sw-error:" + name;
        console.error(error);
    }
}

prepareWorker();
refresh();
