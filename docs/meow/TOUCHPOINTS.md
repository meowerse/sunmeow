# Touch-point registry

Every edit to a file we did not create is marked in place with a `MEOW-TOUCH(<feature>)`
comment and recorded here. See `CLAUDE.md` §2–§3 for the rules this table enforces.

The complete list of places a future upstream merge can hurt:

```bash
git grep -n 'MEOW-TOUCH' -- src/
```

| File | Marker | Why layers 1–3 were insufficient | Added |
| --- | --- | --- | --- |
| `src/platform/linux/kwingrab.cpp` | `MEOW-TOUCH(unified-desktop-capture)` | The `wl_display`, the registry and the `zkde_screencast_unstable_v1` proxy are all private members of `kwin::screencast_t`, which is defined inside the `.cpp`, is not virtual and is not exported — so it cannot be subclassed or wrapped from `src/meow/` (layers 1–3 all fail). What remains upstream: the `zxdg_output_manager_v1`/`xdg_output` bindings, four fields on `output_parameter_t`, the `stream_region` call, and the log statements that report the decision. **All geometry and all policy** — the bounding box, the scale choice, the coverage test, the protocol-version gate and the oversize refusal — live in `src/meow/display_union.h` and are unit tested there. `kwin_t::verify_and_update_display_parameters()` is a layer-2 override of an existing `virtual`, not new upstream logic. | 2026-08-24 |
| `docs/configuration.md` | (no marker — prose only) | Documents the reserved `output_name = all` value next to the existing `output_name` description. A separate page would leave the setting undocumented where users actually look. | 2026-08-24 |
