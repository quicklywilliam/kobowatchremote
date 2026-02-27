# Apple Watch Kobo Page Turner

Page turning remotes are great, but why buy a new thing to lose if you already have a perfectly capable device on your wrist?

## How it works

[Kobo-btpt](https://github.com/tsowell/kobo-btpt/) and the page turner Kobo sells work by communicating as Bluetooth peripherals. However, Apple's Core Bluetooth library does not allow Apple Watch to act as a peripheral. This plugin allows the *Kobo* to act as a Bluetooth peripheral, thereby enabling an Apple Watch to communicate with it (ie as a BLE central).

## Components

### Apple Watch App (`KoboRemote Watch App`)

Super simple WatchOS app that acts as a BLE central. You turn the crown to change pages. You'll get some gentle haptic feedback so you can manage all this without looking at your watch (or taking it out from under the covers). There's also a Next Page control center widget, so that Apple Watch Ultra users can set up their action button to open the app and advance the page. 

There is a substantially identical iPhone app for testing purposes (ie because developing on Apple Watch is slow and annoying).

### BLE Peripheral (`kobo_bt/ble_peripheral.c`)

Standalone binary running on the Kobo. Reverse engineers the MediaTek BT middleware (`libmtk_bt_service_client.so`) via dlopen to run the Bluetooth advertisement, GATT server and page-turning service, etc.
When it receives commands, it then forward them over a Unix socket.

### NickelHook Plugin (`kobo_bt/src/kobo_ble_remote.cc`)

Shared library loaded into Nickel's process via [NickelHook](https://github.com/pgaskin/NickelHook). Receives the commands and turns them into next/prev page events. It uses Nickel's power management to handle sleep, waking, etc.

## Building

### Plugin + BLE Peripheral (cross-compile for Kobo)

```sh
cd kobo_bt
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/pgaskin/nickeltc:1 make
```

Produces `libkobo-ble-remote.so` and `ble_peripheral`.

### Apple Watch App

Open `KoboRemote.xcodeproj` in Xcode and build for Apple Watch.

## Installation

1. Copy `ble_peripheral` to `/usr/local/Kobo/` on the Kobo
2. Copy `libkobo-ble-remote.so` to `/usr/local/Kobo/imageformats/`
3. Reboot the Kobo
4. Enable Bluetooth in Kobo settings
5. Install the WatchOS app and connect

To uninstall, create `/mnt/onboard/.kobo-ble-remote-uninstall` and reboot.
