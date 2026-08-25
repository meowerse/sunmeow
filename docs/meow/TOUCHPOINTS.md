# Upstream touch-point registry

Every edit to a file **we did not create** is recorded here, and marked in place in the source
with a `MEOW-TOUCH` comment. This registry is the complete list of places a future upstream
merge can hurt.

Required by [`CLAUDE.md` §3](../../CLAUDE.md). Read [`CLAUDE.md` §2](../../CLAUDE.md) — the
additive-only prime directive — before adding a row.

---

## Current state: no upstream file has content **removed**

Verified 2026-08-25 against `origin-upstream/master` (Sunshine `790d70f0`).

Six upstream **C++ sources** are now modified: two by `unified-desktop-capture` and four by `viewport` (two of them one and two lines respectively, to register a setting). An earlier
revision of this file said *"Zero `MEOW-TOUCH` markers exist in `src/`"* and described that as
the state to keep. The first half stopped being true the moment whole-desktop capture landed;
the second half is still the goal. `git grep -n 'MEOW-TOUCH' -- src/ src_assets/` is the authority — if
this table disagrees with it, this table is wrong.

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `src/platform/linux/kwingrab.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | The `wl_display`, the registry and the `zkde_screencast_unstable_v1` proxy are all private members of `kwin::screencast_t`, which is defined inside the `.cpp`, is not virtual and is not exported — so it cannot be subclassed or wrapped from `src/meow/` (layers 1–3 all fail). What remains upstream: the `zxdg_output_manager_v1`/`xdg_output` bindings, extra fields on `output_parameter_t`, the `stream_region` call, `static_assert`s pinning `meow::display_union::output_transform_t` to `enum wl_output_transform`, and the log statements that report the decision. The `xdg_output` logical position is stored in its own fields and never written back over `pos_x`/`pos_y`, so binding the protocol is unobservable to single-output capture. **All geometry and all policy** — the bounding box, the scale choice, the coverage test, the protocol-version gate and the oversize refusal — live in `src/meow/display_union.h` and are unit tested there. `kwin_t::verify_and_update_display_parameters()` is a layer-2 override of an existing `virtual`, not new upstream logic. | 2026-08-24 |
| `src/platform/linux/misc.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | Two hooks: an include, and three lines after capture-source selection that log a warning when `output_name` requests whole-desktop capture on a backend that cannot provide it. Layer 1 fails because the fact being reported — which capture source won — exists only in this function's `sources` bitset, which is file-static and not exported. Layer 2 has nothing to subclass (`init()` is a free function). The decision itself is layer 1: `meow::display_union::union_backend_warning()` returns the message as a string and is unit tested; the upstream lines only choose whether to log it. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(viewport)` | Five hooks, `0` deletions: an include, a `control_viewport_t` packed struct, `send_viewport()`, one registration call, one drain of pending revocation echoes inside the existing per-session loop, and one `reset()` call. Layer 1 fails because everything the echo needs — `control_header_v2`, the `#pragma pack(1)` region, `encode_control()`, `session_t`, `control_server_t` — is defined inside this `.cpp` and is not exported, so the payload cannot be framed or encrypted from `src/meow/`. Layer 2 has nothing to subclass (`controlBroadcastThread` is a free function and `send_hdr_mode()` is its sibling). What is *not* here: the packet number, the payload layout, the parsing, the validation, the reference-frame coordinate transform (the wire is in negotiated-stream-resolution pixels, not desktop pixels) and every geometry decision all live in `src/meow/viewport.h` and are unit tested. Upstream's `packetTypes` array is **not modified** — the handler is registered by value via `meow::viewport::map_request_handler()`, which is handed the array only so it can refuse to register on a collision. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(viewport)` | Three hooks plus an include, `0` deletions, all inside `avcodec_software_encode_device_t`: re-plan at the top of `convert()`, shift the plane pointers to the crop origin before `sws_scale_frame()`, and publish the scaler geometry in `init()`. Notably **not** a fourth hook: `prefill()` is *not* reused to re-blacken the surface per crop change, because `av_frame_get_buffer()` is documented to leak on an already-allocated frame; `meow::viewport::reblack()` does the half that is wanted, and `MeowViewportUpstream.CroppedConvertFillsTheStripAndDoesNotReallocate` fails if that is ever undone. Layers 1–3 all fail for the same reason — `sws_input_frame`, `sws_output_frame`, `offsetW`, `offsetH`, `prefill()` and `reinit_sws()` are private members of a class declared in `src/video.h` and constructed by `make_avcodec_encode_session()`, so they can only be reached from inside a member function. The hooks contain no geometry: `meow::viewport::plan_for_frame()` decides, `configure_scaler()` writes the six fields, and `offset_source_planes()` does the pointer arithmetic, all unit tested against real `AVFrame`s and a real pixel buffer in `tests/unit/meow/test_viewport.cpp`. | 2026-08-25 |
| `src/config.h` | `MEOW-TOUCH(viewport)` | **One line**: a `bool viewport_following` member on `video_t`. Layers 1–3 cannot register a setting — `apply_config()` writes into this struct by reference and nothing else can be substituted for it. The alternative was tried and reverted: parsing the key out of the config file from `src/meow/` alone touched no upstream file, but Sunshine's own parser then never learned the key existed and logged `Unrecognized configurable option [meow_viewport_following]` at every startup, telling a user who had just enabled the feature that the setting does not exist. §2's hierarchy exists to keep merges cheap, not to make the product worse. Registering it is also the only route to the Web UI. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(viewport)` | **Two lines**, both additions to existing lists: `false,  // viewport_following` in the `video` default initializer, and one `bool_f(vars, "meow_viewport_following", …)` next to `output_name`. Nothing is restructured, so a concurrent edit elsewhere in the same table conflicts only if it lands on the same two lines. `tests/integration/test_config_consistency.cpp` then *requires* the matching entries in `configuration.md`, `config.html` and `en.json` below — they are not optional extras. | 2026-08-25 |
| `src_assets/common/assets/web/config.html` | (no marker — one line in a JS defaults object) | One key in the `av` tab's defaults, positioned after `output_name` so the ordering matches `configuration.md`; `ConfigConsistencyTest.ConfigOptionsInSameOrderWithinSections` enforces that. | 2026-08-25 |
| `src_assets/common/assets/web/public/assets/locale/en.json` | (no marker — JSON) | Two strings, label and description, inserted in the existing alphabetical order. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles`. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/AudioVideo.vue` | `MEOW-TOUCH(viewport)` | One `<Checkbox>` between `DisplayOutputSelector` and `DisplayDeviceOptions`, matching the documented option order. Without it the setting exists but has no control, which is the same "supported option the product disowns" failure in a different place. | 2026-08-25 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description, and (2026-08-25) adds the `meow_viewport_following` section after it. A separate page would leave the settings undocumented where users actually look, and the consistency test requires this one to be here. | 2026-08-24 |

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

Everything else we have added is a genuinely new file: `CLAUDE.md`, `README.meow.md`, this
file, `docs/meow/viewport-following.md`, `src/meow/*.h` and `tests/unit/meow/*.cpp`.

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
git grep -n 'MEOW-TOUCH' -- src/ src_assets/

# 2. every upstream file we differ from at all, and by how much.
#    Any non-zero DELETION count is a red flag: we removed upstream content.
git diff --numstat origin-upstream/master -- .
```

Run both **before every upstream sync** ([`CLAUDE.md` §4](../../CLAUDE.md)). The first grep
now returns the `unified-desktop-capture` and `viewport` markers; reconcile them against the
table above row by row. Any marker the table does not list is the bug.

What matters in (2) is that **every deletion count is `0`** — we only ever add lines to
upstream files. The insertion counts change with every edit to our own docs, so do not treat
them as fixed; check the middle column:

```
1	0	.gitignore
17	0	AGENTS.md
<n>	0	src/platform/linux/kwingrab.cpp
<n>	0	src/platform/linux/misc.cpp
<n>	0	src/stream.cpp
<n>	0	src/video.cpp
1	0	src/config.h
2	0	src/config.cpp
1	0	src_assets/common/assets/web/config.html
2	0	src_assets/common/assets/web/public/assets/locale/en.json
<n>	0	src_assets/common/assets/web/configs/tabs/AudioVideo.vue
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
