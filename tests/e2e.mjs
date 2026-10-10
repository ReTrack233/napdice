// End-to-end test: real Dice! core + dice-host + fake NapCat.
//   node tests/e2e.mjs <path-to-dice-host> <path-to-dice-core>
import { execSync, spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { MockNapCat } from './mock-napcat.mjs';

const [hostBin, coreLib] = process.argv.slice(2).map(p => path.resolve(p));
if (!hostBin || !coreLib) { console.error('usage: node tests/e2e.mjs <dice-host> <core>'); process.exit(2); }

const SELF = 10000, MASTER = 20001, USER = 20002, GROUP = 30001;
const TOKEN = 'secret-token';
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64');

const results = [];
let logs = '';
const logWaiters = [];

function onLog (chunk) {
  const s = chunk.toString();
  logs += s;
  process.stdout.write(s.split('\n').filter(Boolean).map(l => '    | ' + l).join('\n') + (s.trim() ? '\n' : ''));
  for (const w of [...logWaiters]) if (w.re.test(logs.slice(w.from))) { logWaiters.splice(logWaiters.indexOf(w), 1); w.resolve(); }
}
function waitLog (re, timeout = 30000, from = 0) {
  if (re.test(logs.slice(from))) return Promise.resolve();
  return new Promise((resolve, reject) => {
    const w = { re, resolve, from };
    logWaiters.push(w);
    setTimeout(() => { const i = logWaiters.indexOf(w); if (i >= 0) { logWaiters.splice(i, 1); reject(new Error('timeout waiting for log ' + re)); } }, timeout);
  });
}
async function step (name, fn) {
  const t0 = Date.now();
  try { await fn(); results.push([name, true, Date.now() - t0]); console.log(`  ✔ ${name} (${Date.now() - t0}ms)`); } catch (e) { results.push([name, false, e.message]); console.log(`  ✘ ${name}: ${e.message}`); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
const LABEL = path.basename(coreLib);

// GitHub Actions annotation: readable through the checks API even when raw job logs are not.
function annotate (level, title, text) {
  if (!process.env.GITHUB_ACTIONS) return;
  const esc = s => s.replace(/%/g, '%25').replace(/\r/g, '%0D').replace(/\n/g, '%0A');
  console.log(`::${level} title=${esc(title).replace(/:/g, '%3A').replace(/,/g, '%2C')}::${esc(text)}`);
}
const tail = (s, n) => s.split('\n').slice(-n).join('\n');
function sh (cmd, timeout = 120000) {
  try {
    return execSync(cmd, { timeout, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'], maxBuffer: 64 << 20 });
  } catch (e) {
    return `${e.stdout || ''}${e.stderr || ''}\n(${String(e.message).split('\n')[0]})`;
  }
}
const groupReply =(re, since) => mock.waitAction(a => a.action === 'send_group_msg' && Number(a.params.group_id) === GROUP && re.test(a.params.message), 20000, since);

// ---------------------------------------------------------------- fixture
// Resolve to the real long path: on Windows %TEMP% is often an 8.3 short path (C:\Users\RUNNER~1\…),
// while dice-host canonicalizes root_dir, so expected absolute paths must use the same form.
const tmp = fs.realpathSync.native(fs.mkdtempSync(path.join(os.tmpdir(), 'napdice-')));
// Space + CJK on purpose. On Windows the core needs paths representable in the ANSI code page,
// which on CI runners (cp1252) excludes CJK, so use ASCII there.
const IS_WIN = process.platform === 'win32';
const root = path.join(tmp, IS_WIN ? 'dice root' : '骰子 root');
const dice = path.join(root, `Dice${SELF}`);
fs.mkdirSync(path.join(dice, 'PublicDeck'), { recursive: true });
fs.mkdirSync(path.join(dice, 'mod', 'napdice_test', 'reply'), { recursive: true });
fs.mkdirSync(path.join(dice, 'mod', 'napdice_test', 'event'), { recursive: true });
fs.mkdirSync(path.join(dice, 'mod', 'napdice_test', 'script'), { recursive: true });
fs.mkdirSync(path.join(root, 'data', 'image'), { recursive: true });
fs.copyFileSync(coreLib, path.join(root, 'w4123.Dice.dll'));
// Keep the run offline and fast: the core's WebUI would otherwise git-clone its pages on first start.
fs.mkdirSync(path.join(dice, 'conf'), { recursive: true });
fs.writeFileSync(path.join(dice, 'conf', 'console.yaml'), 'config:\n  EnableWebUI: 0\n');
fs.writeFileSync(path.join(root, 'data', 'image', 'napdice,test.png'), PNG);
fs.writeFileSync(path.join(dice, 'PublicDeck', 'napdice.json'), JSON.stringify({ napdice测试: ['甲', '乙'] }));
fs.writeFileSync(path.join(dice, 'mod', 'napdice_test.json'), JSON.stringify({ title: 'NapDice测试', ver: '1.0.0', author: 'napdice', dice_build: 600, brief: 'e2e' }));
fs.writeFileSync(path.join(dice, 'mod', 'napdice_test', 'reply', 'test.toml'), `
[reply.nd_extra]
type = "Order"
keyword.prefix = "测试extra"
echo.lua = """
local r = askExtra({action="get_group_info", params={group_id=msg.gid}})
if r and r.status == "ok" then return "GN:" .. r.data.group_name end
return "askExtra失败"
"""

[reply.nd_members]
type = "Order"
keyword.prefix = "测试成员"
echo.lua = """
local r = askExtra({action="get_group_member_list", params={group_id=msg.gid}})
local n = 0
if r and r.data then for _ in pairs(r.data) do n = n + 1 end end
return "members:" .. n
"""

[reply.nd_image]
type = "Order"
keyword.prefix = "测试图片"
echo = "看图[CQ:image,file=napdice&#44;test.png]"

[reply.nd_at]
type = "Order"
keyword.prefix = "测试at"
echo = "[CQ:at,id=${USER}]你好"
`);
fs.writeFileSync(path.join(dice, 'mod', 'napdice_test', 'event', 'poke.toml'), `
[event.nd_poke]
title = "戳一戳"
trigger.hook = "Poke"
action.lua = "nd_poke"
`);
fs.writeFileSync(path.join(dice, 'mod', 'napdice_test', 'script', 'nd_poke.lua'), 'sendMsg("poked:" .. event.uid, event.gid, 0)\n');

const mock = new MockNapCat({ selfId: SELF, token: TOKEN });
mock.friends = [{ user_id: MASTER, nickname: '主人' }, { user_id: USER, nickname: '玩家' }];
for (let i = 0; i < 3000; i++) mock.friends.push({ user_id: 5000000 + i, nickname: `好友${i}`, remark: '' }); // >64KB frame
const members = [{ user_id: SELF, nickname: '测试骰娘', role: 'admin' }, { user_id: MASTER, nickname: '主人', role: 'owner' }, { user_id: USER, nickname: '玩家', card: '调查员小明' }];
for (let i = 0; i < 2500; i++) members.push({ user_id: 6000000 + i, nickname: `群友${i}` });
mock.addGroup(GROUP, '测试群', members);
const port = await mock.start();

function writeConfig (file, token) {
  fs.writeFileSync(file, `// test config\n${JSON.stringify({
    ws_url: `ws://127.0.0.1:${port}/`, access_token: token, root_dir: root, core_path: path.join(root, 'w4123.Dice.dll'),
    workers: 3, log_level: 'info', log_to_file: true,
  }, null, 2)}\n`);
}

console.log(`fixture: ${tmp}\nmock NapCat: ws://127.0.0.1:${port}/`);

// ---------------------------------------------------------------- 1. bad token
await step('错误 Token 时给出清晰提示', async () => {
  const cfg = path.join(tmp, 'bad.json');
  writeConfig(cfg, 'wrong');
  const p = spawn(hostBin, ['--worker', '--config', cfg]);
  p.stdout.on('data', onLog); p.stderr.on('data', onLog);
  const from = logs.length;
  await waitLog(/access_token 不正确/, 15000, from);
  p.kill(IS_WIN ? undefined : 'SIGINT');
  await new Promise(r => p.on('exit', r));
  if (!mock.authFailures) throw new Error('mock saw no auth attempt');
});

// ---------------------------------------------------------------- 2. main run (supervisor mode)
const cfg = path.join(tmp, 'dice-host.json');
writeConfig(cfg, TOKEN);
const host = spawn(hostBin, ['--config', cfg]);
host.stdout.on('data', onLog); host.stderr.on('data', onLog);
let hostExit = null;
host.on('exit', (code, sig) => { hostExit = { code, sig }; });

await step('连接 NapCat 并启动核心', async () => {
  await waitLog(/Dice! 核心已就绪/, 180000);
  if (!/好友 3002 个，群 1 个/.test(logs)) throw new Error('cache prefetch wrong');
});
// Every later step needs a running core; without one they would only time out one by one.
if (!/Dice! 核心已就绪/.test(logs)) {
  console.log('\n核心没有就绪，收集诊断信息后结束，其余步骤不再执行');
  diagnoseStartup();
  await finish();
}

let masterKey = '';
await step('首次运行打印认主口令，私聊 .master 认主', async () => {
  const m = logs.match(/\.master (\S+) \/\/私骰作成/);
  if (!m) throw new Error('no master key in logs');
  masterKey = m[1];
  const since = mock.actions.length;
  mock.privateMsg(MASTER, `.master ${masterKey}`);
  await mock.waitAction(a => a.action === 'send_private_msg' && Number(a.params.user_id) === MASTER, 20000, since);
});

await step('群聊 .r 1d100 正常回复', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.r 1d100');
  await groupReply(/1D100/i, since);
});

await step('回复引用 + @骰娘 + 指令 能被识别', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, `[CQ:reply,id=12345][CQ:at,qq=${SELF}] .r 2d6`);
  await groupReply(/2D6/i, since);
});

await step('牌堆 .draw', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.draw napdice测试');
  await groupReply(/[甲乙]/, since);
});

await step('Lua askExtra → OneBot 动作', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '测试extra');
  await groupReply(/GN:测试群/, since);
});

await step('askExtra 大数据量（2503 人成员列表）', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '测试成员');
  await groupReply(/members:2503/, since);
});

await step('CQ 图片相对路径改写为绝对路径', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '测试图片');
  const a = await groupReply(/看图/, since);
  const abs = path.join(root, 'data', 'image', 'napdice,test.png').replaceAll('&', '&amp;').replaceAll(',', '&#44;');
  if (!a.params.message.includes(`[CQ:image,file=${abs}]`)) throw new Error('got: ' + a.params.message);
});

await step('[CQ:at,id=] 改写为 [CQ:at,qq=]', async () => {
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '测试at');
  await groupReply(new RegExp(`\\[CQ:at,qq=${USER}\\]你好`), since);
});

await step('戳一戳 → Poke hook', async () => {
  const since = mock.actions.length;
  mock.pushEvent({ post_type: 'notice', notice_type: 'notify', sub_type: 'poke', group_id: GROUP, user_id: USER, target_id: SELF });
  await groupReply(new RegExp(`poked:${USER}`), since);
});

await step('好友申请 → set_friend_add_request(flag)', async () => {
  const since = mock.actions.length;
  mock.pushEvent({ post_type: 'request', request_type: 'friend', user_id: 20009, comment: '你好', flag: 'flag-friend-1' });
  await mock.waitAction(a => a.action === 'set_friend_add_request' && a.params.flag === 'flag-friend-1', 20000, since);
});

await step('主人邀请入群 → 同意 set_group_add_request(flag)', async () => {
  const since = mock.actions.length;
  mock.pushEvent({ post_type: 'request', request_type: 'group', sub_type: 'invite', group_id: 30009, user_id: MASTER, comment: '', flag: 'flag-group-1' });
  const a = await mock.waitAction(a => a.action === 'set_group_add_request' && a.params.flag === 'flag-group-1', 25000, since);
  if (a.params.approve !== true) throw new Error('expected approve=true, got ' + JSON.stringify(a.params));
});

await step('分片帧 + 帧间 ping', async () => {
  mock.fragmentEvents = true;
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.r 1d3');
  await groupReply(/1D3/i, since);
  mock.fragmentEvents = false;
});

await step('NapCat 断开后自动重连', async () => {
  const before = mock.connectCount;
  mock.closeAll();
  await waitLog(/连接断开/, 10000, logs.length - 2000);
  const t0 = Date.now();
  while (mock.connectCount === before) { if (Date.now() - t0 > 15000) throw new Error('no reconnect'); await sleep(100); }
  await sleep(500);
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.r 1d5');
  await groupReply(/1D5/i, since);
});

function childPidOf (ppid) {
  const cmd = IS_WIN
    ? `powershell -NoProfile -Command "(Get-CimInstance Win32_Process -Filter 'ParentProcessId=${ppid}' | Where-Object { $_.Name -eq 'dice-host.exe' }).ProcessId"`
    : `pgrep -P ${ppid}`;
  return Number(execSync(cmd).toString().trim().split(/\s+/)[0]);
}

await step('工作进程被强杀后守护进程自动拉起', async () => {
  const pid = childPidOf(host.pid);
  if (!pid) throw new Error('worker pid not found');
  const from = logs.length;
  process.kill(pid, 'SIGKILL');
  await waitLog(/异常退出/, 10000, from);
  await waitLog(/Dice! 核心已就绪/, 60000, from);
  await sleep(500);
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.r 1d9');
  await groupReply(/1D9/i, since);
});

await step('.system reload → 守护进程重启核心', async () => {
  const from = logs.length;
  mock.privateMsg(MASTER, '.system reload');
  await waitLog(/重新启动骰娘/, 20000, from);
  await waitLog(/Dice! 核心已就绪/, 60000, from);
  await sleep(500);
  const since = mock.actions.length;
  mock.groupMsg(GROUP, USER, '.r 1d7');
  await groupReply(/1D7/i, since);
});

await step(IS_WIN ? '.system die → Killme 优雅退出并保存数据' : 'Ctrl+C 优雅退出并保存数据', async () => {
  // Windows has no SIGINT for child processes; the core's own exit command exercises Killme instead.
  if (IS_WIN) mock.privateMsg(MASTER, '.system die');
  else host.kill('SIGINT');
  const t0 = Date.now();
  while (!hostExit) { if (Date.now() - t0 > 40000) throw new Error('did not exit'); await sleep(100); }
  if (hostExit.code !== 0) throw new Error('exit ' + JSON.stringify(hostExit));
  if (!fs.existsSync(path.join(dice, 'user', 'UserConf.dat'))) throw new Error('UserConf.dat not saved');
  if (!fs.readdirSync(path.join(path.dirname(hostBin), 'logs')).length) throw new Error('no log file');
});

await finish();

async function finish () {
  await mock.stop();
  if (!hostExit) {
    if (IS_WIN) sh(`taskkill /T /F /PID ${host.pid}`); // also takes down the worker
    else {
      try { const w = childPidOf(host.pid); if (w) process.kill(w, 'SIGKILL'); } catch { }
      host.kill('SIGKILL');
    }
  }
  if (mock.protocolErrors) results.push(['客户端帧均带掩码', false, `${mock.protocolErrors} unmasked frames`]);
  const failed = results.filter(r => !r[1]);
  console.log(`\n${results.length - failed.length}/${results.length} passed`);
  if (failed.length) {
    annotate('error', `${LABEL}: ${failed.length} 项失败`,
      failed.map(r => `✘ ${r[0]}: ${r[2]}`).join('\n') + '\n\n' + tail(logs, 40));
  }
  process.exit(failed.length ? 1 : 0);
}

// The core never came up. Record where the worker is stuck: loader message boxes, the core's DLL
// imports, and every thread's stack (main thread in full). Output goes to stdout, to
// logs/startup-diagnostics.txt (uploaded with the e2e logs) and to annotations.
function diagnoseStartup () {
  const out = [];
  const add = (title, body) => {
    body = String(body).trim() || '(empty)';
    out.push([title, body]);
    console.log(`\n==== ${title}\n${body}`);
  };
  if (IS_WIN) {
    const pf86 = process.env['ProgramFiles(x86)'] || 'C:\\Program Files (x86)';
    add('Application Popup 事件（系统弹窗）', sh('powershell -NoProfile -Command "Get-WinEvent -FilterHashtable @{LogName=\'System\'; ProviderName=\'Application Popup\'} -MaxEvents 10 -ErrorAction SilentlyContinue | Format-List TimeCreated,Message | Out-String -Width 400"'));
    let pid = 0;
    try { pid = childPidOf(host.pid); } catch { }
    const dbgArch = /（32 位）/.test(logs) ? 'x86' : 'x64';
    const cdb = [pf86, process.env.ProgramFiles || 'C:\\Program Files']
      .map(p => path.join(p, 'Windows Kits', '10', 'Debuggers', dbgArch, 'cdb.exe')).find(p => fs.existsSync(p));
    if (!pid) add('工作进程线程栈', 'worker pid not found');
    else if (!cdb) {
      add('工作进程线程栈', `cdb (${dbgArch}) not found. Windows Kits:\n` +
        sh(`dir /s /b "${path.join(pf86, 'Windows Kits', '10')}\\cdb.exe" "${path.join(pf86, 'Windows Kits', '10')}\\Debuggers"`, 60000).split('\n').slice(0, 15).join('\n'));
    }
    else {
      const sym = `srv*${path.join(os.tmpdir(), 'symcache')}*https://msdl.microsoft.com/download/symbols;${path.dirname(coreLib)}`;
      add(`工作进程 ${pid} 线程栈（cdb）`,
        sh(`"${cdb}" -pv -p ${pid} -y "${sym}" -c ".lines -d; !cs -o ntdll!LdrpLoaderLock; ~0kc 80; ~*kc 20; q"`, 300000));
    }
    try {
      const vs = sh(`"${path.join(pf86, 'Microsoft Visual Studio', 'Installer', 'vswhere.exe')}" -latest -property installationPath`).trim().split(/\r?\n/)[0];
      const msvc = path.join(vs, 'VC', 'Tools', 'MSVC');
      const dumpbin = path.join(msvc, fs.readdirSync(msvc).sort().pop(), 'bin', 'Hostx64', 'x64', 'dumpbin.exe');
      add('核心依赖的 DLL（dumpbin /dependents）', sh(`"${dumpbin}" /nologo /dependents "${coreLib}"`));
    } catch (e) { add('核心依赖的 DLL', 'dumpbin not found: ' + e.message); }
  }
  add('dice-host 日志末尾', tail(logs, 15));
  try {
    const dir = path.join(path.dirname(hostBin), 'logs');
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'startup-diagnostics.txt'), out.map(([t, b]) => `==== ${t}\n${b}\n`).join('\n'));
  } catch { }
  // At most 10 annotations per level per step, so pack everything into a few ~3 KB chunks.
  const text = out.map(([t, b]) => `## ${t}\n${b}`).join('\n\n');
  for (let i = 0, n = 1; i < text.length && n <= 9; i += 3000, n++) {
    annotate('warning', `${LABEL} 启动诊断 ${n}`, text.slice(i, i + 3000));
  }
}
