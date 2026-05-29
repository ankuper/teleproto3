# Upstream Rebase Workflow — Runbook

Quick-reference for rebasing ankuper/* forks against upstream Telegram.

## Quick Start

```bash
# Dry run (preview only, no changes)
./teleproto3/ci/rebase-upstream.sh tdesktop --dry-run

# Full rebase: tdesktop
./teleproto3/ci/rebase-upstream.sh tdesktop

# Full rebase: Android
./teleproto3/ci/rebase-upstream.sh android

# Full rebase: iOS
./teleproto3/ci/rebase-upstream.sh ios

# Cleanup old backup branches
./teleproto3/ci/rebase-upstream.sh tdesktop --cleanup-backups
```

## Branch Architecture

```
upstream/{dev,master}   (official Telegram — weekly+ updates)
    │
    ▼  git rebase (our commits on top)
teleproto3-support      ← Type3-proxy patches (5-40 files per client)
    │
    ▼  git rebase (trivial — brand files rarely conflict)
t3chatm                 ← app_id, icons, default proxy, CI workflows
```

## Per-Repo Configuration

| Property | tdesktop | Android | iOS |
|----------|----------|---------|-----|
| Upstream URL | telegramdesktop/tdesktop | DrKLO/Telegram | TelegramMessenger/Telegram-iOS |
| Upstream branch | `dev` | `master` | `master` |
| Config file | `rebase.config.tdesktop` | `rebase.config.android` | `rebase.config.ios` |
| Clone dir | `/Volumes/BuildCache/tdesktop/tdesktop` | `/Volumes/BuildCache/Telegram-Android` | `/Volumes/BuildCache/Telegram-iOS` |

## What the Script Does

### Step 1: Pre-flight
- Verifies working tree is clean
- Checks commit messages for non-atomic patterns (WIP, fixup, etc.)
- Validates `/* === TYPE3-PROXY BEGIN/END === */` markers in patched files
- iOS only: verifies `DEBUG_PREFIX_MAP` is set (prevents path leaks)

### Step 2: Backup
- Creates `teleproto3-support-backup-YYYYMMDD` and `t3chatm-backup-YYYYMMDD`
- Uses `git branch -f` (overwrites same-day backups)

### Step 3: Rebase
- Fetches upstream and shows diff preview
- Rebases `teleproto3-support` onto `upstream/{dev,master}`
- If conflicts: prints structured resolution guide and exits (code 2)
- Then rebases `t3chatm` onto `teleproto3-support`

### Step 4: Post-rebase gates
- Runs `identity-audit.sh` (blocks push if real name/email leaked)
- iOS: re-checks `DEBUG_PREFIX_MAP` survived rebase
- Shows surviving patches for visual confirmation

### Step 5: Push
- `git push --force-with-lease` (fails if remote moved unexpectedly)

## Conflict Resolution

When rebase stops with conflicts:

1. Open conflicting files: `git diff --name-only --diff-filter=U`
2. Find our code between markers:
   ```c
   /* === TYPE3-PROXY BEGIN === */
   // ... our patches ...
   /* === TYPE3-PROXY END === */
   ```
3. Upstream changes are **outside** those markers — keep both
4. Resolve: `git add <file> && git rebase --continue`
5. If stuck: `git rebase --abort` (backups exist)

## Restoring from Backup

```bash
# Abort in-progress rebase
git rebase --abort

# Hard reset to backup
git checkout teleproto3-support
git reset --hard teleproto3-support-backup-20260529

git checkout t3chatm
git reset --hard t3chatm-backup-20260529
```

## Backup Retention

- `--cleanup-backups` deletes branches older than 30 days
- Always keeps minimum 2 most-recent backups per branch
- Naming convention: `{branch}-backup-{YYYYMMDD}`

## PATCHED_FILES (Conflict Surfaces)

These upstream files are modified by our patches. Upstream changes to these files **will** cause conflicts.

### tdesktop
- `Telegram/CMakeLists.txt`
- `Telegram/SourceFiles/boxes/connection_box.{cpp,h}`
- `Telegram/SourceFiles/calls/calls_call.{cpp,h}`
- `Telegram/SourceFiles/calls/calls_instance.cpp`
- `Telegram/SourceFiles/core/application.cpp`
- `Telegram/SourceFiles/core/core_settings_proxy.cpp`
- `Telegram/SourceFiles/lang/lang_keys.h`
- `Telegram/SourceFiles/main/main_account.cpp`
- `Telegram/SourceFiles/mtproto/connection_abstract.{cpp,h}`
- `Telegram/SourceFiles/mtproto/connection_tcp.cpp`
- `Telegram/SourceFiles/mtproto/mtproto_proxy_data.cpp`
- `CMakeLists.txt`

### Android
- `TMessagesProj/src/main/java/org/telegram/ui/ProxySettingsActivity.java`
- `TMessagesProj/src/main/res/values/strings.xml`

### iOS (planned)
- `submodules/MtProtoKit/Sources/MTTcpConnection.m`

## force-with-lease

We use `--force-with-lease` instead of `--force`. It refuses to push if the remote ref was updated since last fetch — protecting against overwrites from another machine.
