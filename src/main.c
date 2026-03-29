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
 *   — located in the ChromeOS Quick Settings panel header (top-right).
 *
 * References:
 *  - CE Programming Toolchain: https://github.com/CE-Programming/toolchain
 *  - USB HID Spec 1.11: https://www.usb.org/hid
 *  - ChromeOS keyboard shortcuts: https://support.google.com/chromebook/answer/183101
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
 * (0–32767).  The formula maps [0, MAX_PX-1] linearly to [0, 32767].
 */
#define SIGNOUT_HID_X \
    ((uint16_t)(((uint32_t)(SIGNOUT_PIXEL_X) * 32767UL) / (SCREEN_WIDTH  - 1)))
#define SIGNOUT_HID_Y \
    ((uint16_t)(((uint32_t)(SIGNOUT_PIXEL_Y) * 32767UL) / (SCREEN_HEIGHT - 1)))

/* --------------------------------------------------------------------------
 * USB HID keycodes and modifier bits (USB HID Usage Tables 1.12, §10)
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

/* --------------------------------------------------------------------------
 * USB Descriptor constants
 * -------------------------------------------------------------------------- */
#define USB_DESC_DEVICE         0x01U
#define USB_DESC_CONFIG         0x02U
#define USB_DESC_STRING         0x03U
#define USB_DESC_INTERFACE      0x04U
#define USB_DESC_ENDPOINT       0x05U
#define USB_DESC_HID            0x21U
#define USB_DESC_HID_REPORT     0x22U

#define USB_CLASS_HID           0x03U
#define USB_SUBCLASS_BOOT       0x01U
#define USB_PROTOCOL_KEYBOARD   0x01U
#define USB_PROTOCOL_MOUSE      0x02U
#define USB_PROTOCOL_NONE       0x00U

#define USB_EP_IN               0x80U
#define USB_EP_INTERRUPT        0x03U

/* Endpoint addresses: EP1 IN = keyboard, EP2 IN = mouse */
#define EP_KBD_ADDR   (0x01U | USB_EP_IN)
#define EP_MOUSE_ADDR (0x02U | USB_EP_IN)
#define EP_MAX_PKT    8U

/* HID class-specific requests */
#define HID_REQ_GET_REPORT      0x01U
#define HID_REQ_GET_IDLE        0x02U
#define HID_REQ_GET_PROTOCOL    0x03U
#define HID_REQ_SET_REPORT      0x09U
#define HID_REQ_SET_IDLE        0x0AU
#define HID_REQ_SET_PROTOCOL    0x0BU

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
 *  x, y    : absolute position, 0–32767
 */
typedef struct __attribute__((packed)) {
    uint8_t  buttons;
    uint16_t x;
    uint16_t y;
} mouse_report_t;

/* --------------------------------------------------------------------------
 * HID Report Descriptors
 * -------------------------------------------------------------------------- */

/**
 * Keyboard report descriptor — standard 8-byte boot keyboard with
 * 8 modifier bits, 1 reserved byte, and 6 key-code bytes.
 * Plus a 5-bit LED output report (Num/Caps/Scroll/Compose/Kana Lock).
 */
static const uint8_t kbd_report_desc[] = {
    0x05, 0x01,        /* Usage Page: Generic Desktop Controls     */
    0x09, 0x06,        /* Usage: Keyboard                          */
    0xA1, 0x01,        /* Collection: Application                  */
    /* ---- Modifier keys: 8 x 1-bit ---- */
    0x05, 0x07,        /*   Usage Page: Keyboard / Keypad          */
    0x19, 0xE0,        /*   Usage Minimum: Left Ctrl  (0xE0)       */
    0x29, 0xE7,        /*   Usage Maximum: Right GUI  (0xE7)       */
    0x15, 0x00,        /*   Logical Minimum: 0                     */
    0x25, 0x01,        /*   Logical Maximum: 1                     */
    0x75, 0x01,        /*   Report Size: 1 bit                     */
    0x95, 0x08,        /*   Report Count: 8                        */
    0x81, 0x02,        /*   Input: Data, Variable, Absolute        */
    /* ---- Reserved byte ---- */
    0x95, 0x01,        /*   Report Count: 1                        */
    0x75, 0x08,        /*   Report Size: 8 bits                    */
    0x81, 0x01,        /*   Input: Constant                        */
    /* ---- LED output (5 LEDs + 3-bit pad) ---- */
    0x95, 0x05,        /*   Report Count: 5                        */
    0x75, 0x01,        /*   Report Size: 1 bit                     */
    0x05, 0x08,        /*   Usage Page: LEDs                       */
    0x19, 0x01,        /*   Usage Minimum: Num Lock                */
    0x29, 0x05,        /*   Usage Maximum: Kana                    */
    0x91, 0x02,        /*   Output: Data, Variable, Absolute       */
    0x95, 0x01,        /*   Report Count: 1                        */
    0x75, 0x03,        /*   Report Size: 3 bits (padding)          */
    0x91, 0x01,        /*   Output: Constant                       */
    /* ---- Key array: 6 x 8-bit ---- */
    0x95, 0x06,        /*   Report Count: 6                        */
    0x75, 0x08,        /*   Report Size: 8 bits                    */
    0x15, 0x00,        /*   Logical Minimum: 0                     */
    0x25, 0xFF,        /*   Logical Maximum: 255                   */
    0x05, 0x07,        /*   Usage Page: Keyboard / Keypad          */
    0x19, 0x00,        /*   Usage Minimum: 0 (Reserved / no event) */
    0x29, 0xFF,        /*   Usage Maximum: 255                     */
    0x81, 0x00,        /*   Input: Data, Array                     */
    0xC0               /* End Collection                           */
};

/**
 * Absolute mouse report descriptor.
 *  - 3 mouse buttons (bits)  + 5-bit padding = 1 byte
 *  - X absolute coordinate   (16-bit, 0–32767)
 *  - Y absolute coordinate   (16-bit, 0–32767)
 * Total input report size: 5 bytes.
 */
static const uint8_t mouse_report_desc[] = {
    0x05, 0x01,              /* Usage Page: Generic Desktop Controls    */
    0x09, 0x02,              /* Usage: Mouse                            */
    0xA1, 0x01,              /* Collection: Application                 */
    0x09, 0x01,              /*   Usage: Pointer                        */
    0xA1, 0x00,              /*   Collection: Physical                  */
    /* ---- 3 buttons: 3 x 1-bit ---- */
    0x05, 0x09,              /*     Usage Page: Button                  */
    0x19, 0x01,              /*     Usage Minimum: Button 1 (left)      */
    0x29, 0x03,              /*     Usage Maximum: Button 3 (middle)    */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x25, 0x01,              /*     Logical Maximum: 1                  */
    0x75, 0x01,              /*     Report Size: 1 bit                  */
    0x95, 0x03,              /*     Report Count: 3                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    /* ---- 5-bit padding ---- */
    0x75, 0x05,              /*     Report Size: 5 bits                 */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x01,              /*     Input: Constant                     */
    /* ---- X absolute: 16-bit, 0–32767 ---- */
    0x05, 0x01,              /*     Usage Page: Generic Desktop         */
    0x09, 0x30,              /*     Usage: X                            */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x27, 0xFF, 0x7F, 0x00, 0x00, /* Logical Maximum: 32767 (4-byte)  */
    0x75, 0x10,              /*     Report Size: 16 bits                */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    /* ---- Y absolute: 16-bit, 0–32767 ---- */
    0x09, 0x31,              /*     Usage: Y                            */
    0x15, 0x00,              /*     Logical Minimum: 0                  */
    0x27, 0xFF, 0x7F, 0x00, 0x00, /* Logical Maximum: 32767 (4-byte)  */
    0x75, 0x10,              /*     Report Size: 16 bits                */
    0x95, 0x01,              /*     Report Count: 1                     */
    0x81, 0x02,              /*     Input: Data, Variable, Absolute     */
    0xC0,                    /*   End Collection (Physical)             */
    0xC0                     /* End Collection (Application)            */
};

/* --------------------------------------------------------------------------
 * USB Descriptors
 * -------------------------------------------------------------------------- */

/** USB Device Descriptor (18 bytes) */
static const uint8_t device_descriptor[] = {
    0x12,                   /* bLength                */
    USB_DESC_DEVICE,        /* bDescriptorType        */
    0x00, 0x02,             /* bcdUSB = 2.00          */
    0x00,                   /* bDeviceClass (per-intf)*/
    0x00,                   /* bDeviceSubClass        */
    0x00,                   /* bDeviceProtocol        */
    0x40,                   /* bMaxPacketSize0 = 64   */
    0x51, 0x04,             /* idVendor  = 0x0451 (TI)*/
    0x00, 0x5F,             /* idProduct = 0x5F00     */
    0x00, 0x01,             /* bcdDevice = 1.00       */
    0x01,                   /* iManufacturer          */
    0x02,                   /* iProduct               */
    0x00,                   /* iSerialNumber          */
    0x01                    /* bNumConfigurations     */
};

/*
 * Sizes of the class-specific HID descriptors embedded in the
 * configuration block.
 */
#define KBD_REPORT_DESC_SIZE   sizeof(kbd_report_desc)
#define MOUSE_REPORT_DESC_SIZE sizeof(mouse_report_desc)

/*
 * Total length of the configuration descriptor block:
 *   1 config (9) + 2 × [interface(9) + HID(9) + endpoint(7)] = 9 + 50 = 59
 */
#define CONFIG_TOTAL_LEN  59U

/**
 * Full USB Configuration Descriptor block (packed).
 * Interface 0 = HID keyboard, Interface 1 = HID absolute mouse.
 */
static const uint8_t config_descriptor[] = {
    /* ---- Configuration Descriptor (9 bytes) ---- */
    0x09,                   /* bLength                */
    USB_DESC_CONFIG,        /* bDescriptorType        */
    CONFIG_TOTAL_LEN, 0x00, /* wTotalLength (LE)      */
    0x02,                   /* bNumInterfaces         */
    0x01,                   /* bConfigurationValue    */
    0x00,                   /* iConfiguration         */
    0xA0,                   /* bmAttributes: bus-pwr, remote wakeup */
    0x32,                   /* bMaxPower = 100 mA     */

    /* ==== Interface 0: HID Keyboard ==== */

    /* Interface Descriptor (9 bytes) */
    0x09,                   /* bLength                */
    USB_DESC_INTERFACE,     /* bDescriptorType        */
    0x00,                   /* bInterfaceNumber       */
    0x00,                   /* bAlternateSetting      */
    0x01,                   /* bNumEndpoints          */
    USB_CLASS_HID,          /* bInterfaceClass        */
    USB_SUBCLASS_BOOT,      /* bInterfaceSubClass     */
    USB_PROTOCOL_KEYBOARD,  /* bInterfaceProtocol     */
    0x00,                   /* iInterface             */

    /* HID Descriptor (9 bytes) */
    0x09,                   /* bLength                */
    USB_DESC_HID,           /* bDescriptorType: HID   */
    0x11, 0x01,             /* bcdHID = 1.11          */
    0x00,                   /* bCountryCode           */
    0x01,                   /* bNumDescriptors        */
    USB_DESC_HID_REPORT,    /* bDescriptorType: Report*/
    (uint8_t)(KBD_REPORT_DESC_SIZE & 0xFF),
    (uint8_t)((KBD_REPORT_DESC_SIZE >> 8) & 0xFF),

    /* Endpoint Descriptor — EP1 IN Interrupt (7 bytes) */
    0x07,                   /* bLength                */
    USB_DESC_ENDPOINT,      /* bDescriptorType        */
    EP_KBD_ADDR,            /* bEndpointAddress: EP1 IN */
    USB_EP_INTERRUPT,       /* bmAttributes: Interrupt */
    EP_MAX_PKT, 0x00,       /* wMaxPacketSize = 8     */
    0x0A,                   /* bInterval = 10 ms      */

    /* ==== Interface 1: HID Absolute Mouse ==== */

    /* Interface Descriptor (9 bytes) */
    0x09,                   /* bLength                */
    USB_DESC_INTERFACE,     /* bDescriptorType        */
    0x01,                   /* bInterfaceNumber       */
    0x00,                   /* bAlternateSetting      */
    0x01,                   /* bNumEndpoints          */
    USB_CLASS_HID,          /* bInterfaceClass        */
    USB_SUBCLASS_BOOT,      /* bInterfaceSubClass     */
    USB_PROTOCOL_NONE,      /* bInterfaceProtocol (0: no boot protocol for abs mouse) */
    0x00,                   /* iInterface             */

    /* HID Descriptor (9 bytes) */
    0x09,                   /* bLength                */
    USB_DESC_HID,           /* bDescriptorType: HID   */
    0x11, 0x01,             /* bcdHID = 1.11          */
    0x00,                   /* bCountryCode           */
    0x01,                   /* bNumDescriptors        */
    USB_DESC_HID_REPORT,    /* bDescriptorType: Report*/
    (uint8_t)(MOUSE_REPORT_DESC_SIZE & 0xFF),
    (uint8_t)((MOUSE_REPORT_DESC_SIZE >> 8) & 0xFF),

    /* Endpoint Descriptor — EP2 IN Interrupt (7 bytes) */
    0x07,                   /* bLength                */
    USB_DESC_ENDPOINT,      /* bDescriptorType        */
    EP_MOUSE_ADDR,          /* bEndpointAddress: EP2 IN */
    USB_EP_INTERRUPT,       /* bmAttributes: Interrupt */
    EP_MAX_PKT, 0x00,       /* wMaxPacketSize = 8     */
    0x0A                    /* bInterval = 10 ms      */
};

/* String Descriptors */
static const uint8_t string_lang[] = {
    0x04, USB_DESC_STRING,
    0x09, 0x04   /* English (US) */
};
static const uint8_t string_manufacturer[] = {
    /* bLength = 2 + 17 chars × 2 bytes = 36 = 0x24 */
    0x24, USB_DESC_STRING,
    'T',0, 'e',0, 'x',0, 'a',0, 's',0, ' ',0,
    'I',0, 'n',0, 's',0, 't',0, 'r',0, 'u',0, 'm',0,
    'e',0, 'n',0, 't',0, 's',0
};
static const uint8_t string_product[] = {
    0x2C, USB_DESC_STRING,
    'C',0, 'B',0, ' ',0, 'S',0, 'i',0, 'g',0, 'n',0, '-',0,
    'O',0, 'u',0, 't',0, ' ',0, 'H',0, 'I',0, 'D',0, ' ',0,
    'D',0, 'e',0, 'v',0, 'i',0, 'c',0, 'e',0
};

/* Pointer table for string descriptors (index 0–2) */
static const uint8_t *const string_descs[] = {
    string_lang,
    string_manufacturer,
    string_product
};
#define NUM_STRING_DESCS (sizeof(string_descs) / sizeof(string_descs[0]))

/* --------------------------------------------------------------------------
 * Application state machine
 * -------------------------------------------------------------------------- */
typedef enum {
    STATE_INIT,           /**< Waiting for USB to initialise.         */
    STATE_CONNECTED,      /**< USB enumeration complete, ready.       */
    STATE_SEND_SHORTCUT,  /**< Sending Alt+Shift+S.                   */
    STATE_RELEASE_KEYS,   /**< Releasing all keys.                    */
    STATE_WAIT_MENU,      /**< Waiting for the Quick Settings to open.*/
    STATE_MOVE_MOUSE,     /**< Moving mouse to sign-out button.       */
    STATE_CLICK,          /**< Pressing left mouse button.            */
    STATE_RELEASE_CLICK,  /**< Releasing mouse button.                */
    STATE_DONE,           /**< Sequence complete.                     */
    STATE_ERROR           /**< Unrecoverable error.                   */
} app_state_t;

/* Global state */
static volatile app_state_t g_state      = STATE_INIT;
static volatile bool        g_configured = false;
static usb_device_t         g_usb_device = NULL;

/* Keyboard and mouse report buffers */
static kbd_report_t   g_kbd_report;
static mouse_report_t g_mouse_report;

/*
 * Static buffers for single-byte GET_PROTOCOL / GET_IDLE responses.
 * These must be at file scope so their addresses remain valid after the
 * callback returns — usb_Transfer() may hold the pointer until the IN
 * transaction is actually completed on the wire.
 */
static const uint8_t g_hid_protocol = 0x01; /* Report protocol  */
static const uint8_t g_hid_idle     = 0x00; /* No auto-repeat   */

/* --------------------------------------------------------------------------
 * USB Event Callback
 * -------------------------------------------------------------------------- */

/**
 * Handle USB setup (control) transfer packets.
 * Returns USB_SUCCESS if the request was handled, USB_IGNORE otherwise.
 */
static usb_error_t handle_setup(usb_device_t dev,
                                const usb_setup_packet_t *setup)
{
    uint8_t  req_type = setup->bmRequestType;
    uint8_t  request  = setup->bRequest;
    uint16_t value    = setup->wValue;
    uint16_t length   = setup->wLength;

    /* --- Standard GET_DESCRIPTOR --- */
    if ((req_type == 0x80) && (request == 0x06 /* GET_DESCRIPTOR */)) {
        uint8_t desc_type  = (uint8_t)(value >> 8);
        uint8_t desc_index = (uint8_t)(value & 0xFF);

        if (desc_type == USB_DESC_DEVICE) {
            usb_Transfer(dev, 0 | USB_EP_IN, device_descriptor,
                         sizeof(device_descriptor), 0, NULL, NULL);
            return USB_SUCCESS;
        }
        if (desc_type == USB_DESC_CONFIG) {
            usb_Transfer(dev, 0 | USB_EP_IN, config_descriptor,
                         (length < CONFIG_TOTAL_LEN) ? length : CONFIG_TOTAL_LEN,
                         0, NULL, NULL);
            return USB_SUCCESS;
        }
        if (desc_type == USB_DESC_STRING) {
            if (desc_index < NUM_STRING_DESCS) {
                const uint8_t *s = string_descs[desc_index];
                uint16_t slen = s[0]; /* bLength */
                usb_Transfer(dev, 0 | USB_EP_IN, s,
                             (length < slen) ? length : slen,
                             0, NULL, NULL);
                return USB_SUCCESS;
            }
        }
        /* HID Report Descriptor: wValue = 0x2200 + interface, wIndex = interface */
        if (desc_type == USB_DESC_HID_REPORT) {
            uint8_t iface = (uint8_t)(setup->wIndex & 0xFF);
            if (iface == 0) {
                usb_Transfer(dev, 0 | USB_EP_IN, kbd_report_desc,
                             (length < KBD_REPORT_DESC_SIZE)
                                 ? length : KBD_REPORT_DESC_SIZE,
                             0, NULL, NULL);
                return USB_SUCCESS;
            }
            if (iface == 1) {
                usb_Transfer(dev, 0 | USB_EP_IN, mouse_report_desc,
                             (length < MOUSE_REPORT_DESC_SIZE)
                                 ? length : MOUSE_REPORT_DESC_SIZE,
                             0, NULL, NULL);
                return USB_SUCCESS;
            }
        }
    }

    /* --- Standard SET_CONFIGURATION --- */
    if ((req_type == 0x00) && (request == 0x09 /* SET_CONFIGURATION */)) {
        g_configured = true;
        /* Send zero-length status packet (ZLP). */
        usb_Transfer(dev, 0 | USB_EP_IN, NULL, 0, 0, NULL, NULL);
        return USB_SUCCESS;
    }

    /* --- Standard SET_ADDRESS --- handled automatically by hardware --- */

    /* --- HID class-specific requests --- */
    if ((req_type & 0x60) == 0x20 /* Class request */) {
        switch (request) {
        case HID_REQ_SET_IDLE:
            /* Acknowledge with ZLP. */
            usb_Transfer(dev, 0 | USB_EP_IN, NULL, 0, 0, NULL, NULL);
            return USB_SUCCESS;
        case HID_REQ_SET_PROTOCOL:
            usb_Transfer(dev, 0 | USB_EP_IN, NULL, 0, 0, NULL, NULL);
            return USB_SUCCESS;
        case HID_REQ_GET_PROTOCOL:
            usb_Transfer(dev, 0 | USB_EP_IN, &g_hid_protocol, 1, 0, NULL, NULL);
            return USB_SUCCESS;
        case HID_REQ_GET_IDLE:
            usb_Transfer(dev, 0 | USB_EP_IN, &g_hid_idle, 1, 0, NULL, NULL);
            return USB_SUCCESS;
        case HID_REQ_GET_REPORT:
            /* Return current report for the requested interface. */
            if ((setup->wIndex & 0xFF) == 0) {
                usb_Transfer(dev, 0 | USB_EP_IN,
                             &g_kbd_report, sizeof(g_kbd_report),
                             0, NULL, NULL);
            } else {
                usb_Transfer(dev, 0 | USB_EP_IN,
                             &g_mouse_report, sizeof(g_mouse_report),
                             0, NULL, NULL);
            }
            return USB_SUCCESS;
        default:
            break;
        }
    }

    return USB_IGNORE; /* Let the driver handle it. */
}

/**
 * Main USB event callback.
 */
static usb_error_t usb_callback(usb_event_t event,
                                void *event_data,
                                usb_callback_data_t *callback_data)
{
    (void)callback_data;

    switch (event) {
    case USB_DEVICE_ENABLED_EVENT:
        g_usb_device = (usb_device_t)event_data;
        break;

    case USB_DEVICE_DISABLED_EVENT:
        g_usb_device  = NULL;
        g_configured  = false;
        if (g_state != STATE_DONE)
            g_state = STATE_INIT;
        break;

    case USB_DEFAULT_SETUP_EVENT:
        return handle_setup((usb_device_t)event_data,
                            (const usb_setup_packet_t *)
                            usb_GetSetupPacket((usb_device_t)event_data));

    default:
        break;
    }

    return USB_SUCCESS;
}

/* --------------------------------------------------------------------------
 * Report sending helpers
 * -------------------------------------------------------------------------- */

/**
 * Send a keyboard HID report on EP1 IN.
 * @param modifier  Modifier bitmask (HID_MOD_*).
 * @param key0      First key code (or 0 for key-up).
 * @return true on success.
 */
static bool send_kbd_report(uint8_t modifier, uint8_t key0)
{
    if (!g_configured || g_usb_device == NULL)
        return false;

    g_kbd_report.modifier = modifier;
    g_kbd_report.reserved = 0x00;
    g_kbd_report.keys[0]  = key0;
    g_kbd_report.keys[1]  = 0;
    g_kbd_report.keys[2]  = 0;
    g_kbd_report.keys[3]  = 0;
    g_kbd_report.keys[4]  = 0;
    g_kbd_report.keys[5]  = 0;

    usb_error_t err = usb_Transfer(g_usb_device, EP_KBD_ADDR,
                                   &g_kbd_report, sizeof(g_kbd_report),
                                   0, NULL, NULL);
    return (err == USB_SUCCESS);
}

/**
 * Send a mouse HID report on EP2 IN.
 * @param buttons  Button bitmask (HID_BTN_*).
 * @param x        Absolute X coordinate (0–32767).
 * @param y        Absolute Y coordinate (0–32767).
 * @return true on success.
 */
static bool send_mouse_report(uint8_t buttons, uint16_t x, uint16_t y)
{
    if (!g_configured || g_usb_device == NULL)
        return false;

    g_mouse_report.buttons = buttons;
    g_mouse_report.x       = x;
    g_mouse_report.y       = y;

    usb_error_t err = usb_Transfer(g_usb_device, EP_MOUSE_ADDR,
                                   &g_mouse_report, sizeof(g_mouse_report),
                                   0, NULL, NULL);
    return (err == USB_SUCCESS);
}

/* --------------------------------------------------------------------------
 * Display helpers
 * -------------------------------------------------------------------------- */

/** Render status text on the calculator screen. */
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
 * Busy-wait delay (approximate, ~ms granularity)
 *
 * The inner loop count (2400) is calibrated for the TI 84+ CE running at
 * its default 48 MHz CPU clock.  If you need more accurate timing, replace
 * this with the CE toolchain hardware timer (e.g. timer_Set / timer_Wait
 * from <sys/timers.h>) which uses the 32 kHz crystal oscillator.
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
 * Main program entry point
 * -------------------------------------------------------------------------- */
int main(void)
{
    gfx_Begin();
    show_status("Connect USB cable", "Press ENTER to start");

    /* Wait for the user to press ENTER before initialising USB. */
    kb_Scan();
    while (!kb_IsDown(kb_KeyEnter)) {
        delay_ms(20);
        kb_Scan();
        if (kb_IsDown(kb_KeyClear))
            goto cleanup;
    }

    show_status("Initialising USB...", NULL);

    /* Initialise USB in device mode (calculator acts as HID device). */
    usb_error_t init_err = usb_Init(usb_callback, NULL, NULL,
                                    USB_DEFAULT_INIT_FLAGS);
    if (init_err != USB_SUCCESS) {
        show_status("USB init FAILED", "Check cable & retry");
        delay_ms(3000);
        goto cleanup;
    }

    g_state = STATE_INIT;

    /* Main event loop */
    while (g_state != STATE_DONE && g_state != STATE_ERROR) {

        usb_HandleEvents();
        kb_Scan();

        /* Allow user to abort at any time with [CLEAR]. */
        if (kb_IsDown(kb_KeyClear)) {
            show_status("Aborted by user", NULL);
            delay_ms(1500);
            break;
        }

        switch (g_state) {

        case STATE_INIT:
            if (g_configured) {
                g_state = STATE_CONNECTED;
                show_status("USB connected!", "Press ENTER to sign out");
            }
            break;

        case STATE_CONNECTED:
            /* Wait for the user to press ENTER to begin the sequence. */
            if (kb_IsDown(kb_KeyEnter)) {
                show_status("Sending Alt+Shift+S", NULL);
                g_state = STATE_SEND_SHORTCUT;
                delay_ms(200); /* Debounce. */
            }
            break;

        case STATE_SEND_SHORTCUT:
            /*
             * Send Alt + Shift + S.
             * Modifier byte: Left Alt (0x04) | Left Shift (0x02) = 0x06
             * Key code: S = 0x16
             */
            if (send_kbd_report(HID_MOD_LALT | HID_MOD_LSHIFT, HID_KEY_S)) {
                g_state = STATE_RELEASE_KEYS;
                delay_ms(100);
            }
            break;

        case STATE_RELEASE_KEYS:
            /* Release all keys. */
            if (send_kbd_report(HID_MOD_NONE, 0x00)) {
                show_status("Waiting for menu...", NULL);
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
             * Move the absolute mouse cursor to the "Sign out" button.
             * Coordinates from reference screenshot (image.png):
             *   Pixel (1299, 352) on a 1366x768 display.
             * Mapped to HID absolute range 0–32767:
             *   X = 1299 * 32767 / (SCREEN_WIDTH  - 1) ≈ 31182
             *   Y =  352 * 32767 / (SCREEN_HEIGHT - 1) ≈ 15037
             */
            if (send_mouse_report(0x00, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                delay_ms(80);
                show_status("Clicking Sign out", NULL);
                g_state = STATE_CLICK;
            }
            break;

        case STATE_CLICK:
            /* Press left mouse button at the sign-out position. */
            if (send_mouse_report(HID_BTN_LEFT, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                delay_ms(80);
                g_state = STATE_RELEASE_CLICK;
            }
            break;

        case STATE_RELEASE_CLICK:
            /* Release left mouse button. */
            if (send_mouse_report(0x00, SIGNOUT_HID_X, SIGNOUT_HID_Y)) {
                show_status("Done! Signed out.", "Press CLEAR to exit");
                g_state = STATE_DONE;
            }
            break;

        default:
            break;
        }
    }

    /* Keep the display up until the user presses CLEAR. */
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
