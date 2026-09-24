# Going live — switching this machine from the distro package to sunmeow

Everything in `src/meow/` is merged and tested, but **none of it is running on the reference
machine yet**, and that is deliberate. This document is the missing step: how to move from the
packaged Sunshine to this fork, how to get back if it goes badly, and how to actually turn the
two features on once you are there.

Building is covered in [`README.meow.md`](../../README.meow.md) and is not repeated here.

---

## 1. Know what is running before you change it

On the reference machine, the thing streaming today is **not this fork**:

```bash
readlink -f /proc/$(pgrep -x sunshine)/exe   # /usr/bin/sunshine
pacman -Qo /usr/bin/sunshine                 # sunshine 2026.724.5619-1
systemctl --user show sunshine -p FragmentPath
                        # /usr/lib/systemd/user/app-dev.lizardbyte.app.Sunshine.service
```

Two facts that matter more than they look:

- `sunshine.service` is the **distro package's** alias; the real unit there is
  `app-dev.lizardbyte.app.Sunshine.service`. This fork does not claim that name -- its own unit
  is `app-meow.alxnko.sunmeow.service`, aliased to `sunmeow.service`.

  **Upgrading from a build made before that fix:** the old alias was materialised at enable
  time and `systemctl --user disable` only removes symlinks the *current* unit text declares,
  so a stale `~/.config/systemd/user/sunshine.service` pointing at our unit survives the
  upgrade and keeps shadowing the distro's. Remove it once:

  ```bash
  systemctl --user disable app-meow.alxnko.sunmeow.service
  rm -f ~/.config/systemd/user/sunshine.service
  systemctl --user daemon-reload
  systemctl --user --now enable app-meow.alxnko.sunmeow.service
  ```

  Commands aimed at our alias work, but `systemctl --user status sunmeow` reporting `alias`
  rather than a state is not an error.
- The packaged binary already carries the capabilities KMS capture needs:
  ```bash
  getcap /usr/bin/sunshine     # cap_sys_admin,cap_sys_nice=p
  ```
  **A locally built binary does not.** This is the single most common reason a self-built
  Sunshine produces a black screen on Wayland while the packaged one is fine — see
  [`README.meow.md`](../../README.meow.md) § *Running on KDE Plasma 6 / Wayland*.

---

## 2. Back up the configuration first — this is not boilerplate

Since the rebrand the two binaries **no longer share a directory**: the distro package reads
`~/.config/sunshine/`, this fork reads `~/.config/sunmeow/`. That separation is the point of
the rebrand -- there is no shared mutable state left to corrupt, and switching between the two
no longer risks one overwriting the other's settings, apps or paired clients.

Back ours up anyway, because the hazards below are ours:

```bash
cp -a ~/.config/sunmeow ~/.config/sunmeow.bak-$(date +%F-%H%M)
```

If you are coming from a pre-rebrand build of this fork, your state is still under
`~/.config/sunshine/`; back that up too and copy it across.

`sunmeow_state.json` holds your **paired clients**. Losing it does not lose settings or apps,
but every paired device has to re-pair with a PIN.

> **This section used to describe a cause that no longer exists — the leak class did not go away. Corrected 2026-09-03 by
> measurement, not by reading the code.** It claimed the test binary overwrites
> `sunmeow_state.json` with a fixture via `tests/unit/test_http_pairing.cpp`. That was true once.
> It is not true now, and acting on it wastes time chasing phantom data loss.
>
> What was actually measured, by md5-summing the live file around each run:
>
> | Run | Effect on `~/.config/sunmeow/sunmeow_state.json` |
> | --- | --- |
> | `--gtest_filter='*PairingTest*'` | **unchanged** |
> | Each hardware suite individually | **unchanged** |
> | Whole suite minus `EncoderTest` (703 tests) | **unchanged** |
>
> Why it is fixed: upstream's 2026-09-02 security work added `nvhttp::test_support::reset_client_state()`,
> and `PairingTest` now sets `FRESH_STATE` and resets in-memory state in `SetUp`/`TearDown`
> (`tests/unit/test_http_pairing.cpp`), while the new `tests/unit/test_nvhttp_client_auth.cpp`
> redirects `config::nvhttp.file_state` into the build tree and restores it afterwards.
>
> **Two real leaks did survive, and are fixed here.** `tests/unit/test_httpcommon.cpp` built its
> download path from `platf::appdata()` — i.e. `~/.config/sunmeow` on a real desktop — and created
> a stray `~/.config/sunmeow/tests/` directory on every run. It never touched
> `sunmeow_state.json`, so no pairing was ever lost by it, but a test writing into the directory
> that holds `apps.json`, `credentials` and the paired-client list is one edit away from doing
> real damage. It now writes to `SUNSHINE_TEST_BIN_DIR`, the same build-tree location the auth
> test already used. `tests/unit/test_file_handler.cpp` did the same through
> `platf::appdata()/tests/path/` (its cleanup removed only `path/`) and now writes there too.
>
> **This is per-test hygiene, and it is not enough on its own.** The capture tests still resolve
> the XDG portal restore token through `platf::appdata()`, so on a real desktop they read — and
> can rewrite — `~/.config/sunmeow/portal_token`, and the live pairings were lost again on
> 2026-09-14, after the measurement above. Do not rely on individual tests behaving: run the
> suite against a throwaway config directory. `tests/meow/config_sandbox.cpp` builds that into
> the test binary itself (it overrides `XDG_CONFIG_HOME`, and `HOME` on macOS, before anything
> reads them); the `env` line below is a second, independent layer on top of it.
>
> ```bash
> md5sum ~/.config/sunmeow/sunmeow_state.json
> env -u CONFIGURATION_DIRECTORY -u SUNSHINE_MIGRATE_CONFIG \
>   HOME=/var/tmp/sunmeow-testhome XDG_CONFIG_HOME=/var/tmp/sunmeow-testhome/.config \
>   ./build/tests/test_sunshine --gtest_filter='-EncoderVariants/EncoderTest*'
> md5sum ~/.config/sunmeow/sunmeow_state.json   # must be identical
> ls ~/.config/sunmeow/tests 2>/dev/null && echo 'LEAK IS BACK'
> ```
>
> Back up anyway before an upgrade. Not because the tests will eat it, but because §3 below
> replaces a running binary and the state file is the only thing you cannot regenerate.

---

## 3. The switchover, and the way back

Run the fork in the foreground first. Do not enable a unit for it until you have watched it work
at least once — a service that fails at 3am is much harder to diagnose than one failing in front
of you.

```bash
systemctl --user stop sunshine        # the packaged instance releases port 47989/47990
./build/sunshine                      # foreground, logs to the terminal
```

Verify, in this order, because each step rules out the layer below it:

1. The web UI answers at `https://localhost:47990` (self-signed certificate on first run).
2. Your existing pairing survived — the client list is populated, and moonmeow connects without
   asking for a PIN. If it asks, restore the backup from step 2 and start again.
3. A stream actually starts, and the picture is the desktop rather than black. Black here almost
   always means capabilities, not code:
   ```bash
   sudo cp build/sunshine /tmp
   sudo setcap cap_sys_admin,cap_sys_nice+p /tmp/sunshine
   sudo mv /tmp/sunshine build/sunshine
   ```
   The detour through `/tmp` is upstream's, and it exists because `setcap` fails on filesystems
   without xattr support.

**Going back** is one command, and it is worth rehearsing before you need it:

```bash
# Ctrl-C the foreground sunmeow first
systemctl --user start sunshine
```

The packaged binary and the fork never run simultaneously — they contend for the same ports and
the same config directory. If you want them to coexist, give the fork its own config with
`XDG_CONFIG_HOME`, and accept that it then has its own separate pairings.

---

## 4. The features, and turning them off

Since 2026-09-24 every meow feature is **on by default** (owner decision D1), for fresh
installs and for existing ones whose `sunmeow.conf` does not mention the key. Each has an
explicit off switch, in the web UI (**Audio/Video** tab) or in `~/.config/sunmeow/sunmeow.conf`:

| Key | Default | What it does |
| --- | --- | --- |
| `meow_viewport_following` | `enabled` | Host crops the capture to the rectangle the client is displaying, and tells it which frame first carries the crop. Off: requests are answered with the full desktop. |
| `meow_cursor_reporting` | `enabled` | KWin sends the cursor as metadata, the host draws it back in and sends its position (`0x3004`) to clients that subscribe, so a zoomed view follows the mouse. |
| `meow_adaptive_bitrate` | `enabled` | Encoder bitrate follows the path: loss, rising round-trip time, goodput and the client's receiver reports (`0x3005`). |
| `adaptive_bitrate_min` | `0` (automatic) | Floor in kbps; automatic is `max(1000, 25% of negotiated)`. |
| `adaptive_bitrate_max` | `0` (automatic) | Ceiling in kbps; automatic is the client's advertised maximum when above the negotiated rate, else the negotiated rate. `max_bitrate` always applies. |
| `output_name` | first display | `all` streams every monitor as one region. |

```ini
# to turn a feature off:
meow_viewport_following = disabled
meow_cursor_reporting   = disabled
meow_adaptive_bitrate   = disabled
```

An existing `adaptive_bitrate_min` keeps working: it is now an explicit floor instead of the
switch that turns adaptation on. None of the new messages opens a port or needs a firewall or
Tailscale ACL change: they ride the existing encrypted control stream, and none exceeds 24
bytes.

Full reference: [`docs/configuration.md`](../configuration.md) and
[`docs/meow/viewport-following.md`](./viewport-following.md).

---

## 5. What to expect on a 5360x1440 desktop over 5–8 Mbps

The arithmetic is the whole reason viewport following exists.

| | Pixels | At 30 fps | Bits per pixel at 8 Mbps |
| --- | --- | --- | --- |
| Whole desktop, 5360x1440 | 7.72 M | 232 Mpx/s | **0.035** |
| Phone-shaped crop, ~2400x1080 | 2.59 M | 78 Mpx/s | **0.103** |

H.265 needs roughly 0.05–0.10 bpp before text stops smearing. Streaming the whole desktop at
8 Mbps sits *below* that floor — which is why the full desktop looks soft no matter how the
encoder is tuned. Cropping to what you are actually looking at buys roughly **3x the bits per
pixel** without spending another kilobit, and that is the difference between readable and
unreadable code.

Adaptive bitrate is the other half: it keeps you at the top of whatever the link will carry
right now, instead of at a fixed guess that is either wasteful or lossy depending on the hour.

---

### Measured on the reference machine (2026-09-24), and what it means for a slow link

All with this tree's bundled FFmpeg or the system `ffmpeg` on the RTX 5050; the probes are in
`tools/meow/` so every number can be re-run.

- **A bitrate change costs one IDR, but no burst.** FFmpeg's NVENC wrapper reconfigures with
  `resetEncoder`/`forceIDR` set and exposes no option to avoid it, so every adaptive-bitrate
  change produces an IDR - measured: 1 per change, 3 in 540 frames. Because Sunshine opens NVENC
  as CBR with a one-frame VBV, that IDR is squeezed into one frame's budget (6255 bytes against
  neighbours of 6250 at 3 Mbps; 16672 against 16667 at 8 Mbps): a momentarily softer frame, not
  a bandwidth spike. That is why the controller changes the rate at most every 3 s.
- **Only NVENC and libx264 follow a runtime change.** VA-API (AMD/Intel) and libx265 ignore it.
- **The cropped upload saves most of the per-frame copy.** Copying the 5360x1440 frame to the
  GPU takes 3.0-4.6 ms; with a 16:9 crop of one monitor 1.2-1.4 ms, with a 2x phone zoom 0.6 ms
  (`tools/meow/cuda_upload_probe.cu`, median of 300 interleaved copies on a shared GPU).
- **`nvenc_preset` matters enormously for scrolling text on HEVC and AV1 - worth changing by
  hand.** On code scrolling at half a pixel per frame (a smooth scroll decelerating), HEVC at
  Sunshine's default `nvenc_preset = 1` (p1) spent 1708 kbps for 32.2 dB PSNR, while p3 spent
  **323 kbps for 37.6 dB**; at a 5 Mbps target, 4454 kbps/35.4 dB against 420 kbps/42.3 dB. AV1
  p1 behaves like HEVC p1; AV1 p2 and up is the best of all (665 kbps/46.3 dB). H.264 is not
  affected (37.5 dB at p1 and p3), and whole-pixel scrolling is fine at p1 for every codec. p3
  costs roughly 0.3-0.5 ms more per frame at 720p and 1-2 ms at 1080p/1440p on this GPU, so the
  default was **not** changed; on a slow link with an HEVC or AV1 client, set
  `nvenc_preset = 3`. Spatial and temporal AQ made no measurable difference (-0.1 to -0.5 dB).
- **H.264 NVENC pads a static desktop to the full bitrate.** With `cbr_padding` off, an idle
  H.264 stream still carries one filler NAL (type 12) per frame and uses the whole 8 Mbps, where
  HEVC uses 0.04 Mbps and AV1 0.03 Mbps for the same idle desktop. Prefer HEVC or AV1 on the
  client for desktop use over a constrained link.

---

## 6. Known gaps — read before concluding something is broken

- **VA-API is not cropped, and does not adapt its bitrate.** The crop is implemented for the
  software scaler and the CUDA/NVENC path; the VA-API path goes through `egl::sws_t`, which is
  untouched, so an AMD/Intel-encode host never answers viewport requests. VA-API also ignores a
  runtime bitrate change (measured, see `src/meow/adaptive_bitrate_encoder.h`); the log says so.
- **The cursor is drawn by the host only on memory-buffer capture.** With KWin capture on a
  hybrid laptop (desktop on the iGPU, NVENC on the dGPU) frames arrive in system memory and the
  host draws the cursor. On a DMA-BUF path (pure NVIDIA, VA-API, Vulkan) KWin keeps drawing the
  cursor and no position is sent; the client falls back to dead reckoning. If KWin negotiates a
  format the blend cannot write (10-bit), the capture restarts once with the embedded cursor and
  logs why.
- **Metadata cursor mode is unverified on hardware.** KWin only authorises the installed
  `/usr/local/bin/sunmeow` for `zkde_screencast`, so every unit test ran without it. The first
  real run should confirm: the stream shows a cursor, it moves on an idle desktop, the cursor
  shape (premultiplied RGBA assumed) looks right, and the log has
  `[kwingrab] Cursor: metadata (drawn by the host, ...)`.
- **NvFBC is reasoned about, not tested.** No NvFBC hardware was available.

State each of these plainly if you report a problem; "the crop does nothing" means something
very different on AMD than on NVIDIA.
