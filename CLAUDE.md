# sunmeow — agent rules

Host-side streaming server. Fork of **LizardByte/Sunshine**.

Read this fully before your first edit. Every rule here exists because violating it
has already cost us real time — the rationale is recorded so you can judge when a
rule genuinely doesn't apply, rather than guessing.

---

## 1. What this repo is, and why

`sunmeow` is based on **LizardByte/Sunshine** (`master`), not on ClassicOldSong/Apollo.

We evaluated Apollo (the fork whose features we wanted) and rejected it as a base
with measurements, not taste:

| Measurement | Value |
| --- | --- |
| Apollo behind Sunshine | 616 commits |
| Apollo fork point | 2025-09-26 |
| Commits touching Linux/Wayland/KMS/NVENC Apollo was missing | 146 (55 are `fix(…)`) |
| Commits touching Linux/Wayland/KMS/NVENC Apollo contributed | 14 |
| Files conflicting on a Sunshine→Apollo merge | 103 |
| Apollo's headline feature (virtual display) | **Windows only** |

Re-verified 2026-08-24 on full (non-shallow) history. If a shallow clone makes these
numbers collapse, run `git fetch --unshallow <remote>` before trusting anything.

Count conflicts by **paths only** — `git merge-tree --name-only` follows the path list with a
blank line and then an informational `Auto-merging`/`CONFLICT` block, so a naive `tail -n +2`
counts the commentary too and reports ~320 (or ~337 with stderr folded in) instead of 103:

```bash
git merge-tree --write-tree --name-only $APOLLO $SUNSHINE 2>/dev/null | sed -n '2,/^$/p' | grep -c .
git merge-tree --write-tree --name-only $APOLLO $SUNSHINE 2>/dev/null | grep -c '^CONFLICT'
```

The merge was attempted and aborted deliberately. It failed not on volume but on
**mutually exclusive API rewrites** — two examples that decided it:

```cpp
// src/crypto.h — cannot hold both
Apollo:    void add(p_named_cert_t &named_cert_p);  // underpins Apollo's permission system
Sunshine:  void add(x509_t &&cert);                 // Sunshine's rewritten cert handling
```

```cpp
// src/platform/linux/wlgrab.cpp — Sunshine fixed a real Wayland bug Apollo still has
Apollo:    img->frame_timestamp = frame_timestamp;
Sunshine:  img->frame_timestamp = current_frame->frame_timestamp;
```

`src/platform/linux/vaapi.cpp` was a full rewrite of rate-control selection on both
sides. There is no resolution that preserves both intents.

**Apollo remains a reference, not an ancestor.** To adopt an Apollo feature, port it
deliberately as an isolated module under the rules in §2 — never by merging Apollo.

A remote pointing at Apollo exists for reading their implementation.

> **Check `git remote -v` before you fetch anything.** These are local names and they are
> currently the reverse of what you would guess. In this clone:
>
> | Remote | Points at | Use |
> | --- | --- | --- |
> | `origin` | meowerse/sunmeow | ours |
> | `origin-upstream` | **LizardByte/Sunshine** | **the base we sync from** |
> | `upstream` | **ClassicOldSong/Apollo** | **reference only — NEVER merge** |
>
> There is no remote named `apollo`. Fetching or merging `upstream` expecting Sunshine would
> pull Apollo into the tree — the exact outcome this section exists to prevent. If you are
> setting up a fresh clone, prefer unambiguous names:
>
> ```bash
> git remote add sunshine https://github.com/LizardByte/Sunshine.git
> git remote add apollo   https://github.com/ClassicOldSong/Apollo.git
> ```

```bash
git remote -v                                  # confirm which is which, every time
git show upstream/master:src/some_file.cpp     # 'upstream' = Apollo in this clone
```

---

## 2. THE PRIME DIRECTIVE — additive-only

**Never modify an upstream file in place when you can add a new one instead.**

This is the single rule that determines whether we can keep taking upstream fixes.
Apollo is the cautionary tale: it edited upstream files freely, and 11 months later
it could not merge 146 Linux/Wayland/KMS/NVENC commits it badly needed (55 of them fixes). We are one careless refactor
away from the same trap.

### The hierarchy — try in order, stop at the first that works

1. **New file in `src/meow/`** — our code lives here. No upstream file changes.
2. **Subclass / wrap** an upstream type from `src/meow/`, rather than editing it.
3. **One-line hook** in an upstream file that calls into `src/meow/`. Nothing more
   than a call — no logic in upstream files.
4. **Multi-line upstream edit** — requires a `MEOW-TOUCH` entry (§3) and a note in
   the PR explaining why 1–3 were impossible.

### Forbidden without explicit human sign-off

- Renaming upstream symbols, files, or namespaces
  (A namespace rename in our Android client touched 170 files — reverted for exactly
  this reason.)
- Reformatting or re-indenting upstream files
- Changing an upstream function signature
- "Drive-by" cleanups in upstream code

Each of these turns a clean future merge into a conflict in every affected file, and
buys nothing a user can see.

---

## 3. Touch-point registry — every upstream edit is declared

Any edit to a file we did not create is marked in-place:

```cpp
// MEOW-TOUCH(clipboard): dispatch to our clipboard bridge
meow::clipboard::on_stream_start(session);  // <- the entire edit
```

and recorded in `docs/meow/TOUCHPOINTS.md`:

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |

Before every upstream sync, run:

```bash
git grep -n 'MEOW-TOUCH'
```

That grep is the complete list of places a merge can hurt. Keep it short. A growing
registry is a design smell — it means features are being welded into upstream code
instead of layered beside it.

---

## 4. Syncing with upstream

Do this **monthly at minimum**. Apollo's failure was 11 months of drift, not one bad
merge. Small syncs are cheap; large ones are unresolvable.

**The remote you sync from is `origin-upstream` (LizardByte/Sunshine), not `upstream`**
(which is Apollo — see §1). Set `SUNSHINE` once and use it, so a slip cannot merge the wrong
project:

```bash
SUNSHINE=origin-upstream/master      # verify with: git remote -v

git fetch origin-upstream
git log --oneline HEAD..$SUNSHINE | wc -l                # how far behind
git merge-tree --write-tree --name-only HEAD $SUNSHINE   # conflicts, WITHOUT touching the tree
```

`git merge-tree` computes the conflict set without modifying anything. Always run it
before merging so you know the size of the job in advance.

Then:

```bash
git branch backup/sync-$(date +%Y%m%d)   # BEFORE resolving anything
git merge $SUNSHINE                      # NOT upstream/master — that is Apollo
```

**Rules while resolving:**

- Never `checkout --ours/--theirs`, never `merge -X ours/theirs`. Blanket-accepting a
  side is silent data loss that looks like a clean resolution.
- Read both sides of every hunk. Preserve both intents.
- If you cannot determine what a side meant — **stop and ask a human**. A wrong guess
  here is indistinguishable from success until a user reports a missing feature.
- Delete-vs-edit and rename conflicts drop content most often. Decide those out loud.

**After resolving, prove nothing vanished:**

```bash
git log --oneline backup/sync-<date>..HEAD    # our commits still present
git grep -nE '^(<<<<<<<|=======|>>>>>>>)'     # no leftover markers
```

Then run the full gate (§6). A merge resolution is new, unreviewed code.

### Measure drift against the ORIGINAL upstream — never against the fork you branched from

**A clean `git submodule status` proves nothing about currency.** A submodule's `origin` can
point at somebody's fork; it will report clean while sitting months behind the real project.
This is not hypothetical — it is exactly how Apollo's protocol core rotted (see §1).

So for every submodule, check *where it points* before you check whether it is current:

```bash
# where each submodule actually points
git show HEAD:.gitmodules | grep -E 'path|url' | paste - -

# measure the pin against the TRUE upstream, not against origin
# no remote needed, and unlike `remote add` this is safe to re-run
git -C third-party/moonlight-common-c ls-remote \
    https://github.com/moonlight-stream/moonlight-common-c.git HEAD
# compare that SHA with our pin:
git ls-tree HEAD third-party/moonlight-common-c
```

Beware: a submodule's checked-out `origin` may be a leftover from an older base even when
`.gitmodules` is correct. Trust `.gitmodules` at `HEAD`, not the remote in the working copy.

#### Verified good state — 2026-08-24, base `a2b5da60`

Of the 16 submodules, **13 point at the originating project and 3 point at LizardByte's own
mirror org** (see caveats below). The protocol core is exactly at upstream HEAD:
`third-party/moonlight-common-c` -> `moonlight-stream/moonlight-common-c`, pinned `874ac954`,
which is `HEAD`/`refs/heads/master` on that remote as of 2026-08-18.

Moving from the Apollo base to Sunshine advanced this submodule **39 commits** and moved it
from `ClassicOldSong/moonlight-common-c` (a fork, pinned `c999436`, 2025-09-01) onto the
original project. Those 39 commits include the security fix `7b026e7 Harden RTSP handling for
malformed Session headers and oversized responses`, `518b244 Rewrite MbedTLS codepaths to use
the modern PSA APIs`, and nanors SIMD/GFNI + enet updates.

**Two caveats on "all legitimate upstreams":**

- Three submodules point at **`LizardByte-infrastructure/*`**, which is LizardByte's own
  mirror org rather than the originating project:
  `Simple-Web-Server` (originally `eidheim/Simple-Web-Server`), `wayland-protocols`, and
  `wlr-protocols`. These are vendored mirrors, not upstream. Treat them as a drift risk and
  re-check them on each sync.
- **`upload-pack: not our ref` means your module config is stale — it is NOT an upstream
  bug.** This bites on any clone that once tracked Apollo. `.gitmodules` at `HEAD` is correct,
  but `.git/modules/<path>/config` keeps the OLD remote (e.g. `ClassicOldSong/*`) and is not
  re-read automatically, so git asks the wrong server for a SHA it has never heard of. The
  pin itself is fine — verify with `git ls-remote <correct-url>` before blaming anyone. Fix:
  ```bash
  git submodule sync --recursive        # rewrites module configs from .gitmodules
  git submodule update --init --recursive
  ```
  `git submodule sync` skips deinitialised submodules; run `git submodule init <path>` first
  if you have deinitialised one. **Never "fix" this by re-pinning** — re-pinning to silence a
  local misconfiguration is how a fork silently drifts off upstream.

**"Moonlight upstream is dead" is false**, and acting on it would be a costly mistake. It is
true only of the Android app (`moonlight-android`: last `master` commit 2024-07-27, last
release v12.1 Feb 2024). The protocol core is actively maintained — `moonlight-common-c` and
`moonlight-qt` both had commits in August 2026. Never generalise the app's dormancy to the org.

---

## 5. Testing — what we inherit is not enough

Sunshine ships **25** test translation units under `tests/unit` + `tests/integration`
(GoogleTest; 26 `.cpp` under `tests/` if you also count the `tests_main.cpp` runner, which is
not itself a test, plus `tests/unit/platform/macos/test_av_audio.mm`, an Objective-C++ TU that
a `.cpp` glob misses). That is a real suite and we keep it green. But the coverage has holes
precisely where our hardware lives.

### Known gaps — re-verified 2026-08-24 against this base commit

**Core modules with no dedicated test (8 of 23 in `src/*.cpp`)** — some are still linked in
and incidentally exercised as dependencies of other tests (`config.h`, `globals.h`, and
`rtsp.h` are each included by other test TUs), so "untested" here means "nothing tests it on
purpose":
`cbs.cpp`, `config.cpp`, `globals.cpp`, `main.cpp`, `rtsp.cpp`, `stat_trackers.cpp`,
`upnp.cpp`, `video_colorspace.cpp`

> **`nvhttp.cpp` is NOT on this list — it is tested.** `tests/unit/test_http_pairing.cpp`
> (267 lines, `#include <src/nvhttp.h>`) drives all four pairing phases plus an
> out-of-order-call case. An earlier revision of this document listed it as untested; that was
> wrong, and it contradicted §7, which tells you to run `test_http_pairing` on auth changes.
> The coverage is *partial*, not absent: `nvhttp.cpp` is ~1477 lines and the tests cover
> pairing, not the client-list / app-list / launch endpoints.

`config.cpp` is the settings parser — security-relevant and genuinely untested. Note that
`tests/integration/test_config_consistency.cpp` does **not** cover it: that test checks that
option names stay consistent across docs, the web UI, and config files. It never includes
`src/config.h` and never exercises parsing, defaults, or validation.

**`src/platform/linux/`: 13 of 14 files untested.** Only `wayland.cpp` has a test
(`tests/unit/platform/linux/test_wayland.cpp` — two cases, both on monitor mode selection).
Untested: `kmsgrab.cpp`, `wlgrab.cpp`, `vaapi.cpp`, `pipewire.cpp`, `portalgrab.cpp`,
`kwingrab.cpp`, `x11grab.cpp`, `cuda.cpp`, `graphics.cpp`, `vulkan_encode.cpp`,
`misc.cpp`, `publish.cpp`, `audio.cpp`

(An earlier revision said "12 of 14", which contradicted its own "only `wayland.cpp` has a
test", and omitted `audio.cpp` from the list. `tests/unit/test_audio.cpp` covers the
cross-platform `src/audio.cpp`, not the Linux backend. Likewise
`tests/unit/platform/test_common.cpp` and `test_virtualhid_input.cpp` are cross-platform.)

`kmsgrab.cpp` is the path this project actually streams through on Wayland, and an
untested display-enumeration bug in it blacked out both monitors during development.
Upstream has since fixed that class of bug (`fix(linux/kms): use same methodology for
display name matching and list generation`) — with no test guarding it.

### Rules

1. **Every change to `src/meow/` ships with tests.** No exceptions, no "I'll add them
   after".
2. **Every bug fix starts with a failing test** that reproduces it. If you cannot
   write one, say so explicitly and explain why — do not skip silently.
3. **Touching an untested upstream module?** Add characterization tests for the
   existing behaviour *first*, so you can prove your change didn't alter it.
4. **Prefer testing real behaviour over mocks.** A test that asserts a mock was called
   proves nothing about whether streaming works.
5. **Display/capture logic must be testable without hardware.** Enumeration, name
   matching, and mode selection are pure functions over device descriptions — extract
   them so they can be tested. That is what would have caught the KMS bug.

### Priority order for new tests

1. `config.cpp` — parsing, defaults, validation of untrusted input. Fully untested today.
2. `nvhttp.cpp` **beyond pairing** — the client-list, app-list, and launch endpoints.
   Pairing is already covered by `test_http_pairing.cpp`; the rest of this security
   boundary is not.
3. KMS/Wayland display enumeration and selection.
4. `rtsp.cpp` — session setup.
5. Everything else.

---

## 6. The gate — run before every push

```bash
cmake -B build -G Ninja -DBUILD_TESTS=ON
cmake --build build
./build/tests/test_sunshine        # full suite must pass
```

Nothing gets pushed on a red gate. "It built" is not "it works" — for any change to
capture, encoding, or input, also stream once and confirm the picture and the
keyboard actually work.

---

## 7. Security

This server accepts network connections and injects synthetic input into the host
desktop. Treat every boundary as hostile.

- **Never weaken pairing or auth** to make development easier. Not temporarily.
- **Validate all input from the network** — client-supplied resolutions, codecs, and
  paths reach real APIs.
- **Never log secrets** — no keys, PINs, tokens, or certificates in log output.
- **`upnp` stays off by default.** Automatic port-forwarding punches a hole in the
  user's router and silently defeats a Tailscale-only deployment.
- **Bind narrowly.** The server listening on `0.0.0.0` means a firewall is the only
  thing between it and the LAN.
- **New dependencies need justification** in the PR. Each one is attack surface.
- Run the security-relevant tests (`test_crypto`, `test_http_pairing`,
  `test_confighttp`) on any change near auth, and say in the PR that you did.

---

## 8. Working in parallel

- One feature per branch, each confined to its own `src/meow/` module.
- Two agents must not edit the same upstream file in the same cycle — coordinate
  through `TOUCHPOINTS.md` before touching upstream code.
- Rebase on the synced base before opening a PR; never merge a stale branch.

---

## 9. Reporting

State what you verified and how, with command output. Distinguish:

- **verified** — ran it, here is the output
- **assumed** — believed, not checked
- **not done** — and why

"Should work" is not a status. If a step was skipped, say which and why. A report
that hides a skipped gate is worse than no report, because it stops anyone else
from checking.
