#!/usr/bin/env bash
# rebase-upstream.sh — Upstream rebase workflow for Telegram client forks.
# Usage: ./ci/rebase-upstream.sh [tdesktop|android|ios] [--cleanup-backups] [--dry-run] [--force]
#
# Executes: squash-check → backup → rebase teleproto3-support → rebase t3chatm
#           → identity-audit → push --force-with-lease
#
# Exit codes:  0 = success, 1 = pre-flight abort, 2 = conflict (paused)

set -euo pipefail

# ── Arg parsing ──────────────────────────────────────────────────────────────

REPO="${1:?Usage: rebase-upstream.sh [tdesktop|android|ios] [--cleanup-backups] [--dry-run]}"
shift
DRY_RUN=false
CLEANUP_BACKUPS=false
FORCE=false
for arg in "$@"; do
  case "$arg" in
    --dry-run)         DRY_RUN=true ;;
    --cleanup-backups) CLEANUP_BACKUPS=true ;;
    --force)           FORCE=true ;;
    *) echo "Unknown flag: $arg"; exit 1 ;;
  esac
done

# Helper: prompt user or auto-accept in --force mode
ask_continue() {
  if $FORCE; then return 0; fi
  read -rp "$1 [y/N] " cont
  [[ "$cont" =~ ^[Yy]$ ]]
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIG="$SCRIPT_DIR/rebase.config.$REPO"
if [[ ! -f "$CONFIG" ]]; then
  echo "❌ Config not found: $CONFIG"
  echo "   Available: $(ls "$SCRIPT_DIR"/rebase.config.* 2>/dev/null | xargs -I{} basename {})"
  exit 1
fi
# shellcheck source=/dev/null
source "$CONFIG"

TODAY="$(date +%Y%m%d)"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

log()  { echo -e "${GREEN}▸${NC} $*"; }
warn() { echo -e "${YELLOW}⚠${NC} $*"; }
fail() { echo -e "${RED}✖${NC} $*"; exit 1; }
info() { echo -e "${CYAN}ℹ${NC} $*"; }

# ── Helpers ──────────────────────────────────────────────────────────────────

_date_to_epoch() {
  local d="$1"
  if date --version &>/dev/null; then
    date -d "$d" +%s 2>/dev/null  # GNU
  else
    date -j -f "%Y%m%d" "$d" +%s 2>/dev/null  # BSD/macOS
  fi
}

# ── Cleanup backups mode ────────────────────────────────────────────────────

cleanup_backups() {
  log "Cleaning up backup branches older than 30 days (keeping last 2)..."
  cd "$CLONE_DIR" || fail "Cannot cd to $CLONE_DIR"

  local now; now=$(date +%s)
  local cutoff=$((now - 30 * 86400))
  local -a candidates=()

  while IFS= read -r branch; do
    branch="${branch#  }"  # strip leading spaces
    [[ "$branch" == *-backup-* ]] || continue
    local datepart="${branch##*-backup-}"
    local epoch; epoch=$(_date_to_epoch "$datepart" 2>/dev/null) || continue
    if (( epoch < cutoff )); then
      candidates+=("$branch")
    fi
  done < <(git branch)

  if (( ${#candidates[@]} == 0 )); then
    info "No stale backups found."
    return
  fi

  # Sort by date, keep last 2 per base-branch
  local -A counts=()
  for b in "${candidates[@]}"; do
    local base="${b%-backup-*}"
    counts[$base]=$(( ${counts[$base]:-0} + 1 ))
  done

  echo ""
  echo "  Candidates for deletion:"
  for b in "${candidates[@]}"; do
    echo "    $b"
  done
  echo ""

  if $DRY_RUN; then
    info "(dry-run) Would delete ${#candidates[@]} branches."
    return
  fi

  read -rp "Delete ${#candidates[@]} stale backup branches? [y/N] " confirm
  if [[ "$confirm" =~ ^[Yy]$ ]]; then
    for b in "${candidates[@]}"; do
      git branch -D "$b"
      log "Deleted: $b"
    done
  fi
}

if $CLEANUP_BACKUPS; then
  cleanup_backups
  exit 0
fi

# ── Pre-flight checks ───────────────────────────────────────────────────────

cd "$CLONE_DIR" || fail "Cannot cd to $CLONE_DIR"
log "Repo: $REPO ($CLONE_DIR)"
log "Upstream: $UPSTREAM_URL → $UPSTREAM_BRANCH"
log "Branches: $SUPPORT_BRANCH → $BRAND_BRANCH"
echo ""

# 1. Ensure upstream remote exists
if ! git remote get-url upstream &>/dev/null; then
  warn "Adding upstream remote: $UPSTREAM_URL"
  git remote add upstream "$UPSTREAM_URL"
fi

# 2. Ensure we're on the support branch
git checkout "$SUPPORT_BRANCH" || fail "Cannot checkout $SUPPORT_BRANCH"

# 3. Check for dirty working tree (ignore submodule changes)
if ! git diff --quiet --ignore-submodules || ! git diff --cached --quiet --ignore-submodules; then
  fail "Working tree is dirty. Commit or stash first."
fi

# 4. Squash cleanliness check
log "Checking commit messages for non-atomic commits..."
BAD_COMMITS=$(
  git log "upstream/$UPSTREAM_BRANCH..$SUPPORT_BRANCH" --format="%h %s" 2>/dev/null \
    | grep -Ei "^[a-f0-9]+ (wip|fix|oops|fixup|squash|temp|tmp|asdf|todo|hack)\b" || true
)
if [[ -n "$BAD_COMMITS" ]]; then
  warn "Non-atomic commits detected (consider squashing):"
  echo "$BAD_COMMITS" | sed 's/^/    /'
  echo ""
  ask_continue "Continue anyway?" || fail "Aborted. Run: git rebase -i HEAD~N to squash."
fi

# 5. TYPE3-PROXY marker check (only on files that exist)
if (( ${#PATCHED_FILES[@]} > 0 )); then
  log "Checking TYPE3-PROXY markers in patched upstream files..."
  MISSING_MARKERS=()
  for f in "${PATCHED_FILES[@]}"; do
    [[ -f "$f" ]] || continue
    if ! grep -q "TYPE3-PROXY" "$f" 2>/dev/null; then
      MISSING_MARKERS+=("$f")
    fi
  done
  if (( ${#MISSING_MARKERS[@]} > 0 )); then
    warn "Files missing TYPE3-PROXY BEGIN/END markers:"
    printf "    %s\n" "${MISSING_MARKERS[@]}"
    echo "  Add: /* === TYPE3-PROXY BEGIN === */ ... /* === TYPE3-PROXY END === */"
    ask_continue "Continue anyway?" || fail "Aborted. Add markers first."
  fi
fi

# 6. iOS: DEBUG_PREFIX_MAP check
if ${CHECK_DEBUG_PREFIX_MAP:-false}; then
  log "Checking DEBUG_PREFIX_MAP (iOS)..."
  FOUND_DPM=false
  for f in "${DEBUG_PREFIX_MAP_FILES[@]:-}"; do
    [[ -f "$f" ]] && grep -q "DEBUG_PREFIX_MAP" "$f" && FOUND_DPM=true && break
  done
  if ! $FOUND_DPM; then
    # Fallback: scan all xcconfig files
    if find . -name "*.xcconfig" -exec grep -l "DEBUG_PREFIX_MAP" {} + &>/dev/null; then
      FOUND_DPM=true
    fi
  fi
  $FOUND_DPM || warn "DEBUG_PREFIX_MAP not found! Path leak risk in compiled binary."
fi

# ── Backup ───────────────────────────────────────────────────────────────────

log "Creating backup branches..."
SUPPORT_BACKUP="${SUPPORT_BRANCH}-backup-${TODAY}"
BRAND_BACKUP="${BRAND_BRANCH}-backup-${TODAY}"

git branch -f "$SUPPORT_BACKUP" "$SUPPORT_BRANCH" || fail "Cannot create backup $SUPPORT_BACKUP"
log "  $SUPPORT_BACKUP → $(git rev-parse --short "$SUPPORT_BRANCH")"

if git rev-parse --verify "$BRAND_BRANCH" &>/dev/null; then
  git branch -f "$BRAND_BACKUP" "$BRAND_BRANCH" || fail "Cannot create backup $BRAND_BACKUP"
  log "  $BRAND_BACKUP → $(git rev-parse --short "$BRAND_BRANCH")"
fi

# ── Fetch upstream ───────────────────────────────────────────────────────────

echo ""
log "Fetching upstream (no submodule recursion)..."
git fetch upstream --no-recurse-submodules

# Pre-rebase diff summary: show upstream changes touching our patched files
CONFLICT_FILES=$(git log --oneline "upstream/$UPSTREAM_BRANCH..$SUPPORT_BRANCH" -- "${PATCHED_FILES[@]}" 2>/dev/null || true)
if [[ -n "$CONFLICT_FILES" ]]; then
  info "Our commits touching upstream-shared files:"
  echo "$CONFLICT_FILES" | sed 's/^/    /'
fi

UPSTREAM_CHANGES=$(git log --oneline "$SUPPORT_BRANCH..upstream/$UPSTREAM_BRANCH" -- "${PATCHED_FILES[@]}" 2>/dev/null || true)
if [[ -n "$UPSTREAM_CHANGES" ]]; then
  warn "Upstream commits touching our patched files (likely conflicts):"
  echo "$UPSTREAM_CHANGES" | sed 's/^/    /'
fi

echo ""
UPSTREAM_AHEAD=$(git rev-list --count "$SUPPORT_BRANCH..upstream/$UPSTREAM_BRANCH" 2>/dev/null || echo "?")
log "Upstream is $UPSTREAM_AHEAD commits ahead of $SUPPORT_BRANCH."

if [[ "$UPSTREAM_AHEAD" == "0" ]]; then
  info "Already up to date. Nothing to rebase."
  exit 0
fi

if $DRY_RUN; then
  info "(dry-run) Would rebase $SUPPORT_BRANCH onto upstream/$UPSTREAM_BRANCH ($UPSTREAM_AHEAD commits)."
  exit 0
fi

# ── Rebase teleproto3-support ────────────────────────────────────────────────

log "Rebasing $SUPPORT_BRANCH onto upstream/$UPSTREAM_BRANCH..."
if ! git rebase "upstream/$UPSTREAM_BRANCH"; then
  echo ""
  echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
  echo -e "${RED}  CONFLICT — manual resolution needed${NC}"
  echo -e "${RED}═══════════════════════════════════════════════════════════════${NC}"
  echo ""
  echo "  Conflicting files:"
  git diff --name-only --diff-filter=U | sed 's/^/    /'
  echo ""
  echo "  === TYPE3-PROXY CONFLICT RESOLUTION ==="
  echo "  1. Open conflicting file(s): git diff --name-only --diff-filter=U"
  echo "  2. Find your code between:  /* === TYPE3-PROXY BEGIN === */"
  echo "                              /* === TYPE3-PROXY END === */"
  echo "  3. Upstream changes are outside those markers."
  echo "  4. Preserve BOTH the upstream change AND your block."
  echo "  5. After resolving: git add <file> && git rebase --continue"
  echo "  6. If stuck: git rebase --abort (backups still exist)"
  echo "  ========================================"
  echo ""
  echo "  Backups: $SUPPORT_BACKUP, $BRAND_BACKUP"
  echo "  To restore: git checkout $SUPPORT_BRANCH && git reset --hard $SUPPORT_BACKUP"
  echo ""
  echo "  After resolving all conflicts, re-run this script to continue."
  exit 2
fi
log "✅ $SUPPORT_BRANCH rebased successfully."

# ── Rebase t3chatm (brand layer) ────────────────────────────────────────────

if git rev-parse --verify "$BRAND_BRANCH" &>/dev/null; then
  log "Rebasing $BRAND_BRANCH onto $SUPPORT_BRANCH..."
  git checkout "$BRAND_BRANCH"
  if ! git rebase "$SUPPORT_BRANCH"; then
    echo ""
    warn "Brand-layer conflict! This is unusual — check that no functional code was added to $BRAND_BRANCH."
    echo "  Conflicting files:"
    git diff --name-only --diff-filter=U | sed 's/^/    /'
    echo ""
    echo "  To restore: git rebase --abort && git reset --hard $BRAND_BACKUP"
    exit 2
  fi
  log "✅ $BRAND_BRANCH rebased successfully."
  git checkout "$SUPPORT_BRANCH"
fi

# ── Post-rebase gates ────────────────────────────────────────────────────────

echo ""
log "Running post-rebase gates..."

# Identity audit
AUDIT_SCRIPT="$SCRIPT_DIR/identity-audit.sh"
if [[ -x "$AUDIT_SCRIPT" ]]; then
  log "Running identity audit..."
  if ! "$AUDIT_SCRIPT" --check-range "upstream/$UPSTREAM_BRANCH..HEAD" 2>/dev/null; then
    fail "Identity audit failed! Fix commits before pushing."
  fi
  log "✅ Identity audit passed."
else
  warn "identity-audit.sh not found at $AUDIT_SCRIPT — skipping."
fi

# iOS: re-verify DEBUG_PREFIX_MAP post-rebase
if ${CHECK_DEBUG_PREFIX_MAP:-false}; then
  log "Re-checking DEBUG_PREFIX_MAP post-rebase..."
  for f in "${DEBUG_PREFIX_MAP_FILES[@]:-}"; do
    if [[ -f "$f" ]] && ! grep -q "DEBUG_PREFIX_MAP" "$f"; then
      fail "DEBUG_PREFIX_MAP lost during rebase in $f! Restore it."
    fi
  done
fi

# Patch survival check
log "Verifying patches survived rebase..."
echo "  Our commits on $SUPPORT_BRANCH:"
git log --oneline "upstream/$UPSTREAM_BRANCH..$SUPPORT_BRANCH" | sed 's/^/    /'

# ── Push ─────────────────────────────────────────────────────────────────────

echo ""
log "Pushing with --force-with-lease..."
git push --force-with-lease origin "$SUPPORT_BRANCH"
log "  $SUPPORT_BRANCH → $(git rev-parse --short "$SUPPORT_BRANCH")"

if git rev-parse --verify "$BRAND_BRANCH" &>/dev/null; then
  git push --force-with-lease origin "$BRAND_BRANCH"
  log "  $BRAND_BRANCH → $(git rev-parse --short "$BRAND_BRANCH")"
fi

# ── Done ─────────────────────────────────────────────────────────────────────

echo ""
echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
echo -e "${GREEN}  ✅ Rebase complete!${NC}"
echo -e "${GREEN}═══════════════════════════════════════════════════════════════${NC}"
echo ""
echo "  Upstream:  $UPSTREAM_BRANCH ($UPSTREAM_AHEAD commits absorbed)"
echo "  Support:   $SUPPORT_BRANCH → $(git rev-parse --short "$SUPPORT_BRANCH")"
if git rev-parse --verify "$BRAND_BRANCH" &>/dev/null; then
  echo "  Brand:     $BRAND_BRANCH → $(git rev-parse --short "$BRAND_BRANCH")"
fi
echo "  Backups:   $SUPPORT_BACKUP, $BRAND_BACKUP"
echo ""
echo "  Next: trigger CI builds and verify."
