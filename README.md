# Omni2 Mouse 2 to Xbox Adaptive Controller

[Download the complete joystick bridge and Mac editor source ZIP](https://github.com/kortorder/omni2-pico-xbox-joystick/raw/refs/heads/main/omni2-pico-xbox-joystick-source.zip). Extract it before following the setup steps below.

Use the Bluetooth **Mouse 2** mode of a Permobil Omni2 as a right stick for games. A Raspberry Pi Pico W receives relative mouse motion over Bluetooth Classic and presents a USB joystick to the **right USB port** of an Xbox Adaptive Controller (XAC). A gentle hold turns the camera. Quick chair clicks or scroll nudges can tap any of twelve USB button positions. The mappings are saved on the Pico.

The chair supplies ordinary Bluetooth mouse input. This project does not control chair movement, change drive programming, or connect to wheelchair wiring. No phone, app account or Wi-Fi password is required. This is a source project. Each operator builds an image for their own Pico and USB identity.

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
