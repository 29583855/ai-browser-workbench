/* 用途：实际运行编译后的TS薄客户端并覆盖成功、业务拒绝与传输边界；日期：2026-11-05。 */
const { Client, AwbError } = require(process.argv[2]);
const fs = require('node:fs');

// 用途：只从测试创建的私有端点读取令牌，标准输出不包含配置；日期：2026-11-05。
async function main() {
  const config = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
  const client = new Client(config.url, config.token);
  if (process.argv[4]) {
    const expected = process.argv[4];
    try { await client.call('profile_list', {}); } catch (error) {
      if (!(error instanceof AwbError) || error.code !== expected || !error.outcomeUnknown) throw error;
      process.stdout.write(JSON.stringify({result: 'passed', rejection: expected}));
      return;
    }
    throw new Error('Missing expected rejection');
  }
  const health = await client.connect();
  if (!health.workflowTypedVariables || health.workflowDsl !== '1.1') throw new Error('Missing actual capabilities');
  const doctor = await client.call('doctor', {});
  if (!doctor.readOnly || doctor.quota.enforced) throw new Error('Doctor misrepresented quotas');
  const profiles = await client.call('profile_list', {});
  if (profiles.profiles.length !== 1) throw new Error('TS did not observe Python-created profile');
  try { await client.call('browser_start', {profileId: '0'.repeat(32)}); } catch (error) {
    if (!(error instanceof AwbError) || error.code !== 'PROFILE_NOT_FOUND' || error.outcomeUnknown) throw error;
    process.stdout.write(JSON.stringify({result: 'passed', contract: true, sharedProfile: true, businessError: true}));
    return;
  }
  throw new Error('Missing expected business rejection');
}

// 用途：异常仅输出固定错误码，避免端点令牌出现在测试证据；日期：2026-11-05。
main().catch(function(error) { process.stderr.write(error.code || error.name); process.exitCode = 1; });
