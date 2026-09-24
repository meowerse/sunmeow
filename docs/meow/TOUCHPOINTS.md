# Upstream touch-point registry

Every edit to a file **we did not create** is recorded here, and marked in place in the source
with a `MEOW-TOUCH` comment. This registry is the complete list of places a future upstream
merge can hurt.

Required by [`CLAUDE.md` §3](../../CLAUDE.md). Read [`CLAUDE.md` §2](../../CLAUDE.md) — the
additive-only prime directive — before adding a row.

---

## Current state: one upstream file has content **removed**

Verified 2026-09-24 against `origin-upstream/master` (Sunshine `c48e50e4`), immediately
after the sync that merged 97 upstream commits including five security merges (the privileged
DRM worker). The previous verification was 2026-09-03 against `41c25716` (35 commits, four
security fixes).

**What the 2026-09-24 sync changed in this registry:** the `cmake-deps` touch-point is retired
(upstream `f54f9dfc` made the Boost pin a `1.89.0` *minimum*, which is exactly what our
`1.92.0` pin was for — see "Retired touch-points" below); the four rebranded dockerfiles
became one, because upstream folded `ubuntu-*.dockerfile` into `docker/debian.dockerfile`
(`1edbbcd1`); `config.html` no longer exists — upstream turned the web UI into a single-page
app (`d23336dc`) and moved every option default into `configs/config_tabs.json`, where our
three keys now live; upstream's new Alpine packaging was rebranded like every other package;
and the `web-deps` codecov removal was re-argued, because upstream fixed both of the reasons
it was originally made for.

**This changed on 2026-08-25.** `vite.config.js` is the first upstream file we delete
*functional* content from: `4` added, `10` deleted as of 2026-09-24 (`web-deps`, removing
`codecovVitePlugin`; it was `4`/`8` before upstream grew the plugin's options).
Earlier revisions of this file opened with *"no upstream file has content removed"* and that
claim is retired rather than quietly left standing. The bar for the next one is unchanged and
deliberately high — see the row's justification below, and §2 of `CLAUDE.md`.

Two further corrections to earlier revisions, both found by *running* the greps below rather
than by reading the table:

- **`src/platform/linux/kwingrab.cpp` does not have a `0` deletion count** — it is `282` added,
  `8` deleted. Those eight are lines *modified in place*, not functionality removed, so the
  spirit of the rule held; but the number printed in the regeneration block said `0` and was
  simply wrong. They are enumerated below.
- **The documented grep did not cover the whole repository.** It scanned `src/ src_assets/`
  only, so a `MEOW-TOUCH` marker in a repo-root file such as `vite.config.js` was invisible to
  it — the marker would exist and the pre-sync check would never report it. The grep below is
  widened accordingly.

Nine upstream **C++ sources** are now modified: two by `unified-desktop-capture`
(`kwingrab.cpp`, `misc.cpp`), four — `config.h`, `config.cpp`, `stream.cpp`, `video.cpp` —
that carry **two** markers each, one from `adaptive-bitrate` and one from `viewport`, and three
by `viewport-cuda` (`cuda.h`, `cuda.cu`, `cuda.cpp`).
An earlier revision of this file said *"Zero `MEOW-TOUCH` markers exist in `src/`"* and
described that as the state to keep. The first half stopped being true the moment
whole-desktop capture landed; the second half is still the goal.
`git grep -n 'MEOW-TOUCH'` is the authority — if this table disagrees
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
| `src/platform/linux/cuda.h` | `MEOW-TOUCH(viewport-cuda)` | **Two additions, `0` deletions**: a `source_t` POD next to the existing `viewport_t`, and a `source_t source` member on `sws_t` next to the existing `float scale`. They are declared here rather than in `src/meow/` because `cuda.cu` is compiled by nvcc at C++17 and must not include anything from `src/meow/` (`viewport.h` uses defaulted `operator==`, which is C++20 — and NVCC's own comment in `cuda.cu` warns that standard headers break it). `src/meow/viewport_cuda.h` therefore writes into these two types through a template rather than naming them, so the dependency points one way only; `MeowViewportCuda.WritesStraightIntoTheRealCudaTypes` pins the field-name agreement. Kept deliberately distinct from `viewport_t`, which is a *destination* rectangle — conflating the two spaces is the bug this whole feature is most likely to grow. **One consequence a syncing agent must know:** `float scale` is now read only by `sws_t`'s own constructor, which uses it to seed `source = {0, 0, scale, scale}`. The kernels consume `source`. So an upstream change that alters how `scale` is computed still works, but an upstream change that *writes* `scale` anywhere else will compile cleanly and do nothing. The declaration carries that warning in place; check it on every sync. | 2026-08-25 |
| `src/platform/linux/cuda.cpp` | `MEOW-TOUCH(viewport-cuda)` | **Two hooks plus two includes, `0` deletions**: one line at the top of `cuda_ram_t::convert()` and of `cuda_vram_t::convert()`, and one line at the end of `cuda_t::set_frame()`. The two member functions those lines call (`meow_viewport_init()`, `meow_viewport_apply()`) are also here, and they are the part that could not be layered: `sws`, `stream`, `frame`, `is_yuv444` and `linear_interpolation` are members of `cuda_t`, which is declared inside this `.cpp`, is not exported and is constructed by `make_avcodec_encode_device()` — so layers 1–3 all fail exactly as they do for `avcodec_software_encode_device_t` in `src/video.cpp`. They contain no geometry (`meow::viewport::plan_for_frame()` and `cuda_scaler_config()` decide, `apply_cuda_scaler()` assigns) and one piece of genuinely CUDA-specific machinery: a 2x2 all-black texture. With `cudaAddressModeClamp` every coordinate reads black from it, so re-running the *ordinary* kernel against it blanks the surface with exactly the black `apply_colorspace()` writes — instead of a host-side copy of the colour-matrix arithmetic that could drift. That pass runs only when the destination rectangle moves, which the probe shows is load-bearing: without it, reverting a crop leaves 16093 stale pixels in the letterbox padding. | 2026-08-25 |
| `src/config.h` | `MEOW-TOUCH(adaptive-bitrate)` | Two `int` fields added next to `max_bitrate`. `config::video` is a plain aggregate struct with a positional brace initializer in `config.cpp`; there is no registration hook, no virtual, and nothing to subclass, so layers 1–3 all fail for "add a setting". Zero deletions — the fields are appended after `max_bitrate`, before `minimum_fps_target`. | 2026-08-25 |
| `src/config.h` | `MEOW-TOUCH(viewport)` | **One line**: a `bool viewport_following` member on `video_t`. Layers 1–3 cannot register a setting — `apply_config()` writes into this struct by reference and nothing else can be substituted for it. The alternative was tried and reverted: parsing the key out of the config file from `src/meow/` alone touched no upstream file, but Sunshine's own parser then never learned the key existed and logged `Unrecognized configurable option [meow_viewport_following]` at every startup, telling a user who had just enabled the feature that the setting does not exist. §2's hierarchy exists to keep merges cheap, not to make the product worse. Registering it is also the only route to the Web UI. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | Three hooks: an include, two entries in the positional defaults initializer (which must stay positionally aligned with `config.h`, so it cannot move to another file), and two `int_f` calls plus a one-line call to `meow::adaptive_bitrate::validate_config()`. **All validation logic is layer 1** — `validate_config()` lives in `src/meow/adaptive_bitrate.h` and is unit tested against negatives, zero, inverted ranges and absurd magnitudes; the upstream lines only parse and log what it returns. `apply_config()` is a free function in an anonymous namespace with no extension point. | 2026-08-25 |
| `src/config.cpp` | `MEOW-TOUCH(viewport)` | **Two lines**, both additions to existing lists: `false,  // viewport_following` in the `video` default initializer, and one `bool_f(vars, "meow_viewport_following", …)` next to `output_name`. Nothing is restructured, so a concurrent edit elsewhere in the same table conflicts only if it lands on the same two lines. `tests/integration/test_config_consistency.cpp` then *requires* the matching entries in `configuration.md`, `configs/config_tabs.json` (formerly `config.html`) and `en.json` below — they are not optional extras. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include plus one `server->map()` registration for inbound `0x5502` (`SS_FRAME_FEC_STATUS`). Layer 1 fails because `control_server_t` is defined inside the `.cpp` and is not exported, so the handler table can only be populated from `controlBroadcastThread()`. The handler body is three lines and contains no logic: parsing, length validation and hostile-input rejection are all in `meow::adaptive_bitrate::parse_frame_fec_status()`, which is unit tested against every payload length from 0 to 64 and against arithmetic no honest client can produce. | 2026-08-25 |
| `src/stream.cpp` | `MEOW-TOUCH(viewport)` | Five hooks, `0` deletions: an include, a `control_viewport_t` packed struct, `send_viewport()`, one registration call, one drain of pending revocation echoes inside the existing per-session loop, and one `reset()` call. Layer 1 fails because everything the echo needs — `control_header_v2`, the `#pragma pack(1)` region, `encode_control()`, `session_t`, `control_server_t` — is defined inside this `.cpp` and is not exported, so the payload cannot be framed or encrypted from `src/meow/`. Layer 2 has nothing to subclass (`controlBroadcastThread` is a free function and `send_hdr_mode()` is its sibling). What is *not* here: the packet number, the payload layout, the parsing, the validation, the reference-frame coordinate transform (the wire is in negotiated-stream-resolution pixels, not desktop pixels) and every geometry decision all live in `src/meow/viewport.h` and are unit tested. Upstream's `packetTypes` array is **not modified** — the handler is registered by value via `meow::viewport::map_request_handler()`, which is handed the array only so it can refuse to register on a collision. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(adaptive-bitrate)` | An include, two lines constructing a `meow::adaptive_bitrate::governor_t` in `encode_run()` (placed after `fail_guard` so the guard, which can move the session away on teardown, destructs before the governor borrowing its codec context), and one `ab_governor.tick()` call in the encode loop. 10 insertions, 0 deletions — kept deliberately tiny because `viewport-host` is editing this file in the same cycle (CLAUDE.md §8). Layers 1–3 fail because `avcodec_encode_session_t` is declared inside `video.cpp`, is not exported by `video.h`, and `encode_run()` is a free function in `namespace video` with no virtual to override — so the live `AVCodecContext` is reachable from nowhere else. **Every decision is layer 1**: the controller in `src/meow/adaptive_bitrate.h` is pure and unit tested without a GPU; the governor in `src/meow/adaptive_bitrate_encoder.h` only copies its answer onto the codec context. | 2026-08-25 |
| `src/video.cpp` | `MEOW-TOUCH(viewport)` | Three hooks plus an include, `0` deletions, all inside `avcodec_software_encode_device_t`: re-plan at the top of `convert()`, shift the plane pointers to the crop origin before `sws_scale_frame()`, and publish the scaler geometry in `init()`. Notably **not** a fourth hook: `prefill()` is *not* reused to re-blacken the surface per crop change, because `av_frame_get_buffer()` is documented to leak on an already-allocated frame; `meow::viewport::reblack()` does the half that is wanted, and `MeowViewportUpstream.CroppedConvertFillsTheStripAndDoesNotReallocate` fails if that is ever undone. Layers 1–3 all fail for the same reason — `sws_input_frame`, `sws_output_frame`, `offsetW`, `offsetH`, `prefill()` and `reinit_sws()` are private members of a class declared in `src/video.h` and constructed by `make_avcodec_encode_session()`, so they can only be reached from inside a member function. The hooks contain no geometry: `meow::viewport::plan_for_frame()` decides, `configure_scaler()` writes the six fields, and `offset_source_planes()` does the pointer arithmetic, all unit tested against real `AVFrame`s and a real pixel buffer in `tests/unit/meow/test_viewport.cpp`. | 2026-08-25 |
| `src_assets/common/assets/web/configs/config_tabs.json` | (no marker — JSON) | **`3` added, `0` deleted.** Three keys in the `av` tab's defaults, from two features. `adaptive-bitrate`: `adaptive_bitrate_min` and `adaptive_bitrate_max` after `max_bitrate`. `viewport`: `meow_viewport_following` after `output_name`, so the ordering matches `configuration.md`. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles` and `ConfigOptionsInSameOrderWithinSections`; a new option that is absent here fails the gate. **Moved here on 2026-09-24:** these keys used to sit in the JS defaults object inside `config.html`; upstream `d23336dc` replaced that page with the `Config.vue` SPA component and moved all option defaults into this file, and the consistency test now reads it instead. `Config.vue` itself is upstream's, unmodified. JSON admits no comment, so this row is the declaration. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue` | (no marker — Vue SFC) | Two `<input type="number">` blocks appended after the existing `max_bitrate` input. Without them the settings exist in the config file and the locale file but are unreachable from the web UI, and their `en.json` strings are dead — the user asked to "set range in settings", and for this project settings means the web UI. Appended after the last existing field so the merge seam is one line. | 2026-08-25 |
| `src_assets/common/assets/web/configs/tabs/AudioVideo.vue` | `MEOW-TOUCH(viewport)` | One `<Checkbox>` between `DisplayOutputSelector` and `DisplayDeviceOptions`, matching the documented option order. Without it the setting exists but has no control, which is the same "supported option the product disowns" failure in a different place. | 2026-08-25 |
| `src_assets/common/assets/web/public/assets/locale/en.json` | (no marker — JSON) | Six strings total, from two features. `adaptive-bitrate`: Four strings (label + description for each setting). Required by the same consistency test. English only, per the upstream rule in `AGENTS.md`. `viewport`: Two strings, label and description, inserted in the existing alphabetical order. Required by `ConfigConsistencyTest.AllConfigOptionsExistInAllFiles`. | 2026-08-25 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description, and (2026-08-25) adds the `meow_viewport_following` section after it. A separate page would leave the settings undocumented where users actually look, and the consistency test requires this one to be here. `adaptive-bitrate` additionally documents `adaptive_bitrate_min` and `adaptive_bitrate_max` next to the existing `max_bitrate`, which is where a user comparing the two will look. `tests/integration/test_config_consistency.cpp` requires every option in `config.cpp` to appear here, in `configs/config_tabs.json` (formerly `config.html`) and in `en.json`, **in the same order within a section** — so these are a hard requirement of the gate, not a choice. | 2026-08-25 |

The edits are otherwise additive, and the geometry and policy they hook into live in
`src/meow/display_union.h`, `src/meow/adaptive_bitrate.h`, `src/meow/viewport.h` and
`src/meow/viewport_cuda.h`, all unit tested without hardware.

**Counting files with non-zero deletions stopped being useful at the `rebrand` change.** Renaming
a string is a delete plus an add, and `rebrand` renames identity strings across ~20 files, so the
old list of "the two files with deletions" is permanently obsolete. Use the shape of the numbers
instead — it distinguishes the two cases the rule actually cares about:

```bash
mb=$(git merge-base origin-upstream/master HEAD)
git diff --numstat $mb HEAD -- . ':!docs' ':!package-lock.json' | awk '$2>0'
```

- **`ins` ≈ `del`** — lines *replaced in place*. Benign, and the overwhelmingly common case
  (`1 1 tests/CMakeLists.txt`, `6 6 cmake/packaging/linux.cmake`, `8 8 migrate-config.bat`).
- **`del` materially exceeds `ins`** — upstream *content removed*. Rare, and each instance must be
  argued in the table above. Currently: `vite.config.js` (`4` / `10`, the codecov plugin) and the
  `.github/workflows/*` files this fork deleted wholesale, whose rationale is in `ci.yml`'s header.
  **Known consequence, decided on 2026-09-24:** upstream now reads `.github/workflows/ci-macos.yml`
  as a test fixture on Apple only (`tests/CMakeLists.txt` `if(APPLE)`,
  `tests/integration/test_boost_version_consistency.cpp`, `packaging/sunshine.rb`), so a **macOS**
  configure with `BUILD_TESTS=ON` fails on the missing file. macOS is not a target of this fork
  (reference platform: CachyOS; CI is Arch-only), so the workflow stays deleted and no upstream
  test code is edited for it. If macOS ever becomes a target, restore that one file unmodified
  (it is `workflow_call`-only, so it never runs on its own) rather than patching the tests.
  Upstream's web UI tests, which ran from the deleted `ci-web.yml`, now run from `ci.yml`
  (`npm test`).
- **`ins` far exceeds `del`** — additive, which is the goal.

Two entries are special enough to keep calling out by name:

**`cuda.cu` — 11, and it must stay 11.** These are real edits to upstream content, argued in
the table above. If that number *grows*, someone has started editing the kernel freely instead
of consuming `src/meow/viewport_cuda.h`, and the geometry has begun leaking back into upstream
code. Treat any increase as a defect, not as progress.

**`kwingrab.cpp` — 8, and no upstream content was removed.** All eight are lines *modified in
place*, which git necessarily counts as a delete plus an add. Exactly:

- three `wl_output` listener signatures that lost `[[maybe_unused]]` because the parameter is
  now read — `on_output_geometry` (`transform`), `on_output_mode` (`refresh`), and
  `on_output_scale` (all three arguments);
- `on_output_scale`'s `// Currently unused` comment, which stopped being true;
- the fall-back `if (!output || !out_params)`, which gained a `!region.valid &&` guard;
- the `zkde_screencast_unstable_v1_stream_output(...)` call, moved unchanged into the `else`
  of a new `region.valid` branch;
- two `this->logical_width = 0;` / `logical_height = 0;` lines whose trailing comments were
  rewritten.

Verify rather than trust either list — and note the base must be the **fork point**, not
upstream `master`, or the result is contaminated by upstream's own drift:

```bash
mb=$(git merge-base origin-upstream/master HEAD)
git diff --numstat $mb HEAD -- src/ | awk '$2>0'      # should print exactly cuda.cu and kwingrab.cpp
git diff $mb HEAD -- src/platform/linux/kwingrab.cpp | grep -E '^-' | grep -v '^---'
```

For `kwingrab.cpp`, every deleted line must have a near-identical added line beside it. A
deletion that stands alone is upstream content we removed, and is the thing this check exists
to catch.

Three upstream **build-tooling** files are modified by the `web-deps` change. One of them,
`vite.config.js`, is the tree's only genuine deletion of upstream *functionality* and is argued
at length because §2 requires it:

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `vite.config.js` | `MEOW-TOUCH(web-deps)` | **The only removal of upstream functionality in the tree: `4` added, `10` deleted.** Drops upstream's `codecovVitePlugin()` call, its `import`, and the *"should be after all other plugins"* comment. Layers 1–3 all fail structurally: this file's default export is a plain object literal and the plugin list is an array literal inside it, with no registration hook, no virtual and nothing exported — a new file in `src/meow/` cannot *un*-register a plugin, and a one-line hook can only ever add one. **Re-argued 2026-09-24, because upstream fixed both original reasons.** It was removed on 2026-08-25 because `@codecov/vite-plugin` peer-capped vite at 6.x (so `npm ci` failed `ERESOLVE` on vite 8) and because its failing upload retries cost 71–82% of every web build. Upstream has since added an `overrides` entry for the first and `dryRun: GITHUB_REPOSITORY !== 'LizardByte/Sunshine'` for the second. The removal survives on what is left, **measured** with upstream's config on this sync: the plugin now does nothing for this fork (dry run, telemetry off) **except emit `sunshine-esm-stats.json` into the web output directory** — 39 KB of bundle statistics carrying the builder's absolute `outputPath` — and `cmake/packaging/common.cmake` installs that whole directory, so it would ship in every package. With it removed, the emitted tree is byte-identical to upstream's minus that one file (`diff -r` over both builds). Keeping it would also keep a devDependency and a standing `overrides` entry whose only job is an upload this fork cannot make. `package.json` and `package-lock.json` follow below. If this fork ever owns a Codecov project, this is a one-hunk revert. | 2026-08-25 |
| `package.json` | _(none — JSON, no comment syntax)_ | **`0` added, `6` deleted** (2026-09-24): the `@codecov/vite-plugin` devDependency and upstream's `overrides` block, which exists only to force that plugin's vite peer range. Every version is upstream's. JSON admits no comment, so the marker lives in `vite.config.js` and this row is the declaration. (Before the sync this row also carried our own `vite`/`marked` bumps; upstream has since overtaken both, so they are gone.) | 2026-08-25 |
| `package-lock.json` | _(none — generated)_ | **`0` added, `353` deleted** (2026-09-24): exactly the codecov subtree. Produced by `npm install --package-lock-only` starting from upstream's lock and our `package.json`, never hand-edited or hand-merged. Not a hand-authored touch-point — it is an artifact of the two rows above. | 2026-08-25 |
| `CMakeLists.txt`, `cmake/targets/common.cmake`, `cmake/prep/init.cmake`, `cmake/prep/special_package_configuration.cmake`, `cmake/packaging/linux.cmake`, `src/platform/linux/misc.cpp`, `packaging/linux/app-meow.alxnko.sunmeow.service.in`, `src_assets/linux/misc/postinst`, `packaging/linux/Arch/sunshine.install`, `packaging/linux/Arch/PKGBUILD`, `packaging/linux/copr/Sunshine.spec`, `packaging/linux/flatpak/meow.alxnko.sunmeow.yml`, `packaging/linux/AppImage/AppRun`, `packaging/linux/flatpak/scripts/additional-install.sh`, `packaging/linux/flatpak/scripts/remove-additional-install.sh`, `packaging/sunshine.rb`, `scripts/linux_build.sh`, `packaging/linux/Alpine/APKBUILD`, `packaging/linux/Alpine/sunshine.post-install`, `docker/debian.dockerfile`, `60-sunmeow.rules`, `60-sunmeow.conf` | `MEOW-TOUCH(rebrand)` | **Identity, so this fork cannot be mistaken for — or collide with — the distro `sunshine` package.** Upstream's `PROJECT_NAME`/`PROJECT_FQDN` produce a systemd unit, binary path, udev rule, modules-load conf and config directory that are **byte-identical to the packaged Sunshine's**, so both could not be installed together and desktop-portal notifications could not tell them apart. Changed: product name → `Sunmeow`, FQDN → `meow.alxnko.sunmeow`, binary → `sunmeow` (via `OUTPUT_NAME`; the CMake *target* stays `sunshine`), config dir → `~/.config/sunmeow`, and `sunmeow.conf`/`.log`/`_state.json` inside it. Nine packaging files renamed. **Deliberately unchanged**, same reasoning as moonmeow keeping its `com.limelight` namespace: C++ namespaces, `src/` layout, the CMake target, and `platf::SERVICE_NAME` (Windows-only — Linux advertises `platf::get_host_name()` over mDNS, so it is not user-visible here). **Three cross-cutting checks that could have broken silently:** the virtual-HID device name is a hardcoded `"Sunshine {}"` literal, *not* `PROJECT_NAME`, so the udev rule's `ATTRS{name}` match still fires; the client discovers by service *type* `_nvstream._tcp`, never the instance name, so pairing is unaffected; and `appdata()`'s migration branch now points at **our** old path — left at `~/.config/sunshine`, `SUNSHINE_MIGRATE_CONFIG=1` would have copied the distro package's config here and then `fs::remove_all()`'d the original. **Renaming the binary is not enough on its own — and the first correction was not either.** Seven places *name* the executable rather than build it. `OUTPUT_NAME` fixed one; `SUNSHINE_EXECUTABLE_PATH` and `Alias=` were fixed next, which stopped the generated unit starting the distro package's binary (it shipped `ExecStart=sunshine`, and nothing failed, because that binary runs). A review then found the same defect still live in five more: `postinst` and `Arch/sunshine.install` ran `setcap` against `$(which sunshine)` / `usr/bin/sunshine`, granting `cap_sys_admin,cap_sys_nice` to **another package's** binary while leaving ours with none — which presents as this fork black-screening on KMS capture, the very symptom setcap exists to prevent; the flatpak manifest and `init.cmake`'s flatpak branch named `--command=sunshine`, which `OUTPUT_NAME` no longer produces; and the Arch/copr/homebrew/`linux_build.sh` builds each passed `.../sunshine` explicitly, so they never saw the corrected default at all. **`SUNMEOW_BINARY_NAME` (`CMakeLists.txt`) now owns the name**, and a configure-time check on the generated unit (`cmake/prep/special_package_configuration.cmake`) fails the build rather than shipping this again. The standalone packaging files cannot read a CMake variable, so they remain literal and are listed below. **A second review found the sweep was still not exhaustive, and the pattern was identical every time: the file had been opened and edited, and a later occurrence in the *same* file was never searched for.** Four of the misses broke a build outright rather than merely mis-naming something — `copr/Sunshine.spec`'s `%files` still declared `%{_bindir}/sunshine` (rpmbuild hard-errors on a `%files` entry with no matching installed file), `Arch/PKGBUILD` validated `dev.lizardbyte.app.Sunshine.*` artifacts that `PROJECT_FQDN` no longer generates and ran `./sunshine --version` in `check()`, `AppImage/AppRun`'s `--install`/`--remove` `sed`-ed and deleted a unit name we do not install (so `ExecStart` was never repointed at the AppImage), and all **four** dockerfiles set `ENTRYPOINT ["/usr/bin/sunshine"]` on an image that installs `/usr/bin/sunmeow`, i.e. shipped a container that could not start. `copr/Sunshine.spec` was also listed in this row while carrying **no in-place marker at all** — a §3 violation that made it invisible to the very grep this registry calls its authority. **The detector was wrong too:** that grep was scoped `-- src/ src_assets/ vite.config.js cmake/`, which cannot see a marker under `packaging/`, `scripts/`, `docker/` — or in `CMakeLists.txt` at the repo root. It is now unscoped, and running it reveals markers in 9 files the old scope structurally could not reach. The lesson recorded for the next sweep: **enumerate with `git grep`, never by inspection.** **2026-09-24 sync:** upstream folded the three `ubuntu-*.dockerfile`s into `docker/debian.dockerfile` (renamed from `debian-trixie.dockerfile`, driven by `docker-bake.hcl`); the deletions were accepted and our two hunks (`/config` -> `.config/sunmeow`, `ENTRYPOINT /usr/bin/sunmeow`) carried onto it. Upstream also added `packaging/linux/Alpine/`, which as shipped would validate artifacts `PROJECT_FQDN` never generates, install a `60-sunshine.conf` this tree does not have, and `setcap` the distro package's binary — rebranded the same way as `Arch/PKGBUILD`. The grep also caught `scripts/linux_build.sh`'s validation step still naming `dev.lizardbyte.app.Sunshine.*`, and `copr/Sunshine.spec`'s `%files` still globbing `*-sunshine.rules` (an rpmbuild hard error, since only `60-sunmeow.rules` is installed) — both misses from the original sweep, now fixed. | 2026-08-26 |
| `src/logging.h`, `tests/CMakeLists.txt`, `tests/tests_environment.h`, `tests/unit/test_logging.cpp`, `tests/unit/test_entry_handler.cpp`, `tests/integration/test_external_commands.cpp`, `src_assets/common/assets/web/configs/tabs/Files.vue`, `src/platform/macos/misc.mm`, `src_assets/windows/misc/migration/migrate-config.bat` | `MEOW-TOUCH(rebrand)` | **One line each — the rebrand's long tail, and a §3 violation until 2026-09-03.** Eight of these upstream files were edited when the config dir and log became `sunmeow*`, but carried **no in-place marker and no row here**, so `git grep -n 'MEOW-TOUCH'` — the grep this document calls its authority — could not see them. The ninth, `src/platform/macos/misc.mm`, was added on 2026-09-03 and is a **behaviour change**, not a string: it moves the macOS config directory to `~/.config/sunmeow` with no migration, so an upgrading macOS user starts empty (pairings, `apps.json`, web UI `credentials`, `sunmeow.conf`), exactly as on Linux; the in-place comment says so. Found by a reviewer checking the *inverse* direction of the usual audit: not "is every marked file registered" (it was) but "is every modified upstream file marked" (it was not). Add that direction to the pre-sync check. Layers 1–3 do not apply: these are string literals inside upstream files (a log filename, a test assertion, a placeholder attribute, a CMake source list), with nothing to subclass or hook. **`tests/CMakeLists.txt` is the dangerous one:** upstream rewrote 43 lines of that file in the 2026-09-03 sync, and our `60-sunmeow.rules` entry survived only because the conflict did not land on that hunk. A future reshuffle would silently restore `60-sunshine.rules` and `test_external_commands` would begin verifying a file that does not exist — passing or failing for the wrong reason. `migrate-config.bat` carries the same hazard as `appdata()`: against upstream's filenames it would move the **distro package's** config into our directory. | 2026-09-03 |
| `src_assets/common/assets/web/public/assets/locale/en_GB.json` | _(none — JSON, no comment syntax)_ | **`76` changed, `0` net** — the rebrand applied to the British-English strings: `Sunshine` -> `Sunmeow`, `Moonlight` -> `Moonmeow` in text a user reads. Registered on 2026-09-03; it had been edited since the rebrand with neither a marker (JSON admits no comment) nor a row, so it was invisible to both directions of the §3 audit. **This deliberately departs from upstream's `AGENTS.md`**, which says to update no language other than `en`, explicitly excluding variants like `en-US`/`en_GB` — that rule exists to keep Crowdin the single source for translations, and it is right for *translations*. A product rename is not a translation: leaving `en_GB` alone would ship a UI that calls itself Sunshine to every British-English user. `en.json` is the only other locale that differs from upstream; the remaining ~30 are untouched and still say Sunshine, which is a known gap rather than an oversight — they are Crowdin-owned and will be overwritten by the next translation sync. | 2026-09-03 |
| `src_assets/common/assets/web/Home.vue`, `src_assets/common/assets/web/Apps.vue`, `src_assets/common/assets/web/index.html`, `src_assets/common/assets/web/Navbar.vue`, `src_assets/common/assets/web/NavbarSimple.vue`, `src_assets/common/assets/web/ResourceCard.vue`, `src_assets/common/assets/web/configs/tabs/General.vue` | `MEOW-TOUCH(web-identity)` | **The web UI told the user it was Sunshine.** Reported symptom: the update banner announced *"A new Stable Version is Available!"* comparing our build against **LizardByte/Sunshine's** releases — another product's version numbers. All logic lives in a new layer-1 file, `src_assets/common/assets/web/meow_release_check.js`. **The endpoint swap alone was not enough, and the first version of this row got the reason WRONG** — it claimed both endpoints 404. Measured against the live API: `/releases` returns **200 with `[]`**, only `/releases/latest` 404s. So upstream broke on the LIST endpoint: `.find()` ran fine on the empty array and returned `undefined`, and `new SunshineVersion(undefined, null)` then threw from the constructor's else-branch, mid-`created()`, skipping what followed in the same `try` (on Windows, the virtual-input status and licence fetches). The 404 half was harmless — its body is a truthy object, so the constructor took the `if (release)` branch and the guards suppressed the banner by accident. A `.find`-on-a-non-array TypeError is reachable only from a **403 rate-limit** body. Recorded because a reader debugging a TypeError that never existed would lose real time. **Testing:** upstream's 2026-09-24 sync brought a vitest harness (`tests/web/`, `npm test`), so the old *"no JS test harness at all"* declaration is retired. `tests/web/meow_release_check.test.js` pins the three failure states (`[]`/404, a 403 rate-limit object, offline), the happy path, that only `meowerse/sunmeow` is queried, and — as a source guard — that no web page fetches `repos/LizardByte/Sunshine/releases` directly; that last case fails against the sync's unported `Home.vue`. **Also fixed:** the browser tab `<title>` (now in the SPA shell `index.html`; before the 2026-09-24 sync it was injected site-wide via `ViteEjsPlugin`), the navbar tooltip and image `alt` (what a screen reader announces), the LICENSE/NOTICE buttons which linked to *upstream's* copies rather than the terms shipped with this binary, and the `v-else-if` that asserted *"You are running the latest version"* even when the check never succeeded — an unsubstantiated claim that contradicted this module's own stated rule. **Deliberately unchanged:** the `sunshine_name` config KEY (renaming breaks every existing config and `ConfigConsistencyTest`; only the displayed placeholder changed, and it was already inaccurate upstream since the real default is `platf::get_host_name()`), `sunshine_version.js`/`SunshineVersion`/`sunshine.css` (internal identifiers), the logo artwork (since replaced in place by `green-brand`, below), the **Support button** (`ResourceCard.vue`, still LizardByte's discussions), and the **Documentation button**, which still points at `docs.lizardbyte.dev/projects/sunshine` — the same bug class as the LICENSE fix, left only because this fork has no docs site to point at. **Moved 2026-09-24 by the upstream sync**, which turned the multi-page web UI into a single-page app (`d23336dc`): the release check and the `githubVersion &&` guard moved from `index.html` into `Home.vue` (by hand — git reported no conflict there, so a plain merge would have silently restored upstream's `LizardByte/Sunshine/releases` fetch); `apps.html`'s hunk followed its rename to `Apps.vue`; the tab `<title>` moved from the deleted `template_header.html` (and `ViteEjsPlugin`) into the SPA shell `index.html`; the Navbar hunk was re-applied onto upstream's `<RouterLink>`. | 2026-09-03 |
| `tests/unit/test_httpcommon.cpp`, `tests/unit/test_file_handler.cpp` | `MEOW-TOUCH(test-sandbox)` | **One replaced line each (plus comment).** `DownloadFileTest` and `FileHandlerMakeDirectoryTest` built their paths from `platf::appdata()` — `~/.config/sunmeow` on a real desktop — and left a stray `tests/` directory beside `apps.json`, `credentials` and the paired-client list on every run (`FileHandlerMakeDirectoryTest`'s cleanup removed only `tests/path/`). Both now write under `SUNSHINE_TEST_BIN_DIR`, as `test_nvhttp_client_auth.cpp` already did. Layers 1–3 fail: each path is a local inside an upstream `TEST_P` body, with nothing to hook or override from outside. This is per-test hygiene only; isolating the whole binary is `tests/meow/config_sandbox.cpp`. `test_httpcommon.cpp` marker added 2026-09-03 and registered 2026-09-24; `test_file_handler.cpp` added 2026-09-24. | 2026-09-03 |
| `cmake/targets/common.cmake` | `MEOW-TOUCH(ccache-scope)` | **`10` added, `0` deleted** — two one-line calls plus their comments, around upstream's existing `target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})`. All logic is layer 1, in the new `cmake/meow/version_definitions.cmake`. Layers 1–3 fail because the list is a plain CMake variable that upstream builds in `cmake/prep/build_version.cmake` and consumes here: nothing can intercept a variable between two `include()`s from outside the file that reads it, and the re-application must land *after* `add_subdirectory(tests)`, which only this file can sequence. **Why it exists:** `PROJECT_VERSION` carries a `-<short-sha>` suffix on every branch not named `master`, and CI checkouts are detached — so upstream's global `target_compile_definitions` put the commit identity on all 349 objects' command lines, and ccache hashes the command line. Measured 0/349 hits on a commit that changed only `ci.yml`; an identical-sha re-run hit 349/349 and built in 16s against 836s. After this, 3 of 352 TUs carry it. **Verified behaviour-identical:** the version literal is present in the linked binary, in `main.cpp.o` and `confighttp.cpp.o`, and absent from `stream.cpp.o`. | 2026-08-26 |
| `src/nvhttp.cpp` | `MEOW-TOUCH(uniqueid)` | **`30` added, `1` deleted (the replaced assignment)**, all inside `load_state()`: (1) an empty persisted `uniqueid` is regenerated and loading **continues**; (2) that recovered id is written back with `save_state()`; (3) when the state file cannot be parsed, an id is generated in memory and the file is left untouched. Layers 1–3 fail: `load_state()` is a free function whose parsed `ptree` never leaves it, and each decision sits at a specific point in its control flow (between the absence check and the `named_devices` loop, after the trust store is rebuilt, inside the `read_json` catch). **Why it exists:** `get_optional` returns an engaged optional for `"uniqueid": ""`, so upstream's absence-only check kept the id empty forever; `/serverinfo` then served `<uniqueid/>`, which Moonlight rejects as a missing mandatory field — the host could never be added or paired. An unparseable state file produced the same `<uniqueid/>`. The early `return` stays reserved for a *missing* key: taking it for an empty one would drop every paired client, which a later `save_state()` would persist. Persisting matters because `save_state()` otherwise runs only on pair/unpair/enable changes, so without it a host whose clients keep streaming would serve a new id after every restart, and Moonlight drops a host whose poll returns a different id. Pinned by `tests/unit/test_nvhttp_unique_id.cpp` (six cases, including `EmptyUniqueIdRecoveryPreservesPairedClients` and `RecoveredUniqueIdIsPersistedAndStableAcrossReloads`). The test-suite sandbox that made this safe to test lives in a new file, `tests/meow/config_sandbox.cpp` — not a touch-point. | 2026-09-24 |

Two upstream **non-source** files are appended to. Both are append-only (`0` deletions), so
they cannot conflict except at the very end of the file, but they are upstream files and are
therefore declared here rather than described as "additive":

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `AGENTS.md` | _(none — Markdown, not source)_ | It is the entry point agents read by convention; the name is fixed by that convention, so a new file cannot replace it. Upstream's content is preserved verbatim and our fork rules are **appended** below a marker comment. | 2026-08-24 |
| `.gitignore` | `MEOW-TOUCH(rebrand)` | Ignore rules only take effect in the real `.gitignore`. One line added (`.claude/worktrees/`) to keep agent worktrees out of the index. | 2026-08-24 |

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

### Retired touch-points

| File | Marker | Why it was retired | Retired |
| --- | --- | --- | --- |
| `cmake/dependencies/Boost_Sunshine.cmake` | `MEOW-TOUCH(cmake-deps)` | Pinned `BOOST_VERSION` `1.89.0` -> `1.92.0` so that `find_package(Boost ... EXACT)` accepted Arch's system Boost instead of building Boost from source on every clean configure. Upstream `f54f9dfc` replaced the `EXACT` pin with a `1.89.0` *minimum* (`BOOST_MINIMUM_VERSION`) and a CPM fallback at 1.92, so the system Boost is accepted without our edit. The file is now byte-identical to upstream's; verified by the gate's configure output (`Boost include dirs: /usr/include`). | 2026-09-24 |

---

## Verifying and regenerating this registry

The greps are the source of truth. If they disagree with the table above, **the table is
wrong**.

```bash
# 1. every declared touch-point in our source.
#    NOTE the pathspec includes repo-root build tooling: a marker in vite.config.js is
#    invisible to a 'src/ src_assets/'-only grep, which is how one went unlisted before.
git grep -n 'MEOW-TOUCH'

# 2. every upstream file we differ from at all, and by how much.
#    Any non-zero DELETION count is a red flag: we removed upstream content.
git diff --numstat origin-upstream/master -- .
```

Run both **before every upstream sync** ([`CLAUDE.md` §4](../../CLAUDE.md)). The first grep
now returns **11** marker families (`adaptive-bitrate`, `ccache-scope`, `green-brand`,
`rebrand`, `test-sandbox`, `unified-desktop-capture`, `uniqueid`, `viewport`, `viewport-cuda`,
`web-deps`, `web-identity`) across **64** distinct (file, marker) pairs in **58** files, as of
2026-09-24 after the upstream sync and #16/#18. `cmake-deps` was retired by that sync. (An
earlier revision listed a `clipboard` family; no such marker exists — it is only the example
in `CLAUDE.md` §3, which the regeneration command below excludes.)

**Do not hand-count this, and do not trust the number above without re-running it** — this
document has now been wrong about its own grep three times (twice about the pathspec, once
about the count, which said 22 while omitting the `rebrand`, `cmake-deps` and `ccache-scope`
families entirely). Regenerate:

```bash
# distinct (file, marker) pairs, excluding the three prose files that merely discuss markers
git grep -o 'MEOW-TOUCH([a-z-]*)' \
  | grep -Ev '^(CLAUDE\.md|docs/meow/TOUCHPOINTS\.md|README\.meow\.md)' \
  | sed 's/:[0-9]*:/|/' | sort -u | wc -l
```

Reconcile them against the table
above row by row. Any marker the table does not list is the bug — and any marker *outside the
pathspec* is a bug the grep itself cannot show you, so widen the pathspec whenever a touch-point
lands in a new part of the tree. That has now happened twice: `vite.config.js` sits at the
repo root, which the original `src/ src_assets/` pathspec never scanned, and `ccache-scope`
put a marker under `cmake/`, which neither of the earlier pathspecs covered. Both times the
marker existed and the pre-sync check silently reported nothing. **Widening the pathspec is
part of adding a touch-point in a new directory, not a follow-up.**

What matters in (2) is the **deletion** column. The rule is still *prefer zero* — we add to
upstream files rather than cut them — but it is no longer literally zero everywhere, so treat
any non-zero value as **a question to answer, not an automatic failure**. The large, deliberate ones are argued
above — `vite.config.js` (`10`, the codecov removal), `package.json` (`6`),
`package-lock.json` (`353`, generated), `cuda.cu` (`11`, which must not grow) and
`kwingrab.cpp` (`8`, in-place edits) — and every one-line in-place replacement is listed in the
block below. A non-zero count on any *other* file, or a count larger
than recorded here, means someone removed upstream content — investigate before syncing.
Insertion counts change with every edit to our own docs, so do not treat those as fixed:

```
# <added>	<deleted>	<path>     -- deletions are the column that matters
1	0	.gitignore
17	0	AGENTS.md
282	8	src/platform/linux/kwingrab.cpp    # 8 = in-place signature/comment edits
<n>	0	src/platform/linux/misc.cpp
<n>	0	src/platform/linux/cuda.h
27	11	src/platform/linux/cuda.cu         # 11 = deliberate; MUST NOT GROW
<n>	0	src/platform/linux/cuda.cpp
<n>	0	src/config.h
<n>	0	src/config.cpp
<n>	0	src/stream.cpp
<n>	0	src/video.cpp
3	0	src_assets/common/assets/web/configs/config_tabs.json
<n>	0	src_assets/common/assets/web/configs/tabs/AudioVideo.vue
<n>	0	src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue
<n>	0	src_assets/common/assets/web/public/assets/locale/en.json
<n>	0	docs/configuration.md
4	10	vite.config.js                     # the codecov removal -- see the row above
0	6	package.json                       # codecov devDependency + its overrides block
0	353	package-lock.json                  # generated; regenerate, never hand-merge
<n>	0	src_assets/common/assets/web/sunshine.css   # green-brand: appended block only
<n>	<m>	src_assets/common/assets/web/{Home,Apps}.vue, index.html  # web-identity: in-place replacements
<n>	<m>	src_assets/common/assets/web/{Navbar,NavbarSimple,ResourceCard}.vue, configs/tabs/General.vue  # web-identity, as above
<n>	1	src/platform/macos/misc.mm                        # rebrand: config dir literal replaced
<n>	1	tests/unit/test_httpcommon.cpp                    # test-sandbox: path line replaced
<n>	1	tests/unit/test_file_handler.cpp                  # test-sandbox: path line replaced
<n>	1	src/nvhttp.cpp                                    # uniqueid: replaced assignment
1	27	sunshine.svg                                   # green-brand: GENERATED -- take upstream, re-run
1	75	src_assets/common/assets/web/public/images/sunshine-locked.svg    # generated, as above
1	84	src_assets/common/assets/web/public/images/sunshine-pausing.svg   # generated, as above
1	89	src_assets/common/assets/web/public/images/sunshine-playing.svg   # generated, as above
-	-	<every binary in the green-brand table>        # generated, as above
<n>	0	CLAUDE.md
<n>	0	README.meow.md
<n>	0	docs/meow/TOUCHPOINTS.md
```

A non-zero deletion count against `.gitignore` or `AGENTS.md` means someone removed upstream
content — investigate before syncing. For source files, a non-zero count means *look*: either
it is a modified line with its replacement beside it (fine, and it belongs in the table above),
or it is a genuine removal (not fine). The count alone cannot tell you which. The same applies
to every file above whose deletion column reads `0`.

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

---

## `MEOW-TOUCH(green-brand)` — brand art replaced in place

*Added 2026-09-24.* The application, tray, favicon and marketing art recoloured to meowerse
green (`#00ff82` on the dark `#0d0d0d` plate, per meowerse
`packages/ui/src/styles/tokens.css`), with Sunshine's swirl replaced by a geometric **sun**
(owner decision: sunmeow keeps a sun, moonmeow a moon).

**One master, everything generated.** `branding/meow/sunmeow.svg` is the only hand-authored
artwork; `bash scripts/icons/meow/build.sh` regenerates every file below from it (needs
`rsvg-convert` and Python Pillow; bytes are reproducible with librsvg 2.62 / Pillow 12.3).
Never hand-edit a PNG/ICO/ICNS/JPG or a generated SVG — change the master and re-run.
**On an upstream sync that touches any of these files, take upstream's side of the conflict,
re-run the script, and commit the result**; there is nothing to merge by hand. Upstream's own
`scripts/icons/convert_and_pack.sh` is left untouched (it needs `go-png2ico`, `oxipng` and
Inkscape, none of which this generator requires).

**Why layers 1–3 were insufficient (applies to every row):** `src/system_tray.cpp`,
`src/confighttp.cpp`, `cmake/packaging/{common,linux,macos,windows}.cmake`,
`cmake/compile_definitions/windows.cmake`, `Info.plist.in`, `Navbar.vue`,
`index.html`, `vite.config.js`, `docs/Doxyfile`, `tests/CMakeLists.txt` and `test_process.cpp`
reference these **paths**. Replacing the bytes under the same name edits zero lines of
upstream code; renaming would edit every one of those files. The generated SVGs carry an
in-place `MEOW-TOUCH(green-brand)` comment so `git grep -n 'MEOW-TOUCH'` sees them; binary
files cannot carry a marker, so this table is their declaration (as for the JSON rows above).

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `sunshine.svg` | `MEOW-TOUCH(green-brand)` (generated) | see above. Plated app icon: the hicolor `scalable/apps/<FQDN>.svg` (the only hicolor size upstream installs — no PNG sizes exist to regenerate), default tray icon (emitted as `images/logo-sunshine.svg` by `vite.config.js`'s `emitTrayIconsPlugin` since upstream `7e4a7401`; it was a CMake `configure_file` before — same source file, so no regeneration change), README, Doxygen logo | 2026-09-24 |
| `sunshine.png`, `sunshine.ico` | _(binary — this row)_ | see above. `CPACK_PACKAGE_ICON` (for NSIS that is the MUI header image, not the installer icon — an upstream quirk left as is), Windows exe resource via `windows.rc`, WiX product icon, Doxygen `PROJECT_ICON`. The `.ico` keeps upstream's frame layout: 16–128 px as 32-bit BMP, 256 px as PNG | 2026-09-24 |
| `src_assets/macos/build/sunshine.icns` | _(binary — this row)_ | see above. Plate on Apple's 824/1024 grid; types ic07–ic14 (Pillow has no writer for the legacy 1x ic04/ic05, which macOS derives from ic11/ic12) | 2026-09-24 |
| `src_assets/macos/build/sunshine-background-72dpi.jpg` | _(binary — this row)_ | see above. DMG window background: dark surface, quiet wordmark | 2026-09-24 |
| `src_assets/common/assets/web/public/images/sunshine.ico`, `logo-sunshine-16.png`, `logo-sunshine-45.png` | _(binary — this row)_ | see above. Web UI favicon (all-PNG frames, as upstream's) and navbar logo | 2026-09-24 |
| `src_assets/common/assets/web/public/images/sunshine-{playing,pausing,locked}.svg` | `MEOW-TOUCH(green-brand)` (generated) | see above. Tray states: plated sun + badge — playing = green `#00ff82` play, pausing = amber `#f5a524` bars, locked (pairing) = ink `#f2f2f2` padlock. Sunshine's `#00ff00`/`#00d9ff`/`#999999` badges moved onto the meowerse palette; the three stay distinct by colour **and** shape | 2026-09-24 |
| `src_assets/common/assets/web/public/images/sunshine-{playing,pausing,locked}.{png,ico}`, `…-16.png`, `…-45.png` | _(binary — this row)_ | see above. Raster copies of the tray states | 2026-09-24 |
| `branding/ms-store/box-art.png`, `poster-art.png`, `branding/github/banner.jpg`, `gh-pages-template/assets/img/navbar-avatar.png` | _(binary — this row)_ | see above. Store / marketing art | 2026-09-24 |
| `src_assets/common/assets/web/sunshine.css` | `MEOW-TOUCH(green-brand)` | Theme values are plain CSS custom properties in an upstream stylesheet that every page links; there is no hook to inject a second stylesheet without editing each HTML entry point. So: **one appended block, `0` deletions**, re-declaring the brand tokens of the default `dark` and `light` themes (what `auto` resolves to), which carried Sunshine's yellow gradient. Visibly this is the navbar (`--navbar-bg`, `--navbar-text*`); `--color-accent*` is re-declared for consistency, though today only `--color-accent-light` is read (one placeholder gradient). Semantic colours and every named theme are untouched. Same specificity, later in source order, so the seam is only ever the end of the file | 2026-09-24 |

Tray on KDE Plasma: the StatusNotifierItem shows these SVGs in full colour at 22 px, so the
dark plate is what keeps the sun legible on a light panel (`#00ff82` alone is 1.3:1 on
white). Checked at 16 and 22 px on Breeze light and dark in the PR's contact sheet.

**Known gap, not addressed here:** there is no monochrome / template tray variant. macOS
prefers template images in the menu bar; upstream also ships a full-colour tray there, and a
template icon would need a code change in the tray backend, so it is left for a separate task.

`logo-libvirtualhid.svg` (referenced by `src/system_tray.cpp`) is **not** missing on Linux:
it is the Windows-only Virtual HID driver notification icon, copied from the
`third-party/libvirtualhid` submodule by `cmake/packaging/common.cmake` and
`tests/CMakeLists.txt` under `if(WIN32)`, and only listed in `allIconPaths` under
`#ifdef _WIN32`. Third-party driver art, left as is.
