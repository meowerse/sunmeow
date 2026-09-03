# Upstream touch-point registry

Every edit to a file **we did not create** is recorded here, and marked in place in the source
with a `MEOW-TOUCH` comment. This registry is the complete list of places a future upstream
merge can hurt.

Required by [`CLAUDE.md` §3](../../CLAUDE.md). Read [`CLAUDE.md` §2](../../CLAUDE.md) — the
additive-only prime directive — before adding a row.

---

## Current state: one upstream file has content **removed**

Verified 2026-09-03 against `origin-upstream/master` (Sunshine `41c25716`), immediately
after the sync that merged 35 upstream commits including four security fixes.

**This changed on 2026-08-25.** `vite.config.js` is the first upstream file we delete
*functional* content from: `4` added, `8` deleted (`web-deps`, removing `codecovVitePlugin`).
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
  argued in the table above. Currently: `vite.config.js` (`4` / `8`, the codecov plugin) and the
  `.github/workflows/*` files this fork deleted wholesale, whose rationale is in `ci.yml`'s header.
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
| `vite.config.js` | `MEOW-TOUCH(web-deps)` | **The only removal in the tree: `4` added, `8` deleted.** Drops upstream's `codecovVitePlugin()` call, its `import`, and the now-orphaned *"should be after all other plugins"* comment. Layers 1–3 all fail structurally: this file's default export is a plain object literal and the plugin list is an array literal inside it, with no registration hook, no virtual and nothing exported — a new file in `src/meow/` cannot *un*-register a plugin, and a one-line hook can only ever add one. The additive alternative was considered and rejected on its merits, not for convenience: neutering the plugin in place (`enableBundleAnalysis: false`) still edits this exact file, still ships the devDependency, and — decisively — still leaves the build uninstallable. `@codecov/vite-plugin@2.0.1` is the newest release and peer-caps at `vite "4.x \|\| 5.x \|\| 6.x"`, so on vite 8 `npm ci` fails `ERESOLVE` unless a permanent `overrides` entry forces the resolution. Removal is the only route that does not add a standing resolution hack to `package.json`. What is removed is also **inert here by construction**: it uploads bundle analysis to *LizardByte/Sunshine's* Codecov account via a `CODECOV_TOKEN` this fork does not have and cannot obtain, so the upload never happened — the build merely retried it (`get-pre-signed-url failed after 3 attempts`) at a cost of 71–82% of every web build. Same category as the 24 inherited LizardByte workflows this repo already deleted, whose precedent is recorded in `.github/workflows/ci.yml`'s header. **Proven inert:** the emitted tree is byte-identical with and without the plugin — `diff -r` over all 79 artifacts reports no difference, so the "after all other plugins" ordering constraint is satisfied vacuously. If upstream is ever merged back onto a repo that *does* own the Codecov project, this is a one-hunk revert. | 2026-08-25 |
| `package.json` | _(none — JSON, no comment syntax)_ | `2` added, `3` deleted. Bumps `vite` 6.4.3 → 8.2.2 and `marked` 18.0.10 → 18.0.11, and drops the `@codecov/vite-plugin` devDependency. JSON admits no comment, so the marker lives in `vite.config.js` and this row is the declaration. Deletions are the three replaced/removed dependency lines — no upstream *capability* is removed beyond the plugin argued above. | 2026-08-25 |
| `package-lock.json` | _(none — generated)_ | `522` added, `1251` deleted. Entirely regenerated by `npm`, never hand-edited; the large deletion count is `rollup`/`esbuild` platform packages disappearing because vite 8 bundles with `rolldown`, plus the codecov subtree. Not a hand-authored touch-point — it is an artifact of the two rows above and should be regenerated, never merged by hand. | 2026-08-25 |
| `CMakeLists.txt`, `cmake/targets/common.cmake`, `cmake/prep/init.cmake`, `cmake/prep/special_package_configuration.cmake`, `cmake/packaging/linux.cmake`, `src/platform/linux/misc.cpp`, `packaging/linux/app-meow.alxnko.sunmeow.service.in`, `src_assets/linux/misc/postinst`, `packaging/linux/Arch/sunshine.install`, `packaging/linux/Arch/PKGBUILD`, `packaging/linux/copr/Sunshine.spec`, `packaging/linux/flatpak/meow.alxnko.sunmeow.yml`, `packaging/linux/AppImage/AppRun`, `packaging/linux/flatpak/scripts/additional-install.sh`, `packaging/linux/flatpak/scripts/remove-additional-install.sh`, `packaging/sunshine.rb`, `scripts/linux_build.sh`, `docker/ubuntu-22.04.dockerfile`, `docker/ubuntu-24.04.dockerfile`, `docker/ubuntu-26.04.dockerfile`, `docker/debian-trixie.dockerfile`, `60-sunmeow.rules`, `60-sunmeow.conf` | `MEOW-TOUCH(rebrand)` | **Identity, so this fork cannot be mistaken for — or collide with — the distro `sunshine` package.** Upstream's `PROJECT_NAME`/`PROJECT_FQDN` produce a systemd unit, binary path, udev rule, modules-load conf and config directory that are **byte-identical to the packaged Sunshine's**, so both could not be installed together and desktop-portal notifications could not tell them apart. Changed: product name → `Sunmeow`, FQDN → `meow.alxnko.sunmeow`, binary → `sunmeow` (via `OUTPUT_NAME`; the CMake *target* stays `sunshine`), config dir → `~/.config/sunmeow`, and `sunmeow.conf`/`.log`/`_state.json` inside it. Nine packaging files renamed. **Deliberately unchanged**, same reasoning as moonmeow keeping its `com.limelight` namespace: C++ namespaces, `src/` layout, the CMake target, and `platf::SERVICE_NAME` (Windows-only — Linux advertises `platf::get_host_name()` over mDNS, so it is not user-visible here). **Three cross-cutting checks that could have broken silently:** the virtual-HID device name is a hardcoded `"Sunshine {}"` literal, *not* `PROJECT_NAME`, so the udev rule's `ATTRS{name}` match still fires; the client discovers by service *type* `_nvstream._tcp`, never the instance name, so pairing is unaffected; and `appdata()`'s migration branch now points at **our** old path — left at `~/.config/sunshine`, `SUNSHINE_MIGRATE_CONFIG=1` would have copied the distro package's config here and then `fs::remove_all()`'d the original. **Renaming the binary is not enough on its own — and the first correction was not either.** Seven places *name* the executable rather than build it. `OUTPUT_NAME` fixed one; `SUNSHINE_EXECUTABLE_PATH` and `Alias=` were fixed next, which stopped the generated unit starting the distro package's binary (it shipped `ExecStart=sunshine`, and nothing failed, because that binary runs). A review then found the same defect still live in five more: `postinst` and `Arch/sunshine.install` ran `setcap` against `$(which sunshine)` / `usr/bin/sunshine`, granting `cap_sys_admin,cap_sys_nice` to **another package's** binary while leaving ours with none — which presents as this fork black-screening on KMS capture, the very symptom setcap exists to prevent; the flatpak manifest and `init.cmake`'s flatpak branch named `--command=sunshine`, which `OUTPUT_NAME` no longer produces; and the Arch/copr/homebrew/`linux_build.sh` builds each passed `.../sunshine` explicitly, so they never saw the corrected default at all. **`SUNMEOW_BINARY_NAME` (`CMakeLists.txt`) now owns the name**, and a configure-time check on the generated unit (`cmake/prep/special_package_configuration.cmake`) fails the build rather than shipping this again. The standalone packaging files cannot read a CMake variable, so they remain literal and are listed below. **A second review found the sweep was still not exhaustive, and the pattern was identical every time: the file had been opened and edited, and a later occurrence in the *same* file was never searched for.** Four of the misses broke a build outright rather than merely mis-naming something — `copr/Sunshine.spec`'s `%files` still declared `%{_bindir}/sunshine` (rpmbuild hard-errors on a `%files` entry with no matching installed file), `Arch/PKGBUILD` validated `dev.lizardbyte.app.Sunshine.*` artifacts that `PROJECT_FQDN` no longer generates and ran `./sunshine --version` in `check()`, `AppImage/AppRun`'s `--install`/`--remove` `sed`-ed and deleted a unit name we do not install (so `ExecStart` was never repointed at the AppImage), and all **four** dockerfiles set `ENTRYPOINT ["/usr/bin/sunshine"]` on an image that installs `/usr/bin/sunmeow`, i.e. shipped a container that could not start. `copr/Sunshine.spec` was also listed in this row while carrying **no in-place marker at all** — a §3 violation that made it invisible to the very grep this registry calls its authority. **The detector was wrong too:** that grep was scoped `-- src/ src_assets/ vite.config.js cmake/`, which cannot see a marker under `packaging/`, `scripts/`, `docker/` — or in `CMakeLists.txt` at the repo root. It is now unscoped, and running it reveals markers in 9 files the old scope structurally could not reach. The lesson recorded for the next sweep: **enumerate with `git grep`, never by inspection.** | 2026-08-26 |
| `cmake/dependencies/Boost_Sunshine.cmake` | `MEOW-TOUCH(cmake-deps)` | **`1` changed line** — the Boost version pin, `1.89.0` -> `1.92.0`. Layers 1–3 fail: the value is a plain `set()` consumed by a `find_package(... EXACT)` in the same file, with no hook to intercept it from outside. **Why it matters:** `EXACT` *rejects* a system Boost that is not the pinned version, and the `FetchContent` fallback then compiles Boost from source on every clean configure. Arch ships 1.92.0, so on the reference machine — and in CI, whose container is Arch — upstream's 1.89.0 pin guaranteed a from-source Boost build every single time. Matching the system version skips it entirely. The components used (`filesystem`/`log`/`program_options`/`system`/`locale`) are stable across these minors, and notably exclude `boost::process`, where Boost's API churn lives. **This row was missing until 2026-09-03** — the file carried its marker from the start but was never registered, so the registry and its own authority grep disagreed. Found by running that grep instead of trusting the table. | 2026-08-26 |
| `src/logging.h`, `tests/CMakeLists.txt`, `tests/tests_environment.h`, `tests/unit/test_logging.cpp`, `tests/unit/test_entry_handler.cpp`, `tests/integration/test_external_commands.cpp`, `src_assets/common/assets/web/configs/tabs/Files.vue`, `src_assets/windows/misc/migration/migrate-config.bat` | `MEOW-TOUCH(rebrand)` | **One line each — the rebrand's long tail, and a §3 violation until 2026-09-03.** These eight upstream files were edited when the config dir and log became `sunmeow*`, but carried **no in-place marker and no row here**, so `git grep -n 'MEOW-TOUCH'` — the grep this document calls its authority — could not see them. Found by a reviewer checking the *inverse* direction of the usual audit: not "is every marked file registered" (it was) but "is every modified upstream file marked" (it was not). Add that direction to the pre-sync check. Layers 1–3 do not apply: these are string literals inside upstream files (a log filename, a test assertion, a placeholder attribute, a CMake source list), with nothing to subclass or hook. **`tests/CMakeLists.txt` is the dangerous one:** upstream rewrote 43 lines of that file in the 2026-09-03 sync, and our `60-sunmeow.rules` entry survived only because the conflict did not land on that hunk. A future reshuffle would silently restore `60-sunshine.rules` and `test_external_commands` would begin verifying a file that does not exist — passing or failing for the wrong reason. `migrate-config.bat` carries the same hazard as `appdata()`: against upstream's filenames it would move the **distro package's** config into our directory. | 2026-09-03 |
| `src_assets/common/assets/web/public/assets/locale/en_GB.json` | _(none — JSON, no comment syntax)_ | **`76` changed, `0` net** — the rebrand applied to the British-English strings: `Sunshine` -> `Sunmeow`, `Moonlight` -> `Moonmeow` in text a user reads. Registered on 2026-09-03; it had been edited since the rebrand with neither a marker (JSON admits no comment) nor a row, so it was invisible to both directions of the §3 audit. **This deliberately departs from upstream's `AGENTS.md`**, which says to update no language other than `en`, explicitly excluding variants like `en-US`/`en_GB` — that rule exists to keep Crowdin the single source for translations, and it is right for *translations*. A product rename is not a translation: leaving `en_GB` alone would ship a UI that calls itself Sunshine to every British-English user. `en.json` is the only other locale that differs from upstream; the remaining ~30 are untouched and still say Sunshine, which is a known gap rather than an oversight — they are Crowdin-owned and will be overwritten by the next translation sync. | 2026-09-03 |
| `cmake/targets/common.cmake` | `MEOW-TOUCH(ccache-scope)` | **`10` added, `0` deleted** — two one-line calls plus their comments, around upstream's existing `target_compile_definitions(sunshine PUBLIC ${SUNSHINE_DEFINITIONS})`. All logic is layer 1, in the new `cmake/meow/version_definitions.cmake`. Layers 1–3 fail because the list is a plain CMake variable that upstream builds in `cmake/prep/build_version.cmake` and consumes here: nothing can intercept a variable between two `include()`s from outside the file that reads it, and the re-application must land *after* `add_subdirectory(tests)`, which only this file can sequence. **Why it exists:** `PROJECT_VERSION` carries a `-<short-sha>` suffix on every branch not named `master`, and CI checkouts are detached — so upstream's global `target_compile_definitions` put the commit identity on all 349 objects' command lines, and ccache hashes the command line. Measured 0/349 hits on a commit that changed only `ci.yml`; an identical-sha re-run hit 349/349 and built in 16s against 836s. After this, 3 of 352 TUs carry it. **Verified behaviour-identical:** the version literal is present in the linked binary, in `main.cpp.o` and `confighttp.cpp.o`, and absent from `stream.cpp.o`. | 2026-08-26 |

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
now returns **9** marker families (`adaptive-bitrate`, `ccache-scope`, `clipboard`,
`cmake-deps`, `rebrand`, `unified-desktop-capture`, `viewport`, `viewport-cuda`, `web-deps`)
across **49** distinct (file, marker) pairs in **43** files, as of 2026-09-03.

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
any non-zero value as **a question to answer, not an automatic failure**. Five are known and
justified above: `vite.config.js` (`8`, the codecov removal), `package.json` (`3`),
`package-lock.json` (`1251`, generated), `cuda.cu` (`11`, which must not grow) and
`kwingrab.cpp` (`8`, in-place edits). A non-zero count on any *other* file, or a count larger
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
<n>	0	src_assets/common/assets/web/config.html
<n>	0	src_assets/common/assets/web/configs/tabs/AudioVideo.vue
<n>	0	src_assets/common/assets/web/configs/tabs/audiovideo/DisplayModesSettings.vue
<n>	0	src_assets/common/assets/web/public/assets/locale/en.json
<n>	0	docs/configuration.md
4	8	vite.config.js                     # the codecov removal -- see the row above
2	3	package.json                       # dependency bumps + codecov dropped
522	1251	package-lock.json                  # generated; regenerate, never hand-merge
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
