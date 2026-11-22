# Omni2 Mouse 2 to Xbox Adaptive Controller

[Download the complete joystick bridge and Mac editor source ZIP](https://github.com/kortorder/omni2-pico-xbox-joystick/raw/refs/heads/main/omni2-pico-xbox-joystick-source.zip). Extract it before following the setup steps below.

Use the Bluetooth **Mouse 2** mode of a Permobil Omni2 as a right stick for games. A Raspberry Pi Pico W receives relative mouse motion over Bluetooth Classic and presents a USB joystick to the **right USB port** of an Xbox Adaptive Controller (XAC). A gentle hold turns the camera. Quick chair clicks or scroll nudges can tap any of twelve USB button positions. The mappings are saved on the Pico.

The chair supplies ordinary Bluetooth mouse input. This project does not control chair movement, change drive programming, or connect to wheelchair wiring. The standard Bluetooth-only build requires no phone, app account or Wi-Fi password. The optional shared-stick relay described below also needs private Wi-Fi configuration. This is a source project. Each operator builds an image for their own Pico and USB identity.

## What you need

- An Omni2 with an available **Mouse 2** slot. This implementation selects `R-net Mouse 2`, or the exact alias `Mouse 2` with a pointing-device class. Other slots and Bluetooth Low Energy peripherals are outside its scope.
- An **RP2040 Pico W or Pico WH**, a micro-USB **data** cable and a computer for commissioning. The firmware targets `pico_w`. A plain non-wireless Pico and a Pico 2 W are different boards.
- An XAC for console use. The USB descriptor requests 500 mA. Check the XAC’s accessory power budget and connect an appropriate external supply as described by Microsoft when the attached peripherals require it. The controller needs firmware 5.21 or later for USB buttons 11 and 12. [Microsoft XAC input device specification, §10](https://assets.xboxservices.com/assets/b8/05/b8055a56-827c-48f8-b8ec-2c7aa66ef069.pdf#page=15)
- For building, install CMake 3.20 or later, Ninja, Python 3, an Arm bare-metal GNU toolchain, picotool and Raspberry Pi Pico SDK 2.3.1. The settings editor uses Xcode’s Mac development tools. [Raspberry Pi SDK instructions](https://www.raspberrypi.com/documentation/microcontrollers/c_sdk.html)

## Build the normal firmware

Start with the standard **strict** image. It requires an encrypted Bluetooth link with a 16-byte encryption key and includes the nudge editor protocol. Use a new, separate build directory for each firmware variant.

1. Get the official SDK and its pinned dependencies. These commands are for a clean SDK installation.

   ```sh
   git clone --branch 2.3.1 --depth 1 https://github.com/raspberrypi/pico-sdk.git "$HOME/pico-sdk-2.3.1"
   git -C "$HOME/pico-sdk-2.3.1" submodule update --init --recursive
   export PICO_SDK_PATH="$HOME/pico-sdk-2.3.1"
   ```

2. From this repository, run `bash scripts/check.sh` for the host checks. Then build **one** of the following ways.

   **Private, in-house bench testing** can use the shared test USB identity by explicitly selecting it.

   ```sh
   bash scripts/build.sh "$HOME/omni-local-build/strict" -DOMNI_USE_PRIVATE_TEST_ID=ON
   ```

   **Hardware to distribute, manufacture or sell** requires an appropriate assigned VID/PID. Provide your allocated four-digit hexadecimal values in both the firmware and Mac tools. The allocator limits `1209:0002` to in-house test devices. Other devices can use that same identity. [pid.codes assignment and restrictions](https://pid.codes/1209/0002/)

   ```sh
   bash scripts/build.sh "$HOME/omni-local-build/strict-assigned" \
     -DOMNI_USB_VID="0x${YOUR_ASSIGNED_VID}" -DOMNI_USB_PID="0x${YOUR_ASSIGNED_PID}"
   ```

   The resulting file is `omni_right_stick.uf2` in the selected output directory. Keep private build inputs and outputs outside repositories and shared folders.

## First installation and pairing

1. Keep the chair stationary. Plug the Pico **directly into the computer** while holding its BOOTSEL button, then release the button. A drive called **RPI-RP2** should appear. Confirm it is the intended RP2040 board. Copy the UF2 onto that drive once. The Pico reboots as **Omni Right Stick**. [Raspberry Pi installation instructions](https://www.raspberrypi.com/documentation/microcontrollers/pico-series.html)
2. Choose **Bluetooth mode → Mouse 2** on the Omni2. With its mouse screen visible, hold forward until the first beep (about ten seconds), let go, then hold backward until the second beep and let go. A flashing Bluetooth symbol indicates that the slot can be discovered. Existing slot assignments and chair options vary. [Permobil Omni2 Mouse quick guide](https://hub.permobil.com/hubfs/Education%20Resources/Omni2%20Mouse%20Quick%20Guide.pdf?hsLang=en-us)
3. An unpaired Pico searches for five minutes after startup. Its LED blinks while searching or connecting. It stays on after the encrypted link and relative mouse descriptor are verified. To reopen a pairing window, briefly press and release BOOTSEL on the **already powered** Pico. To deliberately forget the Pico's saved pairing, hold BOOTSEL on the powered board for at least eight seconds, then release. This last step affects the Pico's Bluetooth record.
4. On the computer, use a game-controller input display or the included Mac observer. Hold right, left, up and down individually, releasing between each. Look for the matching signs on the two axes and a return to center. A rising mouse-report count alone cannot establish direction. Leave Bluetooth mode and confirm the joystick returns to neutral. Reconnect the board normally to check its saved pairing.
5. Move the Pico's USB cable from the computer to the **right** USB port of the powered XAC. Test each direction and return to center in Xbox Accessories or a game with a known right-stick action. Firmware derives stick strength from movement over the last 120 ms. With no new motion, the stick centers. In-game dead zones and remapping affect what you feel.

## Four nudge buttons

| Chair nudge | Default right-port input | USB button position |
| --- | --- | ---: |
| Right | B | 12 |
| Left | Right bumper | 4 |
| Up | X | 5 |
| Down | Right-stick click | 3 |

Let the control settle, make a quick nudge and return to center. A recognized mouse click or scroll burst produces a brief button press. Actual pointer movement continues to the stick immediately and cancels a pending shortcut. An ongoing scroll burst produces at most one tap. The game and any active Xbox Accessories profile determine the action.

Connect the Pico to the Mac normally and open **Omni Nudge Editor**. Choose a button or **Off** for each direction, click **Save to Pico**, and wait for the readback confirmation. The stored assignments survive restart. They can be changed without reflashing. Build and use the editor with the same identity used in the firmware. [Mac editor and observer instructions](mac/README.md)

## If strict pairing fails

A scanning, unreachable or busy chair is a different case from a verified short encryption key. Start by checking the slot, flashing symbol, cable, LED and the observer's pairing status. Renew the search with a short BOOTSEL press if needed. Firmware keeps one saved peer and does not silently replace it.

A chair that demonstrably negotiates **seven** encryption-key bytes cannot pass the normal 16-byte requirement. The optional workflow below is for an owner who can verify their own Mouse 2 and accepts a weaker link for that **one** device. Seven bytes provides less protection against key guessing. There is no general lower-security discovery image. [Bluetooth BR/EDR security specification](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/br-edr-controller/security-specification.html)

1. Build a **strict observation** image in a separate private directory by adding `-DOMNI_CAPTURE_SHORT_KEY=ON` to the same normal build command. Keep the original strict UF2 for rollback. Install the observation image on your own Pico and repeat pairing. Its 16-byte floor remains in effect. It omits nudge configuration so the operator can distinguish it from an operating image.
2. On a directly wired Mac, use `mac/build/omni-maintenance --probe` and then `mac/build/omni-maintenance --capture-chair-pin 0xLOCATION`, substituting the location printed by the probe. The capture is available only following a fresh qualifying Mouse 2 authentication with the verified seven-byte result. If no candidate qualifies, stop here. The helper writes six identifying address bytes into a new owner-only file and prints the local path. Keep that file private. [Wired capture instructions](mac/README.md)
3. If the owner chooses compatibility, build in another private directory with the **absolute path** printed by the helper. Use the same permitted USB identity selection as before and add `-DOMNI_APPROVED_MOUSE2_FILE="/absolute/private/path/to/peer.bin"`. Keep the source file, generated header and UF2 private. The locally generated UF2 contains the approved address. Do not post it, the address, or capture output to an issue or a release.
4. Install and repeat the directional and reconnect checks. This operating variant accepts a 7–16-byte encrypted SSP link only for the hardware-pinned Mouse 2 peer. It rejects an unrelated address, fixed-PIN fallback and unverified mouse report descriptors. Reinstall the normal strict image to restore the 16-byte floor. Clear the Pico pairing deliberately if changing peers.

## Verification and limits

The staged firmware was built for RP2040 and its host tests exercise descriptor parsing, peer selection, security refusal, stored bonds, USB neutralization, direction mapping, nudge timing and settings. Both the normal strict and single-peer operating configurations run the complete polling-loop fixture for four nudge mappings, fine motion, disconnect and return to center. The predecessor single-peer setup was observed on a Mac with a physical Omni2. The new strict configuration’s nudge behavior was validated with host fixtures. Each new chair, USB identity, XAC and game still needs the wired and console checks above.

The source includes no prebuilt firmware, chair identifiers, bonds or bench captures. Dependency sources come from the external Pico SDK. [Dependency provenance and notices](THIRD_PARTY.md)
## Optional shared right stick (0.1.6)

The ordinary Bluetooth-only firmware remains available and builds by default. An optional
Wi-Fi receiver lets the phone Pico send its right-stick input to this chair Pico while both
Picos remain in the Xbox Adaptive Controller's USB ports. The phone Pico stays in the left
port and this chair Pico stays in the right port. No app layouts or Xbox mappings change.

The phone's right-stick movement takes priority while it is nonzero. When it centers,
the current wheelchair joystick movement takes over. This does not add the two inputs
together. Bluetooth loss stops only the chair input; Wi-Fi loss stops only the phone input.
USB reset, suspend, maintenance and endpoint failures retire both sources. Nudge settings
and Bluetooth pairing remain in the same final 8 KiB of flash, which every firmware build
now checks it does not overwrite. Existing all-off nudge settings stay off.

This option requires the matching phone Pico relay firmware and a private Wi-Fi header
outside this source directory defining `SC_WIFI_SSID`, `SC_WIFI_PASSWORD` and
`SC_WIFI_COUNTRY`. Add `-DOMNI_PHONE_RELAY=ON -DOMNI_WIFI_CONFIG=/absolute/private/config.h`
to the normal build command; keep the same `OMNI_APPROVED_MOUSE2_FILE` option if this chair
already needs its approved Mouse 2 compatibility build. The standard build needs no Wi-Fi
configuration. Wi-Fi recovery does not deinitialize the shared Bluetooth radio.
The relay build disables Wi-Fi power saving after enabling the station interface to avoid
beacon/sleep delays. If that setup fails, the relay stays unavailable while the chair
Bluetooth and USB paths continue. The 250 ms chair lease is unchanged.

The matching Mac maintenance tool adds `--get-wifi 0xLOCATION`. It reads the chair's IPv4
address, UDP port 42737 and readiness without returning network names/passwords or changing
settings. The phone Pico is built for this exact address; reserve it in the router if it
changes between sessions. Credential-bearing build outputs are private and must not be
included in community source releases.

The relay accepts only SCX2 format 2 right-stick axes with a fresh session/challenge and
initial neutral transfer. It rejects left-stick, button, hat and trigger payloads. Each ACK
follows completion of the actual combined chair USB report, including neutral phone input
that hands back to a moving chair. The chair lease is 250 ms, with the phone refreshing it
every 50 ms. The upstream phone bridge keeps its existing 1-second lease; a fully lost
upstream connection can therefore take about 1.25 seconds to retire through both leases,
plus USB propagation. A stuck USB transfer triggers the existing 350 ms endpoint recovery.
These are software bounds, not measured radio latency guarantees.

Host fixtures cover the real main polling loop, source handoff, all four phone directions,
independent link loss, stale USB completions, maintenance and unchanged standalone behavior.
Real Mac USB and live phone/chair tests confirmed all four phone directions, chair input,
neutral release and source handoff. Complete combined Xbox acceptance remains a separate
hardware check for the optional private build.


## App, phone bridge and beginner help

This chair firmware works alongside the separate [phone Wi-Fi Pico bridge](https://github.com/kortorder/simplecontroller-pico-wifi-bridge). Get [Simple Controller for iPhone](https://github.com/kortorder/simplecontroller-ios) or the [Android community preview](https://github.com/kortorder/simplecontroller-android). Both apps include an illustrated quick start and Support tabs.

Read the [connection FAQ](https://github.com/kortorder/simplecontroller-ios/blob/main/FAQ.txt) and [watch the instructional videos](https://github.com/kortorder/simplecontroller-ios/releases). The free Windows and Mac receiver is a separate phone-to-computer route; it does not replace the chair's Bluetooth pairing. Windows provides a virtual Xbox controller. Mac maps phone controls to keyboard and mouse input, without native analog gamepad output. [PC and Mac setup](https://github.com/kortorder/simplecontroller-ios/blob/main/receiver/README.md). Sony Access does not accept this Pico as a USB accessory, and this project does not claim a verified PlayStation setup.
