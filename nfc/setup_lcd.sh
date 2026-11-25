#!/bin/bash
# Waveshare 3.5" Capacitive Touch LCD (ST7796S display on SPI0, FT6336U touch on I2C1) for Raspberry Pi 5.
# Wiring (as used): VCC->pin1 3V3, GND->pin6, TP_SDA->GPIO2, TP_SCL->GPIO3, TP_INT->GPIO4, TP_RST->GPIO17,
#   LCD_BL->GPIO18, LCD_RST->GPIO27, MOSI->GPIO10, MISO->GPIO9, LCD_DC->GPIO25, SCLK->GPIO11, LCD_CS->GPIO8 (CE0)
#
# Uses the kernel's generic panel-mipi-dbi driver (mipi-dbi-spi overlay) + an ST7796S init file,
# and the edt-ft5x06 driver for the FocalTech touch controller.
#   sudo bash nfc/setup_lcd.sh            install, then reboot
#   sudo bash nfc/setup_lcd.sh --undo     restore the previous config.txt, then reboot
#   sudo bash nfc/setup_lcd.sh --no-invert  colours look inverted? reinstall without display inversion
set -e
CONFIG=/boot/firmware/config.txt
[ -f "$CONFIG" ] || CONFIG=/boot/config.txt
BACKUP="$CONFIG.before-lcd"
MARK="# --- Waveshare 3.5in capacitive LCD (hybrid attendance) ---"

if [ "$(id -u)" != 0 ]; then echo "Run with sudo: sudo bash $0 $*"; exit 1; fi

if [ "$1" = "--undo" ]; then
  if [ -f "$BACKUP" ]; then cp "$BACKUP" "$CONFIG"; echo "Restored $CONFIG from backup. Reboot: sudo reboot"; else echo "No backup found ($BACKUP)"; fi
  exit 0
fi

OVERLAYS="$(dirname "$CONFIG")/overlays"
for o in mipi-dbi-spi edt-ft5406; do
  [ -f "$OVERLAYS/$o.dtbo" ] || { echo "Missing overlay $o.dtbo in $OVERLAYS - update the OS: sudo apt update && sudo apt full-upgrade"; exit 1; }
done

INVERT=1
[ "$1" = "--no-invert" ] && INVERT=0

# 1) ST7796S init sequence in the panel-mipi-dbi firmware format
#    (15-byte magic "MIPI DBI" + 7 zero bytes, version 1, then [cmd, len, params...]; delay = [0x00, 1, ms]).
python3 - "$INVERT" <<'EOF'
import sys
invert = sys.argv[1] == '1'
seq = [
    (0x01, []), ('delay', 120),            # software reset
    (0x11, []), ('delay', 120),            # sleep out
    (0xF0, [0xC3]), (0xF0, [0x96]),        # enable extended command set
    (0x36, [0x48]),                        # memory access: portrait 320x480, BGR
    (0x3A, [0x55]),                        # 16-bit colour
    (0xB4, [0x01]),                        # 1-dot inversion
    (0xB7, [0xC6]),                        # entry mode
    (0xC0, [0x80, 0x45]), (0xC1, [0x13]), (0xC2, [0xA7]), (0xC5, [0x0A]),   # power / VCOM
    (0xE8, [0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33]),
    (0xE0, [0xD0, 0x08, 0x0F, 0x06, 0x06, 0x33, 0x30, 0x33, 0x47, 0x17, 0x13, 0x13, 0x2B, 0x31]),  # gamma +
    (0xE1, [0xD0, 0x0A, 0x11, 0x0B, 0x09, 0x07, 0x2F, 0x33, 0x47, 0x38, 0x15, 0x16, 0x2C, 0x32]),  # gamma -
    (0xF0, [0x3C]), (0xF0, [0x69]),        # disable extended command set
    ('delay', 120),
    (0x21 if invert else 0x20, []),        # display inversion on (IPS panel) / off
    (0x29, []), ('delay', 50),             # display on
]
out = bytearray(b'MIPI DBI' + bytes(7) + bytes([1]))
for cmd, params in seq:
    if cmd == 'delay':
        out += bytes([0x00, 0x01, params])
    else:
        out += bytes([cmd, len(params)] + params)
open('/lib/firmware/st7796s.bin', 'wb').write(out)
print(f'Wrote /lib/firmware/st7796s.bin ({len(out)} bytes, inversion {"on" if invert else "off"})')
EOF

# 2) config.txt (backup once, then replace our block)
[ -f "$BACKUP" ] || cp "$CONFIG" "$BACKUP"
python3 - "$CONFIG" "$MARK" <<'EOF'
import sys
path, mark = sys.argv[1], sys.argv[2]
text = open(path).read()
if mark in text:
    text = text[:text.index(mark)].rstrip('\n') + '\n'
block = f"""
{mark}
[all]
dtparam=spi=on
dtparam=i2c_arm=on
dtoverlay=mipi-dbi-spi,spi0-0,speed=40000000
dtparam=compatible=st7796s\\0panel-mipi-dbi-spi
dtparam=width=320,height=480,width-mm=49,height-mm=74
dtparam=reset-gpio=27,dc-gpio=25,backlight-gpio=18
dtoverlay=edt-ft5406,i2c1,interrupt=4,reset=17,sizex=320,sizey=480
"""
open(path, 'w').write(text.rstrip('\n') + '\n' + block)
print('Updated', path)
EOF

echo
echo "Done. Backup of the old config: $BACKUP"
echo "Reboot to start the LCD:  sudo reboot"
echo "After reboot: the LCD shows the desktop/kiosk. Unplug HDMI to make the LCD the only screen."
echo "Colours inverted?  sudo bash $0 --no-invert && sudo reboot      Undo everything:  sudo bash $0 --undo && sudo reboot"
