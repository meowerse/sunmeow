# Upstream touch-point registry

Every edit to a file **we did not create** is recorded here, and marked in place in the source
with a `MEOW-TOUCH` comment. This registry is the complete list of places a future upstream
merge can hurt.

Required by [`CLAUDE.md` §3](../../CLAUDE.md). Read [`CLAUDE.md` §2](../../CLAUDE.md) — the
additive-only prime directive — before adding a row.

---

## Current state: exactly one upstream file has content **removed**

Verified 2026-08-25 against `origin-upstream/master` (Sunshine `790d70f0`).

Nine upstream **C++ sources** are now modified: two by `unified-desktop-capture`
(`kwingrab.cpp`, `misc.cpp`), four — `config.h`, `config.cpp`, `stream.cpp`, `video.cpp` —
that now carry **two** markers each, one from `adaptive-bitrate` and one from `viewport`, and
three by `viewport-cuda` (`cuda.h`, `cuda.cu`, `cuda.cpp`).
An earlier revision of this file said *"Zero `MEOW-TOUCH` markers exist in `src/`"* and
described that as the state to keep. The first half stopped being true the moment
whole-desktop capture landed; the second half is still the goal.
`git grep -n 'MEOW-TOUCH' -- src/ src_assets/` is the authority — if this table disagrees
with it, this table is wrong.

Both features deliberately kept their hooks tiny and far apart inside those four files, which
is why they merged with a conflict only in the shared include block and in this registry.
Every decision either feature makes — payload validation, bounds resolution, config
correction, crop geometry and the entire control law — lives in `src/meow/adaptive_bitrate.h`
and `src/meow/viewport.h`, neither of which has a GPU, network, FFmpeg or Sunshine
dependency, and both of which are unit tested without hardware. `viewport-cuda` follows the
same shape in `src/meow/viewport_cuda.h`, and lands on three files neither of the other two
features touches.

> **`src/platform/linux/cuda.cu` is the first upstream file we have removed lines from: 11 of
> them.** That is a real cost and it is recorded here rather than smoothed over. Every one is
> inside the two `__global__` conversion kernels, and every one is the *same expression
> generalised*: `float scale` becomes `const source_t source`, and `idX * scale` becomes
> `source.originX + idX * source.stepX`. A crop is a source **origin**, and a kernel whose
> source origin is the literal `(0, 0)` written into the arithmetic cannot be given one
> additively. The alternative that keeps the count at zero — a second pair of
> `RGBA_to_NV12_cropped` / `RGBA_to_YUV444_cropped` kernels beside the originals — was
> rejected deliberately: it would leave two copies of the colour-conversion maths, and the
> failure mode is that upstream fixes one of them and we silently ship the other. Eleven
> mechanical lines in one file, all in one place, resolvable at a glance during a merge, beats
> a duplicated kernel that nobody notices has drifted. `cuda.h` and `cuda.cpp` are still `0`
> deletions.

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `src/platform/linux/kwingrab.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | The `wl_display`, the registry and the `zkde_screencast_unstable_v1` proxy are all private members of `kwin::screencast_t`, which is defined inside the `.cpp`, is not virtual and is not exported — so it cannot be subclassed or wrapped from `src/meow/` (layers 1–3 all fail). What remains upstream: the `zxdg_output_manager_v1`/`xdg_output` bindings, extra fields on `output_parameter_t`, the `stream_region` call, `static_assert`s pinning `meow::display_union::output_transform_t` to `enum wl_output_transform`, and the log statements that report the decision. The `xdg_output` logical position is stored in its own fields and never written back over `pos_x`/`pos_y`, so binding the protocol is unobservable to single-output capture. **All geometry and all policy** — the bounding box, the scale choice, the coverage test, the protocol-version gate and the oversize refusal — live in `src/meow/display_union.h` and are unit tested there. `kwin_t::verify_and_update_display_parameters()` is a layer-2 override of an existing `virtual`, not new upstream logic. | 2026-08-24 |
| `src/platform/linux/misc.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | Two hooks: an include, and three lines after capture-source selection that log a warning when `output_name` requests whole-desktop capture on a backend that cannot provide it. Layer 1 fails because the fact being reported — which capture source won — exists only in this function's `sources` bitset, which is file-static and not exported. Layer 2 has nothing to subclass (`init()` is a free function). The decision itself is layer 1: `meow::display_union::union_backend_warning()` returns the message as a string and is unit tested; the upstream lines only choose whether to log it. | 2026-08-25 |
| `src/platform/linux/cuda.cu` | `MEOW-TOUCH(viewport-cuda)` | **The only upstream file with deletions: 11 lines, 27 added.** All inside `RGBA_to_NV12()` and `RGBA_to_YUV444()`. Layer 1 fails on physics, not on structure: the kernel computes its own source coordinate per destination pixel (`float x = idX * scale`), so the captured frame's origin is baked into that expression as the literal `0`, and no amount of code beside it can move where the kernel reads. Layer 2 has nothing to subclass — these are `__global__` functions. Layer 3 cannot be one line: the parameter, the two coordinate computations and the three neighbour taps of the 2x2 NV12 block all mention the old mapping. The edit itself is mechanical and carries **no geometry**: `float scale` becomes `const source_t source` (a POD declared in `cuda.h`), and the arithmetic becomes `source.originX + idX * source.stepX`. `{0, 0, scale, scale}` reproduces the old behaviour bit for bit, because `0.0f + v == v` for every finite float. Everything that decides *what* the origin and steps are lives in `src/meow/viewport_cuda.h` and is unit tested without a GPU; `tools/meow/viewport_cuda_probe.cpp` then checks on real hardware that the kernel reads the rectangle those numbers describe. | 2026-08-25 |
| `src/platform/linux/cuda.h` | `MEOW-TOUCH(viewport-cuda)` | **Two additions, `0` deletions**: a `source_t` POD next to the existing `viewport_t`, and a `source_t source` member on `sws_t` next to the existing `float scale`. They are declared here rather than in `src/meow/` because `cuda.cu` is compiled by nvcc at C++17 and must not include anything from `src/meow/` (`viewport.h` uses defaulted `operator==`, which is C++20 — and NVCC's own comment in `cuda.cu` warns that standard headers break it). `src/meow/viewport_cuda.h` therefore writes into these two types through a template rather than naming them, so the dependency points one way only; `MeowViewportCuda.WritesStraightIntoTheRealCudaTypes` pins the field-name agreement. Kept deliberately distinct from `viewport_t`, which is a *destination* rectangle — conflating the two spaces is the bug this whole feature is most likely to grow. | 2026-08-25 |
| `src/platform/linux/cuda.cpp` | `MEOW-TOUCH(viewport-cuda)` | **Two hooks plus two includes, `0` deletions**: one line at the top of `cuda_ram_t::convert()` and of `cuda_vram_t::convert()`, and one line at the end of `cuda_t::set_frame()`. The two member functions those lines call (`meow_viewport_init()`, `meow_viewport_apply()`) are also here, and they are the part that could not be layered: `sws`, `stream`, `frame`, `is_yuv444` and `linear_interpolation` are members of `cuda_t`, which is declared inside this `.cpp`, is not exported and is constructed by `make_avcodec_encode_device()` — so layers 1–3 all fail exactly as they do for `avcodec_software_encode_device_t` in `src/video.cpp`. They contain no geometry (`meow::viewport::plan_for_frame()` and `cuda_scaler_config()` decide, `apply_cuda_scaler()` assigns) and one piece of genuinely CUDA-specific machinery: a 2x2 all-black texture. With `cudaAddressModeClamp` every coordinate reads black from it, so re-running the *ordinary* kernel against it blanks the surface with exactly the black `apply_colorspace()` writes — instead of a host-side copy of the colour-matrix arithmetic that could drift. That pass runs only when the destination rectangle moves, which the probe shows is load-bearing: without it, reverting a crop leaves 16093 stale pixels in the letterbox padding. | 2026-08-25 |
| `src/config.h` | `MEOW-TOUCH(adaptive-bitrate)` | Two `int` fields added next to `max_bitrate`. `config::video` is a plain aggregate struct with a positional brace initializer in `config.cpp`; there is no registration hook, no virtual, and nothing to subclass, so layers 1–3 all fail for "add a setting". Zero deletions — the fields are appended after `max_bitrate`, before `minimum_fps_target`. | 2026-08-25 |
| `src/config.h` | `MEOW-TOUCH(viewport)` | **One line**: a `bool viewport_following` member on `video_t`. Layers 1–3 cannot register a setting — `apply_config()` writes into this struct by reference and nothing else can be substituted for it. The alternative was tried and reverted: parsing the key out of the config file from `src/meow/` alone touched no upstream file, but Sunshine's own parser then never learned the key existed and logged `Unrecognized configurable option [meow_viewport_following]` at every startup, telling a user who had just enabled the feature that the setting does not exist. §2's hierarchy exists to keep merges cheap, not to make the product worse. Registering it is also the only route to the Web UI. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | Three hooks: an include, two entries in the positional defaults initializer (which must stay positionally aligned with `config.h`, so it cannot move to another file), and two `int_f` calls plus a one-line call to `meow::adaptive_bitrate::validate_config()`. **All validation logic is layer 1** — `validate_config()` lives in `src/meow/adaptive_bitrate.h` and is unit tested against negatives, zero, inverted ranges and absurd magnitudes; the upstream lines only parse and log what it returns. `apply_config()` is a free function in an anonymous namespace with no extension point. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(viewport)` | **Two lines**, both additions to existing lists: `false,  // viewport_following` in the `video` default initializer, and one `bool_f(vars, "meow_viewport_following", …)` next to `output_name`. Nothing is restructured, so a concurrent edit elsewhere in the same table conflicts only if it lands on the same two lines. `tests/integration/test_config_consistency.cpp` then *requires* the matching entries in `configuration.md`, `config.html` and `en.json` below — they are not optional extras. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include plus one `server->map()` registration for inbound `0x5502` (`SS_FRAME_FEC_STATUS`). Layer 1 fails because `control_server_t` is defined inside the `.cpp` and is not exported, so the handler table can only be populated from `controlBroadcastThread()`. The handler body is three lines and contains no logic: parsing, length validation and hostile-input rejection are all in `meow::adaptive_bitrate::parse_frame_fec_status()`, which is unit tested against every payload length from 0 to 64 and against arithmetic no honest client can produce. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(viewport)` | Five hooks, `0` deletions: an include, a `control_viewport_t` packed struct, `send_viewport()`, one registration call, one drain of pending revocation echoes inside the existing per-session loop, and one `reset()` call. Layer 1 fails because everything the echo needs — `control_header_v2`, the `#pragma pack(1)` region, `encode_control()`, `session_t`, `control_server_t` — is defined inside this `.cpp` and is not exported, so the payload cannot be framed or encrypted from `src/meow/`. Layer 2 has nothing to subclass (`controlBroadcastThread` is a free function and `send_hdr_mode()` is its sibling). What is *not* here: the packet number, the payload layout, the parsing, the validation, the reference-frame coordinate transform (the wire is in negotiated-stream-resolution pixels, not desktop pixels) and every geometry decision all live in `src/meow/viewport.h` and are unit tested. Upstream's `packetTypes` array is **not modified** — the handler is registered by value via `meow::viewport::map_request_handler()`, which is handed the array only so it can refuse to register on a collision. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include, two lines constructing a `meow::adaptive_bitrate::governor_t` in `encode_run()` (placed after `fail_guard` so the guard, which can move the session away on teardown, destructs before the governor borrowing its codec context), and one `ab_governor.tick()` call in the encode loop. 10 insertions, 0 deletions — kept deliberately tiny because `viewport-host` is editing this file in the same cycle (CLAUDE.md §8). Layers 1–3 fail because `avcodec_encode_session_t` is declared inside `video.cpp`, is not exported by `video.h`, and `encode_run()` is a free function in `namespace video` with no virtual to override — so the live `AVCodecContext` is reachable from nowhere else. **Every decision is layer 1**: the controller in `src/meow/adaptive_bitrate.h` is pure and unit tested without a GPU; the governor in `src/meow/adaptive_bitrate_encoder.h` only copies its answer onto the codec context. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(viewport)` | Three hooks plus an include, `0` deletions, all inside `avcodec_software_encode_device_t`: re-plan at the top of `convert()`, shift the plane pointers to the crop origin before `sws_scale_frame()`, and publish the scaler geometry in `init()`. Notably **not** a fourth hook: `prefill()` is *not* reused to re-blacken the surface per crop change, because `av_frame_get_buffer()` is documented to leak on an already-allocated frame; `meow::viewport::reblack()` does the half that is wanted, and `MeowViewportUpstream.CroppedConvertFillsTheStripAndDoesNotReallocate` fails if that is ever undone. Layers 1–3 all fail for the same reason — `sws_input_frame`, `sws_output_frame`, `offsetW`, `offsetH`, `prefill()` and `reinit_sws()` are private members of a class declared in `src/video.h` and constructed by `make_avcodec_encode_session()`, so they can only be reached from inside a member function. The hooks contain no geometry: `meow::viewport::plan_for_frame()` decides, `configure_scaler()` writes the six fields, and `offset_source_planes()` does the pointer arithmetic, all unit tested against real `AVFrame`s and a real pixel buffer in `tests/unit/meow/test_viewport.cpp`. | 2026-08-25 |
| `src_assets/common/assets/web/config.html` | (no marker — JS defaults object) | Three keys in the `av` tab's defaults, from two features. `adaptive-bitrate`: Two default entries appended after `max_bitrate`. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` and `ConfigOptionsInSameOrderWithinSections`; a new option that is absent here fails the gate. `viewport`: One key in the `av` tab's defaults, positioned after `output_name` so the ordering matches `configuration.md`; `ConfigConsistencyTest.ConfigOptionsInSameOrderWithinSections` enforces that. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue` | (no marker — Vue SFC) | Two `<input type="number">` blocks appended after the existing `max_bitrate` input. Without them the settings exist in the config file and the locale file but are unreachable from the web UI, and their `en.json` strings are dead — the user asked to "set range in settings", and for this project settings means the web UI. Appended after the last existing field so the merge seam is one line. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/AudioVideo.vue` | `MEOW-TOUCH(viewport)` | One `<Checkbox>` between `DisplayOutputSelector` and `DisplayDeviceOptions`, matching the documented option order. Without it the setting exists but has no control, which is the same "supported option the product disowns" failure in a different place. | 2026-08-25 |
| `src_assets/common/assets/web/public/assets/locale/en.json` | (no marker — JSON) | Six strings total, from two features. `adaptive-bitrate`: Four strings (label + description for each setting). Required by the same consistency test. English only, per the upstream rule in `AGENTS.md`. `viewport`: Two strings, label and description, inserted in the existing alphabetical order. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles`. | 2026-08-25 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description, and (2026-08-25) adds the `meow_viewport_following` section after it. A separate page would leave the settings undocumented where users actually look, and the consistency test requires this one to be here. `adaptive-bitrate` additionally documents `adaptive_bitrate_min` and `adaptive_bitrate_max` next to the existing `max_bitrate`, which is where a user comparing the two will look. `tests/integration/test_config_consistency.cpp` requires every option in `config.cpp` to appear here, in `config.html` and in `en.json`, **in the same order within a section** — so these are a hard requirement of the gate, not a choice. | 2026-08-25 |

Every deletion count against these files is `0` except `src/platform/linux/cuda.cu` (11), for
the reason stated in the box above. The edits are otherwise additive, and the geometry and
policy they hook into live in `src/meow/display_union.h`, `src/meow/adaptive_bitrate.h`,
`src/meow/viewport.h` and `src/meow/viewport_cuda.h`, all unit tested without hardware.

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
upstream files. The single exception is `src/platform/linux/cuda.cu`, which is `11` and must
stay `11`: if that number grows, someone has started editing the kernel freely instead of
consuming `src/meow/viewport_cuda.h`. The insertion counts change with every edit to our own docs, so do not treat
them as fixed; check the middle column:

```
1	0	.gitignore
17	0	AGENTS.md
<n>	0	src/platform/linux/kwingrab.cpp
<n>	0	src/platform/linux/misc.cpp
1	0	.gitignore
17	0	AGENTS.md
<n>	0	src/platform/linux/kwingrab.cpp
<n>	0	src/platform/linux/misc.cpp
<n>	0	src/platform/linux/cuda.h
<n>	11	src/platform/linux/cuda.cu
<n>	0	src/platform/linux/cuda.cpp
<n>	0	src/config.h
<n>	0	src/config.cpp
<n>	0	src/stream.cpp
<n>	0	src/video.cpp
<n>	0	src_assets/common/assets/web/config.html
<n>	0	src_assets/common/assets/web/configs/tabs/AudioVideo.vue
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
