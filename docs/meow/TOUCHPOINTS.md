# Touch-point registry

Every edit to a file we did not create is marked in place with a `MEOW-TOUCH(<feature>)`
comment and recorded here. See `CLAUDE.md` §2–§3 for the rules this table enforces.

The complete list of places a future upstream merge can hurt:

```bash
git grep -n 'MEOW-TOUCH' -- src/
```

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `src/platform/linux/kwingrab.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | The `wl_display`, the registry and the `zkde_screencast_unstable_v1` proxy are all private to `kwin::screencast_t`. Issuing `stream_region` and binding `zxdg_output_manager_v1` for logical geometry has to happen where those Wayland objects live, and `screencast_t` is neither virtual nor exported, so it cannot be subclassed or wrapped from `src/meow/`. All geometry *logic* was extracted to `src/meow/display_union.h`; what remains upstream is protocol plumbing plus one `stream_region` call. | 2026-08-24 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description. A separate page would leave the setting undocumented where users actually look. | 2026-08-24 |
