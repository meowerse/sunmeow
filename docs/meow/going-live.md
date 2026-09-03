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

> **This section used to describe a defect that no longer exists. Corrected 2026-09-03 by
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
> **One real leak did survive, and is fixed here.** `tests/unit/test_httpcommon.cpp` built its
> download path from `platf::appdata()` — i.e. `~/.config/sunmeow` on a real desktop — and created
> a stray `~/.config/sunmeow/tests/` directory on every run. It never touched
> `sunmeow_state.json`, so no pairing was ever lost by it, but a test writing into the directory
> that holds `apps.json`, `credentials` and the paired-client list is one edit away from doing
> real damage. It now writes to `SUNSHINE_TEST_BIN_DIR`, the same build-tree location the auth
> test already used.
>
> **So: running the test suite no longer touches your live configuration at all.** Verify that
> claim yourself rather than trusting this paragraph — it is exactly the kind of statement that
> rots:
>
> ```bash
> md5sum ~/.config/sunmeow/sunmeow_state.json
> ./build/tests/test_sunshine --gtest_filter='-EncoderVariants/EncoderTest*'
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

## 4. Turning the two features on

Both ship **off**. That is not timidity: each changes what the client sees, and a host that
silently started cropping would be a worse bug than one that does nothing.

Set these in the web UI (they are in the **Audio/Video** tab) or directly in
`~/.config/sunmeow/sunmeow.conf`:

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
