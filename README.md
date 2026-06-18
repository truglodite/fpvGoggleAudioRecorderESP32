# fpvGoggleAudioRecorderESP32
## A reliable audio recording device for FPV goggles

<img src="https://github.com/truglodite/fpvgoggleaudiorecorderesp32/blob/main/images/IMG_2937_1.jpg" width="600">

This is code for a plug and forget ambient audio recorder, intended for use with FPV goggles (like DJI goggles).

### Hardware:
*No pins installed if using the printable enclosure*
- ESP32 S3 Super Mini: https://www.amazon.com/dp/B0GFDSK5RD
- ICS43434 I2S Microphone: https://www.amazon.com/ICS43434-Microphone-Breakout-Module-Filter/dp/B0FMDGRM8F
- Micro SD Card Breakout (3.3V): https://www.amazon.com/WWZMDiB-Module-Adapter-Memory-Shield/dp/B0BV8ZQ81F
- 6mm Momentary NO Tactile Button: https://www.amazon.com/Momentary-Tactile-Through-Breadboard-Friendly/dp/B07WF76VHT

* Note that an INMP441 mic could be used in place of the ICS43434 if needed, but audio quality is not quite as good. Also the printed enclosure only fits the ICS mic.

### Operation
Simply plug in USB power, and the system will start to record. Unplug USB to stop the recording. If you need to start a new file during a recording session, hit the button, a blue LED will flash, and a new file will be started. This can be useful if for example you wait a while for GPS before launching, hit the button so you have an audio file that starts just before launching.

When powering on, you should first see a blue LED to indicate boot status followed by a pulsating green LED to indicate normal recording, and with an occasional red flash during louder noises to indicate the clipping limiter is active. If you see just a repeating flashing red led, the SD card is failing. If you see an orange flashing LED, the audio buffer is falling behind.

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/IMG_2940_1.jpg" width="600">

The goggle battery should be more than adequate even for very long flying sessions. The circuit draws only ~??mA while recording.

A `manifest.log` file is written to the sd card during operation for debugging purposes. This can be handy if you run in to troubles with recordings. Lines appended to the top of the file look like this:

```
OPEN,/rec_00000001_0001.24bit.raw,12405
CLOSE,/rec_00000001_0001.24bit.raw,132405
METADATA,/rec_00000001_0001.24bit.raw,PEAK=7842100,OVERRUNS=0,AVG_QUEUE=1.42,132410
```
- OPEN/CLOSE: These lines show the milliseconds when a file starts and stops. Calculating `OPEN - CLOSE` can help diagnose time stretching/drifting within a file. Similarly `CLOSE - OPEN` can pinpoint recording "blind spots" between files.
- PEAK: Tells you the highest dynamic audio value recorded in that file segment ($0$ to $8,388,607$). If this hits $8,300,000$, your audio is actively pushing up against the hard limiter ceiling.
- OVERRUNS: Isolates exactly how many buffer overruns happened inside that file only. If a file displays overruns while previous ones showed zero, you know the SD card experienced a heavy sector reallocation delay during that specific 2-minute flight window.
- AVG_QUEUE: Represents the running average depth of the queue over time. A healthy value sits between 0.5 and 3.0. If this numbers climbs up to 15.0 or 30.0 on a file, it means your SD card is barely scraping by, and its write latency is dangerously close to causing a drop.
- POOL_LEAKS: This part is contains 2 values, X,Y. X represents the actual number of leaked blocks from the audio capture queue, and Y is the number of blocks that have passed through the capture queue.

### Recording Format:
Similar to how a dashcam/bodycam works, this code records audio to 24bit raw PCM files with routines that prevent data corruption when power is suddenly lost while recording. When power is cutoff, only the last seconds of the recording session will be lost. The files are named "rec_X-Y.24bit.raw", where X is the recording session, and Y is the audio segment. Both X and Y increment to allow easy reconstruction of wav files. The "manifest.txt" file keeps a log of segments that have been saved, as well as segments that were lost due to power cutoff. The newest segments will be at the top of the manifest file.

The code makes use of an RMS audio compressor with smooth clipping that has been somewhat optimized for the hardware and intended application. Voices and noises farther away will have a similar volume to the voice of the person wearing the mic. Due to "aggressive AGC parameters", there will be some noise underlying the audio, but it is minimal. This ESP32 version of my original fpvGoggleAudioRecorder project (for RP2040) has a more sophisticated DSP thanks to the faster MCU with a hardware floating point unit. So audio quality is even better than before.

### Post Processing:
<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/raw2wav/raw2wav.png" width="600">

For convenience I created `raw2wav.exe`, a Windows executable that converts raw files recorded with the fpvGoggleAudioRecorderESP32 into easy to use wav files (raw2wav.exe, in raw2wav/dist/). To use the converter, run the `raw2wav.exe`, select the input folder (your SD card) and optionally an output folder. If any of the loaded files do not need conversion, uncheck the box next to them in the file selector area. Click the "Start" button to convert the selected files. The log area near the bottom shows which files are being converted and where the converted files are saved. You can click an output path in the log area to open the output folder in explorer.

Alternatively, you can use an audio editing program like Audacity to "import raw data", 24bit unsigned, with "little endian", and "44000" bit rate, and export a wav file.

### Flashing/Compiling:
To flash your ESP32, you will need VSCode with the PlatformIO extension installed on your PC. Download and unzip this repo on your PC, and open it using the "Open Project" button in PlatformIO home. Connect your esp32 to USB, and click the right arrow button (in the bottom bar on VSCode) to compile+upload the firmware to your ESP32.

### Wiring:
Connect the hardware as shown in the table below. The button is optional for manual recording variants, and is not required for autorecording.

Device  |   Pin |   ESP32 Pin
----|-----|----
MIC |   SEL |   GND
MIC |   LRCL |   4
MIC |   BCLK |   5
MIC |   DOUT |   6
MIC |   GND |   GND
MIC |   3V |   3v3
SD |   GND |   GND
SD |   MISO |   1
SD |   MOSI |   2
SD |   CLK |   3
SD |   CS |   10
SD |    3v3 |   3v3
BUTT |   A/B |   6/GND

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/enclosure/IMG_2936_1.jpg" width="600">

### Printable Enclosure:
*In development, but it will be very similar to the original form factor*
Files for a printable enclosure are provided in this repo. One of each part should be printed. Print the "agcRecorderTopWindscreen.stl" if you want to use a 10mm foam or furry cover for the mic. ABS or PETG is recommended for the top, bottom, and shim. Clear PETG filament should be used for the lens. Some glue is needed for a durable assembly; I used T-7000 glue on all of the joints described below.

To assemble the enclosure, first test fit your button, boards, and mic in the enclosure top/bottom. The mic is oriented with pin holes toward the top, with the mic hole facing out the side with the hole in it. The ESP32 is inserted with buttons facing the top, USB out the side. The button is rotated with pin sides facing the mic/usb sides of the enclosure. The SD card board is placed with the metal card insert part facing toward the top. First insert the lens, button, and mic in the top half, then insert the ESP32 board. Insert the SD board in the bottom half. Place the board shim over the ESP32 processor, and press the bottom half onto the top half. There are tabs for a click fit between the top/bottom halves. When assembled, verify the button protrudes out the top enough for easy operation, the SD card is easy to insert/remove, USB cable is easy to get to, and the case halves close completely. There should be an even 1mm gap around the edge where the top/bottom halves fit together. Also make sure you can fit a paper clip or similar rod through the Boot/Reset button access holes located near the lens.

The enclosure is very compact; 30awg silicone wire or smaller is recommended. Make all wires long enough to reach when assembled, but not so long that assembly can pinch a wire. Leave enough length on the SD card wires so the enclosure halves can easily be assembled/disassembled. Solder wires for the button and add heatshrink; break off the unused pins from the other side of the button. Solder wires to the chip side of the microphone. Solder wires to the back side (opposite of boot/reset buttons) on the ESP32. When wires are all soldered, glue the button into place taking care not to allow glue to squirt where it may interfere with operation. Glue the lens into the top half of the case. Add glue to the rails where the ESP32 and SD card boards will rest, and insert the components into the case halves. Snap case halves together taking care not to pinch any wires. Add the windscreen cover if desired; some glue around the bottom of the windstopper basket where it touches the box will offer a more reliable hold.
