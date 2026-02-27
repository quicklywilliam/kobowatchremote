#!/bin/bash
# Fast deploy: build plugin + ble_peripheral, scp both, restart Nickel
set -e

KOBO=root@10.0.1.20
DEST_SO=/usr/local/Kobo/imageformats/libkobo-ble-remote.so
DEST_BLE=/usr/local/Kobo/ble_peripheral

echo "=== Building ==="
docker run --rm -v "$PWD:$PWD" -w "$PWD" ghcr.io/pgaskin/nickeltc:1 make ble_peripheral

echo "=== Deploying to Kobo ==="
scp libkobo-ble-remote.so "$KOBO:$DEST_SO"
scp ble_peripheral "$KOBO:$DEST_BLE"
ssh "$KOBO" "chmod +x $DEST_BLE"

echo "=== Restarting Nickel ==="
ssh "$KOBO" "killall ble_peripheral 2>/dev/null; killall nickel 2>/dev/null; sleep 2; export LD_LIBRARY_PATH=/usr/local/Kobo; LIBC_FATAL_STDERR_=1 /usr/local/Kobo/nickel -platform kobo -skipFontLoad &"

echo "=== Done — check syslog: ssh $KOBO logread -f ==="
