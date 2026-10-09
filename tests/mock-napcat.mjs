// A tiny fake NapCat: OneBot 11 forward WebSocket server with canned data. No dependencies.
import http from 'node:http';
import crypto from 'node:crypto';

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';

export class MockNapCat {
  constructor ({ selfId = 10000, nickname = '测试骰娘', token = '' } = {}) {
    this.selfId = selfId;
    this.nickname = nickname;
    this.token = token;
    this.conns = new Set();
    this.actions = [];
    this.waiters = [];
    this.connectCount = 0;
    this.authFailures = 0;
    this.fragmentEvents = false;
    this.friends = [];
    this.groups = new Map();   // gid -> {group_id, group_name, member_count, max_member_count}
    this.members = new Map();  // gid -> Map(uid -> member)
    this.strangers = new Map();
    this.msgSeq = 1000;
  }

  addGroup (gid, name, members) {
    this.groups.set(gid, { group_id: gid, group_name: name, member_count: members.length, max_member_count: 500 });
    const m = new Map();
    for (const x of members) m.set(x.user_id, { group_id: gid, nickname: '', card: '', role: 'member', last_sent_time: 0, ...x });
    this.members.set(gid, m);
  }

  start (port = 0) {
    this.server = http.createServer((req, res) => { res.writeHead(404); res.end(); });
    this.server.on('upgrade', (req, socket) => this.#upgrade(req, socket));
    return new Promise(resolve => this.server.listen(port, '127.0.0.1', () => resolve(this.server.address().port)));
  }

  stop () {
    for (const c of this.conns) c.socket.destroy();
    this.conns.clear();
    return new Promise(resolve => this.server.close(() => resolve()));
  }

  closeAll () {
    for (const c of this.conns) this.#sendFrame(c, 0x8, Buffer.from([0x03, 0xe8]));
    for (const c of this.conns) setTimeout(() => c.socket.destroy(), 50);
    this.conns.clear();
  }

  #upgrade (req, socket) {
    const url = new URL(req.url, 'http://x');
    const token = url.searchParams.get('access_token') || (req.headers.authorization || '').replace(/^Bearer /, '');
    if (this.token && token !== this.token) {
      this.authFailures++;
      socket.end('HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n');
      return;
    }
    const key = req.headers['sec-websocket-key'];
    const accept = crypto.createHash('sha1').update(key + GUID).digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    const conn = { socket, buf: Buffer.alloc(0), frag: [] };
    this.conns.add(conn);
    this.connectCount++;
    socket.setNoDelay(true);
    socket.on('data', d => { conn.buf = Buffer.concat([conn.buf, d]); this.#parse(conn); });
    socket.on('close', () => this.conns.delete(conn));
    socket.on('error', () => this.conns.delete(conn));
    // lifecycle event like NapCat
    this.#sendJson(conn, { time: now(), self_id: this.selfId, post_type: 'meta_event', meta_event_type: 'lifecycle', sub_type: 'connect' });
    // exercise ping handling
    this.#sendFrame(conn, 0x9, Buffer.from('hi'));
  }

  #parse (conn) {
    for (;;) {
      const b = conn.buf;
      if (b.length < 2) return;
      const fin = (b[0] & 0x80) !== 0;
      const op = b[0] & 0x0f;
      const masked = (b[1] & 0x80) !== 0;
      let len = b[1] & 0x7f;
      let off = 2;
      if (len === 126) { if (b.length < 4) return; len = b.readUInt16BE(2); off = 4; } else if (len === 127) { if (b.length < 10) return; len = Number(b.readBigUInt64BE(2)); off = 10; }
      if (!masked) { this.protocolErrors = (this.protocolErrors || 0) + 1; }
      const mask = masked ? b.subarray(off, off + 4) : null;
      if (masked) off += 4;
      if (b.length < off + len) return;
      const payload = Buffer.from(b.subarray(off, off + len));
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      conn.buf = b.subarray(off + len);
      if (op === 0x8) { this.#sendFrame(conn, 0x8, payload.subarray(0, 2)); conn.socket.end(); return; }
      if (op === 0x9) { this.#sendFrame(conn, 0xa, payload); continue; }
      if (op === 0xa) { this.pongs = (this.pongs || 0) + 1; continue; }
      conn.frag.push(payload);
      if (!fin) continue;
      const text = Buffer.concat(conn.frag).toString('utf8');
      conn.frag = [];
      this.#onRequest(conn, JSON.parse(text));
    }
  }

  #sendFrame (conn, op, payload, fin = true) {
    const len = payload.length;
    let header;
    if (len < 126) { header = Buffer.from([(fin ? 0x80 : 0) | op, len]); } else if (len < 65536) { header = Buffer.alloc(4); header[0] = (fin ? 0x80 : 0) | op; header[1] = 126; header.writeUInt16BE(len, 2); } else { header = Buffer.alloc(10); header[0] = (fin ? 0x80 : 0) | op; header[1] = 127; header.writeBigUInt64BE(BigInt(len), 2); }
    try { conn.socket.write(Buffer.concat([header, payload])); } catch { }
  }

  #sendJson (conn, obj, fragment = false) {
    const data = Buffer.from(JSON.stringify(obj), 'utf8');
    if (!fragment || data.length < 8) return this.#sendFrame(conn, 0x1, data);
    const a = Math.floor(data.length / 3), b2 = Math.floor(data.length * 2 / 3);
    this.#sendFrame(conn, 0x1, data.subarray(0, a), false);
    this.#sendFrame(conn, 0x0, data.subarray(a, b2), false);
    this.#sendFrame(conn, 0x9, Buffer.from('mid')); // control frame between fragments is legal
    this.#sendFrame(conn, 0x0, data.subarray(b2), true);
  }

  pushEvent (ev) {
    const full = { time: now(), self_id: this.selfId, ...ev };
    for (const c of this.conns) this.#sendJson(c, full, this.fragmentEvents);
  }

  groupMsg (gid, uid, raw, extra = {}) {
    const m = this.members.get(gid)?.get(uid) || {};
    this.pushEvent({
      post_type: 'message', message_type: 'group', sub_type: 'normal', message_id: ++this.msgSeq, group_id: gid, user_id: uid,
      raw_message: raw, message: raw, message_format: 'string', font: 14,
      sender: { user_id: uid, nickname: m.nickname || `用户${uid}`, card: m.card || '', role: m.role || 'member' }, ...extra,
    });
  }

  privateMsg (uid, raw, extra = {}) {
    this.pushEvent({
      post_type: 'message', message_type: 'private', sub_type: 'friend', message_id: ++this.msgSeq, user_id: uid,
      raw_message: raw, message: raw, message_format: 'string', font: 14, sender: { user_id: uid, nickname: `用户${uid}` }, ...extra,
    });
  }

  waitAction (pred, timeoutMs = 15000, since = 0) {
    const hit = this.actions.slice(since).find(pred);
    if (hit) return Promise.resolve(hit);
    return new Promise((resolve, reject) => {
      const w = { pred, resolve };
      this.waiters.push(w);
      setTimeout(() => {
        const i = this.waiters.indexOf(w);
        if (i >= 0) { this.waiters.splice(i, 1); reject(new Error('timeout waiting for action')); }
      }, timeoutMs);
    });
  }

  #onRequest (conn, req) {
    const { action, params = {}, echo } = req;
    const rec = { action, params, at: Date.now() };
    this.actions.push(rec);
    for (const w of [...this.waiters]) {
      if (w.pred(rec)) { this.waiters.splice(this.waiters.indexOf(w), 1); w.resolve(rec); }
    }
    let data = null; let ok = true; let message = '';
    const gid = Number(params.group_id); const uid = Number(params.user_id);
    switch (action) {
      case 'get_login_info': data = { user_id: this.selfId, nickname: this.nickname }; break;
      case 'get_version_info': data = { app_name: 'MockNapCat', app_version: '4.18.28', protocol_version: 'v11' }; break;
      case 'get_status': data = { online: true, good: true }; break;
      case 'get_friend_list': data = this.friends; break;
      case 'get_group_list': data = [...this.groups.values()]; break;
      case 'get_group_info': data = this.groups.get(gid) || null; ok = !!data; message = ok ? '' : '群不存在'; break;
      case 'get_group_member_list': data = [...(this.members.get(gid)?.values() || [])]; break;
      case 'get_group_member_info': data = this.members.get(gid)?.get(uid) || null; ok = !!data; message = ok ? '' : '成员不存在'; break;
      case 'get_stranger_info': data = { user_id: uid, nickname: this.strangers.get(uid) || `陌生人${uid}` }; break;
      case 'send_group_msg': case 'send_private_msg': case 'send_msg': data = { message_id: ++this.msgSeq }; break;
      case 'upload_group_file': case 'upload_private_file': data = { file_id: 'f' }; break;
      case 'set_friend_add_request': case 'set_group_add_request': case 'set_group_leave': case 'set_group_kick':
      case 'set_group_ban': case 'set_group_card': case 'set_group_admin': case 'set_group_whole_ban': case 'set_group_special_title':
        data = null; break;
      default: ok = false; message = `不支持的动作 ${action}`;
    }
    const resp = ok ? { status: 'ok', retcode: 0, data, message: '', wording: '', echo } : { status: 'failed', retcode: 1404, data: null, message, wording: message, echo };
    this.#sendJson(conn, resp, this.fragmentEvents);
  }
}

function now () { return Math.floor(Date.now() / 1000); }
