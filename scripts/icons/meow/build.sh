#!/usr/bin/env bash
# Regenerate every sunmeow icon/favicon/tray/banner asset from branding/meow/sunmeow.svg.
# Run from anywhere:  bash scripts/icons/meow/build.sh
# Needs: rsvg-convert (librsvg) and python3 with Pillow. Outputs are byte-identical across
# re-runs with the versions they were produced with (librsvg 2.62, Pillow 12.3); other
# versions may re-encode PNGs with tiny pixel differences. See build_icons.py for the list of
# outputs and why most of them overwrite upstream Sunshine files in place.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
command -v rsvg-convert >/dev/null || { echo "rsvg-convert (librsvg) not found" >&2; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "python3 Pillow not found" >&2; exit 1; }
exec python3 "$HERE/build_icons.py" "$@"
