# Mac tools for the chair bridge

**Omni Nudge Editor** lets you assign a button to each of four chair nudges and save those choices on a USB-connected Pico. Use the chair bridge’s nudge-capable firmware. The app reads the Pico on opening and writes only when you click **Save to Pico**. If you already have the locally built app, skip to [Change the buttons](#change-the-buttons).

### Build the app on your Mac

You need the complete chair-bridge source folder and Apple development tools with the macOS SDK, Swift and C++. The script uses `xcrun`, `codesign` and Python 3. It builds a local app for the Mac running the script, exercises synthetic inputs, and verifies the signed app both before and after ZIP extraction.

1. Open Terminal at the root of the chair-bridge source folder and run `cd mac`.
2. For **in-house testing** with the shared test identity, run `bash build.sh --private-test-id`. For hardware using your allocated USB identity, run `bash build.sh --usb-id VID:PID`, replacing VID and PID with the hexadecimal values used by the firmware. The test VID/PID is shared and may only be used on private in-house test hardware. [pid.codes test identity terms](https://pid.codes/1209/0002/)
3. Open the `build` folder and extract **Omni Nudge Editor.zip**. Place the extracted app in your local Applications folder and open it. The other two outputs are command-line programs for operators: `omni-maintenance` and `omni-observer`.

The app uses an **ad-hoc local signature**. This source package includes no Developer ID signature or Apple notarization. If macOS blocks a download supplied by someone else, follow your Mac’s software approval policy. The build also accepts `--output DIRECTORY` to put its outputs in another folder.

### Change the buttons

1. Plug the chair bridge Pico directly into your Mac using USB. Leave your chair at rest. Open **Omni Nudge Editor**.
2. Each pop-up menu selects what **Right**, **Left**, **Up** or **Down** presses on the XAC's right USB port. Choose **Off** for a direction you want to leave unassigned. The defaults are Right = B, Left = RB, Up = X and Down = RS (right-stick press). Xbox Accessories remapping can change the effect of these buttons.
3. Click **Save to Pico**. Keep the Pico connected until the app shows **Saved and verified on the Pico**. It confirms the stored settings and their revision. Changing an assignment this way does not require reflashing.
4. **Reload from Pico** reads back the current assignments. If another edit, a disconnection or a storage error interrupted a save, reload before trying again. Unsaved menu changes stay local to the window.

Use Tab to move between the four labeled menus and the Reload and Save buttons. The window reports connection and save status to accessibility tools.

### Wired operator tools

Begin with `./build/omni-maintenance --probe`. The tool requires exactly one matching USB device and prints its current Mac USB location. Substitute that printed value for `0xLOCATION` below. Commands that change the Pico require the location explicitly.

| Purpose | Command after `./build/omni-maintenance` |
| --- | --- |
| Read the four nudge slots and current revision | `--get-nudges 0xLOCATION` |
| Save four slots and verify the new revision | `--set-nudges 0xLOCATION CURRENT_REV RIGHT LEFT UP DOWN` |
| Request the Pico's ROM update mode | `--enter-bootsel 0xLOCATION` |
| Capture the eligible chair address in strict capture mode | `--capture-chair-pin 0xLOCATION` |
| Observe incoming motion counters in the owner-pinned diagnostic mode | `--watch-chair-motion 0xLOCATION SECONDS` |

For the settings CLI, each direction takes a HID button slot from 0 to 12. Zero means Off. The editor shows human-readable button names. A save checks the current revision, submits a single write and reads back the next revision and all four assigned slots.

The BOOTSEL command applies only to bridge firmware that implements this wired maintenance capability. Confirm the `RPI-RP2` volume appears before any separate firmware update. For a board's first installation, follow the [firmware instructions](../README.md).

The capture command is for the optional, strict capture firmware and the specific procedure described in the [firmware guide](../README.md). After a fresh qualifying Mouse 2 authentication, capture writes six address bytes to a new user-private directory and file on that Mac. It prints only the file path. The file belongs to the person commissioning their own chair. Keep it out of repositories, issues, logs and shared builds. The regular and owner-pinned images omit this capture endpoint.

For a USB bench check, run `./build/omni-observer --probe 20` while the chair is resting. The observer listens to actual joystick input and reads the Bluetooth diagnostic status. `--watch 30` displays input changes including the twelve USB button bits on nudge firmware. `--directions 90` guides a four-direction check. Use the chair’s Bluetooth Mouse mode for these tests. Observe while the chair is stationary, away from its Drive controls.

For a timed comparison of incoming mouse direction with outgoing USB input in the pinned diagnostic firmware, run:

```sh
python3 compare-chair-motion.py "$PWD/build/omni-maintenance" "$PWD/build/omni-observer" 0xLOCATION 30
```

Wait for **READY**, then try one direction and release. The CHAIR lines count incoming reports carrying a direction, wheel sign or button edge. The USB lines show actual joystick axes and button masks. The helper also supports `--self-test`, which exercises its process coordination using synthetic streams.

### What the offline checks cover

The local build runs the editor model and device-selection tests, the wired protocol tests, the USB observer decoder tests and the comparison helper’s synthetic checks. These check saving and verifying assignments, stale revisions, incomplete saves, bounds on diagnostic data, and button press/release decoding. Verify your own chair, Pico, Mac and XAC with local hardware before relying on their behavior together.
