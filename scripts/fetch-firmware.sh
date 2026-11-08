#!/usr/bin/env bash
# Downloads AICsemi's AIC8800D80 firmware (not open source, so never committed) from the Linux driver repository
# github.com/shenmintao/aic8800d80, pinned to commits and checked against SHA-256 hashes. Two matched sets:
#
#   firmware/aic8800D80              SDK V5, branch main         for chips with chip_mcu_id = 0
#   firmware/aic8800D80-legacy-mcu1  SDK V3, branch legacy-mcu1  for chips with chip_mcu_id = 1 (e.g. UGREEN
#                                                                AX900 a69c:5723, product 0x8820); see Linux
#                                                                issue #58 and docs/HARDWARE_LOG.md
#
# `macwifictl fwload` picks the set from the chip's ID register.
set -euo pipefail
# MW_FW_ROOT: where firmware/ goes (the installer sets it to the install folder); default: the repository.
cd "${MW_FW_ROOT:-$(dirname "$0")/..}"
SHA256="shasum -a 256"; command -v shasum >/dev/null || SHA256=sha256sum

fetch_set() {   # fetch_set <commit> <dest>; reads "sha256 name" lines on stdin
    local commit=$1 dest=$2 sum name got
    local base=https://raw.githubusercontent.com/shenmintao/aic8800d80/$commit/fw/aic8800D80
    mkdir -p "$dest"
    while read -r sum name; do
        [[ -z "$name" ]] && continue
        echo "fetching $dest/$name"
        curl -fsSL "$base/$name" -o "$dest/$name"
        got=$($SHA256 "$dest/$name" | cut -d' ' -f1)
        if [[ "$got" != "$sum" ]]; then echo "SHA-256 mismatch for $dest/$name"; exit 1; fi
    done
}

fetch_set 2e75f8885a053914c1b0328fdf4950f52691495d firmware/aic8800D80 <<'LIST'
9decb77435b7e9713e33e32da483d683b7329ed93b672b2d1b134031d7da5f67 fw_patch_table_8800d80_u02.bin
a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56 fw_adid_8800d80_u02.bin
f0e2f5bbc17bc327ca7f1574ff55370dfd863d931514347bb4abc18a74f6218f fw_patch_8800d80_u02.bin
ca738f5ea5aca6021c4f48b60ff35e835dfdaf072a8490e3ba8ba0a42440e3e5 fw_patch_8800d80_u02_ext0.bin
23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670 fmacfw_8800d80_u02.bin
c5c8c774dfae93aed71f8da7489e35159192135e4929591ea8a945b76deba6bd fmacfw_8800d80_h_u02.bin
LIST
for f in aic_userconfig_8800d80.txt aic_powerlimit_8800d80.txt; do
    curl -fsSL "https://raw.githubusercontent.com/shenmintao/aic8800d80/2e75f8885a053914c1b0328fdf4950f52691495d/fw/aic8800D80/$f" \
        -o "firmware/aic8800D80/$f"
done

fetch_set bbf6cb4015c8da4e7e113a6a6282f191213e0854 firmware/aic8800D80-legacy-mcu1 <<'LIST'
58c07eb4c79de6e6beff683686495dec152688f7de59816cfbe71a0c664ca7c8 fw_patch_table_8800d80_u02.bin
a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56 fw_adid_8800d80_u02.bin
3c5fa5bb678b835adbe42cfe0326074cf8aa5c6d3b93d16d2943b63ad4106b72 fw_patch_8800d80_u02.bin
1ec680c2b63dcaa0e5d33c5fb6d1857d030f8145c05c385e243760388a61a0da fmacfw_8800d80_u02.bin
3108517dc4d783788a33b2c467309afe2159d46284e65c52cf21d5ec6b8dbcc5 aic_userconfig_8800d80.txt
LIST

echo "OK: firmware in firmware/aic8800D80 and firmware/aic8800D80-legacy-mcu1"
ls -l firmware/aic8800D80 firmware/aic8800D80-legacy-mcu1
