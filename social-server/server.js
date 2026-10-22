'use strict';

// KoGaMaZ social server - profiles with a Telegram-style @username, nick search,
// friend requests, online status, "what is my friend playing", a public list
// of hosted worlds so friends can join without a code, private chat between
// friends and posts on the profile.
//
// Deliberately dependency-free (node:http + node:crypto + node:sqlite).
// Passwords are salted scrypt hashes; sessions are bearer tokens stored hashed.
//
//   GET  /api/health
//   POST /api/register            {nick, handle, password}
//   POST /api/login               {nick, password}     <- nick or @handle
//   POST /api/logout
//   GET  /api/me
//   GET  /api/search?nick=alice   -> {results:[{id, nick, handle, relation}]}
//   GET  /api/friends             -> {friends:[{id, nick, handle, online, world, address}]}
//   GET  /api/requests            -> {incoming, outgoing}
//   POST /api/requests            {targetId}
//   POST /api/requests/:id/accept
//   POST /api/requests/:id/reject
//   POST /api/requests/:id/cancel
//   POST /api/presence            {world, address, public} <- heartbeat ~30s
//   GET  /api/hosting             -> public list of open worlds to join
//
//   GET  /api/messages?peer=<id>&before=<created>&limit=<n>
//                                 -> {messages:[{id, mine, body, created, read}], hasMore}
//   POST /api/messages            {peer, body}   <- friends only, <=1000 chars
//   POST /api/messages/read       {peer}         <- mark the peer's side read
//   GET  /api/unread              -> {unread:[{player, count}]}
//   GET  /api/posts?player=<id>   -> {player, nick, handle, posts:[{id, body, created}]}
//   POST /api/posts               {body}         <- <=500 chars, friends can read
//   DELETE /api/posts/:id                        <- own posts only
//
// Auth header: Authorization: Bearer <token>

const http = require('node:http');
const crypto = require('node:crypto');
const { openDatabase } = require('./db');

const VERSION = '1.1.0';
const PORT = Number(process.env.PORT) || 8080;
// За реверс-прокси (nginx на VPS) слушаем только loopback: порт наружу не
// торчит, наружу ходит один nginx со своим TLS. Вариант "в лоб", когда сервер
// сам отвечает на HTTPS, требует 0.0.0.0.
const HOST = process.env.HOST || '0.0.0.0';
const ONLINE_WINDOW_MS = 90000;       // no heartbeat for 90s -> offline
const MAX_BODY = 64 * 1024;
const MIN_PASSWORD = 6;
const NICK_MIN = 3;
const NICK_MAX = 20;
const HANDLE_MIN = 3;
const HANDLE_MAX = 20;
const SEARCH_LIMIT = 20;
const LIST_LIMIT = 100;
// Этап 2: чат между друзьями и посты в профиле.
const MESSAGE_MAX = 1000;   // одна строка чата - короткое сообщение, не письмо
const POST_MAX = 500;
const CHAT_PAGE = 50;       // за одну прокрутку отдаём построчно, а не всю историю
const MESSAGE_LIMIT = 30;   // в минуту - защищает от спама рассылкой по друзьям
// Этап 3: роли соавторов карты.
//
// Роли живут только здесь, в лаунчере: игровой сервер решает доступ к стройке
// одним общим joinMode на весь сервер, а по-отдельности закрывать стройку
// каждому игроку умеет только его плагин на .NET (SDK в проде нет). Поэтому
// роль - это книга учёта, кто кого позвал и кто может звать дальше, а не
// настоящий запрет.
//
// owner не приходит из запроса: его роль менять нельзя, автор карты - всегда
// он. MEMBER_ROLES - что можно назначать через API.
const MEMBER_ROLES = ['co-owner', 'tester', 'member'];
// вес роли: выше вес = больше прав. Нужен только чтобы owner/co-owner решали,
// кто может приглашать, а ниже стоящие - нет.
const ROLE_RANK = { owner: 3, 'co-owner': 2, tester: 1, member: 1 };

function roleCanManage(role) {
  return ROLE_RANK[role] >= ROLE_RANK['co-owner'];
}

// ---------------------------------------------------------------- validation

function cleanNick(raw) {
  if (typeof raw !== 'string') return '';
  // strip control characters, collapse inner whitespace, then trim
  let nick = '';
  for (const ch of raw) {
    const code = ch.codePointAt(0);
    if (code < 0x20 || code === 0x7f) continue;
    nick += ch;
  }
  nick = nick.replace(/\s+/g, ' ').trim();
  if (nick.length < NICK_MIN || nick.length > NICK_MAX) return '';
  return nick;
}

function nickKey(nick) {
  return nick.toLowerCase();
}

// @username - постоянный уникальный идентификатор в духе Telegram. Его человек
// придумывает сам, по нему ищут и добавляют в друзья. Отображаемый ник (nick)
// можно менять сколько угодно и на общий поиск он тоже идёт.
function cleanHandle(raw) {
  if (typeof raw !== 'string') return '';
  // один ведущий "@" допускаем и убираем, остальное - строгая транслитерация
  let handle = raw.trim().toLowerCase();
  if (handle.startsWith('@')) handle = handle.slice(1);
  let clean = '';
  for (const ch of handle) {
    const code = ch.codePointAt(0);
    const allowed = (code >= 0x61 && code <= 0x7a) || (code >= 0x30 && code <= 0x39) || ch === '_';
    if (allowed) clean += ch;
  }
  if (clean.length < HANDLE_MIN || clean.length > HANDLE_MAX) return '';
  return clean;
}

function handleKey(handle) {
  return handle;   // уже в нижнем регистре
}

// Общий вывод для сообщений чата и постов: убираем управляющие символы
// (кроме перевода строки - в чате он уместен), схлопываем пустые строки
// и режем по лимиту. Пустой текст после чистки - ошибка, а не «тишина».
function cleanText(raw, max) {
  if (typeof raw !== 'string') return '';
  let text = '';
  for (const ch of raw) {
    const code = ch.codePointAt(0);
    if (code === 0x0a || code === 0x09) { text += ch; continue; }
    if (code < 0x20 || code === 0x7f) continue;
    text += ch;
  }
  text = text.replace(/\r\n?/g, '\n').replace(/\n{3,}/g, '\n\n').trim();
  if (text.length > max) text = text.slice(0, max);
  return text;
}

// Целое из query-параметра для постраничной навигации по истории.
function cursorOf(raw) {
  const value = Number(raw);
  return Number.isFinite(value) && value > 0 ? Math.floor(value) : 0;
}

// ------------------------------------------------------------------ password

function hashPassword(password) {
  const salt = crypto.randomBytes(16).toString('hex');
  const derived = crypto.scryptSync(password, salt, 64, { N: 16384, r: 8, p: 1 });
  return 'scrypt$16384$' + salt + '$' + derived.toString('hex');
}

function checkPassword(password, stored) {
  if (typeof stored !== 'string') return false;
  const parts = stored.split('$');
  if (parts.length !== 4 || parts[0] !== 'scrypt') return false;
  const cost = Number(parts[1]) || 16384;
  let derived;
  try {
    derived = crypto.scryptSync(password, parts[2], 64, { N: cost, r: 8, p: 1 });
  } catch {
    return false;
  }
  const want = Buffer.from(parts[3], 'hex');
  if (!want.length) return false;
  return derived.length === want.length && crypto.timingSafeEqual(derived, want);
}

// --------------------------------------------------------------------- utils

function sha256(text) {
  return crypto.createHash('sha256').update(text).digest('hex');
}

function send(res, code, body) {
  const payload = JSON.stringify(body);
  res.writeHead(code, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(payload),
    'Cache-Control': 'no-store',
  });
  res.end(payload);
}

function fail(res, code, error, message) {
  send(res, code, { error: error, message: message });
}

function readJson(req) {
  return new Promise((resolve) => {
    let size = 0;
    const chunks = [];
    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      resolve(value);
    };
    req.on('data', (chunk) => {
      size += chunk.length;
      if (size > MAX_BODY) {
        finish({ __tooLarge: true });
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => {
      try {
        const text = Buffer.concat(chunks).toString('utf8');
        const parsed = text ? JSON.parse(text) : {};
        finish(parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : { __badJson: true });
      } catch {
        finish({ __badJson: true });
      }
    });
    req.on('error', () => finish({ __badJson: true }));
  });
}

// Simple in-memory throttle so nick+password cannot be brute forced forever.
const bucket = new Map();
function throttle(ip, route, limit, windowMs) {
  const key = ip + '|' + route;
  const now = Date.now();
  const entry = bucket.get(key) || { count: 0, reset: now + (windowMs || 60000) };
  if (now > entry.reset) {
    entry.count = 0;
    entry.reset = now + (windowMs || 60000);
  }
  entry.count += 1;
  bucket.set(key, entry);
  if (bucket.size > 5000) bucket.clear();
  return entry.count <= (limit || 15);
}

function clientIp(req) {
  const forwarded = req.headers['x-forwarded-for'];
  if (typeof forwarded === 'string' && forwarded.length) return forwarded.split(',')[0].trim();
  return req.socket.remoteAddress || '?';
}

async function playerOf(req, db) {
  const header = req.headers.authorization || '';
  const token = header.startsWith('Bearer ') ? header.slice(7).trim() : '';
  if (!token) return null;
  const row = await db.get('SELECT player FROM tokens WHERE token = ?', [sha256(token)]);
  return row ? row.player : null;
}

async function requirePlayer(req, res, db) {
  const player = await playerOf(req, db);
  if (!player) {
    fail(res, 401, 'unauthorized', 'Sign in to use social features.');
    return null;
  }
  return player;
}

function pairOf(a, b) {
  return a < b ? [a, b] : [b, a];
}

async function relationOf(db, me, other) {
  if (me === other) return 'self';
  const pair = pairOf(me, other);
  const friend = await db.get('SELECT low FROM friends WHERE low = ? AND high = ?', [pair[0], pair[1]]);
  if (friend) return 'friend';
  const out = await db.get('SELECT id FROM requests WHERE sender = ? AND target = ?', [me, other]);
  if (out) return 'outgoing';
  const incoming = await db.get('SELECT id FROM requests WHERE sender = ? AND target = ?', [other, me]);
  if (incoming) return 'incoming';
  return 'none';
}

function isOnline(row) {
  const updated = Number(row && row.updated);
  return updated > 0 && Date.now() - updated <= ONLINE_WINDOW_MS;
}

// ------------------------------------------------------------------- handlers

async function handle(req, res, db, url) {
  const method = req.method;
  const route = url.pathname;
  const ip = clientIp(req);

  if (method === 'GET' && (route === '/' || route === '/api/health')) {
    return send(res, 200, { ok: true, name: 'kogamaz-social', version: VERSION, backend: db.backend });
  }

  // --- account -------------------------------------------------------------
  if (method === 'POST' && route === '/api/register') {
    if (!throttle(ip, 'register', 15)) return fail(res, 429, 'rate_limited', 'Too many attempts, wait a minute.');
    const body = await readJson(req);
    if (body.__tooLarge) return fail(res, 413, 'too_large', 'Request too large.');
    if (body.__badJson) return fail(res, 400, 'bad_json', 'Expected a JSON body.');
    const nick = cleanNick(body.nick);
    if (!nick) return fail(res, 400, 'bad_nick', 'Nickname must be ' + NICK_MIN + '-' + NICK_MAX + ' characters.');
    const handle = cleanHandle(body.handle);
    if (!handle) {
      return fail(res, 400, 'bad_handle',
        'Username must be ' + HANDLE_MIN + '-' + HANDLE_MAX + ' letters, digits or underscores.');
    }
    const password = typeof body.password === 'string' ? body.password : '';
    if (password.length < MIN_PASSWORD) {
      return fail(res, 400, 'bad_password', 'Password needs at least ' + MIN_PASSWORD + ' characters.');
    }

    const id = crypto.randomUUID();
    try {
      await db.run('INSERT INTO players (id, nick, nick_key, handle, handle_key, pass, created) VALUES (?, ?, ?, ?, ?, ?, ?)',
        [id, nick, nickKey(nick), handle, handleKey(handle), hashPassword(password), Date.now()]);
    } catch (e) {
      if (/unique|constraint/i.test(String(e && e.message))) {
        // обе колонки уникальны, поэтому разбираем, какая именно накрылась
        const clash = await db.get('SELECT id FROM players WHERE nick_key = ? OR handle_key = ?',
          [nickKey(nick), handleKey(handle)]);
        const byHandle = clash && await db.get('SELECT id FROM players WHERE handle_key = ?', [handleKey(handle)]);
        return fail(res, 409, byHandle ? 'handle_taken' : 'nick_taken',
          byHandle ? 'That username is taken. Pick another one.' : 'That nickname is already taken.');
      }
      throw e;
    }
    const token = crypto.randomBytes(32).toString('hex');
    await db.run('INSERT INTO tokens (token, player, created) VALUES (?, ?, ?)', [sha256(token), id, Date.now()]);
    return send(res, 201, { token: token, id: id, nick: nick, handle: handle });
  }

  if (method === 'POST' && route === '/api/login') {
    if (!throttle(ip, 'login', 15)) return fail(res, 429, 'rate_limited', 'Too many attempts, wait a minute.');
    const body = await readJson(req);
    if (body.__badJson) return fail(res, 400, 'bad_json', 'Expected a JSON body.');
    const nick = cleanNick(body.nick);
    const password = typeof body.password === 'string' ? body.password : '';
    if (!nick || !password) return fail(res, 401, 'bad_credentials', 'Nickname or password is wrong.');
    // входить можно и по @username, и по отображаемому нику - как кому удобнее
    const handle = cleanHandle(body.handle || body.nick);
    const player = await db.get('SELECT id, nick, pass FROM players WHERE nick_key = ? OR handle_key = ?',
      [nickKey(nick), handleKey(handle || nick)]);
    if (!player || !checkPassword(password, player.pass)) {
      return fail(res, 401, 'bad_credentials', 'Nickname or password is wrong.');
    }
    const mine = await db.get('SELECT handle FROM players WHERE id = ?', [player.id]);
    const token = crypto.randomBytes(32).toString('hex');
    await db.run('INSERT INTO tokens (token, player, created) VALUES (?, ?, ?)', [sha256(token), player.id, Date.now()]);
    return send(res, 200, { token: token, id: player.id, nick: player.nick, handle: mine ? mine.handle : '' });
  }

  if (method === 'POST' && route === '/api/logout') {
    const header = req.headers.authorization || '';
    const token = header.startsWith('Bearer ') ? header.slice(7).trim() : '';
    if (token) await db.run('DELETE FROM tokens WHERE token = ?', [sha256(token)]);
    return send(res, 200, { ok: true });
  }

  if (method === 'GET' && route === '/api/me') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const row = await db.get('SELECT id, nick, handle, created FROM players WHERE id = ?', [me]);
    if (!row) return fail(res, 401, 'unauthorized', 'Unknown account.');
    return send(res, 200, { id: row.id, nick: row.nick, handle: row.handle || '', created: row.created });
  }

  // --- search --------------------------------------------------------------
  if (method === 'GET' && route === '/api/search') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    // приоритет: точный @username -> @username-префикс -> совпадение по нику.
    // Так друга находит именно по @username, а ник остаётся просто подписью.
    const term = String(url.searchParams.get('nick') || url.searchParams.get('handle') || '')
      .trim().toLowerCase().replace(/^@/, '');
    if (!term) return send(res, 200, { results: [] });

    const exact = await db.all('SELECT id, nick, handle FROM players WHERE handle_key = ? LIMIT ?', [term, SEARCH_LIMIT]);
    const prefixed = await db.all('SELECT id, nick, handle FROM players WHERE handle_key LIKE ? AND handle_key <> ? LIMIT ?',
      [term + '%', term, SEARCH_LIMIT]);
    const byNick = await db.all('SELECT id, nick, handle FROM players WHERE nick_key LIKE ? LIMIT ?',
      ['%' + term + '%', SEARCH_LIMIT]);

    const seen = new Set();
    const ordered = [...exact, ...prefixed, ...byNick].filter((row) => {
      if (seen.has(row.id)) return false;
      seen.add(row.id);
      return true;
    }).slice(0, SEARCH_LIMIT);

    const results = [];
    for (const row of ordered) {
      results.push({
        id: row.id,
        nick: row.nick,
        handle: row.handle || '',
        relation: await relationOf(db, me, row.id),
      });
    }
    return send(res, 200, { results: results });
  }

  // --- friends -------------------------------------------------------------
  if (method === 'GET' && route === '/api/friends') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const rows = await db.all(
      `SELECT p.id AS id, p.nick AS nick, p.handle AS handle,
              pr.world AS world, pr.address AS address, pr.updated AS updated
         FROM friends f
         JOIN players p
           ON p.id = (CASE WHEN f.low = ? THEN f.high ELSE f.low END)
         LEFT JOIN presence pr ON pr.player = p.id
        WHERE f.low = ? OR f.high = ?
        ORDER BY p.nick`,
      [me, me, me]);
    return send(res, 200, {
      friends: rows.map((r) => ({
        id: r.id,
        nick: r.nick,
        handle: r.handle || '',
        online: isOnline(r),
        world: r.world || '',
        address: r.address || '',
      })),
    });
  }

  // --- requests ------------------------------------------------------------
  if (method === 'GET' && route === '/api/requests') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const incoming = await db.all(
      `SELECT r.id AS id, r.created AS created, p.id AS player, p.nick AS nick, p.handle AS handle
         FROM requests r JOIN players p ON p.id = r.sender
        WHERE r.target = ? ORDER BY r.created DESC LIMIT ?`, [me, LIST_LIMIT]);
    const outgoing = await db.all(
      `SELECT r.id AS id, r.created AS created, p.id AS player, p.nick AS nick, p.handle AS handle
         FROM requests r JOIN players p ON p.id = r.target
        WHERE r.sender = ? ORDER BY r.created DESC LIMIT ?`, [me, LIST_LIMIT]);
    return send(res, 200, {
      incoming: incoming.map((r) => ({ id: r.id, player: r.player, nick: r.nick, handle: r.handle || '', created: r.created })),
      outgoing: outgoing.map((r) => ({ id: r.id, player: r.player, nick: r.nick, handle: r.handle || '', created: r.created })),
    });
  }

  if (method === 'POST' && route === '/api/requests') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const body = await readJson(req);
    const target = String(body.targetId || '').trim();
    if (!target) return fail(res, 400, 'bad_target', 'Pick a player first.');
    if (target === me) return fail(res, 400, 'bad_target', 'You cannot add yourself.');
    const exists = await db.get('SELECT id FROM players WHERE id = ?', [target]);
    if (!exists) return fail(res, 404, 'unknown_player', 'That player does not exist.');
    const pair = pairOf(me, target);
    const friend = await db.get('SELECT low FROM friends WHERE low = ? AND high = ?', [pair[0], pair[1]]);
    if (friend) return fail(res, 409, 'already_friends', 'You are already friends.');
    try {
      await db.run('INSERT INTO requests (id, sender, target, created) VALUES (?, ?, ?, ?)',
        [crypto.randomUUID(), me, target, Date.now()]);
    } catch (e) {
      if (/unique|constraint/i.test(String(e && e.message))) {
        return fail(res, 409, 'request_exists', 'A friend request already exists.');
      }
      throw e;
    }
    return send(res, 201, { ok: true });
  }

  const requestMatch = method === 'POST' && /^\/api\/requests\/([^/]+)\/(accept|reject|cancel)$/.exec(route);
  if (requestMatch) {
    const id = requestMatch[1];
    const action = requestMatch[2];
    const me = await requirePlayer(req, res, db);
    if (!me) return;

    if (action === 'cancel') {
      await db.run('DELETE FROM requests WHERE id = ? AND sender = ?', [id, me]);
      return send(res, 200, { ok: true });
    }

    // reject and accept only make sense for requests addressed to me
    const request = await db.get('SELECT id, sender, target FROM requests WHERE id = ? AND target = ?', [id, me]);
    if (!request) return fail(res, 404, 'unknown_request', 'That request no longer exists.');

    if (action === 'reject') {
      await db.run('DELETE FROM requests WHERE id = ?', [id]);
      return send(res, 200, { ok: true });
    }

    const pair = pairOf(request.sender, request.target);
    await db.tx(async (tx) => {
      await tx.run('DELETE FROM requests WHERE id = ?', [id]);
      // a back-request may exist meanwhile; both become friendship
      await tx.run('DELETE FROM requests WHERE (sender = ? AND target = ?) OR (sender = ? AND target = ?)',
        [request.sender, request.target, request.target, request.sender]);
      await tx.run('INSERT INTO friends (low, high, created) VALUES (?, ?, ?) ON CONFLICT (low, high) DO NOTHING',
        [pair[0], pair[1], Date.now()]);
    });
    return send(res, 200, { ok: true });
  }

  // --- presence ------------------------------------------------------------
  if (method === 'GET' && route === '/api/hosting') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    // Публичный список хостящих: любой вошедший видит, у кого сейчас открыт мир,
    // и может зайти без кода. Приватные миры сюда не попадают — они видны только
    // друзьям через /api/friends. Читает ту же таблицу presence, что и пульс,
    // поэтому отдельной рассылки от лаунчера не требуется.
    const rows = await db.all(
      `SELECT p.id AS player, p.nick AS nick, p.handle AS handle,
              pr.world AS title, pr.address AS address, pr.updated AS updated
         FROM presence pr JOIN players p ON p.id = pr.player
        WHERE pr.address <> '' AND pr.public = 1 AND pr.updated > ?
        ORDER BY pr.updated DESC LIMIT ?`,
      [Date.now() - ONLINE_WINDOW_MS, LIST_LIMIT]);
    return send(res, 200, {
      hosting: rows
        .filter((r) => r.address)
        .map((r) => ({
          player: r.player,
          nick: r.nick,
          handle: r.handle || '',
          title: r.title || '',
          address: r.address,
        })),
    });
  }

  if (method === 'POST' && route === '/api/presence') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const body = await readJson(req);
    const world = String(body.world || '').slice(0, 80);
    const address = String(body.address || '').slice(0, 200);
    // Игрок сам решает, виден ли его мир в публичном списке. Приватный хостинг
    // остаётся доступным друзьям через их список, но не показывается всем.
    const isPublic = body.public === true ? 1 : 0;
    await db.run(
      `INSERT INTO presence (player, world, address, public, updated) VALUES (?, ?, ?, ?, ?)
       ON CONFLICT (player) DO UPDATE SET world = excluded.world,
                                          address = excluded.address,
                                          public = excluded.public,
                                          updated = excluded.updated`,
      [me, world, address, isPublic, Date.now()]);
    return send(res, 200, { ok: true, online: true, public: isPublic === 1 });
  }

  // --- chat (Этап 2) -------------------------------------------------------
  // Личные сообщения видны только друзьям: переписка не защищена дружбой
  // превращается в канал спама на незнакомых людей.
  const friendOnly = async (req, res, db, me, peer) => {
    if (!peer) { fail(res, 400, 'bad_peer', 'Pick a player first.'); return false; }
    const relation = await relationOf(db, me, peer);
    if (relation !== 'friend') {
      fail(res, 403, 'not_friends', 'You can only message your friends.');
      return false;
    }
    return true;
  };

  if (method === 'GET' && route === '/api/messages') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const peer = String(url.searchParams.get('peer') || '').trim();
    if (!await friendOnly(req, res, db, me, peer)) return;
    // `before` - курсор по created: так страница уходит по одному разу и
    // сообщения, пришедшие пока листали, не дублируются.
    const before = cursorOf(url.searchParams.get('before'));
    const limit = Math.min(Math.max(cursorOf(url.searchParams.get('limit')) || CHAT_PAGE, 1), CHAT_PAGE);
    const rows = await db.all(
      `SELECT id, sender, recipient, body, created, read_at
         FROM messages
        WHERE ((sender = ? AND recipient = ?) OR (sender = ? AND recipient = ?))
          AND (? = 0 OR created < ? OR (created = ? AND id < ?))
        ORDER BY created DESC, id DESC
        LIMIT ?`,
      [me, peer, peer, me, before, before, before, before, limit]);
    // отдаём старым-к-новым, как принято в мессенджерах
    const messages = rows.reverse().map((r) => ({
      id: r.id, mine: r.sender === me, body: r.body, created: r.created, read: r.read_at > 0,
    }));
    return send(res, 200, { messages: messages, hasMore: rows.length === limit });
  }

  if (method === 'POST' && route === '/api/messages') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    // лимит на отправку: один клиент не должен забить очередь всем друзьям
    if (!throttle(ip, 'message', MESSAGE_LIMIT)) {
      return fail(res, 429, 'rate_limited', 'You are sending messages too fast.');
    }
    const body = await readJson(req);
    if (body.__badJson) return fail(res, 400, 'bad_json', 'Expected a JSON body.');
    const peer = String(body.peer || '').trim();
    if (!await friendOnly(req, res, db, me, peer)) return;
    const text = cleanText(body.body, MESSAGE_MAX);
    if (!text) return fail(res, 400, 'empty_message', 'Message cannot be empty.');
    const id = crypto.randomUUID();
    const created = Date.now();
    await db.run('INSERT INTO messages (id, sender, recipient, body, created, read_at) VALUES (?, ?, ?, ?, ?, 0)',
      [id, me, peer, text, created]);
    return send(res, 201, { ok: true, id: id, mine: true, body: text, created: created });
  }

  if (method === 'POST' && route === '/api/messages/read') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const body = await readJson(req);
    const peer = String(body.peer || '').trim();
    if (!await friendOnly(req, res, db, me, peer)) return;
    await db.run('UPDATE messages SET read_at = ? WHERE sender = ? AND recipient = ? AND read_at = 0',
      [Date.now(), peer, me]);
    return send(res, 200, { ok: true });
  }

  if (method === 'GET' && route === '/api/unread') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const rows = await db.all(
      'SELECT sender, COUNT(*) AS unread FROM messages WHERE recipient = ? AND read_at = 0 GROUP BY sender', [me]);
    return send(res, 200, { unread: rows.map((r) => ({ player: r.sender, count: Number(r.unread) })) });
  }

  // --- posts (Этап 2) ------------------------------------------------------
  // Пост в профиле видят сам автор и его друзья. Чужие - нет: профиль публичен
  // через @username, но содержимое остаётся для своих.
  const visiblePosts = async (db, me, author) => {
    if (author === me) return true;
    const relation = await relationOf(db, me, author);
    return relation === 'friend';
  };

  if (method === 'GET' && route === '/api/posts') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const author = String(url.searchParams.get('player') || me).trim() || me;
    if (!await visiblePosts(db, me, author)) {
      return fail(res, 403, 'not_friends', 'Posts are visible to friends only.');
    }
    const before = cursorOf(url.searchParams.get('before'));
    const limit = Math.min(Math.max(cursorOf(url.searchParams.get('limit')) || CHAT_PAGE, 1), CHAT_PAGE);
    const rows = await db.all(
      `SELECT id, body, created FROM posts
        WHERE author = ? AND (? = 0 OR created < ?)
        ORDER BY created DESC, id DESC LIMIT ?`,
      [author, before, before, limit]);
    const owner = await db.get('SELECT nick, handle FROM players WHERE id = ?', [author]);
    return send(res, 200, {
      player: author,
      nick: owner ? owner.nick : '',
      handle: owner ? owner.handle || '' : '',
      posts: rows.map((r) => ({ id: r.id, body: r.body, created: r.created })),
      hasMore: rows.length === limit,
    });
  }

  if (method === 'POST' && route === '/api/posts') {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const body = await readJson(req);
    if (body.__badJson) return fail(res, 400, 'bad_json', 'Expected a JSON body.');
    const text = cleanText(body.body, POST_MAX);
    if (!text) return fail(res, 400, 'empty_post', 'Post cannot be empty.');
    const id = crypto.randomUUID();
    const created = Date.now();
    await db.run('INSERT INTO posts (id, author, body, created) VALUES (?, ?, ?, ?)', [id, me, text, created]);
    return send(res, 201, { ok: true, id: id, body: text, created: created });
  }

  const postDelete = method === 'DELETE' && /^\/api\/posts\/([^/]+)$/.exec(route);
  if (postDelete) {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    // чужой пост удалить нельзя - иначе профиль становится чужой редакцией
    await db.run('DELETE FROM posts WHERE id = ? AND author = ?', [postDelete[1], me]);
    return send(res, 200, { ok: true });
  }

  // --- world collaborators (Этап 3) ----------------------------------------
  // Роли соавторов одной карты. Мир - номер из игрового сервера, поэтому в
  // регулярке и ловим только цифры: мусорный id просто не найдёт таблицы.

  const worldList = method === 'GET' && /^\/api\/worlds\/(\d+)\/members$/.exec(route);
  const worldSet = method === 'POST' && /^\/api\/worlds\/(\d+)\/members$/.exec(route);
  const worldDrop = method === 'DELETE' && /^\/api\/worlds\/(\d+)\/members\/([^/]+)$/.exec(route);

  // роль игрока в этом мире или null, если он не участник
  const roleOf = (rows, player) => {
    const row = rows.find((r) => r.player === player);
    return row ? row.role : null;
  };

  const membersOf = (world) => db.all(
    `SELECT m.player AS player, m.role AS role, m.created AS created,
            p.nick AS nick, p.handle AS handle
       FROM world_members m JOIN players p ON p.id = m.player
      WHERE m.world = ? ORDER BY m.created ASC`, [world]);

  // Список соавторов. Читать может любой вошедший: без этого приглашённый
  // не увидит, кто ещё работает над миром, а приглашение стало бы слепым.
  if (worldList) {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const rows = await membersOf(worldList[1]);
    const owner = rows.find((r) => r.role === 'owner');
    return send(res, 200, {
      world: worldList[1],
      myRole: roleOf(rows, me) || null,
      owner: owner ? { player: owner.player, nick: owner.nick, handle: owner.handle || '' } : null,
      members: rows.map((r) => ({
        player: r.player, nick: r.nick, handle: r.handle || '',
        role: r.role, created: r.created, self: r.player === me,
      })),
    });
  }

  // Пригласить на карту или поменять роль. Первый, кто позвал кого-то, сам
  // становится owner: лаунчер зовёт этот маршрут с машины хоста, то есть со
  // стороны человека, которому карта и принадлежит.
  if (worldSet) {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const world = worldSet[1];
    const body = await readJson(req);
    if (body.__tooLarge) return fail(res, 413, 'too_large', 'Request too large.');
    if (body.__badJson) return fail(res, 400, 'bad_json', 'Expected a JSON body.');
    const target = String(body.player || '').trim();
    if (!target) return fail(res, 400, 'bad_player', 'Pick a player to invite.');
    const role = String(body.role || 'member').trim().toLowerCase();
    if (!MEMBER_ROLES.includes(role)) {
      return fail(res, 400, 'bad_role', 'Role must be co-owner, tester or member.');
    }
    if (target === me) return fail(res, 400, 'bad_player', 'You already have a role on this map.');

    const rows = await membersOf(world);
    if (!rows.some((r) => r.role === 'owner')) {
      // мир ещё никто не завёл - заводит первый позвавший
      const now = Date.now();
      await db.run('INSERT INTO world_members (world, player, role, created) VALUES (?, ?, ?, ?)',
        [world, me, 'owner', now]);
    }
    const fresh = await membersOf(world);
    const mine = roleOf(fresh, me);
    if (!roleCanManage(mine)) {
      return fail(res, 403, 'forbidden', 'Only the owner or a co-owner can invite.');
    }
    if (!await db.get('SELECT id FROM players WHERE id = ?', [target])) {
      return fail(res, 404, 'unknown_player', 'No such player.');
    }
    // UPSERT: повторный вызов меняет роль, а не плодит вторую запись
    await db.run(
      `INSERT INTO world_members (world, player, role, created) VALUES (?, ?, ?, ?)
         ON CONFLICT (world, player) DO UPDATE SET role = excluded.role`,
      [world, target, role, Date.now()]);
    return send(res, 200, { ok: true, world: world, player: target, role: role });
  }

  // Снять роль: убрать соавтора, смениться самому или выйти из карты.
  if (worldDrop) {
    const me = await requirePlayer(req, res, db);
    if (!me) return;
    const world = worldDrop[1];
    const target = worldDrop[2];
    const rows = await membersOf(world);
    const mine = roleOf(rows, me);
    const theirs = roleOf(rows, target);
    if (!theirs) return fail(res, 404, 'not_a_member', 'That player is not on this map.');
    if (theirs === 'owner') return fail(res, 403, 'owner_stays', 'The owner cannot be removed.');
    if (target === me) {
      // выйти может каждый, кроме owner - он и есть автор карты
    } else if (!roleCanManage(mine)) {
      return fail(res, 403, 'forbidden', 'Only the owner or a co-owner can remove.');
    } else if (theirs === 'co-owner' && mine !== 'owner') {
      // co-owner равен по правам, снимать такого может только автор
      return fail(res, 403, 'needs_owner', 'Only the owner can remove a co-owner.');
    }
    await db.run('DELETE FROM world_members WHERE world = ? AND player = ?', [world, target]);
    return send(res, 200, { ok: true, world: world, player: target, removed: true });
  }

  return fail(res, 404, 'not_found', 'Unknown endpoint.');
}

// --------------------------------------------------------------------- server

async function main() {
  const db = await openDatabase();
  console.log('[kogamaz-social] backend=' + db.backend + (db.file ? ' file=' + db.file : ''));

  const server = http.createServer(async (req, res) => {
    const url = new URL(req.url, 'http://' + (req.headers.host || 'localhost'));
    try {
      await handle(req, res, db, url);
    } catch (e) {
      console.error('[kogamaz-social] handler error:', e && e.stack ? e.stack : e);
      if (!res.headersSent) fail(res, 500, 'server_error', 'Something went wrong.');
      else res.end();
    }
  });

  server.listen(PORT, HOST, () => {
    console.log('[kogamaz-social] listening on ' + HOST + ':' + PORT);
  });

  const shutdown = async () => {
    server.close();
    try { await db.close(); } catch { /* ignore */ }
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

if (require.main === module) {
  main().catch((e) => {
    console.error('[kogamaz-social] failed to start:', e);
    process.exit(1);
  });
}

module.exports = { cleanNick, cleanHandle, hashPassword, checkPassword, pairOf };
