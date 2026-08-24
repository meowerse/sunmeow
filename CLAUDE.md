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
| Linux/Wayland/KMS/NVENC fixes Apollo was missing | 147 |
| Linux commits Apollo contributed itself | 15 |
| Files conflicting on a Sunshine→Apollo merge | 334 |
| Apollo's headline feature (virtual display) | **Windows only** |

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

The `apollo` git remote exists for reading their implementation:

```bash
git remote -v            # upstream = LizardByte/Sunshine, apollo = ClassicOldSong/Apollo
git show apollo/master:src/some_file.cpp
```

---

## 2. THE PRIME DIRECTIVE — additive-only

**Never modify an upstream file in place when you can add a new one instead.**

This is the single rule that determines whether we can keep taking upstream fixes.
Apollo is the cautionary tale: it edited upstream files freely, and 11 months later
it could not merge 147 Linux fixes it badly needed. We are one careless refactor
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
git grep -n 'MEOW-TOUCH' -- src/
```

That grep is the complete list of places a merge can hurt. Keep it short. A growing
registry is a design smell — it means features are being welded into upstream code
instead of layered beside it.

---

## 4. Syncing with upstream

Do this **monthly at minimum**. Apollo's failure was 11 months of drift, not one bad
merge. Small syncs are cheap; large ones are unresolvable.

```bash
git fetch upstream
git log --oneline HEAD..upstream/master | wc -l          # how far behind
git merge-tree --write-tree --name-only HEAD upstream/master   # conflicts, WITHOUT touching the tree
```

`git merge-tree` computes the conflict set without modifying anything. Always run it
before merging so you know the size of the job in advance.

Then:

```bash
git branch backup/sync-$(date +%Y%m%d)   # BEFORE resolving anything
git merge upstream/master
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

---

## 5. Testing — what we inherit is not enough

Sunshine ships 26 test files (GoogleTest, `tests/unit` + `tests/integration`). That is
a real suite and we keep it green. But the coverage has holes precisely where our
hardware lives.

### Known gaps as of this base commit

**Untested core modules:**
`cbs.cpp`, `config.cpp`, `globals.cpp`, `main.cpp`, `nvhttp.cpp`, `rtsp.cpp`,
`stat_trackers.cpp`, `upnp.cpp`, `video_colorspace.cpp`

`nvhttp.cpp` is the **pairing protocol** and `config.cpp` is the settings parser —
both security-relevant and both untested.

**Linux capture backends: 12 of 14 untested.** Only `wayland.cpp` has a test.
Untested: `kmsgrab.cpp`, `wlgrab.cpp`, `vaapi.cpp`, `pipewire.cpp`, `portalgrab.cpp`,
`kwingrab.cpp`, `x11grab.cpp`, `cuda.cpp`, `graphics.cpp`, `vulkan_encode.cpp`,
`misc.cpp`, `publish.cpp`

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

1. `nvhttp.cpp` — pairing and auth. Security boundary.
2. `config.cpp` — parsing, defaults, validation of untrusted input.
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
