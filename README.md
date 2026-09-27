# Waveshare ESP32-S3 USB microphone

Standalone USB Audio Class firmware for the Waveshare ESP32-S3 Touch AMOLED 1.8 V2. It streams the board's analog microphone as mono, signed 16-bit PCM at 24 kHz over the ESP32-S3 native USB port. Its CO5300 display shows a sound-reactive visualization on black. A second USB CDC interface accepts a deliberate recovery command for entering ROM download mode. An optional Wi-Fi stream can feed a Mac-wide BlackHole 2ch virtual microphone after local provisioning. There is no speaker output or dependency on Codex Remote at runtime.

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
idf.py -p /dev/cu.usbmodemXXXX flash  # Use the printed ROM port.
# If macOS still shows the ROM serial device, or the screen stays black:
python tools/leave_bootloader.py
```

Run the helpers with ESP-IDF's Python environment exported; they require `pyserial` and `esptool`. The command accepted by CDC is exactly `MIC BOOTLOADER` followed by a newline. `MIC POWER` reports the PMU's battery presence, voltage, charge percentage, USB power, and charging state. Opening the port or toggling DTR alone does not request a reboot. `leave_bootloader.py` clears the ESP32-S3 RTC force-download bit, then resets the ROM serial port if necessary. The CDC interface is a control path, not a hardware JTAG endpoint or a log console.

## Wi-Fi microphone on macOS

The USB mic remains available. Wireless audio starts after Wi-Fi is configured, the Mac receiver is running, and a Mac process opens **BlackHole 2ch** as an input. The board and Mac must be on the same LAN; the board supports 2.4 GHz Wi-Fi. The receiver broadcasts discovery on UDP port 24242, then sends discovery and input-activity messages directly to the board once it learns its address. The board replies and, while input is active, sends 10 ms packets of 24 kHz mono PCM16 to that receiver. The Mac receiver converts to 48 kHz stereo and writes directly to BlackHole. In any Mac app, select **BlackHole 2ch** as the input. The normal Mac speaker output is unchanged.

1. Install [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) and [PortAudio](https://www.portaudio.com/). On Homebrew: `brew install --cask blackhole-2ch && brew install portaudio`. BlackHole may require a macOS restart before it appears. Set its format to 48 kHz in Audio MIDI Setup if necessary.
2. While the board is connected over USB, provision a 2.4 GHz-capable SSID. The script asks for the Wi-Fi password locally, without writing it to this repository or placing it in a shell command argument:

   ```sh
   "$HOME/.espressif/python_env/idf5.5_py3.10_env/bin/python" tools/configure_wifi.py
   "$HOME/.espressif/python_env/idf5.5_py3.10_env/bin/python" tools/configure_wifi.py --status
   # To remove the saved network later, add --clear instead.
   ```

   The board stores the credentials in its NVS flash partition and restarts after changes. A full-flash backup contains those credentials and should be kept private. If the network has separate 2.4 and 5/6 GHz names, use the 2.4 GHz name.
3. Build and run the receiver on the Mac:

   ```sh
   make -C receiver
   receiver/waveshare-receiver --list  # Must show BlackHole 2ch output.
   receiver/waveshare-receiver
   ```

4. Unplug USB while the board remains powered by its battery. Select **BlackHole 2ch** in an app's microphone menu and open its input. The board wakes, turns on its display and microphone, and begins streaming. Closing the input stops capture and turns the display off while Wi-Fi remains connected for the next wake. An app's live input meter can also keep it awake before Record is pressed. Keep the receiver process running while using wireless audio; this version does not start it automatically at login. To detach it from a Terminal window, start it with `tmux new-session -d -s waveshare-mic -c "$PWD" "$PWD/receiver/waveshare-receiver"`. Inspect its output with `tmux capture-pane -pt waveshare-mic -S -20`, and stop it with `tmux kill-session -t waveshare-mic`. Run only one receiver instance. PWR manually pauses and resumes capture; BOOT still cycles visualizations.

This wireless path sends unencrypted PCM over the local Wi-Fi network and accepts discovery from another host on that network. Use a trusted LAN. It supports one receiver at a time; another discovery sender can take over. Some guest networks block communication between clients or UDP broadcast. Standby powers down the codec input and I2S clocks, and blanks the AMOLED to black, but keeps Wi-Fi associated to receive wake messages, including after a PWR press. The panel controller remains powered because its off command has sometimes left the screen black until a full power cycle. ESP-IDF's default minimum modem sleep can reduce radio power between beacons. This is not deep sleep, and battery-current savings have not been measured. A CoreAudio activity check runs every 250 ms, so the beginning of a recording can contain a short silence while the board wakes. The receiver buffers about 50–100 ms and inserts silence on missing packets; latency and long-session clock stability need physical testing.

## Visualizations and controls

Short-press **BOOT** while the microphone is active to cycle through seven full-screen visualizations:

1. A waveform whose sound bars and colors scroll from right to left together.
2. Twelve color spectrum columns whose cells fade after a frequency band falls; the display updates this mode at most ten times per second.
3. Three flowing aurora ribbons with a center line and shaded color gradients.
4. A thin white-rimmed circle with an angle-colored outer edge that pulses by frequency band.
5. A spectral waterfall that scrolls the recent frequency colors down the screen.
6. A neon oscilloscope showing live PCM samples with a bold pixel stroke and two fading trails.
7. A radial fan whose colored spokes extend with the frequency bands.

The screen has no text or on-screen controls. Every mode is drawn procedurally in RGB565; the [design concepts](design/README.md) are references, and [the renderer preview](design/rendered-preview.png) shows frames generated by the firmware renderer from synthetic audio. During USB or Wi-Fi capture, the stream supplies audio samples to the display. When Wi-Fi is unconfigured, the display can read the codec while no USB app is recording. With Wi-Fi configured, capture starts off until PWR is pressed or a Mac app opens BlackHole input. **BOOT** mode changes are kept in RAM and return to the waveform after a restart.

## Pause for battery life

Short-press the board's physical **PWR** button to toggle capture and the AMOLED image together. Off means the codec input is powered down, the I2S channel and clocks stop, and the screen is filled with black pixels without further redraws. The USB microphone remains connected to the Mac but supplies silence to an app that continues recording. Opening BlackHole input on the Mac also turns capture and the image on; closing it turns both off. These controls have the same behavior on USB and battery power. PWR can turn capture off while a Mac input is open; the Mac's next close/open cycle turns it on again. A BOOT press while off is ignored. Holding BOOT while starting the board still enters ROM download mode.

This is a reversible pause, not a full board shutdown. The ESP32-S3 and Wi-Fi remain awake to receive the Mac's next input-open signal or a PWR press. A Mac input-close signal turns capture off even if PWR previously turned it on. Battery-current reduction has not yet been measured.

To log the PMU's voltage and charge estimate without USB power, leave the screen off, find the board's LAN address in the receiver output, unplug USB, and run `python3 tools/measure_battery.py BOARD_IP --seconds 300 --interval 15`. It sends a short `WMIC_POWER_V1` UDP query and prints time, millivolts, percent, and USB-power presence. The [five-minute idle trace](measurements/2026-09-27-idle-battery.csv) recorded 21 replies with USB absent: 3,851 mV at the start, 3,814 mV after one minute, and 3,772 mV after five minutes. The first minute includes settling after unplugging. The PMU's percentage rose from 29% to 31% during the same run; [Waveshare notes](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.8) that this voltage-based estimate can fluctuate with load and charger state. This short voltage trace does not establish current draw or hours of runtime. Measuring battery current inline or timing a full discharge from a known capacity is required for that.

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

- On 2026-09-27, after wireless audio continued while the screen stayed black, USB CDC diagnostics showed the display task running, wake commands succeeding, and no draw error. A software restart did not restore the image; a full PWR-off power cycle did. The Waveshare V2 board definition has no LCD reset GPIO. The revised image therefore blanks the AMOLED with black pixels instead of sending `DISP_OFF`; it built and flashed with all region hashes verified. macOS enumerated the USB microphone and CDC port again. A six-second BlackHole input session delivered roughly 600 packets and non-silent audio (mean -33.3 dB, peak -3.8 dB); after it closed, the board reported capture idle. Nicolas then confirmed on the flashed board that PWR wakes and darkens the display with USB connected and while battery-powered, and that a wireless recording wakes the screen and closing it darkens the screen again.

- On 2026-09-27, the revised PWR/recording control image built and flashed with all region hashes verified. On USB power, PWR lit and darkened the display, a two-second USB recording while off contained 48,000 zero samples, and a BlackHole input-open/close cycle started and stopped Wi-Fi audio. The PMU reported a present but nearly depleted battery (3%), then charging. After charging, USB was unplugged: the CDC port disappeared, the board kept replying to Wi-Fi discovery while idle, and a 25-second BlackHole input session received about 100 audio packets/s with no reported loss. A concurrent BlackHole capture contained 210,754 nonzero samples out of 211,840; Nicolas confirmed the screen lit for recording and darkened afterward. On battery, PWR lit and darkened the screen; from that PWR-off state, a second Mac input-open/close cycle streamed at about 100 packets/s with no reported loss, and Nicolas saw the screen light and darken again. Battery current and endurance remain unmeasured.

- On 2026-09-27, a full 16 MB backup of the working USB/Wi-Fi image was saved outside the repository as `~/.local/share/esp32-usb-mic/backups/usb-wifi-mic-before-auto-standby-2026-09-27.bin` (SHA-256 `d99f9becd4829efb4ce09d9d0168224aa5b5e0889cbd5235229dd625443986b9`). The automatic standby image built and flashed with all region hashes verified. The Mac receiver detected a separate BlackHole input process opening and closing; audio packets stopped at idle, resumed at about 100 packets/s while the input was open, and stopped again on close. An eight-second BlackHole recording contained 323,141 nonzero samples out of 338,944. A three-second USB recording after a normal reboot contained 70,908 nonzero samples out of 72,000. Nicolas confirmed PWR can wake an automatically dark screen and turn it off again. During one wireless capture, he reported that the display stayed dark. A temporary CDC probe showed the display task running, requesting the panel on, and panel commands returning success while packets streamed; physical display wake was unresolved in that image. Battery current was not measured.

- The built image was flashed with `idf.py flash`; esptool verified each flashed region by hash. After a full power cycle with BOOT released, macOS enumerated `Waveshare USB Microphone` at USB VID:PID `303A:8000` and listed it as the default input, one channel, 24 kHz, USB transport.
- FFmpeg captured 30 seconds of PCM16 from that input. A spoken test phrase containing “penguins dance in Chicago” was transcribed twice with those words intact (the first word was misheard as “Caffery”). This verifies intelligible speech through the board's physical microphone.
- The first static display image showed a bright green strip at the right edge. A user photo confirmed it was a layout defect. Waveshare's V2 ESP-IDF example applies a 16-pixel CO5300 horizontal GRAM offset; this project now applies the same offset. Nicolas confirmed the strip disappeared after the corrected image was flashed. The audio input still enumerated, and a 5-second capture from that image was non-silent.
- The composite image enumerated the same microphone plus a `/dev/cu.usbmodem*` CDC port. CDC `PING` returned `Waveshare USB Microphone`. `enter_bootloader.py` reached the ROM port without physical input; `leave_bootloader.py` returned to the microphone without physical input. A full `enter_bootloader.py` → `idf.py flash` remote update also verified all flashed region hashes and returned to the microphone. FFmpeg captured non-silent audio after that update, and Nicolas confirmed the display still looked correct without the green strip. The ESP-IDF flash run reported a serial-port-disappeared exception after its hash checks because the app re-enumerated; subsequent USB, CDC, audio, and display checks passed.
- The waveform image built and flashed successfully. macOS still showed the microphone and CDC port; an 8-second capture measured about -28.6 dB mean and -8.8 dB peak. After flashing, the board's screen appeared black even though USB was working. A remote `enter_bootloader.py` → `leave_bootloader.py` restart brought the waveform on. Nicolas confirmed that the screen shows the cyan waveform and that its bars move when he speaks near the board. There is no text or status information on this screen.
- The four-mode image was flashed through CDC/ROM, with all three flashed regions hash-verified. macOS enumerated the microphone at `303A:8000` and the CDC port; CDC `PING` replied correctly. A six-second recording was non-silent. The display was initially black after flashing, then lit after `enter_bootloader.py` → `leave_bootloader.py`; Nicolas confirmed that short PWR presses cycle modes. A short BOOT press turned the display off while a four-second USB recording contained exactly 96,000 zero samples. A second BOOT press restored the screen, and the next four-second recording contained 95,904 nonzero samples out of 96,000. Voice intelligibility and display frame rate were not measured for this image.
- The wireframe orb looked static on the board. A temporary CDC timing probe measured about 193 ms drawing and 145 ms transferring each orb frame. The orb was replaced with a frequency-reactive circle, the waveform was changed to scroll right to left, and all four modes were rasterized by their visible shapes into 64-row DMA stripes. The same probe measured about 18 ms drawing and 31 ms transferring spectrum and circle frames, plus the 15 ms task delay. Nicolas confirmed the circle reacts to speech, the waveform scrolls, and the ribbons and circle feel responsive. The temporary probe was then removed from the final source.
- The clean image without the timing probe was built and hash-verified during flashing. After the CDC restart, macOS again enumerated the microphone and CDC port; `PING` replied correctly, the temporary diagnostic command was absent, and a three-second recording contained 71,941 nonzero samples out of 72,000. Nicolas confirmed that the display was lit and animating normally.
- The five-mode image with the spectral waterfall and reversed controls was built and flashed through CDC/ROM, with all flashed regions hash-verified. After the CDC restart, macOS again listed `Waveshare USB Microphone` as an audio input and exposed its CDC port; `PING` returned the product name. A three-second recording contained 71,918 nonzero samples out of 72,000, with a peak magnitude of 4,936. Nicolas confirmed that short PWR presses turn the display off and back on, BOOT cycles the visualizations, and all five modes look good on the board, including the slower spectrum and waterfall. Speech intelligibility, the current pause stream's silence, and battery current were not measured in this build.
- The seven-mode image was built and flashed through CDC/ROM, with all flashed regions hash-verified. The microphone and CDC port returned after the remote restart. A three-second macOS recording contained 71,919 nonzero samples out of 72,000. Nicolas confirmed that the circle's narrower white rim looks better and that both the new oscilloscope and radial fan react clearly to his voice and look good on the board. The frame rate and speech intelligibility of this image were not separately measured.
- The oscilloscope was revised from a thin line to a connected five-pixel trace with bright square points and a lower quiet-speech threshold. The final image was built and re-flashed with all region hashes verified. After the CDC restart, the microphone and serial port returned; a three-second macOS recording contained 71,938 nonzero samples out of 72,000. Nicolas confirmed that the display is lit and the new trace is clear and responsive. Frame rate was not measured.
- The original Codex Remote 16 MB flash image was saved outside the repository before flashing, at `~/.local/share/esp32-usb-mic/backups/codex-remote-2026-09-26-full-flash.bin`, with SHA-256 `f3b5dbf5ef70a54169961dc5cf7241aa75d5b0fbfe4c49b0d4ae70762c61fd94`. Treat that backup as sensitive because it includes NVS.
- The optional Wi-Fi image built and flashed with all region hashes verified. USB CDC `MIC WIFI STATUS` replied `Wi-Fi unconfigured`, and a three-second macOS USB capture contained 71,771 nonzero samples out of 72,000. BlackHole 2ch 0.6.1 installed on the Mac and appeared as a two-channel, 48 kHz virtual input/output after CoreAudio restarted. A synthetic 440 Hz UDP source drove the receiver; a four-second BlackHole recording contained 127,960 nonzero samples with a peak of 7,999.
- Nicolas provisioned the board to a compatible Wi-Fi network with the local password prompt; no secret entered the repository. The receiver then saw about 100 real audio packets per second. With USB physically unplugged and the board powered by its battery, the USB serial port disappeared while the Wi-Fi packets continued at the same rate with no receiver underruns. A 32.8-second BlackHole recording contained 1,565,472 nonzero samples out of 1,573,376, with speech-level peaks while Nicolas spoke near the board. Nicolas confirmed the untethered screen stayed lit. This proves a Mac-wide BlackHole input carrying audio from the physical mic without USB.
- A follow-up image that also stops Wi-Fi on PWR pause built and flashed with all region hashes verified. After the familiar post-flash black screen, the USB recovery restart restored the microphone; a three-second USB recording contained 71,768 nonzero samples out of 72,000. The Mac receiver reconnected to the board with about 100 audio packets per second and no initial loss or underruns. Nicolas confirmed that PWR switched the screen off and back on. The receiver saw audio packets stop during pause and return after wake at roughly 100 per second. Its one underrun occurred at the intentional pause boundary; battery current was not measured. A later three-second BlackHole recording after wake contained 126,715 nonzero samples out of 127,488. USB and Wi-Fi capture ran concurrently for four seconds; USB recorded 95,727 nonzero samples out of 96,000 while Wi-Fi returned to roughly 100 packets per second after one underrun at the handoff.

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
- The battery-powered board may leave the display black immediately after a remote flash. The CDC recovery and return scripts restored it in the observed session; a cold power cycle is the manual fallback.
- The Mac receiver runs in a Terminal or detached `tmux` session. A user LaunchAgent was tried on this macOS 27 prerelease system, but its PortAudio stream setup blocked inside CoreAudio before opening the UDP socket. That LaunchAgent was removed. The manually started receiver is the verified path and will need restarting after logout or reboot.
- The current PWR pause/wake and BOOT mode cycling were visually verified on the board. USB silence while paused was measured in the earlier build, before the physical button actions were swapped. Battery-current reduction has not been measured. The frame timing above was measured in a diagnostic build; that build had the same sparse-stripe renderer without the current visual refinements.
