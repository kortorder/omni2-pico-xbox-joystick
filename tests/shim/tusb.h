// Host-only TinyUSB test boundary; the production build includes the SDK header.
#pragma once
#include <stdint.h>
typedef enum {
    HID_REPORT_TYPE_INVALID,
    HID_REPORT_TYPE_INPUT,
    HID_REPORT_TYPE_OUTPUT,
    HID_REPORT_TYPE_FEATURE
} hid_report_type_t;
enum { CONTROL_STAGE_IDLE, CONTROL_STAGE_SETUP, CONTROL_STAGE_DATA, CONTROL_STAGE_ACK };
struct tusb_control_request_t {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
};
extern "C" {
bool tud_control_status(uint8_t, const tusb_control_request_t *);
bool tud_control_xfer(uint8_t, const tusb_control_request_t *, void *, uint16_t);
bool tud_vendor_control_xfer_cb(uint8_t, uint8_t, const tusb_control_request_t *);
void tud_task_ext(uint32_t, bool);
bool tud_mounted(void);
bool tud_suspended(void);
bool tud_hid_ready(void);
bool tud_disconnect(void);
bool tud_connect(void);
bool tud_hid_report(uint8_t, const void *, uint16_t);
const uint8_t *tud_descriptor_device_cb(void);
const uint8_t *tud_descriptor_configuration_cb(uint8_t);
const uint8_t *tud_hid_descriptor_report_cb(uint8_t);
const uint16_t *tud_descriptor_string_cb(uint8_t, uint16_t);
void tud_mount_cb(void);
void tud_umount_cb(void);
void tud_suspend_cb(bool);
void tud_resume_cb(void);
void tud_hid_report_complete_cb(uint8_t, const uint8_t *, uint16_t);
void tud_hid_report_failed_cb(uint8_t, hid_report_type_t, const uint8_t *, uint16_t);
uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t);
void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t, const uint8_t *, uint16_t);
}
