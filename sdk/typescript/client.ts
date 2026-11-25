import type { ToolArguments, ToolName } from './contract_v1.js';
import { toolSchemas } from './contract_v1.js';

// 用途：递归规范对象键用于完整Schema比较，数组顺序保持契约含义；日期：2026-11-05。
function canonical(value: unknown): string {
  if (Array.isArray(value)) return '[' + value.map(canonical).join(',') + ']';
  if (value !== null && typeof value === 'object') {
    const data = value as Record<string, unknown>;
    return '{' + Object.keys(data).sort().map(key => JSON.stringify(key) + ':' + canonical(data[key])).join(',') + '}';
  }
  return JSON.stringify(value);
}

export class AwbError extends Error {
  // 用途：保留业务错误与未知结果，调用方不得自动重放动作；日期：2026-11-05。
  constructor(public code: string, message: string, public outcomeUnknown = false) { super(message); }
}

export class Client {
  // 用途：绑定显式本机端点、短期令牌和有限请求预算；日期：2026-11-05。
  constructor(private url: string, private token: string, private timeoutMs = 35000) {
    const endpoint = new URL(url);
    if (endpoint.protocol !== 'http:' || endpoint.hostname !== '127.0.0.1' || !endpoint.port || endpoint.pathname !== '/' || endpoint.search || endpoint.hash || endpoint.username || endpoint.password ||
        !token || /[\r\n]/.test(token) || timeoutMs < 1000 || timeoutMs > 120000) throw new Error('Invalid loopback endpoint or timeout');
    this.url = endpoint.origin;
  }

  // 用途：发送一次鉴权请求并保留未知结果，禁止重定向和自动重试；日期：2026-11-05。
  private async request(path: string, args?: Record<string, unknown>): Promise<unknown> {
    const controller = new AbortController();
    // 用途：只中断客户端等待，不宣称取消服务器中的工作流或网页动作；日期：2026-11-05。
    const timer = setTimeout(() => controller.abort(), this.timeoutMs);
    try {
      const response = await fetch(this.url + path, {method: args ? 'POST' : 'GET', redirect: 'error',
        headers: {'Authorization': 'Bearer ' + this.token, 'Content-Type': 'application/json'},
        body: args ? JSON.stringify(args) : undefined, signal: controller.signal});
      if (!response.body) throw new AwbError('CLIENT_INVALID_RESPONSE', 'Missing response body', true);
      const reader = response.body.getReader();
      const chunks: Uint8Array[] = [];
      let bytes = 0;
      try {
        for (;;) {
          const chunk = await reader.read();
          if (chunk.done) break;
          bytes += chunk.value.byteLength;
          if (bytes > 16 * 1024 * 1024) {
            await reader.cancel();
            throw new AwbError('CLIENT_RESPONSE_BUDGET', 'Response exceeds budget', true);
          }
          chunks.push(chunk.value);
        }
      } finally { reader.releaseLock(); }
      const joined = new Uint8Array(bytes);
      let offset = 0;
      for (const chunk of chunks) { joined.set(chunk, offset); offset += chunk.byteLength; }
      const raw = new TextDecoder('utf-8', {fatal: true}).decode(joined);
      const answer = JSON.parse(raw);
      if (!answer.ok) throw new AwbError(answer.error?.code || 'CLIENT_INVALID_RESPONSE', answer.error?.message || 'Request failed', !!answer.error?.outcomeUnknown);
      return answer.result;
    } catch (error) {
      if (error instanceof AwbError) throw error;
      throw new AwbError('TRANSPORT_OUTCOME_UNKNOWN', 'Inspect run/page state before any retry', true);
    } finally { clearTimeout(timer); }
  }

  // 用途：查询服务器真实能力，模型或后台未配置时不伪造能力；日期：2026-11-05。
  async capabilities(): Promise<unknown> { return this.request('/v1/health'); }

  // 用途：连接前比较完整工具集合和参数Schema，拒绝不兼容服务；日期：2026-11-05。
  async connect(): Promise<unknown> {
    const result = await this.request('/v1/tools') as {tools: {name: string, inputSchema: unknown}[]};
    if (!Array.isArray(result.tools) || result.tools.length !== Object.keys(toolSchemas).length)
      throw new AwbError('CLIENT_CONTRACT_MISMATCH', 'Server tool set differs from bound contract');
    const actual = Object.fromEntries(result.tools.map(tool => [tool.name, tool.inputSchema]));
    if (canonical(actual) !== canonical(toolSchemas))
      throw new AwbError('CLIENT_CONTRACT_MISMATCH', 'Server tool schemas differ from bound contract');
    return this.capabilities();
  }

  // 用途：通过生成的工具名与参数类型调用共享契约；日期：2026-11-05。
  async call<T extends ToolName>(tool: T, args: ToolArguments[T]): Promise<unknown> {
    if (!Object.hasOwn(toolSchemas, tool)) throw new AwbError('CLIENT_UNKNOWN_TOOL', 'Invalid tool name');
    return this.request('/v1/tools/' + tool, args);
  }
}
