---
spec_version: 0.1.0-draft
last_updated: 2026-05-14
status: draft
---

> **Normative.** This document defines required behaviour for conforming
> Type3 implementations. Code in `lib/` is illustrative; where this
> document and `lib/` differ, this document wins.

# Operator CLI Stdout Schema

Structured plain-text output schema for installer and operator commands.
Defines status prefixes, indentation hierarchy, key:value format, TTY vs
plain-pipe selection, decoration prohibition, and structured-output
reservation. (Source: ux-design-specification.md §C6 and §CLI Output
Pattern COP-1 through COP-7.)

## 1. Status prefix convention (COP-1)

Every meaningful output line MUST start with one of the following four
status prefixes, exactly as shown (lowercase, square brackets):

| Prefix | Stream | ANSI colour (TTY) | Meaning |
|---|---|---|---|
| `[ok]` | stdout | Green | Operation completed successfully |
| `[warn]` | stdout | Yellow | Operation completed with a non-fatal warning |
| `[info]` | stdout | Neutral (no colour) | Informational context, not a result |
| `[err]` | **stderr** | Red | Operation failed; action required |

**`[err]` MUST be written to stderr.** `[ok]`, `[warn]`, and `[info]`
MUST be written to stdout.

Prefix tokens are exact and case-sensitive: `[OK]`, `[Warn]`, `[ERROR]`
and similar variants are non-conforming. A prefix string MUST NOT be
preceded by any other character on the same output line.

## 2. Indentation hierarchy

- **Unit:** two ASCII space characters (`  `) per indentation level.
  Tabs MUST NOT be used.
- **Maximum depth:** three levels.
- **Overflow:** if a context requires nesting deeper than three levels,
  the key path MUST be collapsed to dot-separated notation on a single
  line at level 3:
  ```
  [ok] service started
    config.tls.cert-path: /etc/teleproto3/cert.pem
  ```
  Not:
  ```
  [ok] service started
    config:
      tls:
        cert-path: /etc/teleproto3/cert.pem
        (level 4 — FORBIDDEN)
  ```

## 3. Format

Output lines follow `key: value` convention.

- **Keys.** Lowercase, hyphenated (e.g. `server-id`, `cert-path`). No
  spaces, no underscores in keys. No multi-column tables in TTY output
  (per COP-3).
- **Values.** A single scalar on the same line as the key.

### 3.1 Multi-line and list values

When a value spans multiple lines or is a list, use continuation lines:

**Multi-line string value.** Continuation lines are indented two additional
spaces beneath the parent key:

```
[ok] config loaded
  notes: First line of the note.
    Second line (continuation, 4 spaces total indent).
```

A value containing a literal newline character MUST escape it as `\\n`
when rendered on a continuation line; implementations MUST NOT emit a
literal 0x0A inside a value.

**List value.** Each list item appears on its own continuation line,
prefixed with `- `:

```
[ok] admin roster loaded
  co-admins:
    - handle: TP3-AAAA0001
    - handle: TP3-AAAA0002
```

### 3.2 Wrap

- **Column limit.** 80 columns default. Implementations MUST wrap lines
  that would exceed 80 columns.
- **Wrap rule.** A wrapped continuation line is indented to the same level
  as the parent `key:` plus two additional spaces:
  ```
  [ok] recovery letter written
    path: /var/lib/teleproto3/
      recovery-letter-2026-05-14.pdf
  ```
- **Per-command output volume.** A single command invocation MUST produce
  ≤ 30 lines of output at 80 columns (per COP-5). If more content is
  needed, the output MUST direct the operator to a file path or
  observability URL.

## 4. TTY vs plain-pipe mode

The output mode (colour vs plain) is selected by testing whether the
output stream is an interactive terminal. This test MUST be
**language-agnostic** — the implementation uses whatever mechanism is
natural for its platform; the method is not prescribed by this spec.

Informative examples:
- POSIX C: `isatty(fileno(stdout))` for stdout; `isatty(fileno(stderr))`
  for stderr.
- Win32: `GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), ...)`.
- Python: `sys.stdout.isatty()`.
- Go: `term.IsTerminal(int(os.Stdout.Fd()))`.

**Override conditions.** Even when `isatty()` reports interactive:

| Condition | Override |
|---|---|
| `TERM=dumb` environment variable | MUST use plain-pipe mode |
| `NO_COLOR` environment variable (any value, even empty) | MUST strip ANSI colour codes |

**Plain-pipe mode** (non-interactive or overridden): ANSI escape codes
MUST NOT be emitted; structure and prefixes are preserved verbatim.

**TTY mode** (interactive, not overridden): ANSI colour codes as per the
table in §1 are emitted; prefix strings remain.

### 4.1 Continuation-line colour

In TTY mode, continuation lines (indented beneath a status-prefix line)
MUST use the default terminal foreground colour. Continuation lines MUST
NOT inherit the colour of the parent line's status prefix. Only the first
line of a status block carries the colour; continuation lines are always
neutral.

## 5. Banned decoration (COP-2)

The following output elements MUST NOT appear in any command output,
regardless of TTY or plain-pipe mode:

| Forbidden element | Examples |
|---|---|
| ASCII art logos | `+---------+`, `| teleproto3 |` |
| Box-drawing characters | `│`, `─`, `┼`, `╔`, `═` |
| Emoji | Any Unicode emoji (U+1F000 and above Emoji_Presentation) |
| Progress bars | `[####----]`, `▓▓▓░░░` |
| Spinners | `⠋`, `⠙`, rotating characters |
| "Welcome" banners | Any greeting text at startup |
| Confirmation ceremony | "Great!", "Done!", "All set!" |

This prohibition applies unconditionally — no `--verbose` or `--pretty`
flag may re-enable any forbidden element.

## 6. Error remediation (COP-6)

Every `[err]` line MUST include at least one actionable next step. Valid
forms:

- A runnable command (e.g. `run: systemctl restart teleproto3`).
- A configuration file path (e.g. `see: /etc/teleproto3/config.yaml`).
- An in-repository documentation link (e.g. `docs: docs/deploy.md §4`).

**Systemic-failure exception.** For failures caused by conditions outside
the operator's direct control — out-of-memory, unexpected OS signal,
kernel panic — the actionable hint MAY be a bug-tracker URL instead of a
runnable command:

```
[err] unexpected signal SIGSEGV — this is a bug
  report: https://github.com/ankuper/teleproto3/issues
```

The systemic-failure exception does NOT apply to configuration errors,
missing files, permission errors, or network failures — those MUST use
runnable-command or config-path forms.

## 7. Typography (UX-DR25)

- **CLI output and identifier blocks:** IBM Plex Mono.
- **Libertinus Serif** MUST NOT replace monospace text in CLI output.

Per UX-DR25 (ux-design-specification.md §8.3). This rule applies to any
rendered documentation or UI surface that displays CLI output verbatim
(e.g. a terminal emulator pane in a GUI installer). The CLI output stream
itself is plain text; font selection is the responsibility of the
rendering surface.

## 8. Structured-output mode (COP-7)

The flags `--output=json` and `--output=yaml` are **reserved** for
machine-readable structured output. Implementations MUST:

- Accept `--output=json` without error (MAY respond with `[err]` if the
  schema is not yet implemented, with message directing to Epic 8
  tracking).
- Accept `--output=yaml` without error (same allowance).
- NOT define any JSON or YAML schema in v0.1.0.

**Schema deferred to Epic 8** (operator-tooling polish). Freezing the
flag names now ensures that operator scripts written against v0.1.0 can
add `--output=json` without a flag-rename breaking change at Epic 8.

## 9. Test vectors

Test vectors for CLI stdout conformance (prefix-compliance, indentation-
depth, 80-column wrap, NO_COLOR behaviour) are tracked under the key
`cli-stdout` in `../conformance/vectors/unit.json`.

**Vector population deferred to Epic 3 / Epic 8.** This story adds no
`unit.json` keys.

## 10. Contested Decisions

- **Rejected: define structured-output JSON / YAML schema in this story.**
  The schema belongs in Epic 8 (operator-tooling polish), where the full
  operator data model is specified. Deferring now avoids a schema-change
  breaking change when Epic 8 lands its richer operator data model.
  Deferred to: Epic 8.

- **Rejected: emoji status glyphs** (e.g. ✅ for `[ok]`, ⚠️ for
  `[warn]`). Emoji violate COP-2 (no decorative output) and the
  AR-C1 spirit on decorative noise. Emoji also render inconsistently
  across terminals, creating width-calculation bugs in plain-pipe
  consumers. Rejected outright for all v0.1.x versions.

## 11. Banned tokens

The following AR-C1 tokens are listed for operator-reference documentation
purposes only. Outside this sentinel block, these tokens MUST NOT appear
in any prose, key, value, alt-text, or comment in this file.

<!-- ban-list-doc: AR-C1 seven-token reference for cli-stdout schema -->
| Token          | Script         |
|----------------|----------------|
| `proxy`        | Latin          |
| `proxy-server` | Latin          |
| `bypass`       | Latin          |
| `censorship`   | Latin          |
| `прокси`       | Cyrillic       |
| `پروکسی`       | Arabic/Persian |
| `代理`         | Han (CJK)      |
<!-- /ban-list-doc -->
