#!/bin/bash
set -u
folder=$1
apk=$(find "$folder" -name 'smoke-*.apk' | head -n 1)
tests=$(find "$folder" -name runtime_tests | head -n 1)
translator=$(find "$folder" -name translator_tests | head -n 1)
status=0
for program in "$tests" "$translator"; do
    adb push "$program" /data/local/tmp/ >/dev/null
    name=$(basename "$program")
    adb shell "chmod 755 /data/local/tmp/$name && cd /data/local/tmp && TMPDIR=/data/local/tmp ./$name > $name.out 2>&1; echo exit \$?" | tee "$name.status"
    adb shell cat "/data/local/tmp/$name.out" | grep -v "^IOS \|^save backup" | tail -n 20
    grep -q "exit 0" "$name.status" || status=1
done
adb install -r "$apk"
adb logcat -c
adb shell am start -W -n io.github.eliasm4r.wondersbeyondrecomp/.GameActivity
sleep 15
adb logcat -d > logcat.txt
pid=$(adb shell pidof io.github.eliasm4r.wondersbeyondrecomp | tr -d '\r')
echo "app process: ${pid:-not running}"
grep -iE "SDL|wondersbeyond|libmain|FATAL|crash|DEBUG|nativeRunMain|message" logcat.txt | grep -vE "SdkSandbox|GameManager|SplashScreen|CoreBack|WindowManagerShell" | tail -n 80
adb shell screencap -p /data/local/tmp/screen.png && adb pull /data/local/tmp/screen.png screen.png >/dev/null
adb shell ls -la /sdcard/Android/data/io.github.eliasm4r.wondersbeyondrecomp/files/ /sdcard/Android/data/io.github.eliasm4r.wondersbeyondrecomp/files/game || true
if grep -q "FATAL EXCEPTION\|Fatal signal" logcat.txt; then
    status=1
fi
if [ -z "$pid" ]; then
    echo "the app is not running"
    status=1
fi
exit $status
