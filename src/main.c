/**
 * @file main.c
 * @brief TI 84+ CE - Chromebook Sign-Out USB HID Keyboard/Mouse Emulator
 *
 * This program runs on a TI 84+ CE graphing calculator and emulates a
 * composite USB HID device (keyboard + absolute mouse) to sign out of
 * a Chromebook automatically.
 *
 * Behavior:
 *  1. Connect the TI 84+ CE to the Chromebook via USB cable.
 *  2. Press [ENTER] on the calculator to begin.
 *  3. The calculator sends the Alt+Shift+S key combination to open the
 *     ChromeOS Quick Settings panel.
 *  4. After a short delay (for the panel to appear), the calculator moves
 *     the mouse cursor to the "Sign out" button and clicks it.
 *
 * Target screen: 1366x768 Chromebook display
 * Sign-out button coordinates (from reference image): (1299, 352)
 *   - located in the ChromeOS Quick Settings panel header (top-right).
 *
 * References:
 *  - CE Programming Toolchain: https://github.com/CE-Programming/toolchain
 *  - USB HID Spec 1.11: https://www.usb.org/hid
 *  - ChromeOS shortcuts: https://support.google.com/chromebook/answer/183101
 *
 * Build with the CE C/C++ Toolchain (ce-toolchain).
 */

#include <tice.h>
#include <usbdrvce.h>
#include <keypadc.h>
#include <graphx.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------------
 * Screen / target configuration
 * -------------------------------------------------------------------------- */

/** Chromebook screen resolution (reference image dimensions). */
#define SCREEN_WIDTH   1366
#define SCREEN_HEIGHT   768

/**
 * Pixel coordinates of the "Sign out" button on the target Chromebook,
 * as determined from the supplied reference screenshot (image.png).
 * The button resides in the ChromeOS Quick Settings panel header.
 */
#define SIGNOUT_PIXEL_X  1299
#define SIGNOUT_PIXEL_Y   352

/**
 * Convert pixel coordinates to the HID absolute mouse logical range
 * (0-32767).  The formula maps [0, MAX_PX-1] linearly to [0, 32767].
 */
#define SIGNOUT_HID_X \
    ((uint16_t)(((uint32_t)(SIGNOUT_PIXEL_X) * 32767UL) / (SCREEN_WIDTH  - 1)))
#define SIGNOUT_HID_Y \
    ((uint16_t)(((uint32_t)(SIGNOUT_PIXEL_Y) * 32767UL) / (SCREEN_HEIGHT - 1)))

/* --------------------------------------------------------------------------
 * USB HID keycodes and modifier bits (USB HID Usage Tables 1.12, sec. 10)
 * -------------------------------------------------------------------------- */
#define HID_MOD_NONE    0x00U
#define HID_MOD_LCTRL   0x01U
#define HID_MOD_LSHIFT  0x02U
#define HID_MOD_LALT    0x04U
#define HID_MOD_LGUI    0x08U
#define HID_MOD_RCTRL   0x10U
#define HID_MOD_RSHIFT  0x20U
#define HID_MOD_RALT    0x40U
#define HID_MOD_RGUI    0x80U

/** HID keycode for the letter 'S' (Usage ID 0x16). */
#define HID_KEY_S       0x16U

/** Mouse button mask: left button = bit 0. */
#define HID_BTN_LEFT    0x01U

/* HID descriptor type codes (USB HID spec 1.11, table 7.1) */
#define USB_HID_DESCRIPTOR_TYPE    0x21U
#define USB_REPORT_DESCRIPTOR_TYPE 0x22U

/* HID class-specific request codes (USB HID spec 1.11, section 7.2) */
#define HID_REQ_GET_REPORT   0x01U
#define HID_REQ_GET_IDLE     0x02U
#define HID_REQ_GET_PROTOCOL 0x03U
#define HID_REQ_SET_REPORT   0x09U
#define HID_REQ_SET_IDLE     0x0AU
#define HID_REQ_SET_PROTOCOL 0x0BU

/* Interrupt endpoint addresses (from the host's perspective: IN = calc->PC) */
#define EP_KBD_ADDR   0x81U   /* EP1 IN: keyboard  */
#define EP_MOUSE_ADDR 0x82U   /* EP2 IN: mouse     */

/* --------------------------------------------------------------------------
 * HID report structs
 * -------------------------------------------------------------------------- */

/** 8-byte boot-protocol keyboard report. */
typedef struct __attribute__((packed)) {
    uint8_t modifier;   /**< Modifier keys bitmask.          */
    uint8_t reserved;   /**< Always 0x00.                    */
    uint8_t keys[6];    /**< Up to 6 simultaneous key codes. */
} kbd_report_t;

/**
 * 5-byte absolute-mouse report.
 *  buttons : bit 0 = left, bit 1 = right, bit 2 = middle
 *  x, y    : absolute position, 0-32767
 */
typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    uint16_t x;
    uint16_t y;
} mouse_report_t;

/* --------------------------------------------------------------------------
 * HID Report Descriptors
 * (must be in RAM for usb_ScheduleTransfer; not const)
 * -------------------------------------------------------------------------- */

/**
 * Keyboard report descriptor: standard 8-byte boot keyboard with
 * 8 modifier bits, 1 reserved byte, 6 key-code bytes, and LED output.
 */
static uint8_t kbd_report_desc[] = {
    0x05, 0x01,        /* Usage Page: Generic Desktop Controls     */
    0x09, 0x06,        /* Usage: Keyboard                          */
    0xA1, 0x01,        /* Collection: Application                  */
    /* Modifier keys: 8 x 1-bit */
    0x05, 0x07,        /*   Usage Page: Keyboard / Keypad          */
    0x19, 0xE0,        /*   Usage Minimum: Left Ctrl  (0xE0)       */
    0x29, 0xE7,        /*   Usage Maximum: Right GUI  (0xE7)       */
    0x15, 0x00,        /*   Logical Minimum: 0                     */
    0x25, 0x01,        /*   Logical Maximum: 1                     */
    0x75, 0x01,        /*   Report Size: 1 bit                     */
    0x95, 0x08,        /*   Report Count: 8                        */
    0x81, 0x02,        /*   Input: Data, Variable, Absolute        */
    /* Reserved byte */
    0x95, 0x01,        /*   Report Count: 1                        */
    0x75, 0x08,        /*   Report Size: 8 bits                    */
    0x81, 0x01,        /*   Input: Constant                        */
    /* LED output: 5 LEDs + 3-bit pad */
    0x95, 0x05,        /*   Report Count: 5                        */
    0x75, 0x01,        /*   Report Size: 1 bit                     */
    0x05, 0x08,        /*   Usage Page: LEDs                       */
    0x19, 0x01,        /*   Usage Minimum: Num Lock                */
    0x29, 0x05,        /*   Usage Maximum: Kana                    */
    0x91, 0x02,        /*   Output: Data, Variable, Absolute       */
    0x95, 0x01,        /*   Report Count: 1                        */
    0x75, 0x03,        /*   Report Size: 3 bits (padding)          */
    0x91, 0x01,        /*   Output: Constant                       */
    /* Key array: 6 x 8-bit */
    0x95, 0x06,        /*   Report Count: 6                        */
    0x75, 0x08,        /*   Report Size: 8 bits                    */
    0x15, 0x00,        /*   Logical Minimum: 0                     */
    0x25, 0xFF,        /*   Logical Maximum: 255                   */
    0x05, 0x07,        /*   Usage Page: Keyboard / Keypad          */
    0x19, 0x00,        /*   Usage Minimum: 0 (Reserved)            */
    0x29, 0xFF,        /*   Usage Maximum: 255                     */
    0x81, 0x00,        /*   Input: Data, Array                     */
    0xC0               /* End Collection                           */
};
#define KBD_REPORT_DESC_SIZE sizeof(kbd_report_desc)

/**
 * Absolute mouse report descriptor.
 *  - 3 buttons + 5-bit padding = 1 byte
 *  - X absolute (16-bit, 0-32767)
 *  - Y absolute (16-bit, 0-32767)
 * Total input report: 5 bytes.
 */
static uint8_t mouse_report_desc[] = {
    0x05, 0x01,              /* Usage Page: Generic Desktop Controls    */
    0x09, 0x02,              /* Usage: Mouse                            */
    0xA1, 0x01,              /* Collection: Application                 */
    0x09, 0x01,              /*   Usage: Pointer                        */
    0xA1, 0x00,              /*   Collection: Physical                  */
    /* 3 buttons: 3 x 1-bit */
    0x05, 0x09,              /*     Usage Page: Button                  */
    0x19, 0x01,              /*     Usage Minimum: Button 1 (left)      */
    0x29, 0x03,              /*     Usage Maximum: Button 3 (middle)    */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x25, 0x01,              /*     Logical Maximum: 1                  */
    0x75, 0x01,              /*     Report Size: 1 bit                  */
    0x95, 0x03,              /*     Report Count: 3                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    /* 5-bit padding */
    0x75, 0x05,              /*     Report Size: 5 bits                 */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x01,              /*     Input: Constant                     */
    /* X absolute: 16-bit, 0-32767 */
    0x05, 0x01,              /*     Usage Page: Generic Desktop         */
    0x09, 0x30,              /*     Usage: X                            */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x27, 0xFF, 0x7F, 0x00, 0x00, /* Logical Maximum: 32767 (4-byte)   */
    0x75, 0x10,              /*     Report Size: 16 bits                */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    /* Y absolute: 16-bit, 0-32767 */
    0x09, 0x31,              /*     Usage: Y                            */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x27, 0xFF, 0x7F, 0x00, 0x00, /* Logical Maximum: 32767 (4-byte)   */
    0x75, 0x10,              /*     Report Size: 16 bits                */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    0xC0,                    /*   End Collection (Physical)             */
    0xC0                     /* End Collection (Application)            */
};
#define MOUSE_REPORT_DESC_SIZE sizeof(mouse_report_desc)

/* --------------------------------------------------------------------------
 * USB Descriptors
 * All descriptor data must reside in RAM (not flash) for the CE USB driver.
 * -------------------------------------------------------------------------- */

/*
 * Device Descriptor using the CE toolchain typed struct so the driver can
 * read fields like bNumConfigurations directly.
 */
static usb_device_descriptor_t s_device_desc = {
    .bLength            = 18,
    .bDescriptorType    = USB_DEVICE_DESCRIPTOR,
    .bcdUSB             = 0x0200,   /* USB 2.0              */
    .bDeviceClass       = 0x00,     /* class per interface  */
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = 64,
    .idVendor           = 0x0451,   /* Texas Instruments    */
    .idProduct          = 0x5F00,
    .bcdDevice          = 0x0100,   /* device v1.0          */
    .iManufacturer      = 1,
    .iProduct           = 2,
    .iSerialNumber      = 0,
    .bNumConfigurations = 1
};

/*
 * Full configuration descriptor blob.
 * Starts with the standard usb_configuration_descriptor_t header
 * (9 bytes), followed by interface, HID class, and endpoint
 * sub-descriptors.  The driver sends wTotalLength bytes from this
 * pointer when the host issues GET_DESCRIPTOR(Configuration).
 *
 * Layout: config(9) + iface0(9) + hid0(9) + ep0(7)
 *                   + iface1(9) + hid1(9) + ep1(7) = 59 bytes
 */
#define CONFIG_TOTAL_LEN 59U

static uint8_t s_config_blob[CONFIG_TOTAL_LEN] = {
    /* Configuration Descriptor (9 bytes) */
    0x09,
    USB_CONFIGURATION_DESCRIPTOR,
    CONFIG_TOTAL_LEN, 0x00,     /* wTotalLength (LE)                 */
    0x02,                       /* bNumInterfaces: 2                 */
    0x01,                       /* bConfigurationValue: 1            */
    0x00,                       /* iConfiguration: none              */
    0xA0,                       /* bmAttributes: bus-pwr+remote wake */
    0x32,                       /* bMaxPower: 100 mA (50 * 2 mA)    */

    /* Interface 0: HID Boot Keyboard */
    0x09, USB_INTERFACE_DESCRIPTOR,
    0x00,                       /* bInterfaceNumber: 0               */
    0x00,                       /* bAlternateSetting: 0              */
    0x01,                       /* bNumEndpoints: 1                  */
    0x03,                       /* bInterfaceClass: HID              */
    0x01,                       /* bInterfaceSubClass: Boot          */
    0x01,                       /* bInterfaceProtocol: Keyboard      */
    0x00,                       /* iInterface: none                  */

    /* HID Class Descriptor for Interface 0 (9 bytes) */
    0x09,
    USB_HID_DESCRIPTOR_TYPE,
    0x11, 0x01,                 /* bcdHID = 1.11                     */
    0x00,                       /* bCountryCode: not localized       */
    0x01,                       /* bNumDescriptors: 1                */
    USB_REPORT_DESCRIPTOR_TYPE,
    (uint8_t)(KBD_REPORT_DESC_SIZE & 0xFF),
    (uint8_t)((KBD_REPORT_DESC_SIZE >> 8) & 0xFF),

    /* Endpoint Descriptor: EP1 IN Interrupt (7 bytes) */
    0x07, USB_ENDPOINT_DESCRIPTOR,
    EP_KBD_ADDR,                /* bEndpointAddress: EP1 IN          */
    0x03,                       /* bmAttributes: Interrupt           */
    0x08, 0x00,                 /* wMaxPacketSize: 8                 */
    0x0A,                       /* bInterval: 10 ms                  */

    /* Interface 1: HID Absolute Mouse */
    0x09, USB_INTERFACE_DESCRIPTOR,
    0x01,                       /* bInterfaceNumber: 1               */
    0x00,                       /* bAlternateSetting: 0              */
    0x01,                       /* bNumEndpoints: 1                  */
    0x03,                       /* bInterfaceClass: HID              */
    0x00,                       /* bInterfaceSubClass: None          */
    0x00,                       /* bInterfaceProtocol: None          */
    0x00,                       /* iInterface: none                  */

    /* HID Class Descriptor for Interface 1 (9 bytes) */
    0x09,
    USB_HID_DESCRIPTOR_TYPE,
    0x11, 0x01,                 /* bcdHID = 1.11                     */
    0x00,                       /* bCountryCode: not localized       */
    0x01,                       /* bNumDescriptors: 1                */
    USB_REPORT_DESCRIPTOR_TYPE,
    (uint8_t)(MOUSE_REPORT_DESC_SIZE & 0xFF),
    (uint8_t)((MOUSE_REPORT_DESC_SIZE >> 8) & 0xFF),

    /* Endpoint Descriptor: EP2 IN Interrupt (7 bytes) */
    0x07, USB_ENDPOINT_DESCRIPTOR,
    EP_MOUSE_ADDR,              /* bEndpointAddress: EP2 IN          */
    0x03,                       /* bmAttributes: Interrupt           */
    0x08, 0x00,                 /* wMaxPacketSize: 8                 */
    0x0A,                       /* bInterval: 10 ms                  */
};

/* Array of configuration descriptor pointers (one configuration). */
static const usb_configuration_descriptor_t *const s_configs[1] = {
    (const usb_configuration_descriptor_t *)s_config_blob
};

/*
 * String Descriptors.
 * usb_string_descriptor_t.bString is wchar_t[] (UTF-16LE on CE).
 * We use raw byte arrays and cast them to avoid wchar_t literal issues.
 *
 * String index 0 = language ID list
 * String index 1 = manufacturer
 * String index 2 = product
 */

/* Language ID descriptor: English (US) = 0x0409 */
static uint8_t s_langid_bytes[] = {
    0x04, USB_STRING_DESCRIPTOR,
    0x09, 0x04
};

/* "Texas Instruments": 17 chars * 2 bytes = 34 + 2 header = 36 = 0x24 */
static uint8_t s_mfr_bytes[] = {
    0x24, USB_STRING_DESCRIPTOR,
    'T',0,'e',0,'x',0,'a',0,'s',0,' ',0,
    'I',0,'n',0,'s',0,'t',0,'r',0,'u',0,'m',0,'e',0,'n',0,'t',0,'s',0
};

/* "CB Sign-Out HID Device": 22 chars * 2 = 44 + 2 header = 46 = 0x2E */
static uint8_t s_prod_bytes[] = {
    0x2E, USB_STRING_DESCRIPTOR,
    'C',0,'B',0,' ',0,'S',0,'i',0,'g',0,'n',0,'-',0,
    'O',0,'u',0,'t',0,' ',0,'H',0,'I',0,'D',0,' ',0,
    'D',0,'e',0,'v',0,'i',0,'c',0,'e',0
};

/* Pointer table: index 0 = manufacturer, index 1 = product */
static const usb_string_descriptor_t *const s_strings[2] = {
    (const usb_string_descriptor_t *)s_mfr_bytes,
    (const usb_string_descriptor_t *)s_prod_bytes
};

/* Complete standard descriptor bundle passed to usb_Init(). */
static const usb_standard_descriptors_t s_usb_descs = {
    .device         = &s_device_desc,
    .configurations = s_configs,
    .langids        = (const usb_string_descriptor_t *)s_langid_bytes,
    .numStrings     = 2,   /* one manufacturer + one product string */
    .strings        = s_strings
};

/* --------------------------------------------------------------------------
 * Application State Machine
 * -------------------------------------------------------------------------- */
typedef enum {
    STATE_INIT,           /**< Waiting for host to configure the device. */
    STATE_WAIT_USER,      /**< Connected; waiting for user to press ENTER.*/
    STATE_SEND_SHORTCUT,  /**< Sending Alt+Shift+S.                       */
    STATE_RELEASE_KEYS,   /**< Releasing all keys.                        */
    STATE_WAIT_MENU,      /**< Waiting 600 ms for the panel to open.      */
    STATE_MOVE_MOUSE,     /**< Moving cursor to sign-out button.          */
    STATE_CLICK,          /**< Pressing the left mouse button.            */
    STATE_RELEASE_CLICK,  /**< Releasing the mouse button.                */
    STATE_DONE            /**< Sequence complete.                         */
} app_state_t;

/* Global application state */
static volatile app_state_t g_state      = STATE_INIT;
static volatile bool        g_configured = false;

/* Interrupt endpoint handles, populated on USB_HOST_CONFIGURE_EVENT */
static usb_endpoint_t g_ep_kbd   = NULL;
static usb_endpoint_t g_ep_mouse = NULL;

/* HID report buffers (must be in RAM; passed to usb_Transfer by address) */
static kbd_report_t   g_kbd_report;
static mouse_report_t g_mouse_report;

/*
 * Single-byte response buffers for GET_PROTOCOL / GET_IDLE.
 * At file scope so the pointer stays valid after the callback returns
 * (usb_ScheduleTransfer may reference the buffer until transfer completes).
 */
static uint8_t g_hid_protocol = 0x01; /* Report protocol  */
static uint8_t g_hid_idle     = 0x00; /* No auto-repeat   */

/*
 * Dummy buffer used for ZLP (zero-length packet) status-stage responses.
 * usb_ScheduleTransfer requires a non-NULL buffer pointer on some builds.
 */
static uint8_t g_zlp_buf[1];

/* --------------------------------------------------------------------------
 * USB Setup Event Handler (Device Mode)
 * -------------------------------------------------------------------------- */

/**
 * Handle USB setup requests not processed automatically by the CE driver.
 * Called from the USB event callback for USB_DEFAULT_SETUP_EVENT.
 *
 * For IN requests (device->host) we schedule the data and return USB_IGNORE.
 * For no-data OUT requests (host->device) we send a ZLP status stage.
 *
 * @param  setup  Pointer to the received setup packet.
 * @return USB_IGNORE  if we scheduled the response manually.
 *         USB_SUCCESS if the driver should handle it (may result in STALL).
 */
static usb_error_t handle_setup(const usb_control_setup_t *setup)
{
    /*
     * The calculator's own control endpoint (EP0) in device mode.
     * usb_RootHub() returns the root-hub device handle which represents
     * the CE's own USB controller when operating as a peripheral.
     */
    usb_endpoint_t ep0 = usb_GetDeviceEndpoint(usb_RootHub(), 0);

    uint8_t  req_type = setup->bmRequestType;
    uint8_t  request  = setup->bRequest;
    uint16_t value    = setup->wValue;
    uint16_t length   = setup->wLength;

    /* ------------------------------------------------------------------
     * GET_DESCRIPTOR for HID Report Descriptor
     * bmRequestType = 0x81 (IN | Standard | Interface)
     * bRequest      = 0x06 (GET_DESCRIPTOR)
     * wValue high   = 0x22 (Report Descriptor type)
     * wIndex        = interface number
     * ------------------------------------------------------------------ */
    if ((req_type == (USB_DEVICE_TO_HOST | USB_STANDARD_REQUEST |
                      USB_RECIPIENT_INTERFACE)) &&
        (request == USB_GET_DESCRIPTOR_REQUEST) &&
        ((uint8_t)(value >> 8) == USB_REPORT_DESCRIPTOR_TYPE))
    {
        uint8_t iface = (uint8_t)(setup->wIndex & 0xFFU);
        if (iface == 0) {
            uint16_t send = (length < (uint16_t)KBD_REPORT_DESC_SIZE)
                            ? length : (uint16_t)KBD_REPORT_DESC_SIZE;
            usb_ScheduleTransfer(ep0, kbd_report_desc, send, NULL, NULL);
            return USB_IGNORE;
        }
        if (iface == 1) {
            uint16_t send = (length < (uint16_t)MOUSE_REPORT_DESC_SIZE)
                            ? length : (uint16_t)MOUSE_REPORT_DESC_SIZE;
            usb_ScheduleTransfer(ep0, mouse_report_desc, send, NULL, NULL);
            return USB_IGNORE;
        }
    }

    /* ------------------------------------------------------------------
     * HID Class-Specific Requests
     * bmRequestType has type field = USB_CLASS_REQUEST (bits [6:5] = 01)
     * ------------------------------------------------------------------ */
    if ((req_type & 0x60U) == (uint8_t)USB_CLASS_REQUEST) {
        switch (request) {

        case HID_REQ_SET_IDLE:
            /* No data stage; acknowledge with IN ZLP status stage. */
            usb_ScheduleTransfer(ep0, g_zlp_buf, 0, NULL, NULL);
            return USB_IGNORE;

        case HID_REQ_SET_PROTOCOL:
            usb_ScheduleTransfer(ep0, g_zlp_buf, 0, NULL, NULL);
            return USB_IGNORE;

        case HID_REQ_SET_REPORT:
            /* Accept but ignore LED / output reports. */
            usb_ScheduleTransfer(ep0, g_zlp_buf, 0, NULL, NULL);
            return USB_IGNORE;

        case HID_REQ_GET_PROTOCOL:
            usb_ScheduleTransfer(ep0, &g_hid_protocol, 1, NULL, NULL);
            return USB_IGNORE;

        case HID_REQ_GET_IDLE:
            usb_ScheduleTransfer(ep0, &g_hid_idle, 1, NULL, NULL);
            return USB_IGNORE;

        case HID_REQ_GET_REPORT: {
            uint8_t iface = (uint8_t)(setup->wIndex & 0xFFU);
            if (iface == 0) {
                usb_ScheduleTransfer(ep0, &g_kbd_report,
                                     sizeof(g_kbd_report), NULL, NULL);
            } else {
                usb_ScheduleTransfer(ep0, &g_mouse_report,
                                     sizeof(g_mouse_report), NULL, NULL);
            }
            return USB_IGNORE;
        }

        default:
            break;
        }
    }

    /* Unknown / unhandled request: let the driver respond (may STALL). */
    return USB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Main USB Event Callback
 * -------------------------------------------------------------------------- */

static usb_error_t usb_callback(usb_event_t event, void *event_data,
                                usb_callback_data_t *callback_data)
{
    (void)callback_data;

    switch (event) {

    case USB_DEVICE_DISABLED_EVENT:
        /* Cable disconnected or device disabled by host. */
        g_configured = false;
        g_ep_kbd     = NULL;
        g_ep_mouse   = NULL;
        if (g_state != STATE_DONE)
            g_state = STATE_INIT;
        break;

    case USB_HOST_CONFIGURE_EVENT:
        /*
         * The PC host sent SET_CONFIGURATION.  Retrieve the interrupt
         * endpoint handles so we can send HID reports.
         */
        g_ep_kbd   = usb_GetDeviceEndpoint(usb_RootHub(), EP_KBD_ADDR);
        g_ep_mouse = usb_GetDeviceEndpoint(usb_RootHub(), EP_MOUSE_ADDR);
        g_configured = (g_ep_kbd != NULL) && (g_ep_mouse != NULL);
        if (g_configured && g_state == STATE_INIT)
            g_state = STATE_WAIT_USER;
        break;

    case USB_DEFAULT_SETUP_EVENT:
        return handle_setup((const usb_control_setup_t *)event_data);

    default:
        break;
    }

    return USB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * HID Report Sending Helpers
 * -------------------------------------------------------------------------- */

/**
 * Send a keyboard HID report over EP1 IN (blocking, up to 3 retries).
 * @param modifier  Modifier bitmask (HID_MOD_*).
 * @param key0      First key code, or 0x00 for key-up.
 * @return true on success.
 */
static bool send_kbd_report(uint8_t modifier, uint8_t key0)
{
    if (!g_configured || g_ep_kbd == NULL)
        return false;

    g_kbd_report.modifier = modifier;
    g_kbd_report.reserved = 0x00;
    g_kbd_report.keys[0]  = key0;
    g_kbd_report.keys[1]  = 0;
    g_kbd_report.keys[2]  = 0;
    g_kbd_report.keys[3]  = 0;
    g_kbd_report.keys[4]  = 0;
    g_kbd_report.keys[5]  = 0;

    size_t xferred;
    usb_error_t err = usb_Transfer(g_ep_kbd, &g_kbd_report,
                                   sizeof(g_kbd_report), 3, &xferred);
    return (err == USB_SUCCESS);
}

/**
 * Send a mouse HID report over EP2 IN (blocking, up to 3 retries).
 * @param buttons  Button bitmask (HID_BTN_*).
 * @param x        Absolute X coordinate (0-32767).
 * @param y        Absolute Y coordinate (0-32767).
 * @return true on success.
 */
static bool send_mouse_report(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (!g_configured || g_ep_mouse == NULL)
        return false;

    g_mouse_report.buttons = buttons;
    g_mouse_report.x       = x;
    g_mouse_report.y       = y;

    size_t xferred;
    usb_error_t err = usb_Transfer(g_ep_mouse, &g_mouse_report,
                                   sizeof(g_mouse_report), 3, &xferred);
    return (err == USB_SUCCESS);
}

/* --------------------------------------------------------------------------
 * Display Helpers
 * -------------------------------------------------------------------------- */

static void show_status(const char *line1, const char *line2)
{
    gfx_FillScreen(gfx_black);
    gfx_SetTextFGColor(gfx_white);
    gfx_SetTextBGColor(gfx_black);
    gfx_SetTextScale(1, 1);

    gfx_PrintStringXY("CB Sign-Out HID", 10, 5);

    if (line1)
        gfx_PrintStringXY(line1, 10, 30);
    if (line2)
        gfx_PrintStringXY(line2, 10, 50);

    gfx_PrintStringXY("Target: (1299,352)", 10, 80);
    gfx_PrintStringXY("Key: Alt+Shift+S",   10, 95);

    gfx_SwapDraw();
}

/* --------------------------------------------------------------------------
 * Busy-Wait Delay (~ms granularity)
 *
 * Calibrated for the TI 84+ CE at its default 48 MHz CPU clock.
 * For more accurate timing use the CE toolchain hardware timer:
 *   timer_Set / timer_Wait from <sys/timers.h> (32 kHz crystal).
 * -------------------------------------------------------------------------- */
static void delay_ms(uint16_t ms)
{
    while (ms--) {
        volatile uint16_t i = 2400; /* ~1 ms inner loop at 48 MHz */
        while (i--)
            ;
        usb_HandleEvents();
    }
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
    gfx_Begin();
    show_status("Connect USB cable", "Press ENTER to start");

    /* Wait for the user to press ENTER before starting USB. */
    kb_Scan();
    while (!kb_IsDown(kb_KeyEnter)) {
        delay_ms(20);
        kb_Scan();
        if (kb_IsDown(kb_KeyClear))
            goto cleanup;
    }

    show_status("Initialising USB...", NULL);

    /*
     * Start the USB driver in device mode.  Passing &s_usb_descs tells
     * the CE USB controller which descriptors to use during enumeration.
     * The Chromebook (USB host) reads these to discover our HID class
     * and interrupt endpoints.
     */
    usb_error_t init_err = usb_Init(usb_callback, NULL, &s_usb_descs,
                                    USB_DEFAULT_INIT_FLAGS);
    if (init_err != USB_SUCCESS) {
        show_status("USB init FAILED", "Check cable & retry");
        delay_ms(3000);
        goto cleanup;
    }

    /* Main event loop */
    while (g_state != STATE_DONE) {

        usb_HandleEvents();
        kb_Scan();

        /* User can abort at any time with [CLEAR]. */
        if (kb_IsDown(kb_KeyClear)) {
            show_status("Aborted by user", NULL);
            delay_ms(1500);
            break;
        }

        switch (g_state) {

        case STATE_INIT:
            /* Waiting for host to configure the device (USB_HOST_CONFIGURE_EVENT). */
            show_status("Waiting for USB...", "Connect to Chromebook");
            delay_ms(200);
            break;

        case STATE_WAIT_USER:
            /* Display "ready" message and wait for ENTER. */
            show_status("USB connected!", "Press ENTER to sign out");
            delay_ms(50);
            if (kb_IsDown(kb_KeyEnter)) {
                show_status("Sending Alt+Shift+S", NULL);
                delay_ms(200); /* Debounce. */
                g_state = STATE_SEND_SHORTCUT;
            }
            break;

        case STATE_SEND_SHORTCUT:
            /*
             * Send Alt + Shift + S.
             * Modifier: Left Alt (0x04) | Left Shift (0x02) = 0x06
             * Key code: S = 0x16
             */
            if (send_kbd_report(HID_MOD_LALT | HID_MOD_LSHIFT, HID_KEY_S)) {
                delay_ms(100);
                g_state = STATE_RELEASE_KEYS;
            }
            break;

        case STATE_RELEASE_KEYS:
            if (send_kbd_report(HID_MOD_NONE, 0x00)) {
                show_status("Waiting for panel...", NULL);
                g_state = STATE_WAIT_MENU;
            }
            break;

        case STATE_WAIT_MENU:
            /*
             * Wait 600 ms for the ChromeOS Quick Settings panel to
             * animate open before moving the mouse.
             */
            delay_ms(600);
            show_status("Moving mouse...", NULL);
            g_state = STATE_MOVE_MOUSE;
            break;

        case STATE_MOVE_MOUSE:
            /*
             * Move the cursor to the "Sign out" button.
             * From reference screenshot (image.png): pixel (1299, 352)
             * on a 1366x768 display.
             * HID absolute range (0-32767):
             *   X = 1299 * 32767 / (SCREEN_WIDTH  - 1) ~= 31182
             *   Y =  352 * 32767 / (SCREEN_HEIGHT - 1) ~= 15037
             */
            if (send_mouse_report(0x00, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                delay_ms(80);
                show_status("Clicking Sign out", NULL);
                g_state = STATE_CLICK;
            }
            break;

        case STATE_CLICK:
            /* Press left button. */
            if (send_mouse_report(HID_BTN_LEFT, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                delay_ms(80);
                g_state = STATE_RELEASE_CLICK;
            }
            break;

        case STATE_RELEASE_CLICK:
            /* Release left button. */
            if (send_mouse_report(0x00, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                show_status("Done! Signed out.", "Press CLEAR to exit");
                g_state = STATE_DONE;
            }
            break;

        case STATE_DONE:
            break;
        }
    }

    /* Hold the done screen until CLEAR. */
    if (g_state == STATE_DONE) {
        kb_Scan();
        while (!kb_IsDown(kb_KeyClear)) {
            delay_ms(20);
            kb_Scan();
        }
    }

cleanup:
    usb_Cleanup();
    gfx_End();
    return 0;
}
