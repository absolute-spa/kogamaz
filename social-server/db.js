'use strict';

// Persistence for the KoGaMaZ social server.
//
// Two backends behind one tiny interface:
//   * node:sqlite  - zero dependencies, file on disk. For local runs, LAN and
//                    port-forwarding on a friend's PC.
//   * pg           - when DATABASE_URL is set (Render/Railway/Neon). Needed for
//                    a real deployment, because free app hosts have an ephemeral
//                    filesystem and would wipe a SQLite file on every redeploy.
//
// The SQL below deliberately stays portable: `?` placeholders are rewritten to
// `$1..$n` for Postgres, and every statement avoids vendor-only syntax.

const path = require('node:path');
const fs = require('node:fs');

const SCHEMA = [
  `CREATE TABLE IF NOT EXISTS players (
     id        TEXT PRIMARY KEY,
     nick      TEXT NOT NULL,
     nick_key  TEXT NOT NULL,
     handle    TEXT NOT NULL DEFAULT '',
     handle_key TEXT NOT NULL DEFAULT '',
     pass      TEXT NOT NULL,
     created   INTEGER NOT NULL
   )`,
  `CREATE UNIQUE INDEX IF NOT EXISTS players_nick_key ON players(nick_key)`,
  `CREATE TABLE IF NOT EXISTS tokens (
     token    TEXT PRIMARY KEY,
     player   TEXT NOT NULL,
     created  INTEGER NOT NULL
   )`,
  `CREATE INDEX IF NOT EXISTS tokens_player ON tokens(player)`,
  `CREATE TABLE IF NOT EXISTS requests (
     id       TEXT PRIMARY KEY,
     sender   TEXT NOT NULL,
     target   TEXT NOT NULL,
     created  INTEGER NOT NULL,
     UNIQUE (sender, target)
   )`,
  `CREATE INDEX IF NOT EXISTS requests_target ON requests(target)`,
  `CREATE INDEX IF NOT EXISTS requests_sender ON requests(sender)`,
  `CREATE TABLE IF NOT EXISTS friends (
     low      TEXT NOT NULL,
     high     TEXT NOT NULL,
     created  INTEGER NOT NULL,
     PRIMARY KEY (low, high)
   )`,
  `CREATE INDEX IF NOT EXISTS friends_high ON friends(high)`,
  `CREATE TABLE IF NOT EXISTS presence (
     player   TEXT PRIMARY KEY,
     world    TEXT NOT NULL DEFAULT '',
     address  TEXT NOT NULL DEFAULT '',
     public   INTEGER NOT NULL DEFAULT 0,
     updated  INTEGER NOT NULL DEFAULT 0
   )`,
  // Личные сообщения между друзьями (Этап 2). `id` - uuid, как в остальных
  // таблицах: AUTOINCREMENT не переносится на Postgres, а последовательность
  // нужна только для ключа, сортировка идёт по паре (created, id).
  `CREATE TABLE IF NOT EXISTS messages (
     id        TEXT PRIMARY KEY,
     sender    TEXT NOT NULL,
     recipient TEXT NOT NULL,
     body      TEXT NOT NULL,
     created   INTEGER NOT NULL,
     read_at   INTEGER NOT NULL DEFAULT 0
   )`,
  `CREATE INDEX IF NOT EXISTS messages_pair ON messages(sender, recipient, created)`,
  `CREATE INDEX IF NOT EXISTS messages_inbox ON messages(recipient, read_at)`,
  // Посты в профиле (Этап 2). Видят только друзья и сам автор.
  `CREATE TABLE IF NOT EXISTS posts (
     id       TEXT PRIMARY KEY,
     author   TEXT NOT NULL,
     body     TEXT NOT NULL,
     created  INTEGER NOT NULL
   )`,
  `CREATE INDEX IF NOT EXISTS posts_author ON posts(author, created)`,
  // Соавторы карт (Этап 3). Мир - номер, как его называет игровой сервер.
  // Роли решают, кто кого позвал и кто может звать дальше; сам доступ к
  // стройке по-прежнему решает joinMode в игровом сервере, здесь его нет.
  `CREATE TABLE IF NOT EXISTS world_members (
     world    TEXT NOT NULL,
     player   TEXT NOT NULL,
     role     TEXT NOT NULL,
     created  INTEGER NOT NULL,
     PRIMARY KEY (world, player)
   )`,
  `CREATE INDEX IF NOT EXISTS world_members_player ON world_members(player)`,
];

// `?` -> `$1`, `$2`, ... for Postgres.
function toPg(sql) {
  let i = 0;
  return sql.replace(/\?/g, () => `$${++i}`);
}

function cleanParams(params) {
  return (params || []).map((p) => (p === undefined ? null : p));
}

// Добавляет колонки, появившиеся позже первой версии: handle у аккаунтов и
// public у присутствия. Уникальный индекс по handle_key не терпит пустых строк,
// а публичный список не должен показывать приватные миры, поэтому миграция
// обязательна, а не «nice to have».
async function migrate(run, all) {
  if (await addColumn(run, 'players', "handle TEXT NOT NULL DEFAULT ''")) {
    await addColumn(run, 'players', "handle_key TEXT NOT NULL DEFAULT ''");
    await backfillHandles(run, all);
  }
  // приватность по умолчанию выключена: никто не становится публичным молча
  await addColumn(run, 'presence', 'public INTEGER NOT NULL DEFAULT 0');
}

// Возвращает true, если колонки не было и её пришлось добавить.
async function addColumn(run, table, column) {
  try {
    await run(`ALTER TABLE ${table} ADD COLUMN ${column}`);
    return true;
  } catch {
    return false;  // колонка уже есть - база свежая или уже мигрированная
  }
}

async function backfillHandles(run, all) {
  const rows = await all('SELECT id, nick FROM players');
  const used = new Set();
  for (const row of rows) {
    let base = String(row.nick || '')
      .toLowerCase()
      .replace(/[^a-z0-9_]+/g, '')
      .replace(/^_+|_+$/g, '');
    if (!base) base = 'player';
    if (base.length < 3) base += 'x'.repeat(3 - base.length);
    let handle = base;
    let suffix = 2;
    while (used.has(handle)) handle = `${base}${suffix++}`;
    used.add(handle);
    await run('UPDATE players SET handle = ?, handle_key = ? WHERE id = ?', [handle, handle, row.id]);
  }
}

// Общая для обоих бэкендов последовательность: схема -> миграция -> индекс по
// @username (создаётся только после того, как колонка гарантированно есть).
async function bootstrap(run, all) {
  for (const statement of SCHEMA) await run(statement);
  await migrate(run, all);
  await run('CREATE UNIQUE INDEX IF NOT EXISTS players_handle_key ON players(handle_key)');
}

async function openDatabase(env = process.env) {
  const url = env.DATABASE_URL;

  if (url) {
    let Pool;
    try {
      ({ Pool } = require('pg'));
    } catch {
      throw new Error('DATABASE_URL is set but the "pg" package is not installed. Run: npm install');
    }
    const pool = new Pool({ connectionString: url, ssl: url.includes('localhost') ? false : { rejectUnauthorized: false } });
    const run = async (sql, params) => { await pool.query(toPg(sql), cleanParams(params)); };
    const get = async (sql, params) => {
      const r = await pool.query(toPg(sql), cleanParams(params));
      return r.rows[0];
    };
    const all = async (sql, params) => (await pool.query(toPg(sql), cleanParams(params))).rows;
    const db = {
      backend: 'postgres',
      run, get, all,
      async tx(work) {
        const client = await pool.connect();
        try {
          await client.query('BEGIN');
          const r = await work({
            run: (sql, p) => client.query(toPg(sql), cleanParams(p)),
            get: (sql, p) => client.query(toPg(sql), cleanParams(p)).then((x) => x.rows[0]),
            all: (sql, p) => client.query(toPg(sql), cleanParams(p)).then((x) => x.rows),
          });
          await client.query('COMMIT');
          return r;
        } catch (e) {
          await client.query('ROLLBACK');
          throw e;
        } finally {
          client.release();
        }
      },
      async close() { await pool.end(); },
    };
    await bootstrap(run, all);
    return db;
  }

  const { DatabaseSync } = require('node:sqlite');
  const file = env.SQLITE_FILE || path.join(process.cwd(), 'kogamaz-social.sqlite');
  if (file !== ':memory:') fs.mkdirSync(path.dirname(file), { recursive: true });
  const sqlite = new DatabaseSync(file);
  sqlite.exec('PRAGMA journal_mode = WAL');

  const run = async (sql, params) => { sqlite.prepare(sql).run(...cleanParams(params)); };
  const get = async (sql, params) => sqlite.prepare(sql).get(...cleanParams(params));
  const all = async (sql, params) => sqlite.prepare(sql).all(...cleanParams(params));

  const db = {
    backend: 'sqlite',
    file,
    run, get, all,
    async tx(work) {
      sqlite.exec('BEGIN');
      try {
        const r = await work({ run, get, all });
        sqlite.exec('COMMIT');
        return r;
      } catch (e) {
        try { sqlite.exec('ROLLBACK'); } catch { /* already rolled back */ }
        throw e;
      }
    },
    async close() { sqlite.close(); },
  };
  await bootstrap(run, all);
  return db;
}

module.exports = { openDatabase, toPg };
