# Prototype firmware dump — 2026-09-02

Full read-out of the MidiBoi prototype as it was found, before any re-flash.

**Why this exists:** the image on the device could not be reproduced from any
source in this repo. It is closest in size to the stale `build/` output from
June 2024 (23766 vs 23772 bytes) but differs materially, so it was built from
working-tree edits that were never committed and no longer exist on disk.
These files are the only remaining copy of that firmware.

## Device

| | |
|---|---|
| MCU | ATmega32U4, signature `0x1e9587` |
| Board | Arduino Leonardo / Pro Micro clone, USB `0x2341:0x8036` |
| USB serial | `MIDI` (enumerates as `/dev/cu.usbmodemMIDI1`) |
| Bootloader | Caterina, 4058 bytes at `0x7000`–`0x7FFF` |
| Application | 23772 bytes at `0x0000`–`0x6FFF` |
| lfuse / hfuse / efuse | `0xFF` / `0xD8` / `0xCB` |
| lock bits | `0xEF` (bootloader section write-protected) |

`hfuse = 0xD8` decodes to BOOTSZ=00 (4 KB boot section, boot reset vector at
`0x7000`) and BOOTRST programmed — stock Arduino Leonardo configuration.

## Files

| File | Region | Notes |
|---|---|---|
| `flash-full.hex` | `0x0000`–`0x7FFF` | Complete 32 KB read, application + bootloader |
| `flash-app.hex` | `0x0000`–`0x6FFF` | Application only — **this is the one to restore via USB** |
| `bootloader.hex` | `0x7000`–`0x7FFF` | Caterina only; needs an ISP programmer, not USB |
| `eeprom.hex` | `0x000`–`0x3FF` | 1 KB EEPROM |

All four verified to round-trip byte-for-byte against the raw avrdude output.

    5380197dcfa971843aa420cd300ee251c2ca3b99053904c1ceed3d63c2daa593  flash-app.hex
    389964aacb5a72e43d712f84bc26de9b6bfc48ffafe4931028ca829ebbfd7a36  flash-full.hex
    5f14019999e18857e3c4a14604917b524bed160d5a4e5a239ad40e8e83b3b8db  bootloader.hex
    236cccc00acc6a9c6673b1bf8614f41736bb6152ec5db896d921e1c81a88885e  eeprom.hex

## EEPROM contents

Only two bytes are written; the rest is erased (`0xFF`):

    0000: 4a 4c ff ff ...

`0x4A` = 74, `0x4C` = 76. `MidiBoi.ino` reads addresses 0 and 1 at startup and
lets them override the compiled defaults, so this device boots with
**wheel 1 = CC 74, wheel 2 = CC 76** — not the documented defaults of CC 26 /
CC 24. No pot min/max ranges were ever saved.

## How this was captured

The 32U4 must be in the Caterina bootloader to be read: open the CDC port at
1200 baud and drop DTR, wait for the port to re-enumerate under a new name,
then read within the ~8 second bootloader window.

    avrdude -C <arduino15>/packages/arduino/tools/avrdude/6.3.0-arduino17/etc/avrdude.conf \
            -c avr109 -p atmega32u4 -P /dev/cu.usbmodemXXXX -b 57600 -D \
            -U flash:r:flash-full.hex:i \
            -U eeprom:r:eeprom.hex:i

## Restoring

Application only, over USB (same 1200-baud bootloader entry as above):

    avrdude -C <conf> -c avr109 -p atmega32u4 -P /dev/cu.usbmodemXXXX -b 57600 -D \
            -U flash:w:flash-app.hex:i

To also restore the saved CC assignments, add:

    -U eeprom:w:eeprom.hex:i

`bootloader.hex` cannot be written over USB — the lock bits protect the boot
section and the bootloader cannot overwrite itself. It needs an ISP programmer
(USBasp, or another Arduino running ArduinoISP) on the 6-pin header.

Note that a normal `make upload` erases application pages only, so EEPROM —
and therefore the CC 74/76 assignment — survives an ordinary re-flash.
