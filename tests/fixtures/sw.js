"use strict";
// 用途：安装测试ServiceWorker后立即激活；日期：2026-11-03。
self.addEventListener("install", () => self.skipWaiting());
// 用途：接管本夹具页面，以验证真实SW独立注册；日期：2026-11-03。
self.addEventListener("activate", event => event.waitUntil(clients.claim()));
