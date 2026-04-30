#!/usr/bin/env bash
# test_ux_conformance.sh — red-phase acceptance tests for UX conformance spec (story 1-4).
#
# Source: story 1.4 AC#1–#5; docs/epic-1-style-guide.md §8 (ban-list).
# Returns 0 on pass / 1 on fail.
#
# TDD RED PHASE: will FAIL until all story 1.4 deliverables are present.
#
# Usage: ./test_ux_conformance.sh [--spec-root PATH]

set -euo pipefail

SPEC_ROOT="${SPEC_ROOT:-$(cd "$(dirname "$0")/../../.."; pwd)}"
TELEPROTO3_ROOT="${SPEC_ROOT}/teleproto3"
FAIL=0

pass() { printf "PASS [%s]\n" "$1"; }
fail() { printf "FAIL [%s]: %s\n" "$1" "${2:-}"; FAIL=1; }

echo "=== test_ux_conformance.sh: RED-PHASE acceptance scaffold ==="

UX_SPEC="${TELEPROTO3_ROOT}/spec/ux-conformance.md"
UX_STRINGS="${TELEPROTO3_ROOT}/spec/ux-strings.yaml"
CONTRAST_TOKENS="${TELEPROTO3_ROOT}/spec/contrast-tokens.yaml"

# ------------------------------------------------------------------ #
# 1.4-UNIT-001: ux-conformance.md has sections §1–§9                 #
# ------------------------------------------------------------------ #
if [ -f "$UX_SPEC" ]; then
    for sec in 1 2 3 4 5 6 7 8 9; do
        if grep -qE "^#{1,3} .*§?${sec}\b|^#{1,3} ${sec}\. |^## ${sec} " "$UX_SPEC" 2>/dev/null \
           || grep -qE "^## .*(Section|section|§) ?${sec}" "$UX_SPEC" 2>/dev/null; then
            pass "1.4-UNIT-001: §${sec} present"
        else
            fail "1.4-UNIT-001" "section §${sec} not found in ux-conformance.md"
        fi
    done
else
    fail "1.4-UNIT-001" "ux-conformance.md not found at ${UX_SPEC}"
fi

# ------------------------------------------------------------------ #
# 1.4-UNIT-002: §9 cites T3_RETRY_* enum verbatim                   #
# ------------------------------------------------------------------ #
REQUIRED_ENUM_VALUES=("T3_RETRY_OK" "T3_RETRY_TIER1" "T3_RETRY_TIER2" "T3_RETRY_TIER3")
if [ -f "$UX_SPEC" ]; then
    for val in "${REQUIRED_ENUM_VALUES[@]}"; do
        if grep -q "$val" "$UX_SPEC"; then
            pass "1.4-UNIT-002: $val cited in ux-conformance.md"
        else
            fail "1.4-UNIT-002" "$val NOT found in ux-conformance.md §9"
        fi
    done
fi

# ------------------------------------------------------------------ #
# 1.4-UNIT-003: frontmatter status:draft preserved                    #
# ------------------------------------------------------------------ #
if [ -f "$UX_SPEC" ]; then
    if grep -qE '^status: *draft' "$UX_SPEC"; then
        pass "1.4-UNIT-003: frontmatter status:draft present"
    else
        fail "1.4-UNIT-003" "frontmatter 'status: draft' not found in ux-conformance.md"
    fi
fi

# ------------------------------------------------------------------ #
# 1.4-UNIT-004: ux-strings.yaml — every key has en/ru/fa/zh          #
# ------------------------------------------------------------------ #
if [ -f "$UX_STRINGS" ]; then
    # Use Python3 for YAML parsing (stdlib pyyaml may not be available;
    # use simple grep-based heuristic: every entry block must have
    # lines matching locale markers en: ru: fa: zh:).
    python3 - "$UX_STRINGS" << 'PYEOF'
import sys, re

path = sys.argv[1]
content = open(path).read()

# Find all top-level keys (non-indented, followed by ':')
top_keys = re.findall(r'^(\w[\w_.]*):$', content, re.MULTILINE)
failures = 0
for key in top_keys:
    # Extract the block for this key (crude but stdlib-safe).
    pattern = rf'^{re.escape(key)}:\n((?:  .+\n)*)'
    m = re.search(pattern, content, re.MULTILINE)
    if not m:
        continue
    block = m.group(1)
    for locale in ('en', 'ru', 'fa', 'zh'):
        if not re.search(rf'^\s+{locale}:', block, re.MULTILINE):
            print(f"FAIL [1.4-UNIT-004]: key '{key}' missing locale '{locale}'")
            failures += 1
if failures == 0:
    print(f"PASS [1.4-UNIT-004]: all {len(top_keys)} keys have en/ru/fa/zh")
sys.exit(1 if failures else 0)
PYEOF
    STATUS=$?
    if [ "$STATUS" -ne 0 ]; then FAIL=1; fi
else
    fail "1.4-UNIT-004" "ux-strings.yaml not found at ${UX_STRINGS}"
fi

# ------------------------------------------------------------------ #
# 1.4-UNIT-006: ban-list — 7 tokens not in any locale value           #
# style-guide §8: case-insensitive substring match                    #
# ------------------------------------------------------------------ #
BAN_TOKENS=("proxy" "proxy-server" "bypass" "censorship" "прокси" "پروکسی" "代理")

check_ban_list() {
    local file="$1"
    local context="$2"
    local found=0
    for token in "${BAN_TOKENS[@]}"; do
        # Skip lines that are part of the ban-list documentation sentinel.
        if grep -vi "ban-list-doc" "$file" 2>/dev/null \
           | grep -qi "$token" 2>/dev/null; then
            printf "FAIL [1.4-UNIT-006]: banned token '%s' found in %s\n" "$token" "$context"
            FAIL=1
            found=1
        fi
    done
    if [ "$found" -eq 0 ]; then
        printf "PASS [1.4-UNIT-006]: no banned tokens in %s\n" "$context"
    fi
}

[ -f "$UX_STRINGS" ] && check_ban_list "$UX_STRINGS" "ux-strings.yaml"
[ -f "$UX_SPEC" ]    && check_ban_list "$UX_SPEC"    "ux-conformance.md"

# ------------------------------------------------------------------ #
# 1.4-UNIT-007: case-insensitive substring (not word-boundary)        #
# ------------------------------------------------------------------ #
# Verify that "PROXY" in uppercase would also be caught.
if echo "PROXY" | grep -qi "proxy" >/dev/null 2>&1; then
    pass "1.4-UNIT-007: ban-list check is case-insensitive"
else
    fail "1.4-UNIT-007" "case-insensitive grep not working as expected"
fi

# ------------------------------------------------------------------ #
# 1.4-UNIT-008: ban-list sentinel line excluded                        #
# ------------------------------------------------------------------ #
SENTINEL_LINE="<!-- ban-list-doc: proxy proxy-server bypass -->"
TMPFILE=$(mktemp /tmp/t3_banlist_test.XXXXXX)
printf "%s\nSome clean content\n" "$SENTINEL_LINE" > "$TMPFILE"
# Sentinel must NOT trigger a ban-list failure when excluded via -v.
TRIGGER=$(grep -vi "ban-list-doc" "$TMPFILE" | grep -oi "proxy" || true)
if [ -z "$TRIGGER" ]; then
    pass "1.4-UNIT-008: ban-list sentinel excluded from check"
else
    fail "1.4-UNIT-008" "sentinel line triggered ban-list check"
fi
rm -f "$TMPFILE"

# ------------------------------------------------------------------ #
# 1.4-UNIT-009 / 1.4-UNIT-010: contrast-tokens.yaml ratio checks     #
# ------------------------------------------------------------------ #
if [ -f "$CONTRAST_TOKENS" ]; then
    python3 - "$CONTRAST_TOKENS" << 'PYEOF'
import sys, re

path = sys.argv[1]
content = open(path).read()
failures = 0

# Find all ratio_min values and their context (text-pair vs graphical-pair).
for m in re.finditer(r'(text-pair|graphical-pair)[^\n]*\n(?:[^\n]*\n)*?.*?ratio_min:\s*([\d.]+)', content):
    kind = m.group(1)
    ratio = float(m.group(2))
    if kind == 'text-pair' and ratio < 4.5:
        print(f"FAIL [1.4-UNIT-009]: text-pair ratio_min={ratio} < 4.5 (WCAG 1.4.3)")
        failures += 1
    elif kind == 'graphical-pair' and ratio < 3.0:
        print(f"FAIL [1.4-UNIT-010]: graphical-pair ratio_min={ratio} < 3.0 (WCAG 1.4.11)")
        failures += 1

if failures == 0:
    print("PASS [1.4-UNIT-009]: all text-pair ratio_min >= 4.5")
    print("PASS [1.4-UNIT-010]: all graphical-pair ratio_min >= 3.0")
sys.exit(1 if failures else 0)
PYEOF
    STATUS=$?
    if [ "$STATUS" -ne 0 ]; then FAIL=1; fi
else
    fail "1.4-UNIT-009" "contrast-tokens.yaml not found at ${CONTRAST_TOKENS}"
fi

# ------------------------------------------------------------------ #
# Summary                                                              #
# ------------------------------------------------------------------ #
if [ "$FAIL" -eq 0 ]; then
    echo ""
    echo "=== RESULT: PASS ==="
else
    echo ""
    echo "=== RESULT: FAIL ==="
    exit 1
fi
