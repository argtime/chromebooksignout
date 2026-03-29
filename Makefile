# =============================================================================
# Makefile for TI 84+ CE — Chromebook Sign-Out HID Keyboard/Mouse Emulator
#
# Uses the CE C/C++ Toolchain (ce-toolchain).
# Install guide: https://ce-programming.github.io/toolchain/static/setup.html
#
# Targets:
#   make         — Build the calculator program (.8xp)
#   make clean   — Remove all generated files
#
# The output file (CBSIGNOUT.8xp) can be transferred to the TI 84+ CE
# using TI Connect™ CE or TILP, then run from the Apps/Programs menu.
# =============================================================================

# Program name (up to 8 chars, uppercase, no spaces)
NAME        = CBSGNOUT

# Human-readable description (shown in TI Connect CE)
DESCRIPTION = Chromebook Sign-Out HID Device

# Source directory
SRCDIR      = src

# Additional compiler flags
CFLAGS      = -Wall -Wextra -Wno-unused-parameter

# Libraries required:
#   usbdrvce — USB device/host driver
#   keypadc  — Keypad scanning
#   graphx   — Graphics library (used for on-screen status display)
LIBS        = usbdrvce keypadc graphx

# Include the standard CE toolchain Makefile
# (the CEDEV environment variable must point to the toolchain root)
include $(shell cedev-config --makefile)
