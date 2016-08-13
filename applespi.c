/**
 * The keyboard and touchpad controller on the MacBook8,1, MacBook9,1 and
 * MacBookPro12,1 can be driven either by USB or SPI. However the USB pins
 * are only connected on the MacBookPro12,1, all others need this driver.
 * The interface is selected using ACPI methods:
 *
 * * UIEN ("USB Interface Enable"): If invoked with argument 1, disables SPI
 *   and enables USB. If invoked with argument 0, disables USB.
 * * UIST ("USB Interface Status"): Returns 1 if USB is enabled, 0 otherwise.
 * * SIEN ("SPI Interface Enable"): If invoked with argument 1, disables USB
 *   and enables SPI. If invoked with argument 0, disables SPI.
 * * SIST ("SPI Interface Status"): Returns 1 if SPI is enabled, 0 otherwise.
 * * ISOL: Resets the four GPIO pins used for SPI. Intended to be invoked with
 *   argument 0, then once more with argument 1.
 *
 * UIEN and UIST are only provided on the MacBookPro12,1.
 */

#define pr_fmt(fmt) "applespi: " fmt

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/acpi.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/property.h>
#include <linux/delay.h>

#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input-polldev.h>

#define APPLESPI_PACKET_SIZE    256

#define PACKET_KEYBOARD         288
#define PACKET_TOUCHPAD         544
#define PACKET_NOTHING          53312

#define MAX_ROLLOVER 		6

#define MAX_FINGERS		6
#define MAX_FINGER_ORIENTATION	16384

#define X_MIN 			-4828
#define X_MAX 			5345

#define Y_MIN 			-203
#define Y_MAX 			6803

struct keyboard_protocol {
	u16		packet_type;
	u8		unknown1[9];
	u8		counter;
	u8		unknown2[5];
	u8		modifiers;
	u8		unknown3;
	u8		keys_pressed[6];
	u8		fn_pressed;
	u16		crc_16;
	u8		unused[228];
};

/* trackpad finger structure, le16-aligned */
struct tp_finger {
	__le16 origin;          /* zero when switching track finger */
	__le16 abs_x;           /* absolute x coodinate */
	__le16 abs_y;           /* absolute y coodinate */
	__le16 rel_x;           /* relative x coodinate */
	__le16 rel_y;           /* relative y coodinate */
	__le16 tool_major;      /* tool area, major axis */
	__le16 tool_minor;      /* tool area, minor axis */
	__le16 orientation;     /* 16384 when point, else 15 bit angle */
	__le16 touch_major;     /* touch area, major axis */
	__le16 touch_minor;     /* touch area, minor axis */
	__le16 unused[2];       /* zeros */
	__le16 pressure;        /* pressure on forcetouch touchpad */
	__le16 multi;           /* one finger: varies, more fingers: constant */
	__le16 padding;
} __attribute__((packed,aligned(2)));

struct touchpad_protocol {
	u16			packet_type;
	u8			unknown1[4];
	u8			number_of_fingers;
	u8			unknown2[4];
	u8			counter;
	u8			unknown3[2];
	u8			number_of_fingers2;
	u8			unknown[2];
	u8			clicked;
	u8			rel_x;
	u8			rel_y;
	u8			unknown4[44];
	struct tp_finger	fingers[MAX_FINGERS];
	u8			unknown5[208];
};

struct applespi_data {
	struct spi_device		*spi;
	struct input_polled_dev		*poll_dev;
	struct input_dev		*touchpad_input_dev;

	u8				*tx_buffer;
	u8				*rx_buffer;

	struct mutex			mutex;

	u8				last_keys_pressed[MAX_ROLLOVER];
	struct input_mt_pos		pos[MAX_FINGERS];
	int				slots[MAX_FINGERS];
};

static const unsigned char applespi_scancodes[] = {
	0,  0,  0,  0,
	KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J,
	KEY_K, KEY_L, KEY_M, KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T,
	KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
	KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
	KEY_ENTER, KEY_ESC, KEY_BACKSPACE, KEY_TAB, KEY_SPACE, KEY_MINUS,
	KEY_EQUAL, KEY_LEFTBRACE, KEY_RIGHTBRACE, KEY_BACKSLASH, 0,
	KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_DOT, KEY_SLASH,
	KEY_CAPSLOCK,
	KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9,
	KEY_F10, KEY_F11, KEY_F12, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	KEY_RIGHT, KEY_LEFT, KEY_DOWN, KEY_UP,
};

static const unsigned char applespi_controlcodes[] = {
	KEY_LEFTCTRL,
	KEY_LEFTSHIFT,
	KEY_LEFTALT,
	KEY_LEFTMETA,
	0,
	KEY_RIGHTSHIFT,
	KEY_RIGHTALT,
	KEY_RIGHTMETA
};

u8 *applespi_init_commands[] = {
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x01\xD0\x00\x00\x04\x00\x00\x40\x89\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x00\x00\x00\x04\x00\x00\x60\x19\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x01\x00\x00\x04\x00\x00\x61\xC8\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x02\x00\x00\x04\x00\x00\x61\xFB\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x03\x00\x00\x04\x00\x00\x60\x2A\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x02\x04\x00\x00\x04\x00\x00\x61\x9D\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\x01\x00\x00\x00\x00\x0A\x00\x32\xBF\x00\x00\x08\x00\x00\x00\xCE\x66\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x2D\xFF",
	"\x40\x01\x00\x00\x00\x00\x0A\x00\x32\x02\x00\x01\x1E\x00\x00\x00\x9A\xE5\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x2D\xFF",
	"\x40\x01\x00\x00\x00\x00\x0E\x00\x52\x09\x00\x02\x04\x00\x04\x00\x09\x00\x00\x00\x0D\x10\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x5F\x19",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x01\x00\x00\x04\x00\x00\x53\xC9\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x01\x00\x00\x04\x00\x00\x53\xC9\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62",
	"\x40\x01\x00\x00\x00\x00\x0C\x00\x51\x01\x00\x03\x02\x00\x02\x00\x01\x00\x6D\xDE\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x66\x6A",
	"\x40\x02\x00\x00\x00\x00\x0C\x00\x52\x02\x00\x00\x02\x00\x02\x00\x02\x01\x7B\x11\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x23\xAB",
	"\x40\xD0\x00\x00\x00\x00\x0A\x00\x20\x10\x02\x00\x00\x04\x00\x00\x53\xFA\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\xD0\x62"
};

static ssize_t
applespi_sync(struct applespi_data *applespi, struct spi_message *message)
{
	struct spi_device *spi;
	int status;

	spi = applespi->spi;

	status = spi_sync(spi, message);

	if (status == 0)
		status = message->actual_length;

	return status;
}

static inline ssize_t
applespi_sync_write_and_response(struct applespi_data *applespi)
{
	/*
	The Windows driver always seems to do a 256 byte write, followed
	by a 4 byte read, followed by a 256 byte read for the real response.

	For some reason, weird things happen at the proper speed (8MHz) but
	everything seems to be OK at 400kHz
	*/
	struct spi_transfer t1 = {
		.tx_buf			= applespi->tx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.cs_change		= 1,
		.speed_hz		= 400000
	};

	struct spi_transfer t2 = {
		.rx_buf			= applespi->rx_buffer,
		.len			= 4,
		.cs_change		= 1,
		.speed_hz		= 400000
	};

	struct spi_transfer t3 = {
		.rx_buf			= applespi->rx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.speed_hz		= 400000
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t1, &m);
	spi_message_add_tail(&t2, &m);
	spi_message_add_tail(&t3, &m);
	return applespi_sync(applespi, &m);
}

static inline ssize_t
applespi_sync_read(struct applespi_data *applespi)
{
	struct spi_transfer t = {
		.rx_buf			= applespi->rx_buffer,
		.len			= APPLESPI_PACKET_SIZE,
		.speed_hz		= 400000
	};
	struct spi_message      m;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);
	return applespi_sync(applespi, &m);
}


static void applespi_open(struct input_polled_dev *dev)
{
	int i;
	struct applespi_data *applespi = dev->private;
	ssize_t items = sizeof(applespi_init_commands) / sizeof(applespi_init_commands[0]);

	for (i=0; i < items; i++) {
		memcpy(applespi->tx_buffer, applespi_init_commands[i], 256);
		applespi_sync_write_and_response(applespi);
//		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, applespi->rx_buffer, APPLESPI_PACKET_SIZE, false);
	}
}

static void applespi_close(struct input_polled_dev *dev)
{
	pr_info("closed");
}

static void applespi_print_touchpad_frame(struct touchpad_protocol* t)
{
	pr_info("x 1: %d, x 2: %d, x 3: %d, x 4: %d, x 5: %d, x 6: %d", t->fingers[0].abs_x, t->fingers[1].abs_x, t->fingers[2].abs_x, t->fingers[3].abs_x, t->fingers[4].abs_x, t->fingers[5].abs_x);
	print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, t, 256, false);
}


// Lifted from the BCM5974 driver
/* convert 16-bit little endian to signed integer */
static inline int raw2int(__le16 x)
{
	return (signed short)le16_to_cpu(x);
}

static void report_finger_data(struct input_dev *input, int slot,
			       const struct input_mt_pos *pos,
			       const struct tp_finger *f)
{
	input_mt_slot(input, slot);
	input_mt_report_slot_state(input, MT_TOOL_FINGER, true);

	input_report_abs(input, ABS_MT_TOUCH_MAJOR,
			 raw2int(f->touch_major) << 1);
	input_report_abs(input, ABS_MT_TOUCH_MINOR,
			 raw2int(f->touch_minor) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MAJOR,
			 raw2int(f->tool_major) << 1);
	input_report_abs(input, ABS_MT_WIDTH_MINOR,
			 raw2int(f->tool_minor) << 1);
	input_report_abs(input, ABS_MT_ORIENTATION,
			 MAX_FINGER_ORIENTATION - raw2int(f->orientation));
	input_report_abs(input, ABS_MT_POSITION_X, pos->x);
	input_report_abs(input, ABS_MT_POSITION_Y, pos->y);
}

static int report_tp_state(struct applespi_data *applespi, struct touchpad_protocol* t)
{
	const struct tp_finger *f;
	struct input_dev *input = applespi->touchpad_input_dev;
	int i, n;

	n = 0;

	for (i = 0; i < MAX_FINGERS; i++) {
		f = &t->fingers[i];
		if (raw2int(f->touch_major) == 0)
			continue;
		applespi->pos[n].x = raw2int(f->abs_x);
		applespi->pos[n].y = Y_MIN + Y_MAX - raw2int(f->abs_y);
		n++;
	}

//	pr_info("x: %d, y: %d", applespi->pos[0].x, applespi->pos[0].y);
	input_mt_assign_slots(input, applespi->slots, applespi->pos, n, 0);

	for (i = 0; i < n; i++)
		report_finger_data(input, applespi->slots[i],
				   &applespi->pos[i], &t->fingers[i]);

	input_mt_sync_frame(input);
	input_report_key(input, BTN_LEFT, t->clicked);

	input_sync(input);
	return 0;
}

static void
applespi_got_data(struct applespi_data *applespi)
{
	struct keyboard_protocol keyboard_protocol;
	int i, j;
	bool still_pressed;

	memcpy(&keyboard_protocol, applespi->rx_buffer, APPLESPI_PACKET_SIZE);

	if (keyboard_protocol.packet_type == PACKET_NOTHING) {
		return;
	} else if (keyboard_protocol.packet_type == PACKET_KEYBOARD) {
//		pr_info("---");
//		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
		for (i=0; i<6; i++) {
			still_pressed = false;
			for (j=0; j<6; j++) {
				if (applespi->last_keys_pressed[i] == keyboard_protocol.keys_pressed[j]) {
					still_pressed = true;
					break;
				}
			}

			if (! still_pressed) {
				input_report_key(applespi->poll_dev->input, applespi_scancodes[applespi->last_keys_pressed[i]], 0);
			}
		}

		for (i=0; i<6; i++) {
			if (keyboard_protocol.keys_pressed[i] < sizeof(applespi_scancodes) && keyboard_protocol.keys_pressed[i] > 0) {
				input_report_key(applespi->poll_dev->input, applespi_scancodes[keyboard_protocol.keys_pressed[i]], 1);
			}
		}

		// Check control keys
		for (i=0; i<8; i++) {
			if (test_bit(i, (long unsigned int *)&keyboard_protocol.modifiers)) {
				input_report_key(applespi->poll_dev->input, applespi_controlcodes[i], 1);
			} else {
				input_report_key(applespi->poll_dev->input, applespi_controlcodes[i], 0);
			}
		}

		input_sync(applespi->poll_dev->input);
		memcpy(&applespi->last_keys_pressed, keyboard_protocol.keys_pressed, sizeof(applespi->last_keys_pressed));
	} else if (keyboard_protocol.packet_type == PACKET_TOUCHPAD) {
//		pr_info("--- %d", keyboard_protocol.packet_type);
//		applespi_print_touchpad_frame((struct touchpad_protocol*)&keyboard_protocol);
		report_tp_state(applespi, (struct touchpad_protocol*)&keyboard_protocol);

	} else {
//		pr_info("--- %d", keyboard_protocol.packet_type);
//		print_hex_dump(KERN_INFO, "applespi: ", DUMP_PREFIX_NONE, 32, 1, &keyboard_protocol, APPLESPI_PACKET_SIZE, false);
	}

}

static void applespi_poll(struct input_polled_dev *dev)
{
	struct applespi_data *applespi = dev->private;

	applespi_sync_read(applespi);
	applespi_got_data(applespi);
}

static int applespi_probe(struct spi_device *spi)
{
	struct applespi_data *applespi;
	int result, i;

	/* Allocate driver data */
	applespi = devm_kzalloc(&spi->dev, sizeof(*applespi), GFP_KERNEL);
	if (!applespi)
		return -ENOMEM;

	/* Initialize the driver data */
	applespi->spi = spi;
	mutex_init(&applespi->mutex);

	/* Create our buffers */
	applespi->tx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);
	applespi->rx_buffer = devm_kmalloc(&spi->dev, APPLESPI_PACKET_SIZE, GFP_KERNEL);

	// Store the driver data
	spi_set_drvdata(spi, applespi);

	pr_info("acpi spi hz: %d", spi->max_speed_hz);
	pr_info("acpi spi bpw: %d", spi->bits_per_word);
	pr_info("acpi spi mode: %d", spi->mode);

	// Setup the poll_dev, which is also the keyboard input
	applespi->poll_dev = devm_input_allocate_polled_device(&spi->dev);

	if (!applespi->poll_dev)
		return -ENOMEM;

	applespi->poll_dev->private = applespi;
	applespi->poll_dev->poll_interval = 1;
	applespi->poll_dev->poll_interval_min = 1;
	applespi->poll_dev->poll_interval_max = 1;

	applespi->poll_dev->open = applespi_open;
	applespi->poll_dev->close = applespi_close;
	applespi->poll_dev->poll = applespi_poll;

	applespi->poll_dev->input->name = "Apple SPI Keyboard";
	applespi->poll_dev->input->phys = "applespi/input0";
	applespi->poll_dev->input->dev.parent = &spi->dev;
	applespi->poll_dev->input->id.bustype = BUS_SPI;

	applespi->poll_dev->input->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_LED) | BIT_MASK(EV_REP);
	applespi->poll_dev->input->ledbit[0] = BIT_MASK(LED_CAPSL);

	for (i = 0; i<sizeof(applespi_scancodes); i++) {
		if (applespi_scancodes[i]) {
			input_set_capability(applespi->poll_dev->input, EV_KEY, applespi_scancodes[i]);
		}
	}

	for (i = 0; i<sizeof(applespi_controlcodes); i++) {
		if (applespi_controlcodes[i]) {
			input_set_capability(applespi->poll_dev->input, EV_KEY, applespi_controlcodes[i]);
		}
	}

	result = input_register_polled_device(applespi->poll_dev);
	if (result) {
		pr_info("Unabled to register polled input device (%d)", result);
		return result;
	}

	// Now, set up the touchpad as a seperate input device
	applespi->touchpad_input_dev = devm_input_allocate_device(&spi->dev);

	if (!applespi->touchpad_input_dev)
		return -ENOMEM;

	applespi->touchpad_input_dev->name = "Apple SPI Touchpad";
	applespi->touchpad_input_dev->phys = "applespi/input1";
	applespi->touchpad_input_dev->dev.parent = &spi->dev;
	applespi->touchpad_input_dev->id.bustype = BUS_SPI;

	applespi->touchpad_input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);

	__set_bit(EV_KEY, applespi->touchpad_input_dev->evbit);
	__set_bit(EV_ABS, applespi->touchpad_input_dev->evbit);

	__set_bit(BTN_LEFT, applespi->touchpad_input_dev->keybit);

	__set_bit(INPUT_PROP_POINTER, applespi->touchpad_input_dev->propbit);
	__set_bit(INPUT_PROP_BUTTONPAD, applespi->touchpad_input_dev->propbit);

	/* finger touch area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_TOUCH_MINOR, 0, 2048, 0, 0);

	/* finger approach area */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MAJOR, 0, 2048, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_WIDTH_MINOR, 0, 2048, 0, 0);

	/* finger orientation */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_ORIENTATION, -MAX_FINGER_ORIENTATION, MAX_FINGER_ORIENTATION, 0, 0);

	/* finger position */
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_X, X_MIN, X_MAX, 0, 0);
	input_set_abs_params(applespi->touchpad_input_dev, ABS_MT_POSITION_Y, Y_MIN, Y_MAX, 0, 0);

	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOOL_FINGER);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_TOUCH);
	input_set_capability(applespi->touchpad_input_dev, EV_KEY, BTN_LEFT);

	input_mt_init_slots(applespi->touchpad_input_dev, MAX_FINGERS,
		INPUT_MT_POINTER | INPUT_MT_DROP_UNUSED | INPUT_MT_TRACK);

	result = input_register_device(applespi->touchpad_input_dev);
	if (result) {
		pr_info("Unabled to register touchpad input device (%d)", result);
		return result;
	}

	pr_info("module probe done");

	return 0;
}

static int applespi_remove(struct spi_device *spi)
{
	struct applespi_data *applespi = spi_get_drvdata(spi);

	mutex_lock(&applespi->mutex);

	pr_info("freeing irq");
	free_irq(spi->irq, applespi);

	mutex_unlock(&applespi->mutex);

	pr_info("module exit");

	return 0;
}

static const struct acpi_device_id applespi_acpi_match[] = {
	{ "APP000D", 0 },
	{ },
};
MODULE_DEVICE_TABLE(acpi, applespi_acpi_match);

static struct spi_driver applespi_driver = {
	.driver		= {
		.name			= "applespi",
		.owner			= THIS_MODULE,

		.acpi_match_table	= ACPI_PTR(applespi_acpi_match),
	},
	.probe		= applespi_probe,
	.remove		= applespi_remove,
};
module_spi_driver(applespi_driver)

MODULE_LICENSE("GPL");
