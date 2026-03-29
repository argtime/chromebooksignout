# Chromebook Sign-Out — TI 84+ CE USB HID Emulator

A C program for the **TI 84+ CE** graphing calculator that emulates a
composite USB HID keyboard and absolute-position mouse to automatically
sign out of a Chromebook.

## How It Works

1. The calculator is connected to the Chromebook via a USB-A to mini-USB cable.
2. The TI 84+ CE presents itself to ChromeOS as a USB HID device containing
   two interfaces:
   - **Interface 0** — HID Keyboard (boot protocol)
   - **Interface 1** — HID Absolute Mouse
3. On user request (press **\[ENTER\]** on the calculator), the program:
   - Sends the **Alt + Shift + S** keyboard combination, which opens the
     ChromeOS Quick Settings panel.
   - Waits ~600 ms for the panel animation to complete.
   - Moves the absolute mouse cursor to the **Sign out** button at
     screen coordinates **(1299, 352)** — identified from the reference
     screenshot (`image.png`, 1366 × 768 px).
   - Clicks the left mouse button to trigger the sign-out action.
4. Press **\[CLEAR\]** on the calculator at any time to abort.

## Reference Image

`image.png` in the repository root is the full 1366 × 768 screenshot of
the target Chromebook screen.  The **Sign out** button is located in the
ChromeOS Quick Settings panel header at pixel coordinates **(1299, 352)**.

These coordinates are mapped to the HID absolute mouse logical range
(0 – 32 767) at build time:

| Axis | Formula                                                              | Value  |
|------|----------------------------------------------------------------------|--------|
| X    | 1299 × 32767 / (SCREEN_WIDTH  − 1) = 1299 × 32767 / 1365 ≈ **31182** | 31182  |
| Y    |  352 × 32767 / (SCREEN_HEIGHT − 1) =  352 × 32767 /  767 ≈ **15037** | 15037  |

## Building

### Requirements

| Tool | Notes |
|------|-------|
| [CE C/C++ Toolchain](https://ce-programming.github.io/toolchain/static/setup.html) | Sets `CEDEV` env var |
| `make` | GNU Make ≥ 3.81 |

### Build

```sh
make
```

The output is `bin/CBSGNOUT.8xp`.

A prebuilt `CBSGNOUT.8xp` (built with CEdev v14.2) is in the repository
root if you only need the compiled program.

### Transfer to Calculator

Use **TI Connect™ CE** or **TILP** to send `CBSGNOUT.8xp` to the
TI 84+ CE.  The program appears under **[PRGM]** → **CBSGNOUT**.

## Usage

1. Connect the TI 84+ CE to the Chromebook USB port with a USB cable
   (USB-A host on the Chromebook side, mini-USB on the calculator side).
2. Unlock the Chromebook if it is on the lock screen.
3. Run `CBSGNOUT` on the calculator (`[PRGM]` → select → `[ENTER]`).
4. Press **\[ENTER\]** when prompted ("Press ENTER to start").
   - ChromeOS will recognise the new HID device within a second or two.
5. Once "USB connected!" is displayed, press **\[ENTER\]** again to
   execute the sign-out sequence.

## USB Descriptor Details

| Field             | Value              |
|-------------------|--------------------|
| Vendor ID         | 0x0451 (Texas Instruments) |
| Product ID        | 0x5F00             |
| USB Version       | 2.0                |
| HID Version       | 1.11               |
| EP1 IN (keyboard) | Interrupt, 8 B, 10 ms |
| EP2 IN (mouse)    | Interrupt, 8 B, 10 ms |

## Keyboard Shortcut Reference

| Shortcut | ChromeOS Action |
|----------|-----------------|
| **Alt + Shift + S** | Open Quick Settings panel |

The sign-out button sits in the header row of the Quick Settings panel
(top-right corner) at approximately (1299, 352) on a 1366 × 768 display.

## Project Structure

```
.
├── image.png       Reference screenshot of target Chromebook screen
├── Makefile        CE toolchain build file
├── README.md       This file
└── src/
    └── main.c      TI 84+ CE USB HID keyboard/mouse emulator
```
