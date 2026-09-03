# sunmeow

**sunmeow** is [meowerse](https://github.com/meowerse)'s fork of
[LizardByte/Sunshine](https://github.com/LizardByte/Sunshine) — the **host** side of a
self-hosted, GPU-accelerated desktop streaming setup.

It pairs with **[moonmeow](https://github.com/meowerse/moonmeow)**, our Android client
(a Moonlight fork). sunmeow captures and encodes the desktop; moonmeow decodes and sends
input back. The intended deployment is a Linux desktop streamed to a phone over
[Tailscale](https://tailscale.com/) for real coding and office work — not a LAN game session.

> Upstream's own documentation stays in [`README.md`](./README.md) and [`docs/`](./docs).
> This file covers only what is specific to our fork. See [`CLAUDE.md`](./CLAUDE.md) for the
> rules every contributor (human or agent) works under.
>
> Note that `docs/Doxyfile` sets `USE_MDFILE_AS_MAINPAGE = ../README.md` and lists its inputs
> explicitly (no `RECURSIVE`). Editing upstream's `README.md` would therefore rewrite the
> published documentation homepage — another reason this file is separate. This file and
> `docs/meow/TOUCHPOINTS.md` are deliberately **not** in that input list; they are contributor
> docs, not published API docs. Please keep it that way.

---

## Licence and attribution

sunmeow is distributed under the **GNU General Public License v3.0**, the same licence as
Sunshine. See [`LICENSE`](./LICENSE) and [`NOTICE`](./NOTICE).

- **[LizardByte/Sunshine](https://github.com/LizardByte/Sunshine)** (GPLv3) — the upstream
  this fork is based on. Nearly all code here is theirs.
- **[ClassicOldSong/Apollo](https://github.com/ClassicOldSong/Apollo)** (GPLv3) — a Sunshine
  fork we use strictly as a **reference implementation**. No Apollo code is merged into this
  tree. See below for why.
- **[moonlight-stream](https://github.com/moonlight-stream)** — `moonlight-common-c`, the
  streaming protocol core, vendored as a submodule.

---

## Why Sunshine, and not Apollo

**This is the single most important decision recorded in this repository. Do not silently
undo it.**

sunmeow was originally forked from **Apollo**, because Apollo has features we wanted. That
base was measured, rejected, and replaced with upstream Sunshine. The decision was made on
numbers, not taste.

### The measurements

Measured **2026-08-24** against Apollo `master` and Sunshine `master`. The fork point is the
merge base of the two.

| Measurement | Value |
| --- | --- |
| Apollo's fork point from Sunshine | `1a96d135`, **2025-09-26** |
| Commits Apollo is **behind** Sunshine | **616** |
| Commits Apollo added itself since forking | 457 |
| Commits touching Linux/Wayland/KMS/NVENC that Apollo is missing | **146** — of which **55** are `fix(…)` |
| Commits touching Linux/Wayland/KMS/NVENC that Apollo contributed | **14** |
| Files that **conflict** on a Sunshine→Apollo merge | **103** |
| Apollo's headline feature (virtual display) | **Windows only** |

Reproduce them — do not trust this table, re-derive it:

```bash
# NOTE: check `git remote -v` first. In this clone the remotes are currently named
#   origin-upstream -> LizardByte/Sunshine     (the base we track)
#   upstream        -> ClassicOldSong/Apollo   (reference only — NEVER merge)
# The names are local config and are easy to get backwards. Confirm before running anything.
SUNSHINE=origin-upstream/master
APOLLO=upstream/master

mb=$(git merge-base $APOLLO $SUNSHINE)
git log -1 --date=short --format='%h %ad' $mb        # fork point
git rev-list --count $mb..$SUNSHINE                  # Apollo behind Sunshine
git rev-list --count $mb..$APOLLO                    # Apollo's own commits
git log --oneline $mb..$SUNSHINE | grep -ciE 'linux|wayland|kms|nvenc'
git log --oneline $mb..$APOLLO   | grep -ciE 'linux|wayland|kms|nvenc'

# conflict set, computed WITHOUT touching the working tree.
# `--name-only` prints: <tree-oid>, then the conflicted paths, then a BLANK LINE, then an
# informational "Auto-merging / CONFLICT" block. Count only the paths, or you will count the
# commentary too (that mistake yields 320, or 337 if stderr is folded in).
git merge-tree --write-tree --name-only $APOLLO $SUNSHINE 2>/dev/null \
  | sed -n '2,/^$/p' | grep -c .          # -> 103
git merge-tree --write-tree --name-only $APOLLO $SUNSHINE 2>/dev/null \
  | grep -c '^CONFLICT'                   # -> 103, independent cross-check
```

Note the asymmetry the raw counts invite: 146 and 14 come from the *same* subject grep, so
compare like with like. The honest sharpening is that 55 of Sunshine's 146 are `fix(…)`
commits (the rest being 57 `build`, 25 `feat`, and 9 assorted), while Apollo's 14 include
duplicate and revert commits.

If the repository is shallow (`git rev-parse --is-shallow-repository` prints `true`), every
count above will be wrong — usually absurdly low. Run `git fetch --unshallow <remote>` first.

### Why the merge is not merely large, but impossible

The Sunshine→Apollo merge was attempted and **deliberately aborted**. It failed not on volume
but on **mutually exclusive API rewrites**. Two examples decided it:

```cpp
// src/crypto.h — the two signatures cannot coexist
Apollo:    void add(p_named_cert_t &named_cert_p);  // underpins Apollo's permission system
Sunshine:  void add(x509_t &&cert);                 // Sunshine's rewritten cert handling
```

```cpp
// src/platform/linux/wlgrab.cpp — Sunshine fixed a real Wayland timestamp bug Apollo still has
Apollo:    img->frame_timestamp = frame_timestamp;                 // local steady_clock::now()
Sunshine:  img->frame_timestamp = current_frame->frame_timestamp;  // the frame's actual timestamp
```

`src/platform/linux/vaapi.cpp` is a full rewrite of rate-control selection on **both** sides
(+226/−64 between the two trees). There is no resolution that preserves both intents.

Apollo's flagship feature — virtual display — is Windows-only. Its value to a Linux/Wayland
deployment was near zero, which is what made the 616-commit deficit indefensible.

### The bonus nobody predicted: the protocol core moved off a fork

Re-basing onto Sunshine did more than recover those 146 Linux/Wayland/KMS/NVENC commits. It also **advanced the
streaming protocol core by 39 commits and moved it from a fork back onto the original
project** — automatically, as a side effect of the submodule pin changing.

| | Apollo base (`adc5c5a0`) | sunmeow on Sunshine |
| --- | --- | --- |
| `moonlight-common-c` source | `ClassicOldSong/moonlight-common-c` (**a fork**) | `moonlight-stream/moonlight-common-c` (**the original**) |
| Pinned commit | `c999436` (2025-09-01) | `874ac95` (2026-08-18) |
| Distance from upstream HEAD | **39 commits behind** | **0 — is upstream HEAD** |

Among those 39 commits Apollo was missing:

- `7b026e7` **Harden RTSP handling for malformed Session headers and oversized responses**
  — a **security** fix in network-facing parsing code.
- `518b244` Rewrite MbedTLS codepaths to use the modern PSA APIs.
- FEC and network library modernisation: nanors with native SIMD and GFNI runtime dispatch
  (`1f76427`, `2ea4775`, `e41355e`) and several enet bumps (`6268780`, `703a069`).

Verify:

```bash
cd third-party/moonlight-common-c
git remote add real https://github.com/moonlight-stream/moonlight-common-c.git
git fetch real
git rev-list --count c999436858471dfefa7617af3b7dc03ec1644ce4..real/master   # -> 39
git log --oneline c999436858471dfefa7617af3b7dc03ec1644ce4..real/master | grep -i rtsp
```

**"Moonlight upstream is dead" is false.** It is true only of the Android app
(`moonlight-android`: last commit on `master` 2024-07-27, last release v12.1 in Feb 2024).
The protocol core is actively maintained — `moonlight-common-c` and `moonlight-qt` both saw
commits in **August 2026**. Never generalise the Android app's dormancy to the org.

### Adopting an Apollo feature

Port it **deliberately**, as an isolated module under the additive-only rules in
[`CLAUDE.md` §2](./CLAUDE.md) — never by merging Apollo. Read their implementation with:

```bash
git show upstream/master:src/some_file.cpp     # 'upstream' = Apollo in this clone; verify first
```

---

## Building on Linux (Arch / CachyOS)

Upstream's full instructions live in [`docs/building.md`](./docs/building.md). What follows is
only what the target machine needs.

Reference platform: **CachyOS (Arch)**, **KDE Plasma 6 on Wayland**, hybrid GPU —
**NVIDIA RTX 5050 (encode)** + **AMD iGPU (display)**.

```bash
sudo pacman -S --needed base-devel cmake ninja git \
  boost openssl libcap libdrm libevdev libva libvdpau \
  wayland wayland-protocols libx11 libxcb libxfixes libxrandr libxtst \
  opus curl miniupnpc numactl avahi doxygen graphviz \
  nodejs npm python vulkan-headers vulkan-icd-loader shaderc \
  cuda            # NVENC — pulls the full CUDA toolkit
```

```bash
git clone --recurse-submodules https://github.com/meowerse/sunmeow.git
cd sunmeow
cmake -B build -G Ninja -DBUILD_TESTS=ON
cmake --build build
./build/tests/test_sunshine      # the gate — see CLAUDE.md §6
```

If you cloned without `--recurse-submodules`:
`git submodule update --init --recursive`.

### Known build blockers

- **`upload-pack: not our ref` on a submodule means YOUR module config is stale — not that
  upstream is broken.** If `git submodule update --init --recursive` fails like this:
  ```
  fatal: remote error: upload-pack: not our ref 546895a9...
  fatal: Fetched in submodule path 'third-party/Simple-Web-Server', but it did not contain 546895a9...
  ```
  the pin is fine; your `.git/modules/<path>/config` still points at the remote from an older
  base (for a clone that once tracked Apollo, `ClassicOldSong/*`). `.gitmodules` at `HEAD` is
  correct, but the module's own config is not re-read automatically. Fix:
  ```bash
  git submodule sync --recursive
  git submodule update --init --recursive
  ```
  CMake otherwise aborts with "does not contain a CMakeLists.txt file", which looks like an
  upstream packaging bug and is not one. (`git submodule sync` skips submodules you have
  deinitialised — re-run `git submodule init <path>` first if you have.)
- **CUDA is required by default.** Without the toolkit, configure fails with `CUDA not found`.
  Pass `-DSUNSHINE_ENABLE_CUDA=OFF` only for non-NVENC work — the reference machine needs it.
- **Vulkan headers** come from the `build-deps` submodule; if that submodule is not fully
  checked out, use `-DSUNSHINE_SYSTEM_VULKAN_HEADERS=ON` or `-DSUNSHINE_ENABLE_VULKAN=OFF`.
- `-DBUILD_DOCS=OFF` skips the hard Doxygen ≥ 1.10 + Graphviz requirement.

> Once it builds and the gate passes, [`docs/meow/going-live.md`](./docs/meow/going-live.md)
> covers the part this section does not: moving the machine off the packaged Sunshine onto this
> fork, the way back, and turning the two features on.

### Running on KDE Plasma 6 / Wayland

Capture backend selection is the thing that actually goes wrong here.

- **KMS capture (`kmsgrab`) is the path this project streams through on Wayland.** The binary
  must be patched with `setcap`; packaged installs handle it. For a local build, use
  upstream's exact procedure from [`docs/building.md`](./docs/building.md) — the copy through
  `/tmp` exists because `setcap` fails on filesystems without xattr support, and
  `cap_sys_nice` is needed alongside `cap_sys_admin`:
  ```bash
  sudo cp build/sunmeow /tmp
  sudo setcap cap_sys_admin,cap_sys_nice+p /tmp/sunmeow
  sudo getcap /tmp/sunmeow
  sudo mv /tmp/sunmeow build/sunmeow
  ```
  Without it, capture silently falls back or fails.
- Plasma also exposes `kwingrab` and the XDG portal path (`portalgrab`/`pipewire`). If the
  picture is black or the wrong monitor appears, the display **enumeration** is the first
  suspect — see the KMS caveat below.
- On a **hybrid GPU**, the encoder and the display can sit on different devices. Confirm which
  card is which before blaming the encoder:
  ```bash
  ls -l /dev/dri/by-path/       # map cards to PCI devices
  vainfo                        # VAAPI (AMD iGPU) capability
  nvidia-smi                    # NVENC (RTX 5050) visibility
  ```
  Encode on the NVIDIA card while the desktop is composited by the iGPU requires the captured
  frames to cross devices; if NVENC is unavailable, sunmeow falls back to VAAPI on the iGPU.
- **Set `upnp` off.** It is off by default and must stay off — automatic port forwarding
  punches a hole in the router and defeats a Tailscale-only deployment
  ([`CLAUDE.md` §7](./CLAUDE.md)).
- Web UI: `https://localhost:47990` (self-signed certificate on first run).

> **KMS display-enumeration caveat.** An enumeration bug in `kmsgrab.cpp` blacked out both
> monitors during development. Upstream has since fixed that class of bug
> (`fix(linux/kms): use same methodology for display name matching and list generation`,
> `fix(linux/kms)!: Use connector type index from KMS instead of self calculation` — both
> present in this base). There is still **no test** guarding it; see
> [`CLAUDE.md` §5](./CLAUDE.md).

---

## How this fork is structured

sunmeow is **additive-only**: our code goes in new files (`src/meow/`), and upstream files are
left untouched wherever possible. That is what keeps upstream fixes mergeable — and it is
precisely the discipline Apollo lacked.

Every unavoidable edit to an upstream file is marked `MEOW-TOUCH` in place and listed in
[`docs/meow/TOUCHPOINTS.md`](./docs/meow/TOUCHPOINTS.md). Two upstream non-source files
(`AGENTS.md` and `.gitignore`) are appended to — never rewritten, zero deletions — and both are
declared in that registry.

**This section used to claim "no upstream C++ source is modified at all". That stopped being
true on 2026-08-25 and the claim has been removed rather than quietly weakened**, because a
contributor who believed it would skip the one check that matters on a sync. Upstream C++ *is*
now touched, in a small and deliberately-argued set of files. Do not restate a count here — it
goes stale exactly the way the last one did. Derive it:

```bash
git grep -l 'MEOW-TOUCH' -- src/                  # files a merge can hurt
git diff --numstat origin-upstream/master -- src/ # any non-zero DELETION count is a red flag
```

The registry records, per file, *why layers 1–3 of the additive-only hierarchy were
insufficient*. That justification is the point of the table; the count is not. Two upstream C++
files currently have a non-zero deletion count, for two different reasons — one deliberate and
capped, one merely lines modified in place — and the registry explains which is which. Read it
there rather than trusting a number restated here.

The full rules — the additive-only hierarchy, what needs human sign-off, and the testing
requirements — are in [`CLAUDE.md`](./CLAUDE.md). Read it before your first edit.

---

## Syncing with upstream

**Monthly at minimum.** Apollo's failure was 11 months of drift, not one bad merge.

The procedure — including the conflict dry-run, the backup branch, the rules against
`--ours`/`--theirs`, and the post-merge loss checks — is
**[`CLAUDE.md` §4](./CLAUDE.md)**. It is not duplicated here on purpose: one copy, one source
of truth.

Two things worth repeating, because getting either wrong is expensive:

1. **Confirm your remote names before you fetch.** In this clone `upstream` is *Apollo*, not
   Sunshine. Running a sync against the wrong remote would merge the very thing this document
   exists to keep out.
2. **Measure drift against the ORIGINAL upstream, never the fork you branched from** — and
   that applies to submodules too. A submodule's `origin` can point at someone's fork while
   `git submodule status` reports perfectly clean. That is exactly how Apollo's protocol core
   drifted 39 commits behind without anyone noticing.
