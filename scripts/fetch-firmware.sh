#!/usr/bin/env bash
# Downloads the MT7612U firmware from linux-firmware (MediaTek, redistributable) into ./firmware/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p firmware
BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/mediatek
for f in mt7662_rom_patch.bin mt7662.bin LICENCE; do
    [[ $f == LICENCE ]] && url=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/LICENCE.ralink_a_mediatek_company_firmware || url=$BASE/$f
    out=firmware/$f; [[ $f == LICENCE ]] && out=firmware/LICENCE.ralink_a_mediatek_company_firmware
    echo "fetching $url"
    curl -fsSL "$url" -o "$out"
done
# Expected sizes as of linux-firmware 2026 (warn, don't fail, if MediaTek updates them).
[[ $(wc -c < firmware/mt7662.bin) -eq 81908 ]] || echo "note: mt7662.bin size changed upstream"
[[ $(wc -c < firmware/mt7662_rom_patch.bin) -eq 26350 ]] || echo "note: mt7662_rom_patch.bin size changed upstream"
ls -l firmware
