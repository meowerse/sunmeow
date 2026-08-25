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

- `sunshine.service` is an **alias**; the real unit is `app-dev.lizardbyte.app.Sunshine.service`.
  Commands aimed at the alias work, but `systemctl --user status sunshine` reporting `alias`
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

Both binaries read the same directory, `~/.config/sunshine/`. It is the only shared mutable
state between them, and it is where every hazard in this document lives.

```bash
cp -a ~/.config/sunshine ~/.config/sunshine.bak-$(date +%F-%H%M)
```

`sunshine_state.json` holds your **paired clients**. Losing it does not lose settings or apps,
but every paired device has to re-pair with a PIN.

> **Known defect, and the reason this backup is mandatory rather than prudent:** the repo's own
> test binary writes into the real config directory. `tests/unit/test_http_pairing.cpp` drives
> the genuine pairing code, which persists to `config::nvhttp.file_state`; `$HOME` does not
> redirect it. Running `./build/tests/test_sunshine` therefore **overwrites
> `sunshine_state.json` with a fixture** (a single device literally named `test`) and creates a
> stray `~/.config/sunshine/tests/` directory. A running Sunshine keeps the real client list in
> memory and will write it back when it next saves — so the damage is latent, not immediate, and
> it only becomes real if the process restarts first. Check `docs/meow/TOUCHPOINTS.md` and the
> repository's open branches for the sandbox fix before assuming this still applies.

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

## 4. Turning the two features on

Both ship **off**. That is not timidity: each changes what the client sees, and a host that
silently started cropping would be a worse bug than one that does nothing.

Set these in the web UI (they are in the **Audio/Video** tab) or directly in
`~/.config/sunshine/sunshine.conf`:

| Key | Default | What it does |
| --- | --- | --- |
| `meow_viewport_following` | off | Host crops the capture to the rectangle the client is actually displaying. |
| `adaptive_bitrate_min` | `0` (off) | Floor, in kbps. **Setting this is what enables adaptation at all.** |
| `adaptive_bitrate_max` | `0` (use ceiling) | Optional cap, in kbps. Has no effect on its own. |
| `output_name` | first display | `all` streams every monitor as one region. |

```ini
meow_viewport_following = enabled
adaptive_bitrate_min    = 3000
adaptive_bitrate_max    = 8000
output_name             = all
```

`adaptive_bitrate_max` alone does nothing, by design — without a floor there is no range to
adapt inside, and Sunshine logs a warning rather than silently reinterpreting your intent. The
effective ceiling is the **smallest** of what Moonlight asked for, `max_bitrate`, and
`adaptive_bitrate_max`; adaptation never raises the bitrate above what the client requested.

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

## 6. Known gaps — read before concluding something is broken

- **Double magnification.** The host crops *and* the client can zoom. Both are working as
  designed; together they over-magnify. Use one or the other until this is reconciled.
- **VA-API is not cropped.** The crop is implemented for the software scaler and the CUDA/NVENC
  path. `gl_cuda_vram_t` and the VA-API path both go through `egl::sws_t`, which is untouched —
  one shader change would cover both, and it has not been made. On the reference machine
  (NVIDIA encode) this does not bite; on an AMD-only host it means the feature silently does
  nothing.
- **NvFBC is reasoned about, not tested.** No NvFBC hardware was available. The kernel is
  covered by the hardware probe; that device's `convert()` is inspection only.
- **No end-to-end session has been run.** Every layer is unit tested and the CUDA kernel is
  verified on the real RTX 5050 by `tools/meow/viewport_cuda_probe.cpp` — but a full
  host-to-phone stream with cropping live has not been performed. That is what this document
  is for.

State each of these plainly if you report a problem; "the crop does nothing" means something
very different on AMD than on NVIDIA.
