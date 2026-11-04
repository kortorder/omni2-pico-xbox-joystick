# Source and dependency notices

This source package contains bridge firmware, host test fixtures and Mac tooling. The build uses a separately installed Raspberry Pi Pico SDK. No SDK checkout, chip firmware image or precompiled application is included here.

| Component | Pinned source | Notices supplied |
| --- | --- | --- |
| Raspberry Pi Pico SDK 2.3.1 | [079c6f39023649b154152db30f1d781e884879bc](https://github.com/raspberrypi/pico-sdk/tree/079c6f39023649b154152db30f1d781e884879bc) | [SDK license](third_party_licenses/pico-sdk-LICENSE.txt), [pico_printf notice](third_party_licenses/pico-printf-LICENSE.txt) |
| TinyUSB | [86ad6e56c1700e85f1c5678607a762cfe3aa2f47](https://github.com/hathach/tinyusb/tree/86ad6e56c1700e85f1c5678607a762cfe3aa2f47) | [TinyUSB license](third_party_licenses/tinyusb-LICENSE.txt) |
| CYW43 driver | [055d64274b014dd7b1c2fc94d26e8a18face7124](https://github.com/georgerobotics/cyw43-driver/tree/055d64274b014dd7b1c2fc94d26e8a18face7124) | [CYW43 license](third_party_licenses/cyw43-LICENSE.txt), [Raspberry Pi terms](third_party_licenses/cyw43-LICENSE.RP.txt) |
| BTstack | [eb0bb8b5ea6d234ccb940313b47f7a5c3b4e20ec](https://github.com/bluekitchen/btstack/tree/eb0bb8b5ea6d234ccb940313b47f7a5c3b4e20ec) | [BTstack license](third_party_licenses/btstack-LICENSE.txt), [Raspberry Pi terms](third_party_licenses/btstack-LICENSE.RP.txt) |
| lwIP | [77dcd25a72509eb83f72b033d219b1d40cd8eb95](https://github.com/lwip-tcpip/lwip/tree/77dcd25a72509eb83f72b033d219b1d40cd8eb95) | [lwIP terms](third_party_licenses/lwip-COPYING.txt) |

The SDK build also compiles vendored BTstack sources with individual notices. See [micro-ecc](third_party_licenses/btstack-micro-ecc-LICENSE.txt), [SEGGER RTT](third_party_licenses/btstack-SEGGER-RTT-License.txt), [yxml](third_party_licenses/btstack-yxml-notice.txt), [MD5](third_party_licenses/btstack-md5-notice.txt) and [Rijndael](third_party_licenses/btstack-rijndael-notice.txt). The external CYW43 driver supplies its [controller firmware through the SDK](https://github.com/georgerobotics/cyw43-driver/tree/055d64274b014dd7b1c2fc94d26e8a18face7124/firmware).

The BOOTSEL button sampler in `src/pair_button.cpp` retains the Raspberry Pi copyright and BSD-3-Clause identifier from the [pico-examples button sample](https://github.com/raspberrypi/pico-examples/blob/master/picoboard/button/button.c). The BTstack and CYW43 Raspberry Pi terms describe the covered hardware and uses. The upstream notices apply to their respective components. This package does not assign new license terms to the original project code.

The shared in-house USB test assignment is separate from copyright permission. Anyone manufacturing, selling or distributing devices needs an appropriate assigned identity. [Test ID terms](https://pid.codes/1209/0002/)
