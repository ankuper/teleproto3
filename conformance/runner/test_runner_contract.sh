#!/usr/bin/env bash
# test_runner_contract.sh — red-phase acceptance tests for the conformance runner.
#
# Source: story 1.9 AC#1, AC#2.
# Returns 0 on pass / 1 on fail.
#
# TDD RED PHASE: will FAIL until run.sh and verify.py are fully
# implemented per story 1.9 Tasks 1–2.

set -euo pipefail

RUNNER="$(dirname "$0")/run.sh"
VERIFY="$(dirname "$0")/verify.py"
UNIT_JSON="$(dirname "$0")/../vectors/unit.json"
FAIL=0

pass() { printf "PASS [%s]\n" "$1"; }
fail() { printf "FAIL [%s]: %s\n" "$1" "${2:-}"; FAIL=1; }

echo "=== test_runner_contract.sh: RED-PHASE acceptance scaffold ==="

# ------------------------------------------------------------------ #
# Helper: run run.sh and capture exit code                            #
# ------------------------------------------------------------------ #
run_sh() {
    bash "$RUNNER" "$@" >/dev/null 2>&1
    echo $?
}

# ------------------------------------------------------------------ #
# 1.9-UNIT-004: --impl + --endpoint → exit 2                         #
# ------------------------------------------------------------------ #
CODE=$(run_sh --impl /bin/true --endpoint 127.0.0.1:9999 2>/dev/null || true)
if [ "$CODE" = "2" ]; then
    pass "1.9-UNIT-004: --impl + --endpoint -> exit 2"
else
    fail "1.9-UNIT-004" "expected exit 2, got $CODE"
fi

# ------------------------------------------------------------------ #
# 1.9-UNIT-005: no args → exit 2                                      #
# ------------------------------------------------------------------ #
CODE=$(bash "$RUNNER" 2>/dev/null; echo $?) || true
if [ "$CODE" = "2" ]; then
    pass "1.9-UNIT-005: no args -> exit 2"
else
    fail "1.9-UNIT-005" "expected exit 2, got $CODE"
fi

# ------------------------------------------------------------------ #
# 1.9-UNIT-001: --level core → only mandatory/ invoked               #
# Verify by capturing scenario directory list (must contain only       #
# mandatory/).                                                         #
# ------------------------------------------------------------------ #
LEVEL_OUT=$( bash "$RUNNER" --impl /bin/true --level core 2>&1 | head -5 || true)
if echo "$LEVEL_OUT" | grep -q 'mandatory'; then
    pass "1.9-UNIT-001: --level core invokes mandatory/"
else
    fail "1.9-UNIT-001" "mandatory/ not mentioned in run.sh --level core output"
fi

if echo "$LEVEL_OUT" | grep -q 'full/' || echo "$LEVEL_OUT" | grep -q 'extended/'; then
    fail "1.9-UNIT-001-extra" "--level core should NOT run full/ or extended/"
else
    pass "1.9-UNIT-001-extra: no full/ or extended/ in core run"
fi

# ------------------------------------------------------------------ #
# 1.9-UNIT-002: --level full → mandatory/ + full/                    #
# ------------------------------------------------------------------ #
LEVEL_OUT=$(bash "$RUNNER" --impl /bin/true --level full 2>&1 | head -10 || true)
if echo "$LEVEL_OUT" | grep -q 'mandatory' && echo "$LEVEL_OUT" | grep -q 'full'; then
    pass "1.9-UNIT-002: --level full runs mandatory/ + full/"
else
    fail "1.9-UNIT-002" "expected both mandatory/ and full/ in output"
fi
if echo "$LEVEL_OUT" | grep -q 'extended/'; then
    fail "1.9-UNIT-002-extra" "--level full should NOT run extended/"
fi

# ------------------------------------------------------------------ #
# 1.9-UNIT-003: --level extended → all 3 dirs                        #
# ------------------------------------------------------------------ #
LEVEL_OUT=$(bash "$RUNNER" --impl /bin/true --level extended 2>&1 | head -15 || true)
for dir in mandatory full extended; do
    if echo "$LEVEL_OUT" | grep -q "$dir"; then
        pass "1.9-UNIT-003: --level extended includes $dir/"
    else
        fail "1.9-UNIT-003" "--level extended missing $dir/"
    fi
done

# ------------------------------------------------------------------ #
# 1.9-UNIT-006: exit codes 0=PASS, 1=FAIL, 2=harness-error          #
# (Already tested above: exit 2 for missing args)                    #
# pass→exit 0: stub IUT that outputs PASS for every scenario.        #
# ------------------------------------------------------------------ #
STUB_IUT=$(mktemp /tmp/t3_stub_iut.XXXXXX)
cat > "$STUB_IUT" << 'IUTSTUB'
#!/usr/bin/env bash
# Stub IUT: output NDJSON PASS for every JSON line received on stdin.
while IFS= read -r line; do
    id=$(echo "$line" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('id','?'))" 2>/dev/null || echo "?")
    printf '{"scenario":"%s","result":"PASS"}\n' "$id"
done
IUTSTUB
chmod +x "$STUB_IUT"

EXIT_CODE=$(bash "$RUNNER" --impl "$STUB_IUT" --level core 2>/dev/null; echo $?) || true
if [ "$EXIT_CODE" = "0" ]; then
    pass "1.9-UNIT-006: stub IUT all PASS -> exit 0"
else
    fail "1.9-UNIT-006" "expected exit 0 from stub IUT, got $EXIT_CODE"
fi
rm -f "$STUB_IUT"

# ------------------------------------------------------------------ #
# 1.9-UNIT-007: verify.py reads secret-format + session-header       #
#               (NOT wire-format)                                     #
# ------------------------------------------------------------------ #
if [ -f "$VERIFY" ]; then
    if grep -q 'wire-format' "$VERIFY"; then
        fail "1.9-UNIT-007" "verify.py references 'wire-format' key — must use 'secret-format' + 'session-header'"
    else
        pass "1.9-UNIT-007: verify.py does not reference 'wire-format'"
    fi
    if grep -q 'secret-format' "$VERIFY" && grep -q 'session-header' "$VERIFY"; then
        pass "1.9-UNIT-007-keys: verify.py reads correct JSON sections"
    else
        fail "1.9-UNIT-007-keys" "verify.py must reference 'secret-format' and 'session-header'"
    fi
else
    fail "1.9-UNIT-007" "verify.py not found at $VERIFY"
fi

# ------------------------------------------------------------------ #
# 1.9-INT-001: end-to-end pipeline                                    #
# run.sh --impl ./iut --level core | verify.py → exit 0             #
# ------------------------------------------------------------------ #
STUB_IUT2=$(mktemp /tmp/t3_stub_iut2.XXXXXX)
printf '#!/bin/sh\nwhile read l; do printf '"'"'{"scenario":"x","result":"PASS","section":"secret-format","observed":{}}\n'"'"'; done\n' > "$STUB_IUT2"
chmod +x "$STUB_IUT2"

PIPE_EXIT=$(bash "$RUNNER" --impl "$STUB_IUT2" --level core 2>/dev/null \
            | python3 "$VERIFY" 2>/dev/null; echo $?) || true
if [ "$PIPE_EXIT" = "0" ]; then
    pass "1.9-INT-001: run.sh | verify.py pipeline exit 0"
else
    fail "1.9-INT-001" "pipeline exit $PIPE_EXIT (expected 0)"
fi
rm -f "$STUB_IUT2"

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
