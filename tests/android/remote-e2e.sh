#!/bin/bash
#
# tests/android/remote-e2e.sh [API] — remote access of the Android app over a camex
# tunnel, end to end, in Docker: a camex server and a camex client "operator" in
# containers, the app (debug APK, x86_64) on an emulator of that API (default 34)
# joins the server with the tunnel library of bundles/camex, and the operator opens
# the app's web page through the server by the app's address in the tunnel.
#
# Checks: without the VPN consent the tunnel says so (screen state and /api/state,
# read over adb); with it (appops, or the system dialog on Android 5.0) the app gets
# an address from the server; the operator gets 401 without the password and the
# state with it, through the tunnel; the page's operation runs (no programmer on the
# emulator: it fails with its message); the tunnel and the page come back by
# themselves after a reboot, with nobody at the device.
#
# Needs the images compot/android, compot/cross, compot/android-emulator and the
# emulator in ~/.cache/compot/toolchains/android-emulator (see camex's
# tests/android/tunnel-e2e.sh, the same setup).
#
set -e
API=${1:-34}
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CAMEX="$ROOT/bundles/camex"
OUT="$ROOT/android/build/remote-e2e"
ANDROID_IMAGE=${ANDROID_IMAGE:-compot/android:2026.10}
CROSS_IMAGE=${CROSS_IMAGE:-compot/cross:2026.11}
EMULATOR_IMAGE=${EMULATOR_IMAGE:-compot/android-emulator:2026.11}
EMULATOR_DIR=${EMULATOR_DIR:-$HOME/.cache/compot/toolchains/android-emulator}
GRADLE_CACHE=${GRADLE_CACHE:-$HOME/.gradle}
# the group of /dev/kvm as the Docker daemon sees it (992 on the compot machine, where
# a sandbox may show nogroup)
KVM_GROUP=${KVM_GROUP:-$(stat -c %g /dev/kvm)}
KEEP=${KEEP:-}      # KEEP=1: leave the containers running at the end, to look around
NAME=${E2E_NAME:-microsnander-remote-e2e-$$}
SRV=$NAME-srv
OP=$NAME-op
NET=$NAME-net
PKG=org.openipc.microsnander
PSK="e2e-secret"
PASSWORD="e2e-page"
PORT=8080
UID_GID="$(id -u):$(id -g)"
pass=0
fail=0
ok() { pass=$((pass + 1)); echo "PASS: $*"; }
bad() { fail=$((fail + 1)); echo "FAIL: $*"; }
cleanup() {
    [ -z "$KEEP" ] || { echo "KEEP: left running: $NAME $SRV $OP (network $NET)"; return; }
    docker rm -f "$NAME" "$OP" "$SRV" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
[ -f "$CAMEX/src/version.h" ] || { echo "no bundles/camex: git submodule update --init"; exit 1; }
rm -rf "$OUT"
mkdir -p "$OUT/w"

echo "== camex (static)"
docker run --rm --pull never -u "$UID_GID" -v "$CAMEX:/src:ro" -v "$OUT:/out" -w /out "$CROSS_IMAGE" sh -c '
    mkdir -p camex && cp -a /src/Makefile /src/src camex/ && cd camex && make -s LDFLAGS=-static' \
    > "$OUT/camex.log" 2>&1 || { tail -30 "$OUT/camex.log"; exit 1; }
cp "$OUT/camex/camex" "$OUT/w/"
KEYS=$("$OUT/w/camex" --genkey)
printf '%s\n' "$KEYS" | sed -n 's/^server_key=/server_key=/p' > "$OUT/w/server.key"
PUBKEY=$(printf '%s\n' "$KEYS" | sed -n 's/^public_key=//p')

echo "== the app (debug, x86_64)"
docker run --rm --pull never -u "$UID_GID" -e HOME=/tmp -v "$ROOT:/src" -v "$GRADLE_CACHE:/cache/gradle" \
    -w /src/android "$ANDROID_IMAGE" gradle --no-daemon -q -PmicrosnanderAbis=x86_64 :app:assembleDebug \
    > "$OUT/gradle.log" 2>&1 || { tail -30 "$OUT/gradle.log"; exit 1; }
cp "$ROOT/android/app/build/outputs/apk/debug/app-debug.apk" "$OUT/w/app.apk"

cat > "$OUT/w/avd.sh" <<EOS
export ANDROID_SDK_ROOT=/emu ANDROID_HOME=/emu ANDROID_AVD_HOME=/w/.emu/avd HOME=/w/.emu/home
A=\$ANDROID_AVD_HOME/e2e.avd
mkdir -p \$A \$HOME
printf 'avd.ini.encoding=UTF-8\npath=%s\ntarget=android-$API\n' \$A > \$ANDROID_AVD_HOME/e2e.ini
printf 'abi.type=x86_64\nhw.cpu.arch=x86_64\nimage.sysdir.1=system-images/android-$API/default/x86_64/\ntag.id=default\nhw.ramSize=2048\ndisk.dataPartition.size=2G\nhw.lcd.width=720\nhw.lcd.height=1280\nhw.lcd.density=320\nhw.gpu.enabled=yes\nhw.gpu.mode=swiftshader_indirect\n' > \$A/config.ini
/emu/emulator/emulator -avd e2e -no-window -no-audio -no-boot-anim -no-snapshot -gpu swiftshader_indirect -accel on > /w/emulator.log 2>&1 &
/emu/platform-tools/adb wait-for-device
until [ "\$(/emu/platform-tools/adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 2; done
echo BOOTED
sleep 100000
EOS

echo "== emulator (API $API), camex server and operator"
docker network create "$NET" >/dev/null
docker run -d --name "$NAME" --network "$NET" --pull never --device /dev/kvm --group-add "$KVM_GROUP" \
    -u "$UID_GID" -v "$EMULATOR_DIR:/emu:ro" -v "$OUT/w:/w" "$EMULATOR_IMAGE" sh /w/avd.sh >/dev/null
docker run -d --name "$SRV" --network "$NET" --pull never --cap-add NET_ADMIN --device /dev/net/tun \
    -e CAMEX_POOL_HOLD=0 -v "$OUT/w:/w" "$CROSS_IMAGE" sh -c "/w/camex --mode server --port 5800 \
    --transport udp --local-cidr 10.200.0.1/24 --server-key-file /w/server.key --psk $PSK > /w/server.log 2>&1" >/dev/null
SRV_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$SRV")
sleep 1
docker run -d --name "$OP" --network "$NET" --pull never --cap-add NET_ADMIN --device /dev/net/tun \
    -v "$OUT/w:/w" "$CROSS_IMAGE" sh -c "/w/camex --mode client --auto --name operator --server-host $SRV_IP \
    --port 5800 --transport udp --server-pubkey $PUBKEY --psk $PSK > /w/operator.log 2>&1" >/dev/null

i=0
until docker logs "$NAME" 2>&1 | grep -q BOOTED; do
    i=$((i + 1))
    [ $i -lt 120 ] || { echo "the emulator did not boot:"; tail -5 "$OUT/w/emulator.log"; exit 1; }
    sleep 5
done
ADB="docker exec $NAME /emu/platform-tools/adb"

$ADB install -r /w/app.apk >/dev/null
[ "$API" -lt 33 ] || $ADB shell pm grant $PKG android.permission.POST_NOTIFICATIONS
# the settings as the setup screen saves them: remote access with a password, the tunnel on
cat > "$OUT/w/remote.xml" <<EOF
<?xml version='1.0' encoding='utf-8' standalone='yes' ?>
<map>
    <boolean name="enabled" value="true" />
    <int name="port" value="$PORT" />
    <string name="password">$PASSWORD</string>
    <boolean name="camex" value="true" />
    <string name="camex.server">$SRV_IP:5800</string>
    <string name="camex.name">box-1</string>
    <string name="camex.transport">UDP</string>
    <string name="camex.pubkey">$PUBKEY</string>
    <string name="camex.psk">$PSK</string>
    <string name="camex.mtu"></string>
    <string name="camex.keepalive"></string>
</map>
EOF
$ADB push /w/remote.xml /data/local/tmp/remote.xml >/dev/null
$ADB shell "run-as $PKG sh -c 'mkdir -p shared_prefs && cat /data/local/tmp/remote.xml > shared_prefs/remote.xml'"

# the state of the page, read on the device itself (no tunnel needed)
page_state() {
    $ADB forward tcp:18080 tcp:$PORT >/dev/null
    docker exec "$NAME" sh -c "curl -s -m 5 -u op:$PASSWORD http://127.0.0.1:18080/api/state" 2>/dev/null
}

echo "== without the VPN consent"
APPOPS_OK=1
$ADB shell appops set $PKG ACTIVATE_VPN deny 2>&1 | grep -q 'Unknown operation' && APPOPS_OK=0
$ADB shell am start -n $PKG/.MainActivity >/dev/null
sleep 8
s=$(page_state)
if grep -q '"tunnel":"Tunnel: the VPN consent is needed' <<<"$s"; then ok "no consent: the page says the tunnel needs it"
else bad "no consent: $s"; fi

echo "== the consent"
if [ $APPOPS_OK = 1 ]; then
    $ADB shell appops set $PKG ACTIVATE_VPN allow
    # appops are written to the disk later: a reboot soon after would lose it
    $ADB shell appops write-settings >/dev/null
else      # Android 5.0: the system dialog, by Allow VPN on the app's screen, confirmed by a tap
    $ADB shell input keyevent KEYCODE_HOME
    $ADB shell am start -n $PKG/.MainActivity >/dev/null
    sleep 3
    xy=""
    for k in 1 2 3 4 5 6; do
        $ADB shell input swipe 360 1100 360 100 200
        $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
        xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' | grep 'b_consent' | head -1 |
            sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p')
        [ -z "$xy" ] || break
    done
    set -- $xy
    [ -n "$xy" ] && $ADB shell input tap $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
    sleep 3
    $ADB shell uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1
    xy=$($ADB shell cat /data/local/tmp/ui.xml | tr '>' '\n' | grep 'resource-id="android:id/button1"' | head -1 |
        sed -n 's/.*bounds="\[\([0-9]*\),\([0-9]*\)\]\[\([0-9]*\),\([0-9]*\)\]".*/\1 \2 \3 \4/p')
    set -- $xy
    [ -n "$xy" ] && $ADB shell input tap $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))
fi
# the app tries again every minute; opening it starts the tunnel at once
$ADB shell am start -n $PKG/.MainActivity >/dev/null

APP_IP=""
for k in $(seq 1 45); do
    APP_IP=$(page_state | sed -n 's/.*"tunnel":"Tunnel: http:\/\/\([0-9.]*\):.*/\1/p')
    [ -z "$APP_IP" ] || break
    sleep 2
done
if [ -n "$APP_IP" ]; then ok "the tunnel is up: the app's address $APP_IP"; else bad "no tunnel: $(page_state)"; fi

echo "== the operator, through the server"
op() { docker exec "$OP" curl -s -m 10 "$@" 2>&1; }
code=$(op -o /dev/null -w '%{http_code}' "http://$APP_IP:$PORT/api/state")
[ "$code" = 401 ] && ok "without the password: 401" || bad "without the password: '$code'"
s=$(op -u op:$PASSWORD "http://$APP_IP:$PORT/api/state")
grep -q '"version":' <<<"$s" && ok "with the password: the state through the tunnel" || bad "state: $s"
grep -q '<title>MicroSNANDer Web</title>' <<<"$(op -u op:$PASSWORD "http://$APP_IP:$PORT/")" && ok "the page" || bad "the page"
r=$(op -u op:$PASSWORD -X POST -H 'X-MicroSNANDer: 1' "http://$APP_IP:$PORT/api/detect")
sleep 2
s=$(op -u op:$PASSWORD "http://$APP_IP:$PORT/api/state")
grep -q 'Started' <<<"$r" && grep -q 'No programmer, or no access to it.' <<<"$s" &&
    ok "an operation from the operator runs (no programmer on the emulator)" || bad "detect: $r / $s"
$ADB logcat -d | grep -q "$PSK" && bad "the psk is in logcat" || ok "no psk in logcat"

echo "== after a reboot, with nobody at the device"
$ADB reboot
sleep 10
$ADB wait-for-device
until [ "$($ADB shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do sleep 2; done
s=""
for k in $(seq 1 60); do
    # the server may give another address after the reconnect: the one the page says
    IP2=$(page_state | sed -n 's/.*"tunnel":"Tunnel: http:\/\/\([0-9.]*\):.*/\1/p')
    [ -z "$IP2" ] || s=$(op -u op:$PASSWORD "http://$IP2:$PORT/api/state")
    grep -q '"version":' <<<"$s" && break
    sleep 2
done
grep -q '"version":' <<<"$s" && ok "after the reboot the operator reaches the page through the tunnel" || bad "after reboot: $s"

echo
echo "passed $pass, failed $fail (API $API)"
[ $fail = 0 ]
