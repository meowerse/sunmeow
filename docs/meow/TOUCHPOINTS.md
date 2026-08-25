# Upstream touch-point registry

Every edit to a file **we did not create** is recorded here, and marked in place in the source
with a `MEOW-TOUCH` comment. This registry is the complete list of places a future upstream
merge can hurt.

Required by [`CLAUDE.md` §3](../../CLAUDE.md). Read [`CLAUDE.md` §2](../../CLAUDE.md) — the
additive-only prime directive — before adding a row.

---

## Current state: no upstream file has content **removed**

Verified 2026-08-25 against `origin-upstream/master` (Sunshine `790d70f0`).

Six upstream **C++ sources** are now modified: two by `unified-desktop-capture`
(`kwingrab.cpp`, `misc.cpp`) and four by `adaptive-bitrate` (`config.h`, `config.cpp`,
`stream.cpp`, `video.cpp`). An earlier revision of this file said *"Zero `MEOW-TOUCH` markers
exist in `src/`"* and described that as the state to keep. The first half stopped being true
the moment whole-desktop capture landed; the second half is still the goal.
`git grep -n 'MEOW-TOUCH' -- src/` is the authority — if this table disagrees with it, this
table is wrong.

`adaptive-bitrate` adds 44 inserted lines across those four files and deletes none. Its four
hooks are a config field pair, a config parse-and-warn call, one control-message handler
registration, and one governor construction plus a per-frame `tick()`. Every decision those
hooks reach — payload validation, bounds resolution, config correction, and the entire
control law — lives in `src/meow/adaptive_bitrate.h`, which has no GPU, network, FFmpeg or
Sunshine dependency and is unit tested by `tests/unit/meow/test_adaptive_bitrate.cpp`.

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `src/platform/linux/kwingrab.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | The `wl_display`, the registry and the `zkde_screencast_unstable_v1` proxy are all private members of `kwin::screencast_t`, which is defined inside the `.cpp`, is not virtual and is not exported — so it cannot be subclassed or wrapped from `src/meow/` (layers 1–3 all fail). What remains upstream: the `zxdg_output_manager_v1`/`xdg_output` bindings, extra fields on `output_parameter_t`, the `stream_region` call, `static_assert`s pinning `meow::display_union::output_transform_t` to `enum wl_output_transform`, and the log statements that report the decision. The `xdg_output` logical position is stored in its own fields and never written back over `pos_x`/`pos_y`, so binding the protocol is unobservable to single-output capture. **All geometry and all policy** — the bounding box, the scale choice, the coverage test, the protocol-version gate and the oversize refusal — live in `src/meow/display_union.h` and are unit tested there. `kwin_t::verify_and_update_display_parameters()` is a layer-2 override of an existing `virtual`, not new upstream logic. | 2026-08-24 |
| `src/platform/linux/misc.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | Two hooks: an include, and three lines after capture-source selection that log a warning when `output_name` requests whole-desktop capture on a backend that cannot provide it. Layer 1 fails because the fact being reported — which capture source won — exists only in this function's `sources` bitset, which is file-static and not exported. Layer 2 has nothing to subclass (`init()` is a free function). The decision itself is layer 1: `meow::display_union::union_backend_warning()` returns the message as a string and is unit tested; the upstream lines only choose whether to log it. | 2026-08-25 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description. A separate page would leave the setting undocumented where users actually look. | 2026-08-24 |
| `src/config.h` | `MEOW-TOUCH(adaptive-bitrate)` | Two `int` fields added next to `max_bitrate`. `config::video` is a plain aggregate struct with a positional brace initializer in `config.cpp`; there is no registration hook, no virtual, and nothing to subclass, so layers 1–3 all fail for "add a setting". Zero deletions — the fields are appended after `max_bitrate`, before `minimum_fps_target`. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | Three hooks: an include, two entries in the positional defaults initializer (which must stay positionally aligned with `config.h`, so it cannot move to another file), and two `int_f` calls plus a one-line call to `meow::adaptive_bitrate::validate_config()`. **All validation logic is layer 1** — `validate_config()` lives in `src/meow/adaptive_bitrate.h` and is unit tested against negatives, zero, inverted ranges and absurd magnitudes; the upstream lines only parse and log what it returns. `apply_config()` is a free function in an anonymous namespace with no extension point. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include plus one `server->map()` registration for inbound `0x5502` (`SS_FRAME_FEC_STATUS`). Layer 1 fails because `control_server_t` is defined inside the `.cpp` and is not exported, so the handler table can only be populated from `controlBroadcastThread()`. The handler body is three lines and contains no logic: parsing, length validation and hostile-input rejection are all in `meow::adaptive_bitrate::parse_frame_fec_status()`, which is unit tested against every payload length from 0 to 64 and against arithmetic no honest client can produce. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include, two lines constructing a `meow::adaptive_bitrate::governor_t` in `encode_run()` (placed after `fail_guard` so the guard, which can move the session away on teardown, destructs before the governor borrowing its codec context), and one `ab_governor.tick()` call in the encode loop. 10 insertions, 0 deletions — kept deliberately tiny because `viewport-host` is editing this file in the same cycle (CLAUDE.md §8). Layers 1–3 fail because `avcodec_encode_session_t` is declared inside `video.cpp`, is not exported by `video.h`, and `encode_run()` is a free function in `namespace video` with no virtual to override — so the live `AVCodecContext` is reachable from nowhere else. **Every decision is layer 1**: the controller in `src/meow/adaptive_bitrate.h` is pure and unit tested without a GPU; the governor in `src/meow/adaptive_bitrate_encoder.h` only copies its answer onto the codec context. | 2026-08-25 |
| `docs/configuration.md` | (no marker — prose only) | Documents `adaptive_bitrate_min` and `adaptive_bitrate_max` next to the existing `max_bitrate`, which is where a user comparing the two will look. `tests/integration/test_config_consistency.cpp` additionally requires every option in `config.cpp` to appear in `docs/configuration.md`, `src_assets/common/assets/web/config.html` and the `en.json` locale file, **in the same order within a section** — so these three are a hard requirement of the gate, not a choice. | 2026-08-25 |
| `src_assets/common/assets/web/config.html` | (no marker — HTML) | Two default entries appended after `max_bitrate`. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` and `ConfigOptionsInSameOrderWithinSections`; a new option that is absent here fails the gate. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue` | (no marker — Vue SFC) | Two `<input type="number">` blocks appended after the existing `max_bitrate` input. Without them the settings exist in the config file and the locale file but are unreachable from the web UI, and their `en.json` strings are dead — the user asked to "set range in settings", and for this project settings means the web UI. Appended after the last existing field so the merge seam is one line. | 2026-08-25 |
| `src_assets/common/assets/web/public/assets/locale/en.json` | (no marker — JSON) | Four strings (label + description for each setting). Required by the same consistency test. English only, per the upstream rule in `AGENTS.md`. | 2026-08-25 |

Every deletion count against these files is still `0` — the edits are additive, and the
geometry and policy they hook into live in `src/meow/display_union.h`, unit tested without
hardware.

Two upstream **non-source** files are appended to. Both are append-only (`0` deletions), so
they cannot conflict except at the very end of the file, but they are upstream files and are
therefore declared here rather than described as "additive":

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `AGENTS.md` | _(none — Markdown, not source)_ | It is the entry point agents read by convention; the name is fixed by that convention, so a new file cannot replace it. Upstream's content is preserved verbatim and our fork rules are **appended** below a marker comment. | 2026-08-24 |
| `.gitignore` | _(none — not source)_ | Ignore rules only take effect in the real `.gitignore`. One line added (`.claude/worktrees/`) to keep agent worktrees out of the index. | 2026-08-24 |

Everything else we have added is a genuinely new file: `CLAUDE.md`, `README.meow.md`, and this
file.

> **History note.** An earlier revision of the base replaced `AGENTS.md` wholesale — 36 lines
> of upstream guidance deleted for a one-line pointer — and this registry then described the
> result as "zero upstream files modified". Both were wrong. The deleted rules were
> load-bearing (doxygen documentation is **required or the build fails**, `.clang-format`
> compliance, `en`-only localization, and *never open issues or PRs against the LizardByte
> organization*). Upstream's text has been restored and our rules appended instead. If you are
> tempted to overwrite an upstream file "because we replaced it anyway" — this is the example
> of why not.

---

## Verifying and regenerating this registry

The greps are the source of truth. If they disagree with the table above, **the table is
wrong**.

```bash
# 1. every declared touch-point in our source
git grep -n 'MEOW-TOUCH' -- src/

# 2. every upstream file we differ from at all, and by how much.
#    Any non-zero DELETION count is a red flag: we removed upstream content.
git diff --numstat origin-upstream/master -- .
```

Run both **before every upstream sync** ([`CLAUDE.md` §4](../../CLAUDE.md)). The first grep
now returns the `unified-desktop-capture` markers; reconcile them against the table above
row by row. Any marker the table does not list is the bug.

What matters in (2) is that **every deletion count is `0`** — we only ever add lines to
upstream files. The insertion counts change with every edit to our own docs, so do not treat
them as fixed; check the middle column:

```
1	0	.gitignore
17	0	AGENTS.md
<n>	0	src/platform/linux/kwingrab.cpp
<n>	0	src/platform/linux/misc.cpp
<n>	0	src/config.h
<n>	0	src/config.cpp
<n>	0	src/stream.cpp
<n>	0	src/video.cpp
<n>	0	src_assets/common/assets/web/config.html
<n>	0	src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue
<n>	0	src_assets/common/assets/web/public/assets/locale/en.json
<n>	0	docs/configuration.md
<n>	0	CLAUDE.md
<n>	0	README.meow.md
<n>	0	docs/meow/TOUCHPOINTS.md
```

A non-zero deletion count against `.gitignore` or `AGENTS.md` means someone removed upstream
content — investigate before syncing.

Check your remote names first — in this clone `upstream` is **Apollo**, not Sunshine:

```bash
git remote -v
```

---

## Adding a row

Do this **only** after layers 1–3 of the [§2](../../CLAUDE.md) hierarchy have genuinely
failed — a new file in `src/meow/`, a subclass or wrapper, or a single-line hook. A
multi-line upstream edit is the last resort and needs the reason stated in the PR.

1. Mark the edit in place, naming the subsystem:

   ```cpp
   // MEOW-TOUCH(clipboard): dispatch to our clipboard bridge
   meow::clipboard::on_stream_start(session);  // <- the entire edit
   ```

2. Add a row here with the file, the marker tag, **why layers 1–3 were insufficient**, and the
   date.
3. Say in the PR why no additive approach worked.

Prefer **appending** over editing in place, and never delete upstream content to make room for
ours. An append conflicts at most at one seam; a rewrite conflicts everywhere and silently
discards guidance that other people depend on.

**A growing registry is a design smell.** It means features are being welded into upstream
code instead of layered beside it — the exact failure that left Apollo unable to merge 146
Linux/Wayland/KMS/NVENC commits it needed. Keep this file short.

> Two agents must not edit the same upstream file in the same cycle. Coordinate through this
> file before touching upstream code ([`CLAUDE.md` §8](../../CLAUDE.md)).
