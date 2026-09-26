# Waveshare ESP32-S3 USB microphone

Standalone USB Audio Class firmware for the Waveshare ESP32-S3 Touch AMOLED 1.8 V2. It streams the board's analog microphone as mono, signed 16-bit PCM at 24 kHz over the ESP32-S3 native USB port. Its CO5300 display shows a static `USB MIC` status screen. A second USB CDC interface accepts a deliberate recovery command for entering ROM download mode. There is no Wi-Fi, speaker output, or dependency on Codex Remote at runtime.

The project uses ESP-IDF 5.5.5, Espressif's `usb_device_uac` 1.3.1 component, its TinyUSB dependency, and Espressif's CO5300 LCD driver. The ES8311 codec driver is adapted from the Apache-2.0 licensed driver already used by Codex Remote; only its I2C transport was changed from Arduino to ESP-IDF. The USB descriptor callback is adapted from the UAC component's MIT-licensed example so macOS shows `Waveshare USB Microphone` as the input name. The codec runs in analog mic mode with 42 dB gain. I2S RX uses GPIO16 MCLK, GPIO9 BCLK, GPIO45 WS, and GPIO10 DIN; I2C uses GPIO15 SDA and GPIO14 SCL at address 0x18. GPIO46 holds the speaker amplifier off. Each 10 ms stereo I2S block selects the more energetic slot for mono USB output.

## Build

Install ESP-IDF 5.5.5 with the `esp32s3` target, then:

```sh
. /path/to/esp-idf-v5.5.5/export.sh
idf.py set-target esp32s3
idf.py build
```

The first build downloads exact component versions recorded in `dependencies.lock`. `sdkconfig.defaults` sets the 16 MB flash size, microphone-only UAC, 24 kHz/16-bit format, macOS compatibility, and USB product strings. The generated `sdkconfig`, `build/`, and `managed_components/` are local artifacts.

## Back up and flash

Before replacing another firmware, identify the board's port and save the **entire 16 MB flash**, including its bootloader, application, NVS, and pairing data. This backup may contain credentials. Keep it outside this repository with permissions restricted to your account.

```sh
mkdir -p "$HOME/.local/share/esp32-usb-mic/backups"
chmod 700 "$HOME/.local/share/esp32-usb-mic/backups"
umask 077
python -m esptool --port /dev/cu.usbmodem101 --baud 115200 \
  read-flash 0 0x1000000 "$HOME/.local/share/esp32-usb-mic/backups/original-full-flash.bin"
shasum -a 256 "$HOME/.local/share/esp32-usb-mic/backups/original-full-flash.bin"

# With ESP-IDF exported and a successful build:
idf.py -p /dev/cu.usbmodemXXXX flash  # Replace with the printed ROM port.
```

The port can change after flashing. The ESP32-S3 hardware USB Serial/JTAG controller and USB OTG controller share one PHY, so hardware JTAG cannot operate at the same time as this USB microphone. The composite TinyUSB CDC interface provides a serial control port while the microphone runs. Espressif's ROM download mode remains the recovery path. This board has **BOOT** and **PWR**, but no separate RESET button: with USB power only, hold BOOT, unplug and reconnect USB, then release BOOT. If a battery keeps the board powered, disconnect USB, hold PWR for about six seconds to switch it off, then hold BOOT while tapping PWR to switch it on. Find the new `/dev/cu.usbmodem*` port and flash or restore through that port. To leave download mode after flashing, disconnect USB, hold PWR for about six seconds, tap PWR once **without touching BOOT**, then reconnect USB. A software reset from the serial port did not reliably leave download mode in the observed battery-powered session.

Once this composite build is running, use its CDC port to request ROM mode without touching the board:

```sh
python tools/enter_bootloader.py
# The script prints the ROM /dev/cu.usbmodem* port after the device reconnects.
idf.py -p /dev/cu.usbmodem101 flash
# If macOS still shows the ROM serial device instead of the microphone:
python tools/leave_bootloader.py
```

Run the helpers with ESP-IDF's Python environment exported; they require `pyserial` and `esptool`. The command accepted by CDC is exactly `MIC BOOTLOADER` followed by a newline. Opening the port or toggling DTR alone does not request a reboot. `leave_bootloader.py` clears the ESP32-S3 RTC force-download bit, then resets the ROM serial port if necessary. The CDC interface is a control path, not a hardware JTAG endpoint or a log console.

## Verify on macOS

1. In **System Information → USB**, confirm `Waveshare USB Microphone` (`303A:8000`). In **Audio MIDI Setup** or **System Settings → Sound → Input**, confirm one input channel at 24 kHz. There is no audio output endpoint. A `/dev/cu.usbmodem*` CDC control port should also appear.
2. List capture devices with `ffmpeg -f avfoundation -list_devices true -i ''`; note the index for `Waveshare USB Microphone`.
3. Speak near the board while recording, using that index in place of `N`:

   ```sh
   ffmpeg -f avfoundation -i ':N' -t 10 -ac 1 -ar 24000 /tmp/waveshare-mic.wav
   ffmpeg -i /tmp/waveshare-mic.wav -af volumedetect -f null -
   afplay /tmp/waveshare-mic.wav
   ```

   Grant microphone access to the terminal or capture app if macOS asks. A changing input meter and non-silent `volumedetect` result demonstrate signal; listening to the WAV checks speech intelligibility.

### Observed on Nicolas's board, 2026-09-26

- The built image was flashed with `idf.py flash`; esptool verified each flashed region by hash. After a full power cycle with BOOT released, macOS enumerated `Waveshare USB Microphone` at USB VID:PID `303A:8000` and listed it as the default input, one channel, 24 kHz, USB transport.
- FFmpeg captured 30 seconds of PCM16 from that input. A spoken test phrase containing “penguins dance in Chicago” was transcribed twice with those words intact (the first word was misheard as “Caffery”). This verifies intelligible speech through the board's physical microphone.
- The first static display image showed a bright green strip at the right edge. A user photo confirmed it was a layout defect. Waveshare's V2 ESP-IDF example applies a 16-pixel CO5300 horizontal GRAM offset; this project now applies the same offset. Nicolas confirmed the strip disappeared after the corrected image was flashed. The audio input still enumerated, and a 5-second capture from that image was non-silent.
- The composite image enumerated the same microphone plus a `/dev/cu.usbmodem*` CDC port. CDC `PING` returned `Waveshare USB Microphone`. `enter_bootloader.py` reached the ROM port without physical input; `leave_bootloader.py` returned to the microphone without physical input. A full `enter_bootloader.py` → `idf.py flash` remote update also verified all flashed region hashes and returned to the microphone. FFmpeg captured non-silent audio after that update, and Nicolas confirmed the display still looked correct without the green strip. The ESP-IDF flash run reported a serial-port-disappeared exception after its hash checks because the app re-enumerated; subsequent USB, CDC, audio, and display checks passed.
- The original Codex Remote 16 MB flash image was saved outside the repository before flashing, at `~/.local/share/esp32-usb-mic/backups/codex-remote-2026-09-26-full-flash.bin`, with SHA-256 `f3b5dbf5ef70a54169961dc5cf7241aa75d5b0fbfe4c49b0d4ae70762c61fd94`. Treat that backup as sensitive because it includes NVS.

## Rollback

With the board in ROM download mode, restore the saved full-flash image. Use the actual backup path and current serial port:

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodem101 \
  write-flash 0 "$HOME/.local/share/esp32-usb-mic/backups/original-full-flash.bin"
```

Reset or reconnect the board afterward. A full-flash restore also restores the original NVS contents; flashing only Codex Remote's application would not.

## Limits

- Fixed 24 kHz mono PCM16 format; the device does not negotiate another rate.
- This macOS-targeted UAC configuration follows Espressif's `UAC_SUPPORT_MACOS` option, which its documentation says may prevent Windows recognition.
- Hardware USB Serial/JTAG cannot coexist with USB audio on the board's single internal PHY. The composite CDC control interface does not expose hardware JTAG or application logs.
- The physical V2 board identification follows the prior successful Codex Remote deployment, not a fresh rear-label inspection.
