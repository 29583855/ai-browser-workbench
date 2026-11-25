import fs from "node:fs";
import path from "node:path";

const [directory, mode, origin] = process.argv.slice(2);
const [port, endpoint] = fs.readFileSync(path.join(directory, "DevToolsActivePort"), "utf8").trim().split(/\r?\n/);
const socket = new WebSocket("ws://127.0.0.1:" + port + endpoint);
let sequence = 0;
const pending = new Map();
// 用途：只接收本测试创建的浏览器CDP响应，不输出页面正文或输入；日期：2026-11-04。
socket.addEventListener("message", event => {
    const message = JSON.parse(String(event.data));
    const entry = pending.get(message.id);
    if (entry) {
        pending.delete(message.id);
        if (message.error) entry.reject(new Error("CDP gesture rejected"));
        else entry.resolve(message.result);
    }
});
// 用途：有界等待本测试连接就绪；日期：2026-11-04。
await new Promise((resolve, reject) => {
    const timer = setTimeout(reject, 10000, new Error("Gesture connection timeout"));
    // 用途：连接成功后清除超时；日期：2026-11-04。
    socket.addEventListener("open", () => { clearTimeout(timer); resolve(); }, {once: true});
    // 用途：连接失败立即报告；日期：2026-11-04。
    socket.addEventListener("error", () => { clearTimeout(timer); reject(new Error("Gesture connection failed")); }, {once: true});
});
// 用途：派发真实浏览器输入消息，作用域只限自有测试页面；日期：2026-11-04。
async function call(method, params = {}, sessionId) {
    const id = ++sequence;
    // 用途：保存有界响应回调；日期：2026-11-04。
    return await new Promise((resolve, reject) => {
        // 用途：超时终止本条测试请求；日期：2026-11-04。
        const timer = setTimeout(() => { pending.delete(id); reject(new Error("Gesture response timeout")); }, 10000);
        // 用途：成功或失败均清除对应定时器；日期：2026-11-04。
        pending.set(id, {resolve: value => { clearTimeout(timer); resolve(value); },
                         // 用途：传播协议错误而不输出其可能包含的数据；日期：2026-11-04。
                         reject: error => { clearTimeout(timer); reject(error); }});
        socket.send(JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})}));
    });
}

try {
    const {targetInfos} = await call("Target.getTargets");
    // 用途：严格选择本测试来源和固定路径的唯一页面；日期：2026-11-04。
    const pages = targetInfos.filter(item => item.type === "page" && item.url === origin + "/recording");
    if (pages.length !== 1) throw new Error("Gesture target is not unique");
    const {sessionId} = await call("Target.attachToTarget", {targetId: pages[0].targetId, flatten: true});
    if (mode === "composition") {
        await call("Runtime.evaluate", {expression: "document.getElementById('user').focus()"}, sessionId);
        await call("Input.imeSetComposition", {text: "zhong", selectionStart: 0, selectionEnd: 5}, sessionId);
        await call("Input.imeSetComposition", {text: "中文", selectionStart: 0, selectionEnd: 2}, sessionId);
        await call("Input.insertText", {text: "中文帐号"}, sessionId);
        const result = await call("Runtime.evaluate", {expression: "document.getElementById('user').value === '中文帐号'", returnByValue: true}, sessionId);
        if (!result.result.value) throw new Error("Composition did not commit the expected final value");
    } else if (mode === "enter") {
        await call("Runtime.evaluate", {expression: "document.getElementById('password').focus()"}, sessionId);
        await call("Input.dispatchKeyEvent", {type: "keyDown", key: "Enter", code: "Enter", text: "\r", unmodifiedText: "\r", windowsVirtualKeyCode: 13}, sessionId);
        await call("Input.dispatchKeyEvent", {type: "keyUp", key: "Enter", code: "Enter", windowsVirtualKeyCode: 13}, sessionId);
    } else if (mode === "picker") {
        // 用途：只计算固定测试按钮的点击中心，不读取业务输入；日期：2026-11-04。
        const result = await call("Runtime.evaluate", {expression: "(()=>{const r=document.getElementById('submit').getBoundingClientRect();return{x:r.x+r.width/2,y:r.y+r.height/2}})()", returnByValue: true}, sessionId);
        const point = result.result.value;
        await call("Input.dispatchMouseEvent", {type: "mouseMoved", ...point}, sessionId);
        await call("Input.dispatchMouseEvent", {type: "mousePressed", ...point, button: "left", clickCount: 1}, sessionId);
        await call("Input.dispatchMouseEvent", {type: "mouseReleased", ...point, button: "left", clickCount: 1}, sessionId);
    } else if (mode === "overflow") {
        await call("Runtime.evaluate", {expression: "for(let i=0;i<1100;i++)document.getElementById('plain').click()"}, sessionId);
    } else throw new Error("Unknown gesture test mode");
    await call("Target.detachFromTarget", {sessionId});
    process.stdout.write("passed\n");
} finally {
    socket.close();
}
