#!/usr/bin/env bash
# build.sh — builds with the free Xcode Command Line Tools (xcode-select --install). $0, ad-hoc signing only.
#
#   ./build.sh test     hardware-free unit tests (macOS uses CommonCrypto; Linux uses OpenSSL libcrypto)
#   ./build.sh cli      build/macwifictl (arm64, macOS 13+)
#   ./build.sh all      test + cli
#   ./build.sh clean
set -euo pipefail
cd "$(dirname "$0")"

B=build
MINOS=13.0
S=Sources/Shared
CFLAGS=(-std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -I "$S")
SHARED=("$S/ieee80211.c" "$S/wpa.c" "$S/crypto_backend.c" "$S/aic_msg.c")
mkdir -p "$B"

run_tests() {
    # macOS ships Bash 3.2, where "${empty[@]}" trips `set -u`, so keep the extra flags in a plain string.
    local cc=${CC:-cc} libs=""
    [[ "$(uname)" == "Darwin" ]] || libs="-lcrypto"
    echo "== unit tests ($cc, $(uname))"
    # shellcheck disable=SC2086
    $cc "${CFLAGS[@]}" Tests/test_wifi.c "${SHARED[@]}" $libs -o "$B/test_wifi"
    "$B/test_wifi"
    $cc "${CFLAGS[@]}" Tests/test_aic.c "$S/aic_msg.c" -o "$B/test_aic"
    "$B/test_aic"
    $cc "${CFLAGS[@]}" -I Sources/macwifictl Tests/test_descdump.c Sources/macwifictl/descdump.c -o "$B/test_descdump"
    "$B/test_descdump" > /dev/null && echo "descdump: OK"
    $cc "${CFLAGS[@]}" Tests/test_fwload.c "$S/aic_fwload.c" "$S/aic_msg.c" -o "$B/test_fwload"
    # With the firmware downloaded (scripts/fetch-firmware.sh), also check the loader against the real files.
    local real=""
    [[ -f firmware/aic8800D80/fw_patch_table_8800d80_u02.bin ]] && real="$real --v5 firmware/aic8800D80"
    [[ -f firmware/aic8800D80-legacy-mcu1/fw_patch_table_8800d80_u02.bin ]] &&
        real="$real --legacy firmware/aic8800D80-legacy-mcu1"
    # shellcheck disable=SC2086
    "$B/test_fwload" $real
    # shellcheck disable=SC2086
    $cc "${CFLAGS[@]}" Tests/test_eapol.c "$S/eapol.c" "$S/wpa.c" "$S/crypto_backend.c" $libs -o "$B/test_eapol"
    "$B/test_eapol"
    $cc "${CFLAGS[@]}" Tests/test_data.c "$S/aic_data.c" "$S/aic_msg.c" -o "$B/test_data"
    "$B/test_data"
    $cc "${CFLAGS[@]}" Tests/test_dhcp.c "$S/dhcp.c" -o "$B/test_dhcp"
    "$B/test_dhcp"
    $cc "${CFLAGS[@]}" Tests/test_bridge.c "$S/bridge.c" -o "$B/test_bridge"
    "$B/test_bridge"
    if [[ "$(uname)" != "Darwin" ]]; then
        # connect logic from main.c against a simulated firmware + AP, behind stand-in IOKit headers
        $cc "${CFLAGS[@]}" -Wno-unused-function -no-pie -I Tests/macstubs -I Sources/macwifictl Tests/sim_connect.c \
            Sources/macwifictl/descdump.c "$S/aic_msg.c" "$S/aic_fwload.c" "$S/aic_fmac.c" "$S/ieee80211.c" \
            "$S/aic_data.c" "$S/dhcp.c" "$S/bridge.c" "$S/eapol.c" "$S/wpa.c" "$S/crypto_backend.c" $libs -lpthread \
            -Wl,--unresolved-symbols=ignore-all -o "$B/sim_connect"
        "$B/sim_connect" > "$B/sim_connect.log" || { cat "$B/sim_connect.log"; exit 1; }
        tail -1 "$B/sim_connect.log"
    fi
    $cc "${CFLAGS[@]}" -I Tests Tests/test_fmac.c "$S/aic_fmac.c" "$S/aic_msg.c" -o "$B/test_fmac"
    if [[ -f firmware/aic8800D80-legacy-mcu1/aic_userconfig_8800d80.txt ]]; then
        "$B/test_fmac" --legacy firmware/aic8800D80-legacy-mcu1
    else
        "$B/test_fmac"
    fi
}

build_cli() {
    [[ "$(uname)" == "Darwin" ]] || { echo "macwifictl needs macOS (IOKit)"; exit 1; }
    echo "== macwifictl (arm64)"
    clang "${CFLAGS[@]}" -arch arm64 -mmacosx-version-min=$MINOS -Wno-deprecated-declarations \
        -I Sources/macwifictl Sources/macwifictl/main.c Sources/macwifictl/descdump.c Sources/macwifictl/feth.c "$S/aic_msg.c" "$S/aic_fwload.c" "$S/aic_fmac.c" "$S/ieee80211.c" "$S/aic_data.c" "$S/dhcp.c" "$S/bridge.c" "$S/eapol.c" "$S/wpa.c" "$S/crypto_backend.c" \
        -framework IOKit -framework CoreFoundation -o "$B/macwifictl"
    codesign --force --sign - "$B/macwifictl"
    file "$B/macwifictl"
}

case "${1:-all}" in
    test)  run_tests ;;
    cli)   build_cli ;;
    all)   run_tests; if [[ "$(uname)" == "Darwin" ]]; then build_cli; fi ;;
    clean) rm -rf "$B" ;;
    *)     echo "usage: $0 test|cli|all|clean"; exit 2 ;;
esac
