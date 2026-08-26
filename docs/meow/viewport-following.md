# Viewport following ("foveated" streaming)

**Status:** host side, software and CUDA/NVENC scaling paths. VA-API not covered. Off by default.

## The problem

Streaming a 5360x1440 two-monitor desktop to a phone at 5-8 Mbps is unreadable. The encoder
is handed a 1280x720 surface, the desktop is 3.7:1, so it letterboxes down to **1280x343** —
440k of the 921k pixels the surface can carry, describing 7.7 million source pixels. Text is
destroyed before the encoder ever sees it. Pinch-zooming on the client magnifies an image
that has already lost the detail.

## The fix

The client tells the host which rectangle of the host desktop it is currently displaying.
The host crops to that rectangle **before scaling into the encoder**. Encode resolution and
bitrate are unchanged, so the same bits describe far fewer pixels.

Cropping a phone-shaped 1920x1080 window out of that desktop fills the whole 1280x720
surface — and the source-pixels-per-encoded-pixel ratio improves by more than 4x. That
number is asserted in `MeowViewportPlan.CroppingRecoversTheWastedSurface`.

## Turning it on

Add to `sunmeow.conf`:

```
meow_viewport_following = enabled
```

or tick **Follow the client's viewport** in the Web UI, under Audio/Video.

It is registered in `src/config.cpp` exactly like every other Sunshine setting, so it accepts
the same values (`enabled`/`on`/`true`/`yes`/`1`, and the corresponding negatives) and
appears in the UI alongside the display options it belongs with.

> **History, because the first attempt was wrong and the reason is worth keeping.** This key
> was originally parsed out of the configuration file by `src/meow/` alone, to avoid editing
> an upstream file at all — the same trade `meow::display_union` made for `output_name = all`.
> It looked clean and it was a bad call: Sunshine's own parser never learned the key existed,
> so it logged `Warning: Unrecognized configurable option [meow_viewport_following]` at every
> startup. A user who had just enabled the feature was being told, in a warning, that the
> setting does not exist. CLAUDE.md §2's hierarchy exists to keep merges cheap, not to make
> the product worse; adding one row to the settings table is the smallest possible upstream
> edit and it has an established pattern. Registering it properly is also the only way it can
> reach the Web UI.

The line that tells you whether it actually took effect is logged when a stream's control
channel starts:

```
Info: meow viewport following: enabled. The client may request a crop of the desktop; ...
```

When it is off, **no handler is registered at all** — the feature is inert rather than
merely quiet, and a viewport packet from a client that speaks the extension falls through to
`control_server_t::call()`'s unknown-type path exactly as it would against stock Sunshine.

## Why it defaults to off

1. **Pointer input is not remapped yet.** Client-supplied absolute pointer and touch
   coordinates go through `video::make_port()`, which knows only the *full* captured
   desktop. While a crop is active, a tap lands in the wrong place. Turning cropping on is
   therefore a trade — far more readable text in exchange for absolute pointer input that
   needs the matching host-side remap. Relative mouse input is unaffected.
2. **The compatibility floor.** An existing working setup must not change behaviour on
   upgrade (CLAUDE.md §2). Nobody's stream changes until they ask for it.
3. **It does not cover every scaling path** — see below. Software and CUDA/NVENC crop;
   VA-API does not, so on those hosts the setting would advertise a feature that silently
   does nothing.

## What is covered, and what is not

| Path | File | Cropped? |
| --- | --- | --- |
| Software scaling (swscale) | `src/video.cpp`, `avcodec_software_encode_device_t::convert()` | **yes** |
| CUDA / NVENC (`cuda_ram_t`, `cuda_vram_t`, NvFBC) | `src/platform/linux/cuda.cu`, `cuda.cpp` | **yes** |
| CUDA via GL dmabuf import (`gl_cuda_vram_t`) | `src/platform/linux/cuda.cpp`, `egl::sws_t` | no |
| VA-API / EGL | `src/platform/linux/graphics.cpp`, `egl::sws_t` | no |
| Windows, macOS | — | no |

> **The CUDA path is the one this machine actually streams through.** With `capture = kwin`
> the host selects `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`, which takes `mem_type_e::cuda`.
> `pipewire_display_t::make_avcodec_encode_device()` then branches on `display_is_nvidia`,
> and on this hybrid laptop the compositor renders on the AMD iGPU (`eglQueryString(EGL_VENDOR)`
> is `Mesa Project`), so `display_is_nvidia` is false and the device is
> `cuda::make_avcodec_encode_device(width, height, false)` — a `cuda_ram_t`, converting
> through `RGBA_to_NV12` in `cuda.cu`. Until this change, the crop was implemented only in
> the swscale path and therefore did nothing at all on this host.
>
> The two remaining gaps are **VA-API** and the **`gl_cuda_vram_t`** device taken when the
> compositor *does* render on the NVIDIA GPU and dmabufs can be imported directly. Both scale
> through `egl::sws_t`, so both need the same treatment applied once to a GLSL shader.

### How the CUDA crop works

Same `plan_t`, different adapter. `src/meow/viewport_cuda.h` turns a plan into the two things
a scaling *kernel* needs, which are not the two things swscale needs:

- a **destination rectangle** (`cuda::viewport_t`) — where the scaled image lands in the
  encode surface. This already existed; it is the letterbox.
- a **source map** (`cuda::source_t`) — the texel the first destination pixel samples, and how
  far the sample point advances per destination pixel, **per axis**. This is new, and it is
  the whole crop: upstream's kernel computed `float x = idX * scale` from an implicit origin
  of `(0, 0)`.

The two are kept as separate types on purpose. Overloading `viewport_t` to mean both would put
a destination offset and a source origin in the same struct, and adding one to the other is a
bug that renders as "the scaling looks slightly wrong".

**The step is per axis** because `plan()` even-aligns each scaled extent independently, and
`floor_even()` removes the same *absolute* two pixels from each — so the shorter axis loses far
more of itself. A 5360x142 strip (a spreadsheet row spanning both monitors) scales into
1280x32: the width loses nothing and the height loses 5.8%, giving steps of 4.1875 and 4.4375.
Reuse the width's step on the height and the bottom rows are never read; reuse the height's
step on the width and the last sample lands at texel 5675 of a 5360-wide frame, which
`cudaAddressModeClamp` renders as a smear of the last column rather than a fault. Per-axis
steps also make this path agree with swscale by construction, since swscale is configured with
an input of `source.width x source.height` and an output of `out_width x out_height`.

**Blanking.** A GPU kernel writes only its destination rectangle, so when a crop shrinks that
rectangle the uncovered region keeps the previous frame's pixels — the same problem
`meow::viewport::reblack()` solves on the software path, with no `av_image_fill_black()`
available. The fix reuses upstream's own machinery: a 2x2 all-black texture, sampled with
`cudaAddressModeClamp` so that *every* coordinate reads black, run through the ordinary
conversion kernel over the whole surface. That produces exactly the black `apply_colorspace()`
writes, with no host-side copy of the colour matrix to drift out of sync. It runs only when
the destination rectangle moves — never on a steady-state frame, and never on a **pan**, where
the source origin changes but the destination does not.

**Bounds.** The rectangle is network-supplied, and a kernel indexing outside its texture is a
memory-safety bug rather than a visual one. `cuda_scaler_config()` re-derives every bound from
the capture and surface sizes it is handed and falls back to the uncropped baseline on
anything it cannot prove: reads inside the captured frame, writes inside the encode surface,
extents even and at least 2 (an odd extent makes the last 2x2 NV12 block write a column past
the rectangle; a zero extent is an invalid launch configuration, not a small picture).

**Verification.** The geometry is unit tested without a GPU in
`tests/unit/meow/test_viewport_cuda.cpp`, including a 20000-rectangle sweep of everything the
16-bit wire can express. That the *kernel* then reads the rectangle those numbers describe is
checked on real hardware by `tools/meow/viewport_cuda_probe.cpp`, which paints two markers on
a synthetic 5360x1440 desktop, crops to one of them, and asserts that the marker lands where
the plan says (within a pixel), that the other marker is **not** in the encoded frame at all,
that it is 2.77x wider than in the uncropped frame (7.7x the encoded pixels for the same
bitrate), and that reverting without the blanking pass leaves 16093 stale pixels in the
padding while reverting with it leaves zero.
That tool is not part of any build target — `tools/` is Windows-only in CMake — and is run by
hand; the command line is in its header.

The **software** path is the one taken when the capture backend hands **system memory** to an
encoder that does not scale on the GPU — on Linux/PipeWire that is `mem_type_e::system`,
which reaches `video.cpp:make_avcodec_encode_session()` with a null `encode_device->data`
and constructs an `avcodec_software_encode_device_t`. The software encoders (`libx264`,
`libx265`, `libsvtav1`) always take it.

The remaining uncovered scaler, `egl::sws_t`, already carries a destination viewport and a
scale factor of its own (`offsetX`/`offsetY`/`out_width`/`out_height`).
`meow::viewport::plan_t` emits exactly those fields and
`meow::viewport::cuda_scaler_config()` shows the shape the adapter takes, so extending to it
is a matter of adding a source origin and a per-axis step to a GLSL shader. That is a separate
change and it is not in this one.

## The wire protocol

Control-stream packet type **`0x3003`**, defined on branch `meow` of
[meowerse/moonlight-common-c](https://github.com/meowerse/moonlight-common-c) as
`packetTypesGen7Enc[IDX_VIEWPORT]`. Ten-byte payload, little endian:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | `uint8` | version (1) |
| 1 | `uint8` | flags (reserved, must be 0) |
| 2 | `uint16` | x |
| 4 | `uint16` | y |
| 6 | `uint16` | width |
| 8 | `uint16` | height |

The same packet travels in both directions. Client to host it is a request; host to client
it is the **applied** rectangle, which is frequently not the requested one.

### The coordinate space, and why `Limelight.h` is wrong about it

`LiSendViewportEvent`'s documentation says host desktop coordinates. **It cannot be.** The
client has no way to learn the host's desktop size — `serverinfo` does not carry it and
neither does anything else in the handshake. What the client actually sends is the rectangle
expressed against the **negotiated stream resolution**, the same reference space
`LiSendMousePositionEvent` already uses and the only one both ends can compute. The client
half of this feature reached the same conclusion independently; the host implements that,
not the comment.

This is not a rescale by a constant, because Sunshine **pads to preserve aspect ratio**. On
this user's setup the 5360x1440 desktop occupies only the middle **1280x343** of the 1280x720
surface, with 188 rows of black above and below:

```
  (0,0)                                        1280
    +----------------------------------------+
    |              188 rows of padding       |
    +----------------------------------------+ 188
    |                                        |
    |   5360 x 1440 desktop, scaled 0.2388   |   the reference frame's content area
    |                                        |
    +----------------------------------------+ 531
    |              188 rows of padding       |
    +----------------------------------------+ 720
```

So "40% down the encoded frame" is not "40% down the desktop", and a proportional mapping
that ignores the padding is wrong by 188 rows — in exactly the configuration this feature
exists for. `meow::viewport::to_desktop()` and `to_reference()` undo and redo that transform
once, at the boundary;
`MeowViewportReference.MapsThroughThePaddingNotAroundIt` pins the difference against the
naive answer so nobody reintroduces it.

Three consequences worth knowing:

- **A rectangle covering the whole encoded frame means the whole desktop**, padding
  included. So "the user is not zoomed in" reads as "stream everything", not as a crop of
  the visible strip.
- **A rectangle entirely inside the padding maps to nothing** and is refused, rather than
  being slid onto the nearest real pixels.
- **The reference frame is always the *uncropped* framing**, and stays that way while a crop
  is active. It is a fixed coordinate system, not "whatever is on screen right now" —
  otherwise successive pans would compose multiplicatively and walk the view off the
  desktop.

  This is the contract the client has to hold up. Once it resets its local zoom to 1:1
  because a crop landed, it must keep sending **the same reference-frame rectangle**, not
  the rectangle it is now displaying — which is the whole frame, and which this host reads
  as "stop cropping". Repeating an identical request is idempotent here precisely because
  the reference frame does not move under it.

### The echo, and closing the gap permanently

The echo is **load-bearing, not informational**. Without it a host that crops leaves the
client showing the crop under its own local zoom — magnified twice, with absolute pointer
coordinates addressing the crop instead of the desktop. The echo is what lets the client
reset to 1:1 once a crop lands, and it is why the client ships its own preference off by
default until the host answers.

The host echoes the applied rectangle **in the same reference space the request arrived in**,
and appends the captured desktop size:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | `uint8` | version (1) |
| 1 | `uint8` | flags — bit 0 (`flag_desktop_extent`) set |
| 2..9 | | applied rectangle, reference-frame pixels |
| 10 | `uint16` | captured desktop width |
| 12 | `uint16` | captured desktop height |

With the negotiated stream resolution (which the client already has) plus the desktop size,
the client can compute the host's `min()` scalar and padding offsets itself — which closes
the coordinate-space gap permanently. It costs no version bump: the client's version-1
parser reads the first ten bytes and ignores both unknown flag bits and trailing bytes.
`MeowViewportWire.EchoPayloadCarriesTheDesktopExtent` proves that by feeding the 14-byte echo
to the host's own ten-byte parser.

### Keeping the two numberings in agreement

`sunmeow` does not consume the client's header: `third-party/moonlight-common-c` is pinned to
upstream `moonlight-stream/moonlight-common-c`, which has no `IDX_VIEWPORT`, and
`src/stream.cpp` carries its own `packetTypes` table. Three things make the agreement
checkable rather than trusted:

1. **The host never indexes `packetTypes` for this feature.** It uses
   `meow::viewport::control_packet_type` directly, so there is no table entry to forget and
   no way to read off the end of the table. Upstream's table is untouched.
2. **A collision refuses registration.** `meow::viewport::map_request_handler()` is handed
   the real `packetTypes` array and checks it. If any upstream message ever takes `0x3003`,
   the handler is not installed, the reason is logged at error level, and the stream falls
   back to full desktop — rather than dispatching a genuine upstream message into the
   viewport handler.
3. **The contract is pinned as literals in a test.**
   `MeowViewportWire.PinsTheClientContract` asserts the packet number, the version, the
   length and a golden byte vector; `DoesNotCollideWithUpstreamPacketTypes` carries a copy of
   the upstream table.

**If the two ever drift**, the failure is loud and harmless: the host logs the unknown packet
type at debug level in `control_server_t::call()` and viewport following simply never
engages. Nothing is mis-dispatched, because three different 16-bit numbers cannot collide by
accident, and (2) catches the one case where they could.

## What the host does with a request

All of it is pure and unit tested in `src/meow/viewport.h`:

1. **Parse.** Wrong length, wrong version or a zero extent — discarded whole, never parsed
   field by field.
2. **Map out of the client's coordinate system.** Intersect with the part of the surface
   that shows desktop, then undo the aspect padding. Entirely padding — refused.
3. **Clamp.** Origin into `[0, capture)`, extent trimmed to what is left. A rectangle wholly
   outside the desktop is refused rather than slid back inside, because silently showing the
   user somewhere they did not ask for is worse than refusing.
4. **Grow.** Below 64 px on an axis the rectangle is grown to that floor rather than
   refused — a user pinching in hard should hit a limit, not lose their zoom.
5. **Even-align.** Every cropped coordinate is even. NV12 chroma is subsampled 2x2; an odd
   origin fringes the crop edge and an odd destination offset fringes the letterbox seam.
6. **Scale and place.** `scalar = min(surface_w / src_w, surface_h / src_h)`, centred — the
   same arithmetic upstream applies to the full frame.
7. **Refuse slivers.** If the scaled result would be under 32 px on an axis, the full desktop
   is streamed instead. This is the "aspect ratio wildly different from the encode surface"
   guard: a 5360x64 request scales to 1280x15, which is valid and useless.

**Every rejection lands on exactly the same plan as no request at all**, and that plan is
bit-identical to what `avcodec_software_encode_device_t::init()` computes today — asserted by
`MeowViewportPlan.NoRequestReproducesUpstreamFormula` and
`EveryRejectionFallsBackToTheFullFramePlan`. Stock Moonlight, an older `moonmeow`, or any
client that never sends the packet gets an unchanged stream.

## When there is no echo

The host answers **including when what it applied was the full desktop**, so the client can
tell "refused" apart from "lost in transit". The only case with no echo at all is a session
that never published any scaler geometry — one that runs through neither the software nor the
CUDA scaling path — where the host does not know the captured desktop size and therefore
cannot even name the coordinate system an answer would be in.

## Resetting

A stale crop cannot leak forward:

- `on_scaler_init()` clears the pending rectangle, so a new session, a display mode change
  or an encoder reinit all start uncropped;
- `reset()` runs when the control broadcast ends;
- the owning-scaler check means a scaler only ever acts on state it published itself;
- with no rectangle pending, the owner's plan **is** the full-frame plan, so a scaler that
  was cropped reverts on the very next frame rather than waiting for a reinit.

On a host whose encoder takes neither the software nor the CUDA scaling path,
`on_scaler_init()` is never called, nothing ever claims the state, and the host correctly
stays silent rather than echoing a crop it did not apply. On the CUDA path `on_scaler_init()`
is called from `cuda_t::meow_viewport_init()`, and only when the setting is enabled — so a
host that never opted in allocates nothing and answers nothing. The one residual gap is a
broadcast that runs a session on one of those paths and then a session on neither *without*
the broadcast restarting: the second session would be answered against the first's geometry.
That needs the encoder choice to change mid-broadcast, which it cannot — it is fixed by host
config and probed once — so it is recorded here rather than defended against with another hook
on the hot path.

## Cost per frame

In the steady state, one relaxed atomic load, roughly a dozen integer operations and six
integer comparisons — measured at well under 1 us/call by
`MeowViewportSession.PerFramePlanningIsCheap`, against a 5.5 ms frame budget at 180 Hz.
**No allocation and no extra copy on a steady-state frame or on a pan.**

A **pan** (same crop size, new origin) costs two pointer additions and nothing else: the
scaler is not rebuilt, because its dimensions did not change. `MeowViewportEndToEnd`
asserts both that a pan skips reconfiguration and that it still lands on the right pixels.

On the CUDA path the steady state is the same load and planning plus four integer comparisons
in `apply_cuda_scaler()`, and a pan is those comparisons and four float stores: the kernel
launch is identical, only its parameters differ. Nothing is allocated on either.

A **zoom** (crop size changed) does allocate, once, on the software path: swscale's filter
tables are rebuilt and the intermediate output frame is reallocated at the new size. On the
CUDA path a zoom allocates nothing at all — it costs one extra full-surface kernel launch to
re-blacken the encode surface. The client rate limits viewport updates to one per 50 ms, so
either cost is bounded at 20 Hz in the worst case and is zero while the user is reading. The
encoder is never reinitialised and the encode surface never changes size.

What a zoom deliberately does **not** do is reallocate the encode surface. Upstream's
`prefill()` is the obvious way to re-blacken the padding, and it is the wrong one:
`av_frame_get_buffer()` is documented "if frame already has been allocated, calling this
function will leak memory", and the surface has been allocated since `init()`. At 1280x720
NV12, twenty times a second during a pinch-zoom, that is tens of megabytes a second on a
long-running server. `meow::viewport::reblack()` does only the black-fill, and
`MeowViewportUpstream.CroppedConvertFillsTheStripAndDoesNotReallocate` drives the real
encode device through a zoom sequence and fails if the surface buffer ever changes.

If reinitialising swscale fails, the **crop** is dropped and the previous working
configuration restored — not the session. Upstream reinitialised swscale at most twice per
session, so a failure there was effectively unreachable after startup; a crop makes it
reachable on every zoom, driven by network input, and `convert()` returning nonzero ends the
stream.
