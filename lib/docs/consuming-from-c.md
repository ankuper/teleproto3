# Consuming libteleproto3 from C

Integration guide for pure-C consumers. For C++, the same API works via
`extern "C"`.

## 1. Get the library

Preferred: use a tagged release. Pin by tag name + SHA:

```
URL:  https://github.com/ankuper/teleproto3/releases/download/lib-vX.Y.Z/libteleproto3-vX.Y.Z.tar.gz
SHA256: <record in your build system>
```

Alternative: vendor via `git subtree` or submodule, pinned to a
`lib-vX.Y.Z` tag.

## 2. Link

Static: link against `lib/build/libteleproto3.a`. Include path:
`lib/include/`. Single public header: `t3.h`.

## 3. Minimal call sequence

_TBD(lib-v0.1.0): expand once API is real._

```c
#include "t3.h"

t3_secret_t *s = NULL;
t3_session_t *sess = NULL;

if (t3_secret_parse(bytes, len, &s) != T3_OK) { /* handle */ }
if (t3_session_new(s, &sess) != T3_OK) { /* handle */ }
/* ... frame processing ... */
t3_session_free(sess);
t3_secret_free(s);
```

## 4. Threading

_TBD._ Document reentrancy and safe-to-share assumptions.

## 5. Error handling

Every public API returns `t3_result_t`. Non-zero results are errors;
consumers MUST NOT use out-parameters when the return is non-zero.
