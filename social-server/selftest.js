'use strict';

// End-to-end self check for the KoGaMaZ social server.
// Spawns the real server on a random port with an in-memory database, then
// drives the whole stage-1 flow over HTTP.
//
//   node selftest.js

const { spawn } = require('node:child_process');
const path = require('node:path');

const PORT = 20000 + Math.floor(Math.random() * 20000);
const BASE = 'http://127.0.0.1:' + PORT;

let failures = 0;
let checks = 0;

function check(label, ok, detail) {
  checks += 1;
  if (ok) {
    console.log('  ok   ' + label);
  } else {
    failures += 1;
    console.log('  FAIL ' + label + (detail === undefined ? '' : ' -> ' + JSON.stringify(detail)));
  }
}

async function call(method, route, body, token) {
  const headers = { 'Content-Type': 'application/json' };
  if (token) headers.Authorization = 'Bearer ' + token;
  const res = await fetch(BASE + route, {
    method,
    headers,
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  let parsed = {};
  try { parsed = await res.json(); } catch { /* empty body */ }
  return { status: res.status, body: parsed };
}

function waitFor(child) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error('server did not start in time')), 15000);
    let buffer = '';
    child.stdout.on('data', (chunk) => {
      buffer += chunk;
      if (buffer.includes('listening on')) {
        clearTimeout(timer);
        resolve(buffer);
      }
    });
    child.stderr.on('data', (chunk) => { buffer += chunk; });
    child.on('exit', (code) => {
      clearTimeout(timer);
      reject(new Error('server exited early, code=' + code + '\n' + buffer));
    });
  });
}

async function main() {
  const child = spawn(process.execPath, [path.join(__dirname, 'server.js')], {
    env: { ...process.env, PORT: String(PORT), SQLITE_FILE: ':memory:' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });

  let childError = '';
  child.stderr.on('data', (c) => { childError += c; });

  try {
    await waitFor(child);
  } catch (e) {
    console.error('could not start server:', e.message, childError);
    process.exit(1);
  }

  try {
    console.log('--- health ---');
    const health = await call('GET', '/api/health');
    check('health responds', health.status === 200 && health.body.ok === true, health);
    check('health reports sqlite backend', health.body.backend === 'sqlite', health.body);

    console.log('--- registration ---');
    const alice = await call('POST', '/api/register', { nick: 'Alice', handle: '@AliceDev', password: 'secret1' });
    check('register Alice', alice.status === 201 && !!alice.body.token, alice.body);
    check('handle is normalised to lowercase', alice.body.handle === 'alicedev', alice.body);
    const bob = await call('POST', '/api/register', { nick: 'Bob', handle: 'bob_plays', password: 'secret2' });
    check('register Bob', bob.status === 201 && bob.body.handle === 'bob_plays', bob.body);
    const carol = await call('POST', '/api/register', { nick: 'Carol', handle: 'carol', password: 'secret3' });
    check('register Carol', carol.status === 201, carol.body);

    const dup = await call('POST', '/api/register', { nick: 'alice', handle: 'someoneelse', password: 'secret9' });
    check('duplicate nick rejected (case-insensitive)', dup.status === 409 && dup.body.error === 'nick_taken', dup);

    const dupHandle = await call('POST', '/api/register', { nick: 'AnotherNick', handle: 'ALICEDEV', password: 'secret9' });
    check('duplicate handle rejected (case-insensitive)', dupHandle.status === 409 && dupHandle.body.error === 'handle_taken', dupHandle);

    const badHandle = await call('POST', '/api/register', { nick: 'DaveOk', handle: 'x!', password: 'secret9' });
    check('handle with symbols rejected', badHandle.status === 400 && badHandle.body.error === 'bad_handle', badHandle);

    const shortHandle = await call('POST', '/api/register', { nick: 'DaveOk', handle: 'ab', password: 'secret9' });
    check('too-short handle rejected', shortHandle.status === 400 && shortHandle.body.error === 'bad_handle', shortHandle);

    const shortNick = await call('POST', '/api/register', { nick: 'ab', handle: 'goodhandle', password: 'secret9' });
    check('too-short nick rejected', shortNick.status === 400, shortNick);

    const shortPass = await call('POST', '/api/register', { nick: 'DaveOk', handle: 'daveok', password: 'abc' });
    check('too-short password rejected', shortPass.status === 400, shortPass);

    console.log('--- login ---');
    const login = await call('POST', '/api/login', { nick: 'alice', password: 'secret1' });
    check('login is case-insensitive and returns a token', login.status === 200 && !!login.body.token, login.body);
    check('login returns the handle too', login.body.handle === 'alicedev', login.body);
    const loginByHandle = await call('POST', '/api/login', { nick: '@Bob_Plays', password: 'secret2' });
    check('login works by @username', loginByHandle.status === 200 && loginByHandle.body.id === bob.body.id, loginByHandle.body);
    const badLogin = await call('POST', '/api/login', { nick: 'Alice', password: 'wrongpass' });
    check('wrong password rejected', badLogin.status === 401, badLogin);

    const me = await call('GET', '/api/me', undefined, alice.body.token);
    check('me returns the account', me.status === 200 && me.body.nick === 'Alice', me.body);
    check('me returns the handle', me.body.handle === 'alicedev', me.body);

    const noAuth = await call('GET', '/api/friends');
    check('friends requires a token', noAuth.status === 401, noAuth);

    console.log('--- search by @username ---');
    const selfSearch = await call('GET', '/api/search?nick=ali', undefined, alice.body.token);
    const selfHit = (selfSearch.body.results || []).find((r) => r.id === alice.body.id);
    check('found self with relation=self', !!selfHit && selfHit.relation === 'self', selfSearch.body);
    check('search result carries the handle', selfHit && selfHit.handle === 'alicedev', selfSearch.body);

    const search = await call('GET', '/api/search?nick=bob', undefined, alice.body.token);
    const bobHit = (search.body.results || []).find((r) => r.id === bob.body.id);
    check('found Bob by @username prefix', !!bobHit && bobHit.relation === 'none', search.body);
    check('Bob found via @username shows his handle', bobHit && bobHit.handle === 'bob_plays', search.body);

    const exactSearch = await call('GET', '/api/search?nick=@Bob_Plays', undefined, alice.body.token);
    check('exact @username match comes back', exactSearch.body.results.length === 1
      && exactSearch.body.results[0].id === bob.body.id, exactSearch.body);

    const nickSearch = await call('GET', '/api/search?nick=caro', undefined, alice.body.token);
    const carolHit = (nickSearch.body.results || []).find((r) => r.id === carol.body.id);
    check('search still finds people by nickname', !!carolHit, nickSearch.body);

    const noTerms = await call('GET', '/api/search?nick=', undefined, alice.body.token);
    check('empty query returns nothing', (noTerms.body.results || []).length === 0, noTerms.body);

    console.log('--- friend requests ---');
    const sent = await call('POST', '/api/requests', { targetId: bob.body.id }, alice.body.token);
    check('Alice -> Bob request created', sent.status === 201, sent.body);

    const again = await call('POST', '/api/requests', { targetId: bob.body.id }, alice.body.token);
    check('duplicate request rejected', again.status === 409, again);

    const toSelf = await call('POST', '/api/requests', { targetId: alice.body.id }, alice.body.token);
    check('self request rejected', toSelf.status === 400, toSelf);

    const bobRequests = await call('GET', '/api/requests', undefined, bob.body.token);
    check('Bob sees the incoming request',
      bobRequests.status === 200 && bobRequests.body.incoming.length === 1
      && bobRequests.body.incoming[0].nick === 'Alice', bobRequests.body);

    const aliceRequests = await call('GET', '/api/requests', undefined, alice.body.token);
    check('Alice sees her outgoing request', aliceRequests.body.outgoing.length === 1, aliceRequests.body);

    const bobSearch = await call('GET', '/api/search?nick=alice', undefined, bob.body.token);
    const relationAfter = (bobSearch.body.results || []).find((r) => r.id === alice.body.id);
    check('search shows relation=incoming for Bob', relationAfter && relationAfter.relation === 'incoming', bobSearch.body);

    console.log('--- reject ---');
    const rejected = await call('POST', '/api/requests/' + bobRequests.body.incoming[0].id + '/reject',
      undefined, bob.body.token);
    check('reject accepted', rejected.status === 200, rejected.body);
    const afterReject = await call('GET', '/api/friends', undefined, bob.body.token);
    check('no friendship after reject', afterReject.body.friends.length === 0, afterReject.body);
    const afterRejectAlice = await call('GET', '/api/requests', undefined, alice.body.token);
    check('outgoing request gone after reject', afterRejectAlice.body.outgoing.length === 0, afterRejectAlice.body);

    console.log('--- accept ---');
    await call('POST', '/api/requests', { targetId: bob.body.id }, alice.body.token);
    const pending = await call('GET', '/api/requests', undefined, bob.body.token);
    const toAccept = pending.body.incoming[0] && pending.body.incoming[0].id;
    const accepted = await call('POST', '/api/requests/' + toAccept + '/accept', undefined, bob.body.token);
    check('accept accepted', accepted.status === 200, accepted.body);

    const friendsAlice = await call('GET', '/api/friends', undefined, alice.body.token);
    check('Alice now has Bob as a friend',
      friendsAlice.body.friends.length === 1 && friendsAlice.body.friends[0].nick === 'Bob', friendsAlice.body);
    check('friend entry carries the @username', friendsAlice.body.friends[0].handle === 'bob_plays', friendsAlice.body);
    check('friend starts offline', friendsAlice.body.friends[0].online === false, friendsAlice.body.friends);
    const friendsBob = await call('GET', '/api/friends', undefined, bob.body.token);
    check('friendship is visible from both sides', friendsBob.body.friends.length === 1, friendsBob.body);

    const consumed = await call('GET', '/api/requests', undefined, bob.body.token);
    check('request cleared from both queues',
      consumed.body.incoming.length === 0 && consumed.body.outgoing.length === 0, consumed.body);

    const nowFriend = await call('GET', '/api/search?nick=bob', undefined, alice.body.token);
    const friendHit = (nowFriend.body.results || []).find((r) => r.id === bob.body.id);
    check('search shows relation=friend', friendHit && friendHit.relation === 'friend', nowFriend.body);

    console.log('--- presence ---');
    const presence = await call('POST', '/api/presence',
      { world: 'De_Dust2 CSGO', address: 'p2p:ABCDEF' }, bob.body.token);
    check('presence heartbeat accepted', presence.status === 200, presence.body);

    const online = await call('GET', '/api/friends', undefined, alice.body.token);
    const friend = online.body.friends[0];
    check('friend now shows online', friend.online === true, online.body);
    check('friend shows what they play', friend.world === 'De_Dust2 CSGO', online.body);
    check('friend exposes a joinable address', friend.address === 'p2p:ABCDEF', online.body);

    console.log('--- public hosting list ---');
    // Публичность - явный выбор игрока: по умолчанию приватный хостинг не должен
    // светиться в общем списке, иначе незнакомцы заходят без спроса.
    const hostingBefore = await call('GET', '/api/hosting', undefined, alice.body.token);
    check('hosting list is readable', hostingBefore.status === 200, hostingBefore.status);
    check('private world is not listed', (hostingBefore.body.hosting || []).length === 0,
      hostingBefore.body);

    const wentPublic = await call('POST', '/api/presence',
      { world: 'De_Dust2 CSGO', address: 'p2p:ABCDEF', public: true }, bob.body.token);
    check('presence acknowledges the public flag', wentPublic.body.public === true, wentPublic.body);

    const hostingAfter = await call('GET', '/api/hosting', undefined, alice.body.token);
    const hosted = hostingAfter.body.hosting && hostingAfter.body.hosting[0];
    check('public world appears in the hosting list', !!hosted, hostingAfter.body);
    check('hosting entry names the host', hosted && hosted.nick === 'Bob', hosted);
    check('hosting entry carries the @username', hosted && hosted.handle === 'bob_plays', hosted);
    check('hosting entry carries the world title', hosted && hosted.title === 'De_Dust2 CSGO', hosted);
    check('hosting entry carries a joinable address', hosted && hosted.address === 'p2p:ABCDEF', hosted);

    // Список должен показывать только публичных: Карол не хостит вовсе.
    const others = (hostingAfter.body.hosting || []).filter((h) => h.nick !== 'Bob');
    check('nobody else is listed', others.length === 0, hostingAfter.body);

    const wentPrivate = await call('POST', '/api/presence',
      { world: 'De_Dust2 CSGO', address: 'p2p:ABCDEF', public: false }, bob.body.token);
    check('presence acknowledges going private', wentPrivate.body.public === false, wentPrivate.body);
    const hostingHidden = await call('GET', '/api/hosting', undefined, alice.body.token);
    check('going private hides the world again', (hostingHidden.body.hosting || []).length === 0,
      hostingHidden.body);

    // Вход в список требует аккаунта: без токена список не читается.
    const hostingAnon = await call('GET', '/api/hosting');
    check('hosting list requires a token', hostingAnon.status === 401, hostingAnon.status);

    console.log('--- cancel an outgoing request ---');
    const cancelSent = await call('POST', '/api/requests', { targetId: carol.body.id }, alice.body.token);
    check('request to Carol created', cancelSent.status === 201, cancelSent.body);
    const before = await call('GET', '/api/requests', undefined, alice.body.token);
    const cancelId = before.body.outgoing[0] && before.body.outgoing[0].id;
    const cancelled = await call('POST', '/api/requests/' + cancelId + '/cancel', undefined, alice.body.token);
    check('cancel accepted', cancelled.status === 200, cancelled.body);
    const after = await call('GET', '/api/requests', undefined, alice.body.token);
    check('outgoing queue empty after cancel', after.body.outgoing.length === 0, after.body);
    const carolSeen = await call('GET', '/api/requests', undefined, carol.body.token);
    check('Carol never saw the cancelled request', carolSeen.body.incoming.length === 0, carolSeen.body);

    console.log('--- chat between friends ---');
    // Алиса и Боб - друзья (см. выше), Карол - нет. Переписка без дружбы
    // была бы каналом спама на незнакомых людей.
    const denied = await call('POST', '/api/messages',
      { peer: carol.body.id, body: 'hi carol' }, alice.body.token);
    check('messaging a non-friend is refused', denied.status === 403, denied.body);
    const deniedRead = await call('POST', '/api/messages/read', { peer: carol.body.id }, alice.body.token);
    check('reading a non-friend is refused', deniedRead.status === 403, deniedRead.body);
    const deniedLog = await call('GET', '/api/messages?peer=' + carol.body.id, undefined, alice.body.token);
    check('conversation with a non-friend is closed', deniedLog.status === 403, deniedLog.status);

    const empty = await call('POST', '/api/messages', { peer: bob.body.id, body: '   ' }, alice.body.token);
    check('blank message is refused', empty.status === 400, empty.body);

    const delivered = await call('POST', '/api/messages',
      { peer: bob.body.id, body: 'Привет, Боб!\r\nИзучаем наш лаунчер.' }, alice.body.token);
    check('message to a friend accepted', delivered.status === 201, delivered.body);
    check('stored message keeps its text',
      delivered.body.body === 'Привет, Боб!\nИзучаем наш лаунчер.', delivered.body);
    check('sender knows the message is theirs', delivered.body.mine === true, delivered.body);

    const tooLong = await call('POST', '/api/messages',
      { peer: bob.body.id, body: 'x'.repeat(5000) }, alice.body.token);
    check('oversized message is cut, not rejected',
      tooLong.status === 201 && tooLong.body.body.length === 1000, tooLong.body && tooLong.body.body.length);

    // Обе стороны видят одну переписку, но помечают своё как своё. В истории
    // уже два сообщения от Алисы: обычное и обрезанное сверхдлинное.
    const bobInbox = await call('GET', '/api/messages?peer=' + alice.body.id, undefined, bob.body.token);
    check('recipient sees both messages', bobInbox.body.messages.length === 2, bobInbox.body);
    check('recipient sees them as not theirs', bobInbox.body.messages.every((m) => m.mine === false), bobInbox.body);
    const unreadBefore = await call('GET', '/api/unread', undefined, bob.body.token);
    check('unread count reports the peer',
      unreadBefore.body.unread.some((u) => u.player === alice.body.id && u.count === 2), unreadBefore.body);

    const marked = await call('POST', '/api/messages/read', { peer: alice.body.id }, bob.body.token);
    check('marking read accepted', marked.status === 200, marked.body);
    const unreadAfter = await call('GET', '/api/unread', undefined, bob.body.token);
    check('unread clears for that peer only',
      !unreadAfter.body.unread.some((u) => u.player === alice.body.id), unreadAfter.body);
    const bobRead = await call('GET', '/api/messages?peer=' + alice.body.id, undefined, bob.body.token);
    check('history survives marking read', bobRead.body.messages.length === 2, bobRead.body);

    // Ответ приходит после прочтения и идёт новым, не подменяя старые.
    const reply = await call('POST', '/api/messages',
      { peer: alice.body.id, body: 'Привет! Вижу сообщение.' }, bob.body.token);
    check('reply accepted', reply.status === 201, reply.body);
    const aliceLog = await call('GET', '/api/messages?peer=' + bob.body.id, undefined, alice.body.token);
    // старые-к-новым: два своих (обычное и обрезанное), затем чужой ответ
    check('history is ordered oldest to newest',
      aliceLog.body.messages.map((m) => m.mine).join(',') === 'true,true,false', aliceLog.body);
    const aliceReplySeen = aliceLog.body.messages[2];
    check('incoming reply is marked unread', aliceReplySeen.read === false, aliceReplySeen);

    // Постраничная навигация: курсор по created не дублирует и не теряет шаги.
    const paged = await call('GET',
      '/api/messages?peer=' + alice.body.id + '&before=' + aliceLog.body.messages[1].created,
      undefined, bob.body.token);
    check('paging by cursor returns older messages only',
      paged.body.messages.every((m) => m.created <= aliceLog.body.messages[1].created), paged.body);
    check('paging does not repeat the boundary',
      !paged.body.messages.some((m) => m.id === aliceLog.body.messages[2].id), paged.body);

    const anon = await call('GET', '/api/messages?peer=' + bob.body.id);
    check('chat requires a token', anon.status === 401, anon.status);

    console.log('--- posts on the profile ---');
    const noFriends = await call('GET', '/api/posts?player=' + carol.body.id, undefined, bob.body.token);
    check('a stranger cannot read Carol\'s posts', noFriends.status === 403, noFriends.status);

    const blankPost = await call('POST', '/api/posts', { body: '  ' }, alice.body.token);
    check('blank post is refused', blankPost.status === 400, blankPost.body);
    const posted = await call('POST', '/api/posts', { body: 'Первый пост в KoGaMaZ' }, alice.body.token);
    check('post accepted', posted.status === 201, posted.body);

    const byFriend = await call('GET', '/api/posts?player=' + alice.body.id, undefined, bob.body.token);
    check('a friend can read the posts', byFriend.status === 200, byFriend.body);
    check('post body is preserved', byFriend.body.posts[0] && byFriend.body.posts[0].body === 'Первый пост в KoGaMaZ',
      byFriend.body);
    check('posts carry the author handle', byFriend.body.handle === 'alicedev', byFriend.body);
    const bySelf = await call('GET', '/api/posts', undefined, alice.body.token);
    check('own posts are readable without a player id', bySelf.body.posts.length === 1, bySelf.body);

    await call('POST', '/api/posts', { body: 'Второй пост' }, alice.body.token);
    const two = await call('GET', '/api/posts', undefined, alice.body.token);
    check('posts are ordered newest first', two.body.posts[0].body === 'Второй пост', two.body);

    const otherDelete = await call('DELETE', '/api/posts/' + two.body.posts[0].id, undefined, bob.body.token);
    check('a stranger cannot delete the post', otherDelete.status === 200, otherDelete);
    check('the post is still there', two.body.posts.length === 2, two.body);
    const ownDelete = await call('DELETE', '/api/posts/' + two.body.posts[0].id, undefined, alice.body.token);
    check('the author can delete the post', ownDelete.status === 200, ownDelete);
    const one = await call('GET', '/api/posts', undefined, alice.body.token);
    check('only one post remains', one.body.posts.length === 1, one.body);

    console.log('--- world collaborators ---');
    // Пустой мир: список отдаётся, но owner ещё не заведён - его заводит
    // первый позвавший, а пустой id владельца сломал бы приглашение.
    const emptyWorld = await call('GET', '/api/worlds/101/members', undefined, alice.body.token);
    check('empty world lists no members', emptyWorld.status === 200 && emptyWorld.body.members.length === 0,
      emptyWorld.body);
    check('empty world has no owner yet', emptyWorld.body.owner === null, emptyWorld.body);

    const noTokenInvite = await call('POST', '/api/worlds/101/members', { player: bob.body.id, role: 'member' });
    check('inviting without a token is refused', noTokenInvite.status === 401, noTokenInvite.body);
    const noTokenList = await call('GET', '/api/worlds/101/members');
    check('the member list needs a token too', noTokenList.status === 401, noTokenList.body);

    // Первый, кто позвал, сам становится owner: лаунчер шлёт запрос с машины
    // хоста, то есть со стороны человека, которому карта и принадлежит.
    const inviteBob = await call('POST', '/api/worlds/101/members',
      { player: bob.body.id, role: 'co-owner' }, alice.body.token);
    check('first caller becomes the owner', inviteBob.status === 200, inviteBob.body);

    const ownerList = await call('GET', '/api/worlds/101/members', undefined, alice.body.token);
    check('the owner is named', ownerList.body.owner && ownerList.body.owner.player === alice.body.id,
      ownerList.body);
    check('both members are listed', ownerList.body.members.length === 2, ownerList.body);
    check('my own role is visible', ownerList.body.myRole === 'owner', ownerList.body.myRole);
    check('self is flagged', ownerList.body.members.some((m) => m.self), ownerList.body);

    // co-owner приглашает сам - ради этого роль и существует.
    const inviteCarol = await call('POST', '/api/worlds/101/members',
      { player: carol.body.id, role: 'tester' }, bob.body.token);
    check('a co-owner can invite', inviteCarol.status === 200, inviteCarol.body);

    // Ниже стоящий приглашать не может, иначе member сам себя повысит.
    const testerInvites = await call('POST', '/api/worlds/101/members',
      { player: alice.body.id, role: 'member' }, carol.body.token);
    check('a tester cannot invite', testerInvites.status === 403 && testerInvites.body.error === 'forbidden',
      testerInvites.body);

    // Валидация входа, а не прав.
    const badRole = await call('POST', '/api/worlds/101/members',
      { player: carol.body.id, role: 'admin' }, alice.body.token);
    check('an unknown role is refused', badRole.status === 400 && badRole.body.error === 'bad_role', badRole.body);
    const unknownTarget = await call('POST', '/api/worlds/101/members',
      { player: 'nobody-here', role: 'member' }, alice.body.token);
    check('an unknown player is refused', unknownTarget.status === 404
      && unknownTarget.body.error === 'unknown_player', unknownTarget.body);
    const selfInvite = await call('POST', '/api/worlds/101/members',
      { player: alice.body.id, role: 'member' }, alice.body.token);
    check('inviting yourself is refused', selfInvite.status === 400, selfInvite.body);

    // Права на удаление.
    const dropOwner = await call('DELETE', '/api/worlds/101/members/' + alice.body.id, undefined, bob.body.token);
    check('the owner cannot be removed', dropOwner.status === 403 && dropOwner.body.error === 'owner_stays',
      dropOwner.body);
    const dropByTester = await call('DELETE', '/api/worlds/101/members/' + bob.body.id, undefined, carol.body.token);
    check('a tester cannot remove anyone', dropByTester.status === 403, dropByTester.body);
    const dropMissing = await call('DELETE', '/api/worlds/101/members/nobody-here', undefined, alice.body.token);
    check('removing a non-member is 404', dropMissing.status === 404
      && dropMissing.body.error === 'not_a_member', dropMissing.body);
    const dropByCoOwner = await call('DELETE', '/api/worlds/101/members/' + carol.body.id, undefined, bob.body.token);
    check('a co-owner can remove a member', dropByCoOwner.status === 200, dropByCoOwner.body);

    // Повторное приглашение меняет роль, а не плодит вторую запись.
    const demote = await call('POST', '/api/worlds/101/members',
      { player: bob.body.id, role: 'member' }, alice.body.token);
    check('re-inviting changes the role', demote.status === 200, demote.body);
    const afterDemote = await call('GET', '/api/worlds/101/members', undefined, alice.body.token);
    const bobRow = afterDemote.body.members.find((m) => m.player === bob.body.id);
    check('the role really changed', bobRow && bobRow.role === 'member', afterDemote.body);
    check('no duplicate row appeared', afterDemote.body.members.length === 2, afterDemote.body);

    // Каждый может выйти сам - но owner выйти не может, он и есть автор карты.
    await call('POST', '/api/worlds/101/members', { player: bob.body.id, role: 'co-owner' }, alice.body.token);
    await call('POST', '/api/worlds/101/members', { player: carol.body.id, role: 'member' }, alice.body.token);
    const leave = await call('DELETE', '/api/worlds/101/members/' + carol.body.id, undefined, carol.body.token);
    check('a member can leave on their own', leave.status === 200, leave.body);
    const leaveOwner = await call('DELETE', '/api/worlds/101/members/' + alice.body.id, undefined, alice.body.token);
    check('the owner cannot leave', leaveOwner.status === 403, leaveOwner.body);

    // co-owner равен co-owner по правам: снимать такого может только автор.
    await call('POST', '/api/worlds/101/members', { player: carol.body.id, role: 'co-owner' }, alice.body.token);
    const peerDrop = await call('DELETE', '/api/worlds/101/members/' + carol.body.id, undefined, bob.body.token);
    check('a co-owner cannot remove a peer', peerDrop.status === 403 && peerDrop.body.error === 'needs_owner',
      peerDrop.body);
    const ownerDrop = await call('DELETE', '/api/worlds/101/members/' + carol.body.id, undefined, alice.body.token);
    check('the owner can remove a co-owner', ownerDrop.status === 200, ownerDrop.body);

    // Мир - только номер: мусорный id не должен попасть в таблицу вообще.
    const junkWorld = await call('GET', '/api/worlds/notanumber/members', undefined, alice.body.token);
    check('a non-numeric world id is not a route', junkWorld.status === 404, junkWorld.body);

    console.log('--- unknown endpoints ---');
    const missing = await call('GET', '/api/nope');
    check('unknown route returns 404', missing.status === 404, missing);
  } finally {
    child.kill();
  }

  console.log('');
  console.log(failures === 0
    ? 'SELFTEST PASSED: ' + checks + ' checks, 0 failures'
    : 'SELFTEST FAILED: ' + checks + ' checks, ' + failures + ' failures');
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error('selftest crashed:', e);
  process.exit(1);
});
