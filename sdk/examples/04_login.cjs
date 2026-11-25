/* 用途：使用编译后的TS客户端和本机SecretRef执行一次登录并核对业务文本；日期：2026-11-05。 */
const fs = require('node:fs');
const { Client } = require(process.argv[2]);

// 用途：读取显式场景与私有端点，口令不进入参数或标准输出；日期：2026-11-05。
async function main() {
  const scenario = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
  const endpoint = JSON.parse(fs.readFileSync(scenario.endpointFile, 'utf8'));
  const client = new Client(endpoint.url, endpoint.token);
  await client.connect();
  const profileId = scenario.profileId;
  const lease = await client.call('lease_acquire', {profileId, owner: 'sdk-typescript-login', ttlMs: 60000});
  const base = {profileId, lease};
  try {
    await client.call('browser_navigate', {...base, url: scenario.url});
    const username = await client.call('page_locate', {...base, selector: scenario.usernameSelector});
    await client.call('page_fill', {...base, ref: username.ref, value: scenario.username});
    const password = await client.call('page_locate', {...base, selector: scenario.passwordSelector});
    await client.call('page_fill_secret', {...base, ref: password.ref, credentialId: scenario.credentialId});
    const submit = await client.call('page_locate', {...base, selector: scenario.submitSelector});
    await client.call('page_click', {...base, ref: submit.ref});
    await client.call('page_wait_text', {...base, selector: scenario.successSelector,
      expected: scenario.successText, timeoutMs: 10000});
    process.stdout.write(JSON.stringify({result: 'passed', businessAssertion: true, submittedOnce: true}));
  } finally { await client.call('lease_revoke', {profileId}); }
}

// 用途：失败只输出错误码，未知结果保留给调用方核对，不重试或输出秘密；日期：2026-11-05。
main().catch(function(error) { process.stderr.write(error.code || error.name); process.exitCode = 1; });
