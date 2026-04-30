# Upstream subtree log

`server/` is a `git subtree` of:
- **Active upstream:** `github.com/ankuper/teleproxy`
  (branch: `feat/websocket-transport`) — our fork carrying the Type3
  WebSocket transport (RFC 6455). This is the permanent working upstream;
  all routine subtree pulls target this branch.
- **Occasional sync source:** `github.com/teleproxy/teleproxy`
  (branch: `master`) — community-maintained continuation of the archived
  `TelegramMessenger/MTProxy`. Pull from here only when it contains
  relevant security fixes or DC-table updates not yet in our fork.

Historical note: `TelegramMessenger/MTProxy` is the archived ancestor
repo. `ankuper/teleproxy feat/websocket-transport` is our fork with
Type3 WS transport added. The upstream PR (`teleproxy/teleproxy#69`)
was closed without merge — the patches live here permanently.

## Current pin

<!-- pin-sha: a7fff27232176186c7f2942bbd84cf18211b9dda -->

Machine-parseable sentinel above holds the 40-hex-character commit SHA of
the last upstream subtree pull. Extract with:

```sh
# POSIX-portable (no PCRE -oP). ubuntu-latest has GNU grep; this works there too.
grep -oE '<!-- pin-sha: [0-9a-f]{40} -->' server/UPSTREAM.md \
  | sed 's/.*pin-sha: \([0-9a-f]\{40\}\) .*/\1/'
```

Updated automatically by `upstream-sync.yml`; manual edits OK if also
updating the workflow's expected sentinel format.

Initial value is 40 zeros — placeholder until the first real subtree pull
lands (style-guide §7 server caveat: scaffold-time placeholder dirs).

## Pull procedure

```sh
# First-time import (already executed — Story 4-1):
# git read-tree / rsync procedure — see Story 4-1 Dev Notes
# (ankuper/teleproxy feat/websocket-transport via local file:// mirror)

# Subsequent pulls (active upstream — use this for routine updates):
git subtree pull --prefix=server/ \
    git@github.com:ankuper/teleproxy.git feat/websocket-transport --squash

# Occasional sync from community upstream (security fixes, DC table):
# Only when teleproxy/teleproxy master has relevant commits not in ankuper.
# git subtree pull --prefix=server/ \
#     git@github.com:teleproxy/teleproxy.git master --squash
```

Never edit files under `server/common/`, `server/crypto/`, `server/jobs/`,
or `server/mtproto/` outside of a subtree pull — those directories are
strictly upstream.

Fork-local additions live under:
- `server/net/net-type3-dispatch.{c,h}` — dispatch hook into `../lib`.
- `server/net/net-type3-stats.{c,h}` — admin counters (bad_header_drops
  et al.).
- `server/tests/` — fork-local tests.
- `server/docs/` — fork-local docs.
- Root-level: `Makefile`, `Dockerfile`, `README.md`, `CHANGELOG.md`,
  `VERSION`.

## Pull log

Each subtree pull appends an entry here. Use the template below.

### Template

```
## YYYY-MM-DD — pulled <upstream-sha-short>

- Upstream SHA: <full sha>
- Fast-forward / three-way / rebase: <which>
- Conflicts: <none | list>
- Resolution notes: <...>
- Tests run after pull: <build + integration outcome>
```

---

## 2026-04-24 — scaffold initial

- Upstream SHA: _to be recorded on first real pull_
- Notes: `common/`, `crypto/`, `jobs/`, `mtproto/` are empty placeholder
  directories in the scaffold. The first `git subtree add` pull will
  populate them. Until then, a `make` inside `server/` will fail — this
  is expected during scaffold.

---

## 2026-04-27 — pulled a7fff272

- Upstream SHA: a7fff27232176186c7f2942bbd84cf18211b9dda
- Branch: feat/websocket-transport (local mirror: teleproxy/ workspace symlink via file:// remote teleproxy-upstream)
- Merge mode: selective `git read-tree --prefix` into empty subdirs +
  `rsync --ignore-existing` for net/ (Option (c) per story 1-8 Dev Notes)
- Conflicts: none — target dirs were empty placeholders; fork-local
  `net-type3-*.{c,h}` preserved via --ignore-existing
- Resolution notes: Skipped upstream engine/ and vv/ (not required for v0.1.0).
  Pull procedure: subtree pull --prefix=server/ with teleproxy-upstream remote.
- Tests run after pull: build pending (story 1-8 Task 2 wires the Makefile;
  pre-wire build expected to fail — by design per UPSTREAM.md lines 67–71)

---

## 2026-04-28 — pulled a7fff272 (Story 4-1 re-import)

- Upstream SHA: a7fff27232176186c7f2942bbd84cf18211b9dda
- Branch: feat/websocket-transport (local mirror: teleproxy/ workspace symlink via file:// remote teleproxy-upstream)
- Merge mode: selective `git read-tree --prefix` into empty subdirs (common/, crypto/, jobs/, mtproto/) +
  `git read-tree --prefix=net/upstream-tmp/ + rsync --ignore-existing` for net/ +
  `git show` for root-level blobs (Makefile, Dockerfile, start.sh, etc.) with --ignore-existing for fork-local files.
  Note: upstream sources are under src/ (not repo root) — all read-tree paths use `src/<dir>` prefix.
- Conflicts: none — all target dirs were empty after revert (95f413d); fork-local files
  (.env.example, docker-compose.behind-nginx.yml, UPSTREAM.md, tests/) preserved via omission.
- Resolution notes: engine/ and vv/ included (both required by Makefile and headers).
  Prior successful import 8e12b10 was reverted at 95f413d; this re-import is permanent.
  Story: 4-1 (subtree import + upstream rebase baseline).
- Tests run after pull: Docker build PASSED — full multi-stage build produces runtime image
  teleproxy:ws-4.1. Binary starts: "Invoking engine teleproxy-unknown compiled Apr 28 2026"
  confirmed. proxy-secret downloaded from core.telegram.org. AC#4 + AC#5 satisfied.
