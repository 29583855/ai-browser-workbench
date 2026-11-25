// 本机测试专用：连接本次测试自有Profile的临时端点，不是产品公开接口。日期：2026-11-03。
import { readFile, mkdir, writeFile } from "node:fs/promises";
import { createInterface } from "node:readline";
import path from "node:path";

const [profileDirectory, expectedOrigin, outputDirectory] = process.argv.slice(2);
const discovery = (await readFile(path.join(profileDirectory, "DevToolsActivePort"), "utf8")).trim().split(/\r?\n/);
if (!/^\d+$/.test(discovery[0]) || !discovery[1].startsWith("/devtools/browser/")) {
    throw new Error("Invalid owned discovery data");
}
const connection = new WebSocket(`ws://127.0.0.1:${discovery[0]}${discovery[1]}`);
let sequence = 0;
const pending = new Map();
const frames = [];
let session;
let recordingError = null;

// 用途：只向本测试受管CDP连接发送带ID且有截止时间的请求；日期：2026-11-03。
function call(method, params = {}, page = true) {
    const id = ++sequence;
    // 用途：关联测试请求，超时仅失败，禁止自动重发；日期：2026-11-03。
    return new Promise((resolve, reject) => {
        // 用途：移除过期请求并报告录制失败；日期：2026-11-03。
        const timeout = setTimeout(() => { pending.delete(id); reject(new Error("Recorder CDP timeout")); }, 8000);
        pending.set(id, { resolve, reject, timeout });
        connection.send(JSON.stringify({ id, method, params, ...(page ? { sessionId: session } : {}) }));
    });
}

// 用途：接收真实浏览器帧，按CDP要求确认；帧数及单帧大小均有限；日期：2026-11-03。
connection.addEventListener("message", event => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
        const item = pending.get(message.id);
        pending.delete(message.id);
        clearTimeout(item.timeout);
        if (message.error) item.reject(new Error("Recorder command rejected"));
        else item.resolve(message.result);
    } else if (message.method === "Page.screencastFrame" && message.sessionId === session) {
        const item = message.params;
        if (frames.length >= 500 || item.data.length > 2 * 1024 * 1024) {
            recordingError = "frame-budget-exceeded";
        } else {
            frames.push({ timestamp: item.metadata.timestamp, data: item.data });
        }
        // 用途：确认已接收帧并保留异常，避免未处理的Promise失败；日期：2026-11-03。
        call("Page.screencastFrameAck", { sessionId: item.sessionId }).catch(() => { recordingError = "ack-failed"; });
    }
});

// 用途：等待本机连接就绪，失败立即终止而不生成假录像；日期：2026-11-03。
await new Promise((resolve, reject) => {
    connection.addEventListener("open", resolve, { once: true });
    connection.addEventListener("error", reject, { once: true });
});
const targets = await call("Target.getTargets", {}, false);
// 用途：只选择本次测试已导航到明确来源的唯一页面；日期：2026-11-03。
const pages = targets.targetInfos.filter(target => target.type === "page" && target.url.startsWith(expectedOrigin + "/"));
if (pages.length !== 1) throw new Error("Recorder requires one matching owned target");
session = (await call("Target.attachToTarget", { targetId: pages[0].targetId, flatten: true }, false)).sessionId;
await call("Page.enable");
const dom = await call("DOM.getDocument", { depth: -1, pierce: true });
const shadowRoots = new Set();
let iframeNodes = 0;
// 用途：检查真实DOM的Shadow类型及iframe节点，结果不冒充产品跨帧动作；日期：2026-11-03。
function inspectTree(node) {
    if (!node || typeof node !== "object") return;
    if (node.shadowRootType) shadowRoots.add(node.shadowRootType);
    if (node.nodeName === "IFRAME") ++iframeNodes;
    for (const value of Object.values(node)) {
        if (Array.isArray(value)) value.forEach(inspectTree);
        else if (value && typeof value === "object") inspectTree(value);
    }
}
inspectTree(dom.root);
const currentTargets = await call("Target.getTargets", {}, false);
// 用途：确认跨站框架确实被当前Chromium隔离为独立目标；日期：2026-11-03。
const oopif = currentTargets.targetInfos.filter(target => target.type === "iframe" && target.url.endsWith("/frame"));
await mkdir(outputDirectory, { recursive: true });
await writeFile(path.join(outputDirectory, "dom-probe.json"), JSON.stringify({
    browserTargetObserved: true, shadowRootTypes: [...shadowRoots].sort(), iframeNodes,
    oopifTargets: oopif.length, productActionsScope: "main-frame-only",
    closedShadowActionsSupported: false, oopifActionsSupported: false
}, null, 2) + "\n");
await call("Page.startScreencast", { format: "jpeg", quality: 65, maxWidth: 1000, maxHeight: 800, everyNthFrame: 1 });
process.stdout.write("ready\n");

const input = createInterface({ input: process.stdin });
// 用途：接收测试驱动的停止信号，生命周期不延长到测试外；日期：2026-11-03。
await new Promise(resolve => {
    input.once("line", resolve);
    input.once("close", resolve);
});
input.close();
await call("Page.stopScreencast");
await call("Target.detachFromTarget", { sessionId: session }, false);
connection.close();
if (recordingError || frames.length < 3) throw new Error(recordingError || "Insufficient actual screencast frames");
await mkdir(outputDirectory, { recursive: true });
const manifest = [];
for (let index = 0; index < frames.length; ++index) {
    const name = `frame-${String(index).padStart(4, "0")}.jpg`;
    await writeFile(path.join(outputDirectory, name), Buffer.from(frames[index].data, "base64"));
    manifest.push({ name, timestamp: frames[index].timestamp });
}
await writeFile(path.join(outputDirectory, "frames.json"), JSON.stringify({ source: "CDP Page.screencastFrame", frames: manifest }, null, 2) + "\n");
