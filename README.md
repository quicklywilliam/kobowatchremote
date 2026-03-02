# Apple Watch Kobo Page Turner

![demo](https://github.com/user-attachments/assets/37a65574-2fff-4d0a-9343-ac39d6a8d173)

Page turning remotes are great, but why buy a new thing to lose if you already have a perfectly capable device on your wrist?

Simply  turn the crown to change pages, or use a pinch gesture on supported devices (Series 10 or newer). You'll get some gentle haptic feedback so you can manage all this without looking at your watch (or taking it out from under the covers).


## Compatibility

The Kobo code was built for MediaTek-based devices (Clara BW/Colour, Libra Colour, Elipsa 2E) and will not work on others. It probably would be straight forward to implement for older devices, but I do not have one around to build/test on.


## Installation

### Kobo

This produces `KoboRoot.tgz` containing both the plugin and BLE peripheral.

1. Connect Kobo via USB
2. Copy `KoboRoot.tgz` to the `.kobo` folder on the Kobo
3. Eject and reboot — files are installed automatically
4. Enable Bluetooth in Kobo settings

### Apple Watch

Open `KoboRemote.xcodeproj`, build and install the Watch app target. It should connect automatically. There's also a Next Page control center widget, so that Apple Watch Ultra users can set up their action button to open the app and advance the page.


### Uninstall

Create `/mnt/onboard/.kobo-ble-remote-uninstall` and reboot.


## How it works

[Kobo-btpt](https://github.com/tsowell/kobo-btpt/) and the page turner Kobo sells work by communicating as Bluetooth peripherals. However, Apple's Core Bluetooth library does not allow Apple Watch to act as a peripheral. This plugin allows the *Kobo* to act as a Bluetooth peripheral, thereby enabling an Apple Watch to communicate with it (ie as a BLE central).

### Apple Watch App

The WatchOS app is a super basic implementation of a BLE central.

There is a substantially identical iPhone app for testing purposes (ie because developing on Apple Watch is slow and annoying).

### BLE Peripheral (`kobo_bt/ble_peripheral.c`)

Standalone binary running on the Kobo. Reverse engineers the MediaTek BT middleware (`libmtk_bt_service_client.so`) via dlopen to run the Bluetooth advertisement, GATT server and page-turning service, etc.
When it receives commands, it then forward them over a Unix socket.

### NickelHook Plugin (`kobo_bt/src/kobo_ble_remote.cc`)

Shared library loaded into Nickel's process via [NickelHook](https://github.com/pgaskin/NickelHook). Receives the commands and turns them into next/prev page events. It uses Nickel's power management to handle sleep, waking, etc.

## Building

```sh
cd kobo_bt
docker run --rm -v "$(pwd):/work" -w /work ghcr.io/pgaskin/nickeltc:1 make koboroot
```
