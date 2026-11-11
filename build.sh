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
VERSION=$(tr -d ' \n' < VERSION)
S=Sources/Shared
CFLAGS=(-std=gnu11 -O2 -g -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration -I "$S" -DMW_VERSION="\"$VERSION\"")
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
    $cc "${CFLAGS[@]}" Tests/test_json.c "$S/json.c" "$S/knownnet.c" -o "$B/test_json"
    "$B/test_json"
    $cc "${CFLAGS[@]}" Tests/test_rxq.c "$S/rxq.c" -lpthread -o "$B/test_rxq"
    "$B/test_rxq"
    # The app is built with the Command Line Tools only; on newer SDKs these SwiftUI attributes are macros whose
    # plugins ship only with Xcode (users saw "plugin for module 'SwiftUIMacros' not found"). CI's older toolchain
    # wouldn't notice, so check the source.
    if grep -nE '^[^/]*(@State|@Observable|@Bindable|@Entry|#Preview)\b' App/MacWifi/*.swift; then
        echo "App/MacWifi uses a SwiftUI macro; keep that state in Model (@Published) instead"; exit 1
    fi
    $cc "${CFLAGS[@]}" Tests/test_hci.c "$S/hci.c" -o "$B/test_hci"
    "$B/test_hci"
    $cc "${CFLAGS[@]}" Tests/test_bthost.c "$S/bthost.c" "$S/hidboot.c" "$S/hci.c" -o "$B/test_bthost"
    "$B/test_bthost"
    $cc "${CFLAGS[@]}" Tests/test_reorder.c "$S/reorder.c" -o "$B/test_reorder"
    "$B/test_reorder"
    $cc "${CFLAGS[@]}" Tests/test_captive.c "$S/captive.c" -o "$B/test_captive"
    "$B/test_captive"
    $cc "${CFLAGS[@]}" Tests/test_p256.c "$S/p256.c" $libs -lpthread -o "$B/test_p256"
    "$B/test_p256"
    $cc "${CFLAGS[@]}" -I Tests Tests/test_sae.c "$S/sae.c" "$S/p256.c" "$S/wpa.c" "$S/crypto_backend.c" $libs -lpthread -o "$B/test_sae"
    "$B/test_sae"
    if [[ "$(uname)" != "Darwin" ]]; then
        # connect logic from main.c against a simulated firmware + AP, behind stand-in IOKit headers
        $cc "${CFLAGS[@]}" -Wno-unused-function -no-pie -I Tests/macstubs -I Sources/macwifictl Tests/sim_connect.c \
            Sources/macwifictl/descdump.c "$S/aic_msg.c" "$S/aic_fwload.c" "$S/aic_fmac.c" "$S/ieee80211.c" \
            "$S/aic_data.c" "$S/dhcp.c" "$S/bridge.c" "$S/eapol.c" "$S/wpa.c" "$S/crypto_backend.c" "$S/json.c" \
            "$S/knownnet.c" "$S/rxq.c" "$S/reorder.c" "$S/captive.c" "$S/sae.c" "$S/p256.c" "$S/hci.c" "$S/bthost.c" "$S/hidboot.c" $libs -lpthread \
            -Wl,--unresolved-symbols=ignore-all -o "$B/sim_connect"
        "$B/sim_connect" > "$B/sim_connect.log" 2>&1 || { cat "$B/sim_connect.log"; exit 1; }
        if grep -q "horse" "$B/sim_connect.log"; then echo "a test password appeared in the output"; exit 1; fi
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
        -I Sources/macwifictl Sources/macwifictl/main.c Sources/macwifictl/descdump.c Sources/macwifictl/feth.c Sources/macwifictl/power.c Sources/macwifictl/rx_async.c Sources/macwifictl/bt_usb.c "$S/hci.c" "$S/bthost.c" "$S/hidboot.c" "$S/aic_msg.c" "$S/aic_fwload.c" "$S/aic_fmac.c" "$S/ieee80211.c" "$S/aic_data.c" "$S/dhcp.c" "$S/bridge.c" "$S/eapol.c" "$S/wpa.c" "$S/crypto_backend.c" "$S/json.c" "$S/knownnet.c" "$S/rxq.c" "$S/reorder.c" "$S/captive.c" "$S/sae.c" "$S/p256.c" \
        -framework IOKit -framework CoreFoundation -framework SystemConfiguration -o "$B/macwifictl"
    codesign --force --sign - "$B/macwifictl"
    file "$B/macwifictl"
}

build_app() {
    [[ "$(uname)" == "Darwin" ]] || { echo "the menu bar app needs macOS"; exit 1; }
    echo "== MacWifi.app (arm64)"
    local app="$B/MacWifi.app"
    rm -rf "$app"
    mkdir -p "$app/Contents/MacOS"
    cp App/MacWifi/Info.plist "$app/Contents/Info.plist"
    /usr/libexec/PlistBuddy -c "Set :CFBundleShortVersionString $VERSION" "$app/Contents/Info.plist"
    swiftc -O -parse-as-library -target "arm64-apple-macos$MINOS" App/MacWifi/MacWifiApp.swift \
        -o "$app/Contents/MacOS/MacWifi"
    # App icon: our own art (App/MacWifi/Icon/AppIcon.svg, rendered to AppIcon.png) packed into an .icns
    # with tools that ship with macOS. The art already fills Apple's icon grid as a rounded square.
    mkdir -p "$app/Contents/Resources"
    local iconset="$B/AppIcon.iconset"
    rm -rf "$iconset"; mkdir -p "$iconset"
    local sz
    for sz in 16 32 128 256 512; do
        sips -z $sz $sz App/MacWifi/Icon/AppIcon.png --out "$iconset/icon_${sz}x${sz}.png" >/dev/null
        sips -z $((sz*2)) $((sz*2)) App/MacWifi/Icon/AppIcon.png --out "$iconset/icon_${sz}x${sz}@2x.png" >/dev/null
    done
    iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
    rm -rf "$iconset"
    codesign --force --sign - "$app"
}

# The installer package. By default it includes this Mac's downloaded firmware when present (a package for your
# own Macs; AICsemi's firmware has no open-source license, so don't share that package). With MW_PKG_NO_FIRMWARE=1
# (the GitHub release build) it contains none, and the installer downloads and verifies it on the target Mac.
build_pkg() {
    build_cli
    build_app
    local with_fw=1
    [[ "${MW_PKG_NO_FIRMWARE:-0}" == 1 || ! -d firmware/aic8800D80-legacy-mcu1 || ! -d firmware/aic8800D80 ]] && with_fw=0
    echo "== MacWifi.pkg ($VERSION, firmware $([[ $with_fw == 1 ]] && echo included || echo "downloaded at install"))"
    local root="$B/pkgroot" ver=$VERSION
    local A="$root/Library/Application Support/macwifi"
    rm -rf "$root"
    mkdir -p "$A/firmware" "$root/Library/LaunchDaemons" "$root/Library/LaunchAgents" "$root/Applications"
    cp "$B/macwifictl" "$A/macwifid"
    cp "$B/macwifictl" "$A/macwifictl"
    [[ $with_fw == 1 ]] && cp -R firmware/aic8800D80 firmware/aic8800D80-legacy-mcu1 "$A/firmware/"
    cp scripts/uninstall.sh "$A/uninstall.sh"
    cp scripts/fetch-firmware.sh "$A/fetch-firmware.sh"
    cp scripts/report.sh "$A/report.sh"
    cp packaging/io.github.fpm8588.macwifid.plist "$root/Library/LaunchDaemons/"
    cp packaging/io.github.fpm8588.macwifi.menu.plist "$root/Library/LaunchAgents/"
    cp -R "$B/MacWifi.app" "$root/Applications/"
    pkgbuild --root "$root" --component-plist packaging/components.plist --scripts packaging/scripts \
        --identifier io.github.fpm8588.macwifi --version "$ver" --install-location / --ownership recommended \
        "$B/MacWifi-$ver.pkg"
    cp "$B/MacWifi-$ver.pkg" "$B/MacWifi.pkg"
    echo "Built $B/MacWifi-$ver.pkg. Install: sudo installer -pkg $B/MacWifi.pkg -target /"
}

case "${1:-all}" in
    test)  run_tests ;;
    cli)   build_cli ;;
    app)   build_app ;;
    pkg)   build_pkg ;;
    all)   run_tests; if [[ "$(uname)" == "Darwin" ]]; then build_cli; build_app; fi ;;
    clean) rm -rf "$B" ;;
    *)     echo "usage: $0 test|cli|app|pkg|all|clean"; exit 2 ;;
esac
