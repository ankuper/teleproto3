# Upstream subtree log

`server/` is a `git subtree` of:
- **Primary (active upstream):** `github.com/teleproxy/teleproxy`
  (branch: `master`) — the community-maintained continuation of the
  original `TelegramMessenger/MTProxy` (which is archived upstream).
- **Intermediate (fork with WS transport):** `github.com/ankuper/teleproxy`
  (branch: `feat/websocket-transport`) — carries the Type3 v1
  WebSocket transport patches (proposal `teleproxy/teleproxy#69`) that
  landed before monorepo consolidation.

Historical note: `TelegramMessenger/MTProxy` is the archived ancestor
repo. `teleproxy/teleproxy` picked up maintenance after Telegram
stopped updating the official repo. This file tracks the ACTIVE
upstream only.

## Current pin

<!-- pin-sha: 0000000000000000000000000000000000000000 -->

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
# First-time import (already executed during scaffold):
# git subtree add --prefix=server/ \
#     git@github.com:teleproxy/teleproxy.git master --squash

# Subsequent pulls (active upstream):
git subtree pull --prefix=server/ \
    git@github.com:teleproxy/teleproxy.git master --squash

# Intermediate (only during the initial consolidation, to fold in the
# fork-local WebSocket transport patches — not a recurring operation):
# git subtree pull --prefix=server/ \
#     git@github.com:ankuper/teleproxy.git feat/websocket-transport --squash
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
- Branch: feat/websocket-transport (WebSocket transport patches for Type3 protocol,
  local path: /Volumes/BuildCache/teleproxy via file:// remote teleproxy-upstream)
- Merge mode: selective `git read-tree --prefix` into empty subdirs +
  `rsync --ignore-existing` for net/ (Option (c) per story 1-8 Dev Notes)
- Conflicts: none — target dirs were empty placeholders; fork-local
  `net-type3-*.{c,h}` preserved via --ignore-existing
- Resolution notes: Skipped upstream engine/ and vv/ (not required for v0.1.0).
  Pull procedure: subtree pull --prefix=server/ with teleproxy-upstream remote.
- Tests run after pull: build pending (story 1-8 Task 2 wires the Makefile;
  pre-wire build expected to fail — by design per UPSTREAM.md lines 67–71)
