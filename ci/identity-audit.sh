#!/usr/bin/env bash
# identity-audit.sh — AR-G5 operational-identity audit gate.
#
# This is the reference implementation of the Type3 protocol.
# Normative behaviour is defined in spec/. Where they differ, spec/ wins.
#
# Stability: enforced on every public ankuper-repo PR (see architecture.md §A-010).
# Source: Story 7-11. Exit codes: 0=clean, 1=match-found, 2=setup-error.
# NOTE: set -x (xtrace) is NEVER enabled — it would expose token values via expansion.

set -euo pipefail

# ── Locale ─────────────────────────────────────────────────────────────────────
# Force UTF-8 unconditionally so multi-byte denylist patterns (Cyrillic, accented
# Latin) match correctly via grep character classes. Under LC_CTYPE=C, grep
# treats `[АA]` as raw bytes, silently dropping Cyrillic matches — a fail-open
# for the audit gate. We override even when LC_ALL is already set to C/POSIX,
# because the gate must be deterministic regardless of inherited environment.
export LC_ALL=C.UTF-8
export LC_CTYPE=C.UTF-8

# ── Exit-code constants ────────────────────────────────────────────────────────
readonly EXIT_CLEAN=0
readonly EXIT_MATCH=1
readonly EXIT_SETUP=2

# ── Error trap for unexpected failures ─────────────────────────────────────────
_err_handler() {
    local lineno="$1"
    printf 'setup-error: unexpected failure at line %s\n' "$lineno" >&2
    exit "$EXIT_SETUP"
}
trap '_err_handler "$LINENO"' ERR

# ── Temporary workspace ────────────────────────────────────────────────────────
_tmpdir=$(mktemp -d)
trap 'rm -rf "$_tmpdir"; exit' INT TERM
trap 'rm -rf "$_tmpdir"' EXIT

readonly _COMMIT_STREAM="$_tmpdir/stream-commit-metadata"
readonly _DIFF_STREAM="$_tmpdir/stream-diff"
readonly _MATCH_LOG="$_tmpdir/matches"
touch "$_MATCH_LOG"

# ── YAML-light parser ──────────────────────────────────────────────────────────
# Parses the controlled token-list YAML format.
# Outputs one record per rule:
#   Denylist:  D<TAB>id<TAB>pattern<TAB>scope
#   Allowlist: A<TAB>id<TAB>pattern<TAB>test_fp_example<TAB>test_denylist_token_example
_parse_token_list() {
    local file="$1"
    awk '
    function strip(val,    n) {
        gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
        n = length(val)
        if (n >= 2 && substr(val,1,1) == "\x27" && substr(val,n,1) == "\x27")
            val = substr(val, 2, n-2)
        return val
    }
    function field_val(line, key,    val) {
        val = line
        sub("^[[:space:]]*" key "[[:space:]]*:[[:space:]]*", "", val)
        return strip(val)
    }
    function flush_rule(    rec) {
        if (id == "" || pat == "") { id=""; pat=""; scope=""; fp=""; tde=""; return }
        if (section == "denylist")  rec = "D\t" id "\t" pat "\t" scope
        else if (section == "allowlist") rec = "A\t" id "\t" pat "\t" fp "\t" tde
        else { id=""; pat=""; scope=""; fp=""; tde=""; return }
        print rec
        id=""; pat=""; scope=""; fp=""; tde=""
    }
    BEGIN { section=""; id=""; pat=""; scope=""; fp=""; tde="" }
    /^---$/ { next }
    /^denylist:/  { flush_rule(); section="denylist";  next }
    /^allowlist:/ { flush_rule(); section="allowlist"; next }
    /^[a-zA-Z]/ && !/^denylist:/ && !/^allowlist:/ { flush_rule(); section=""; next }
    section == "" { next }
    /^  - id:/ {
        flush_rule()
        id = field_val($0, "-[[:space:]]*id")
        next
    }
    /^    pattern:/                    { pat  = field_val($0, "pattern");                    next }
    /^    scope:/                      { scope = field_val($0, "scope");                     next }
    /^    test_fp_example:/            { fp   = field_val($0, "test_fp_example");            next }
    /^    test_denylist_token_example:/ { tde  = field_val($0, "test_denylist_token_example"); next }
    END { flush_rule() }
    ' "$file"
}

# ── Token-list load & validate ─────────────────────────────────────────────────
_load_token_list() {
    local list_file="$1"

    if [ ! -r "$list_file" ]; then
        printf 'setup-error: token list unreadable: %s\n' "$list_file" >&2
        exit "$EXIT_SETUP"
    fi

    # AC #7 guard: abort if token list is tracked in the target repo.
    # git ls-files --error-unmatch exits 0 if tracked (bad), 1 if not tracked (ok), >1 on error.
    local _ls_rc=0
    git ls-files --error-unmatch "$list_file" >/dev/null 2>&1 || _ls_rc=$?
    if [ "$_ls_rc" -eq 0 ]; then
        printf 'setup-error: token-list-tracked-in-public-repo: %s\n' "$list_file" >&2
        exit "$EXIT_SETUP"
    fi
    # _ls_rc=1 = not tracked (OK); _ls_rc>1 = not in git repo or other error (treat as OK)

    # Check for placeholder sentinels — require operator population before live run
    local _grep_rc=0
    grep -qF '${TOKEN_PLACEHOLDER_' "$list_file" 2>/dev/null || _grep_rc=$?
    if [ "$_grep_rc" -eq 0 ]; then
        printf 'setup-error: token list contains unreplaced placeholders (${TOKEN_PLACEHOLDER_<n>}). Populate before activating audit.\n' >&2
        exit "$EXIT_SETUP"
    fi
    if [ "$_grep_rc" -gt 1 ]; then
        printf 'setup-error: grep failed reading token list\n' >&2
        exit "$EXIT_SETUP"
    fi

    local parsed
    if ! parsed=$(_parse_token_list "$list_file"); then
        printf 'setup-error: malformed-token-list: YAML parse failed\n' >&2
        exit "$EXIT_SETUP"
    fi

    # Emit raw records to caller via _DENY_RECORDS and _ALLOW_RECORDS files
    printf '%s\n' "$parsed" | grep '^D	' > "$_tmpdir/deny_records" || true
    printf '%s\n' "$parsed" | grep '^A	' > "$_tmpdir/allow_records" || true

    # Validate all denylist rules have required fields
    local _bad_rule=0
    while IFS='	' read -r _t _did _dpat _dscope; do
        [ "$_t" = "D" ] || continue
        if [ -z "$_dpat" ]; then
            printf 'setup-error: malformed-token-list: denylist rule "%s" missing pattern\n' "$_did" >&2
            _bad_rule=1
        fi
        if [ -z "$_dscope" ]; then
            printf 'setup-error: malformed-token-list: denylist rule "%s" missing scope\n' "$_did" >&2
            _bad_rule=1
        fi
        # Validate pattern is a legal ERE (guard against grep crashes on malformed regex)
        if [ -n "$_dpat" ]; then
            _grep_test_rc=0
            printf '' | grep -qE "$_dpat" 2>/dev/null || _grep_test_rc=$?
            if [ "$_grep_test_rc" -gt 1 ]; then
                printf 'setup-error: malformed-token-list: denylist rule "%s" pattern is invalid POSIX ERE\n' "$_did" >&2
                _bad_rule=1
            fi
        fi
    done < "$_tmpdir/deny_records"

    # Validate all allowlist rules have required fields
    while IFS='	' read -r _t _aid _apat _fp _tde; do
        [ "$_t" = "A" ] || continue
        if [ -z "$_apat" ]; then
            printf 'setup-error: malformed-token-list: allowlist rule "%s" missing pattern\n' "$_aid" >&2
            _bad_rule=1
        fi
        if [ -z "$_fp" ] || [ -z "$_tde" ]; then
            printf 'setup-error: malformed-token-list: allowlist rule "%s" missing test_fp_example or test_denylist_token_example\n' "$_aid" >&2
            _bad_rule=1
        fi
    done < "$_tmpdir/allow_records"

    if [ "$_bad_rule" -ne 0 ]; then
        exit "$EXIT_SETUP"
    fi

    local ndeny
    ndeny=$(wc -l < "$_tmpdir/deny_records" | tr -d ' ')

    if [ "$ndeny" -lt 1 ]; then
        printf 'setup-error: malformed-token-list: no valid denylist rules found\n' >&2
        exit "$EXIT_SETUP"
    fi
}

# ── Allowlist suppression check ─────────────────────────────────────────────────
# Returns 0 if line should be SUPPRESSED (allowlisted), 1 if not
_is_allowlisted() {
    local matched_text="$1"
    local stream="$2"      # commit-metadata | diff
    local rc=0

    while IFS='	' read -r _type aid apat _fp _tde; do
        [ "$_type" = "A" ] || continue
        [ -n "$apat" ]     || continue
        if printf '%s\n' "$matched_text" | grep -qE "$apat" 2>/dev/null; then
            return 0  # suppressed
        fi
    done < "$_tmpdir/allow_records"

    return 1  # not suppressed
}

# ── Allowlist self-test (AC #2, Fixture K) ─────────────────────────────────────
_run_allowlist_self_test() {
    while IFS='	' read -r _type aid apat fp_ex tde_ex; do
        [ "$_type" = "A" ] || continue
        [ -n "$apat" ]     || continue

        # (a) test_fp_example must be matched (suppressed) by this allowlist rule
        if ! printf '%s\n' "$fp_ex" | grep -qE "$apat" 2>/dev/null; then
            printf 'setup-error: allowlist-overreach: rule=%s: test_fp_example not matched by allow pattern\n' "$aid" >&2
            exit "$EXIT_SETUP"
        fi

        # (b) test_denylist_token_example must NOT be matched by this allowlist rule
        if printf '%s\n' "$tde_ex" | grep -qE "$apat" 2>/dev/null; then
            printf 'setup-error: allowlist-overreach: rule=%s: test_denylist_token_example would be over-suppressed\n' "$aid" >&2
            exit "$EXIT_SETUP"
        fi
    done < "$_tmpdir/allow_records"
}

# ── Stream generator ───────────────────────────────────────────────────────────
_build_streams() {
    local base_sha="$1" head_sha="$2"

    # Verify required git binaries
    if ! command -v git >/dev/null 2>&1; then
        printf 'setup-error: git not found in PATH\n' >&2
        exit "$EXIT_SETUP"
    fi

    # Verify BASE_SHA is resolvable
    if ! git rev-parse --verify "$base_sha^{commit}" >/dev/null 2>&1; then
        printf 'setup-error: BASE_SHA unresolvable: %s\n' "$base_sha" >&2
        exit "$EXIT_SETUP"
    fi

    # Stream A: commit metadata
    git log --format='%an %ae %cn %ce %B' "${base_sha}..${head_sha}" \
        > "$_COMMIT_STREAM" 2>/dev/null || true

    # Stream B: diff body — only ^+ lines, excluding ^+++ file-header lines
    # --unified=0 suppresses context lines so pre-AR-G5 history in '-' lines
    # does not trigger false positives
    git diff --unified=0 "${base_sha}..${head_sha}" 2>/dev/null \
        | awk '/^\+\+\+/ { next } /^\+/ { print substr($0,2) }' \
        > "$_DIFF_STREAM" || true
}

# ── Mock-stream support (for test fixtures) ───────────────────────────────────
# If IDENTITY_AUDIT_MOCK_COMMIT_LOG or IDENTITY_AUDIT_MOCK_DIFF are set, use
# those files instead of running git. Test-only paths.
_maybe_use_mock_streams() {
    if [ -n "${IDENTITY_AUDIT_MOCK_COMMIT_LOG:-}" ]; then
        if [ -r "$IDENTITY_AUDIT_MOCK_COMMIT_LOG" ]; then
            cp "$IDENTITY_AUDIT_MOCK_COMMIT_LOG" "$_COMMIT_STREAM"
        else
            printf 'setup-error: IDENTITY_AUDIT_MOCK_COMMIT_LOG unreadable: %s\n' \
                "$IDENTITY_AUDIT_MOCK_COMMIT_LOG" >&2
            exit "$EXIT_SETUP"
        fi
    fi
    if [ -n "${IDENTITY_AUDIT_MOCK_DIFF:-}" ]; then
        if [ -r "$IDENTITY_AUDIT_MOCK_DIFF" ]; then
            # Apply same ^+ filtering as live path
            awk '/^\+\+\+/ { next } /^\+/ { print substr($0,2) }' \
                "$IDENTITY_AUDIT_MOCK_DIFF" > "$_DIFF_STREAM"
        else
            printf 'setup-error: IDENTITY_AUDIT_MOCK_DIFF unreadable: %s\n' \
                "$IDENTITY_AUDIT_MOCK_DIFF" >&2
            exit "$EXIT_SETUP"
        fi
    fi
}

# ── Core audit engine ──────────────────────────────────────────────────────────
# Writes redacted match records to $_MATCH_LOG. Returns 0 always (errors exit).
_run_audit() {
    local _grep_rc _raw

    while IFS='	' read -r _type did dpat dscope; do
        [ "$_type" = "D" ] || continue
        [ -n "$dpat" ]     || continue

        # Evaluate commit-metadata stream
        if [ "$dscope" = "commit-metadata" ] || [ "$dscope" = "both" ]; then
            if [ -s "$_COMMIT_STREAM" ]; then
                _raw="$_tmpdir/raw_cm_${did}"
                _grep_rc=0
                grep -E "$dpat" "$_COMMIT_STREAM" > "$_raw" 2>/dev/null || _grep_rc=$?
                if [ "$_grep_rc" -gt 1 ]; then
                    printf 'setup-error: grep failed (rule=%s, stream=commit-metadata, rc=%s)\n' \
                        "$did" "$_grep_rc" >&2
                    exit "$EXIT_SETUP"
                fi
                while IFS= read -r _line; do
                    [ -n "$_line" ] || continue
                    if ! _is_allowlisted "$_line" "commit-metadata"; then
                        printf '<REDACTED:rule=%s:stream=commit-metadata>\n' "$did" >> "$_MATCH_LOG"
                    fi
                done < "$_raw"
            fi
        fi

        # Evaluate diff stream
        if [ "$dscope" = "diff" ] || [ "$dscope" = "both" ]; then
            if [ -s "$_DIFF_STREAM" ]; then
                _raw="$_tmpdir/raw_diff_${did}"
                _grep_rc=0
                grep -E "$dpat" "$_DIFF_STREAM" > "$_raw" 2>/dev/null || _grep_rc=$?
                if [ "$_grep_rc" -gt 1 ]; then
                    printf 'setup-error: grep failed (rule=%s, stream=diff, rc=%s)\n' \
                        "$did" "$_grep_rc" >&2
                    exit "$EXIT_SETUP"
                fi
                while IFS= read -r _line; do
                    [ -n "$_line" ] || continue
                    if ! _is_allowlisted "$_line" "diff"; then
                        printf '<REDACTED:rule=%s:stream=diff>\n' "$did" >> "$_MATCH_LOG"
                    fi
                done < "$_raw"
            fi
        fi
    done < "$_tmpdir/deny_records"
}

# ── Main ───────────────────────────────────────────────────────────────────────
main() {
    # Resolve token list path
    local list_file=""
    if [ -n "${IDENTITY_AUDIT_TOKEN_LIST:-}" ]; then
        list_file="$IDENTITY_AUDIT_TOKEN_LIST"
    elif [ ! -t 0 ]; then
        # Stdin is piped — read token list from stdin
        list_file="$_tmpdir/stdin-token-list"
        cat > "$list_file"
        if [ ! -s "$list_file" ]; then
            printf 'setup-error: IDENTITY_AUDIT_TOKEN_LIST unset and stdin is empty\n' >&2
            exit "$EXIT_SETUP"
        fi
    else
        printf 'setup-error: IDENTITY_AUDIT_TOKEN_LIST env var unset and no piped stdin\n' >&2
        printf 'usage: IDENTITY_AUDIT_TOKEN_LIST=<path> BASE_SHA=<sha> HEAD_SHA=<sha> %s\n' \
            "$(basename "$0")" >&2
        exit "$EXIT_SETUP"
    fi

    # Determine mode: mock streams (tests) or live git
    local mock_mode=false
    if [ -n "${IDENTITY_AUDIT_MOCK_COMMIT_LOG:-}" ] || [ -n "${IDENTITY_AUDIT_MOCK_DIFF:-}" ]; then
        mock_mode=true
    fi

    # Validate SHA env vars when running live (not mock)
    local base_sha="${BASE_SHA:-}"
    local head_sha="${HEAD_SHA:-}"
    if ! $mock_mode; then
        if [ -z "$base_sha" ] || [ -z "$head_sha" ]; then
            printf 'setup-error: BASE_SHA and HEAD_SHA must be set (or use IDENTITY_AUDIT_MOCK_* for tests)\n' >&2
            exit "$EXIT_SETUP"
        fi
    fi

    # Load and validate token list
    _load_token_list "$list_file"

    # Allowlist self-test — must pass before any audit logic runs
    _run_allowlist_self_test

    # Build or mock streams
    touch "$_COMMIT_STREAM" "$_DIFF_STREAM"
    if $mock_mode; then
        _maybe_use_mock_streams
    else
        _build_streams "$base_sha" "$head_sha"
    fi

    # Run audit — all rules × all streams evaluated before reporting
    _run_audit

    # Report results (redacted) and exit
    if [ -s "$_MATCH_LOG" ]; then
        cat "$_MATCH_LOG"
        exit "$EXIT_MATCH"
    else
        exit "$EXIT_CLEAN"
    fi
}

main "$@"
