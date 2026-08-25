# Viewport following ("foveated" streaming)

**Status:** host side, software scaling path only. Off by default.

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

Add to `sunshine.conf`:

```
meow_viewport_following = enabled
```

The key is read once at first use. It accepts the same values as every other Sunshine
boolean (`enabled`/`on`/`true`/`yes`/`1`, and the corresponding negatives).

Sunshine will log `Warning: Unrecognized configurable option [meow_viewport_following]` at
startup. **That is expected and harmless** — it means Sunshine's own parser did not claim the
key, which is exactly the point; ours reads it independently from the same file. The line
that tells you whether it actually took effect is logged when a stream's control channel
starts:

```
Info: meow viewport following: enabled. The client may request a crop of the desktop; ...
```

It is deliberately **not** registered in `src/config.cpp`, so it does not appear in the Web
UI. Registering it there would drag `config.h`, `configuration.md`, `config.html` and
`en.json` along with it — `tests/integration/test_config_consistency.cpp` enforces exactly
that — for a setting that needs no UI. This is the same trade `meow::display_union` made for
`output_name = all`. Unknown keys are ignored by `config::parse_config()`, so an unpatched
Sunshine reading the same file is unaffected.

## Why it defaults to off

1. **Pointer input is not remapped yet.** Client-supplied absolute pointer and touch
   coordinates go through `video::make_port()`, which knows only the *full* captured
   desktop. While a crop is active, a tap lands in the wrong place. Turning cropping on is
   therefore a trade — far more readable text in exchange for absolute pointer input that
   needs the matching host-side remap. Relative mouse input is unaffected.
2. **The compatibility floor.** An existing working setup must not change behaviour on
   upgrade (CLAUDE.md §2). Nobody's stream changes until they ask for it.
3. **It only covers one scaling path today** — see below. Defaulting it on would advertise a
   feature that silently does nothing for VA-API and CUDA users.

## What is covered, and what is not

| Path | File | Cropped? |
| --- | --- | --- |
| Software scaling (swscale) | `src/video.cpp`, `avcodec_software_encode_device_t::convert()` | **yes** |
| VA-API / EGL | `src/platform/linux/graphics.cpp`, `egl::sws_t` | no |
| CUDA / NVENC | `src/platform/linux/cuda.cu` | no |
| Windows, macOS | — | no |

> **On the machine this was developed against, viewport following does nothing today.**
> Verified by running the built binary: with `capture = kwin` the host selects
> `h264_nvenc`/`hevc_nvenc`/`av1_nvenc`, which takes `mem_type_e::cuda` and therefore the
> CUDA scaler — not the software one. Until the CUDA path is wired up, seeing any benefit on
> an NVIDIA host means forcing a software encoder (`encoder = software`), which costs far
> more CPU than the crop saves for most people. This is the single most important follow-up,
> and the reason the setting defaults to off.
>
> The CUDA change is small and well-shaped: `cuda::sws_t` already has `float scale` and a
> `viewport_t`, and the kernels already compute `float x = idX * scale`. Cropping is a
> source origin added there plus a per-frame assignment of `scale`/`viewport` from
> `meow::viewport::plan_t`. It needs `cuda.h`, `cuda.cu` and `cuda.cpp`, an nvcc build, and
> a GPU to verify — which is why it is a separate change rather than this one.

The software path is the one taken when the capture backend hands **system memory** to an
encoder that does not scale on the GPU — on Linux/PipeWire that is `mem_type_e::system`,
which reaches `video.cpp:make_avcodec_encode_session()` with a null `encode_device->data`
and constructs an `avcodec_software_encode_device_t`. The software encoders (`libx264`,
`libx265`, `libsvtav1`) always take it.

The other two scalers already carry a destination viewport and a scale factor of their own —
`egl::sws_t::offsetX/offsetY/out_width/out_height`, and `cuda::sws_t::viewport`/`scale`.
`meow::viewport::plan_t` emits exactly those fields, so extending to them is a matter of
adding a source origin to a GLSL shader and a CUDA kernel. That is a separate change with a
separate build dependency, and it is not in this one.

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
that never published any scaler geometry — one that does not run through the software
scaling path — where the host does not know the captured desktop size and therefore cannot
even name the coordinate system an answer would be in.

## Resetting

A stale crop cannot leak forward:

- `on_scaler_init()` clears the pending rectangle, so a new session, a display mode change
  or an encoder reinit all start uncropped;
- `reset()` runs when the control broadcast ends;
- the owning-scaler check means a scaler only ever acts on state it published itself;
- with no rectangle pending, the owner's plan **is** the full-frame plan, so a scaler that
  was cropped reverts on the very next frame rather than waiting for a reinit.

## Cost per frame

In the steady state, one relaxed atomic load, roughly a dozen integer operations and six
integer comparisons — measured at well under 1 us/call by
`MeowViewportSession.PerFramePlanningIsCheap`, against a 5.5 ms frame budget at 180 Hz. No
allocation and no extra copy on any frame.

A **pan** (same crop size, new origin) costs two pointer additions and nothing else: the
scaler is not rebuilt, because its dimensions did not change. `MeowViewportEndToEnd`
asserts both that a pan skips reconfiguration and that it still lands on the right pixels.

A **zoom** (crop size changed) rebuilds the swscale filter tables once. The client rate
limits viewport updates to one per 50 ms, so this is bounded at 20 Hz in the worst case and
is zero while the user is reading. The encoder is never reinitialised and the encode surface
never changes size.
