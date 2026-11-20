# Mimir: the Status/Admin dashboard (design, 2026-11-20)

Tom, to Lir, Louhi, Mimir and Kvasir, verbatim: "You will expose an optional
(configurable) Status/Admin dashboard, served via an API over http, written
in React. The base directory of this react build must be configurable, and
default to your own install. the http server must be SPA capable (routes
first, then all routes serve the SPA), this must also be configurable. You
will expose OpenAPI for the API. Styling and structure should be taken from
Lugh's admin interface. You will allow both a username/password and Lugh for
authentication for this."

Status: designed; not built. Built on shared pieces, as placed by the stack:
- Dannan's HTTP core: the server, static files from a configured directory,
  routes first and then the SPA. Approved by Tom as the plan's Q1, not yet
  built.
- Dannan's credential helper (item 11), for username and password.
- lugh_client's RelyingParty, for Lugh sign-in.
- The shared shell: Lugh's package `@lugh/admin-ui` (lugh/ui), settled
  2026-11-20. It holds the styles, AdminLayout, NavBar, Messages, Confirm,
  useAPI, useLoad, useAct, lib/format, and a sign-in screen with both
  methods. Mimir fetches Lugh at a tag into its build tree and depends on
  `"@lugh/admin-ui": "file:<fetched path>/ui"`; there is no registry. Stack:
  React 19, React Router 7 as a static SPA, Vite 7, TypeScript 5.9, plain
  CSS, Vitest. Lugh carves it after its P8.0. Mimir asked Lugh for generic
  pieces (a cursor-paged table, a detail panel, useEvents over SSE, a status
  badge, number tiles, a text editor area), and writes them itself if no
  one else needs them.

What follows is Mimir's own part: the settings, the API and the screens.

## Settings (YAML, through dannan::config)

```yaml
admin:
  enabled: false          # off by default: the dashboard is optional
  listen: {host: 127.0.0.1, port: 6390}   # not the RESP ports
  tls: {certificate: ..., key: ...}       # required unless on loopback
  web_root: /usr/local/share/mimir/dashboard   # default: this install's
  spa: true               # unmatched GETs serve index.html
  auth:
    users: true           # Mimir's ACL users with the +@admin category
    lugh:                 # optional: sign in with Lugh
      issuer: https://lugh.example
      audience: mimir-admin
```

Flags and `MIMIR_ADMIN_*` variables, as every setting has.

## Who may sign in

- **Username and password:** a Mimir ACL user that is enabled and may run
  `@admin` commands. The password is checked as AUTH checks it (SHA-256,
  constant time), through Dannan's credential helper. A session is a signed
  cookie with a lifetime, checked on every request. The same plaintext rule
  as AUTH (ruling 28): refused over plain HTTP except on loopback.
- **Lugh:** OIDC through lugh_client's RelyingParty. A Lugh principal is
  admitted when its token holds `mimir:admin`, the dashboard's scope, beside
  the plugin's `mimir:connect`. Principal events close a disabled principal's
  sessions, as for connections.
- What a session may change follows the ACL user's permissions (for a Lugh
  principal, the ACL bound to it). Every change goes through the same
  command path as a client's, so it is by agreement where the command is.

## The API (`/api/v1`, JSON; OpenAPI at `/api/v1/openapi.json`)

Every node serves it, for the whole cluster where the answer is
cluster-wide (no special node).

| Method and path | Answers |
|---|---|
| GET /status | this node: version, uptime, id, name, readiness (acceptors voting, caught up), memory, connections, commands a second |
| GET /nodes | MIMIR NODES: every node's id, name, client address, and each one's /status, fetched through Dannan's routes |
| GET /shards | each shard: its replica set and version, this node's acceptor state (ready, names held), last commit version |
| GET /keyspace | per database: keys, keys with an expiry, average TTL (INFO keyspace); cluster-wide, as ruling 33 has it |
| GET /keys?db=&match=&cursor=&count= | SCAN, paged; GET /keys/{db}/{key}: TYPE, TTL, MEMORY USAGE, a bounded preview |
| GET /clients | CLIENT LIST, this node; POST /clients/{id}/kill |
| GET /pubsub | channels, patterns and shard channels with their subscriber counts (PUBSUB) |
| GET /slowlog, DELETE /slowlog | SLOWLOG GET and RESET |
| GET /acl/users, GET /acl/users/{name}, PUT, DELETE | ACL LIST and GETUSER; SETUSER and DELUSER by agreement (ruling 27) |
| GET /acl/log, DELETE /acl/log | ACL LOG and RESET |
| GET /functions, POST, DELETE /functions/{library} | FUNCTION LIST; LOAD and DELETE by agreement (ruling 39) |
| GET /config, PUT /config/{name} | CONFIG GET; CONFIG SET (cluster state by agreement once that is built) |
| GET /events (server-sent events) | live counters and roster changes for the status screen |
| POST /session, DELETE /session | sign in and out with username and password; GET /session/lugh starts the OIDC flow |

Errors are JSON `{error, message}` with the HTTP status. A command's refusal
carries its Valkey words.

## Screens (structure and styling from Lugh's admin)

- **Overview:** the cluster at a glance: nodes, shards ready, keys,
  commands a second, memory, the last refusals.
- **Nodes:** each node's status and client address; one that is gone shown as
  gone, not as an error.
- **Shards:** the replica sets, which node holds what, catch-up progress.
- **Keyspace:** databases, then a key browser (scan, inspect, delete).
- **Clients:** connections, with kill.
- **Pub/Sub:** channels and subscribers.
- **Users:** ACL users, edit and delete; the ACL log.
- **Functions:** libraries, load and delete.
- **Settings:** CONFIG.
- **Account:** sign out; who is signed in and how.

## Tests

- GoogleTest for the API (each route, auth, refusals) against an in-process
  node.
- The React app's own unit and end-to-end tests as Lugh's dashboard has them
  (vitest, Playwright), run on the cluster.
- The OpenAPI document is checked against the routes in a test, so the two
  cannot drift.

## Order

1. Waits on Dannan's HTTP core, and the shared shell's placement.
2. Then: settings and auth; the read-only API (status, nodes, shards,
   keyspace, clients, pub/sub, slowlog, ACL and functions lists); the
   screens for them; the OpenAPI.
3. Then the changing routes (ACL, functions, config, kill, delete key).
