#!/usr/bin/env bash
# Downloads the AIC8800D80 firmware (AICsemi; not open source, so never committed) into ./firmware/aic8800D80,
# pinned to a commit of github.com/shenmintao/aic8800d80 and checked against SHA-256 hashes.
set -euo pipefail
cd "$(dirname "$0")/.."
COMMIT=2e75f8885a053914c1b0328fdf4950f52691495d
BASE=https://raw.githubusercontent.com/shenmintao/aic8800d80/$COMMIT/fw/aic8800D80
DEST=firmware/aic8800D80
SHA256="shasum -a 256"; command -v shasum >/dev/null || SHA256=sha256sum
mkdir -p "$DEST"
while read -r sum name; do
    [[ -z "$name" ]] && continue
    echo "fetching $name"
    curl -fsSL "$BASE/$name" -o "$DEST/$name"
    got=$($SHA256 "$DEST/$name" | cut -d' ' -f1)
    if [[ "$got" != "$sum" ]]; then echo "SHA-256 mismatch for $name"; exit 1; fi
done <<'LIST'
9decb77435b7e9713e33e32da483d683b7329ed93b672b2d1b134031d7da5f67 fw_patch_table_8800d80_u02.bin
a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56 fw_adid_8800d80_u02.bin
f0e2f5bbc17bc327ca7f1574ff55370dfd863d931514347bb4abc18a74f6218f fw_patch_8800d80_u02.bin
ca738f5ea5aca6021c4f48b60ff35e835dfdaf072a8490e3ba8ba0a42440e3e5 fw_patch_8800d80_u02_ext0.bin
23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670 fmacfw_8800d80_u02.bin
c5c8c774dfae93aed71f8da7489e35159192135e4929591ea8a945b76deba6bd fmacfw_8800d80_h_u02.bin
LIST
for f in aic_userconfig_8800d80.txt aic_powerlimit_8800d80.txt; do curl -fsSL "$BASE/$f" -o "$DEST/$f"; done
echo "OK: firmware in $DEST"
ls -l "$DEST"
