# KoGaMaZ social server

Backend for launcher social features: profiles with an `@username`, nick
search, friend requests, online status, "what a friend is playing", joining
them, private chat between friends and posts on the profile.

One folder, two stages sharing the same account/token plumbing. Stage 1 is
accounts, friends and presence. Stage 2 adds chat and posts.

## Endpoints

### Stage 1: accounts, friends, presence

| Method | Route | Body / query | Notes |
| --- | --- | --- | --- |
| GET | `/api/health` | – | liveness, used by Render; reports `version` |
| POST | `/api/register` | `{nick, handle, password}` | `201 {token, id, nick, handle}` |
| POST | `/api/login` | `{nick, password}` | `200 {token, id, nick, handle}`; `nick` may be the `@username` |
| POST | `/api/logout` | – | drops the bearer token |
| GET | `/api/me` | – | current account |
| GET | `/api/search` | `?nick=ali` | exact `@username` first, then prefix, then nick substring |
| GET | `/api/friends` | – | `{friends:[{id, nick, handle, online, world, address}]}` |
| GET | `/api/requests` | – | `{incoming, outgoing}` |
| POST | `/api/requests` | `{targetId}` | send a friend request |
| POST | `/api/requests/:id/accept` | – | creates the friendship |
| POST | `/api/requests/:id/reject` | – | drops the incoming request |
| POST | `/api/requests/:id/cancel` | – | drops your own outgoing request |
| POST | `/api/presence` | `{world, address, public}` | heartbeat, every ~30 s; `public` opts into the join list |
| GET | `/api/hosting` | – | `{hosting:[{player, nick, handle, title, address}]}` - only `public` hosts |

### Stage 2: chat and posts

| Method | Route | Body / query | Notes |
| --- | --- | --- | --- |
| GET | `/api/messages` | `?peer=&before=&limit=` | tail of the chat, oldest to newest; `before` is a `created` cursor |
| POST | `/api/messages` | `{peer, body}` | `201 {ok, id, body, created}`; friends only |
| POST | `/api/messages/read` | `{peer}` | clears the unread counter for that peer |
| GET | `/api/unread` | – | `{unread:[{player, count}]}` |
| GET | `/api/posts` | `?player=&before=` | own posts, or a friend's with `player`; newest first |
| POST | `/api/posts` | `{body}` | `201 {ok, id, body}` |
| DELETE | `/api/posts/:id` | – | removes your own post |

Auth: `Authorization: Bearer <token>`.
`relation` is one of `self`, `friend`, `outgoing`, `incoming`, `none`.

Limits: a message is at most 1000 characters, a post 500, a page holds 50
rows, and one account may send at most 30 messages a minute.

Chat and posts are closed to strangers: starting or reading a chat, and
reading posts, return `403 not_friends` unless the two accounts are friends.
Reading your own posts needs no `player`. A delete of someone else's post is
accepted but changes nothing - a stranger cannot tell "deleted" from "not
yours", and cannot enumerate posts that way.

A player is considered online when their last presence heartbeat is younger
than 90 seconds, so a crashed or force-closed launcher drops to "offline" by
itself - no explicit logout needed.

The `handle` is your `@username`: 3-20 characters of `a-z`, `0-9` and `_`,
stored lowercased, with a leading `@` stripped. It is unique per account, so
friends can find each other even when display names collide. The display nick
can be changed later; the handle stays.

## Run locally

Requires Node **22.5+** (uses the built-in `node:sqlite`, so there is nothing
to compile).

```
node server.js                 # SQLite file in the current directory
node selftest.js               # 95 end-to-end checks against a spawned server
```

Local URL is then `http://127.0.0.1:8080`.

## Why a database URL matters

On a free app host the filesystem is **ephemeral**: a SQLite file is deleted on
every redeploy and on every cold start. Friends lists and accounts would vanish.
So in production set `DATABASE_URL` to a real Postgres - the code talks to both
through the same SQL.

| Hosting | Database | Persistent? |
| --- | --- | --- |
| Local / LAN / port-forward | SQLite (default) | yes, on your disk |
| Render free web service | Neon Postgres (free, no card) | yes |
| Railway | Railway Postgres or Neon | yes |

The only npm dependency is `pg`, loaded only when `DATABASE_URL` is set.

## Deploy to Render

1. Put this folder into a GitHub repository (Render builds from git).
2. Create a free Postgres at neon.tech and copy the connection string
   (looks like `postgres://user:pass@host/db?sslmode=require`).
3. Render -> **New -> Blueprint** -> pick the repo. It reads `render.yaml`.
4. In the created service, set `DATABASE_URL` to that connection string.
5. Deploy. The service URL - for example
   `https://kogamaz-social.onrender.com` - goes into the launcher settings.

Free Render services sleep after ~15 minutes without traffic, so the first
request after idle takes 30-60 seconds. The launcher retries on its own.

### Railway instead

```
npm install -g @railway/cli
railway login
railway init
railway up
railway add --database postgres
railway variables set DATABASE_URL=...   # printed by the previous command
```

Railway does not sleep the service, but its free trial runs out - then either
switch back to Render or pay.

## Launcher configuration

The launcher reads the base URL from `settings.ini`:

```
[social]
url=https://kogamaz-social.onrender.com
```

Empty value means "social features are off".
