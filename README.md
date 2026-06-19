# fpvGoggleAudioRecorderESP32
## A reliable audio recording device for FPV goggles

<img src="https://github.com/truglodite/fpvgoggleaudiorecorderesp32/blob/main/images/assembly2.jpg" width="600">

A simple and reliable ambient audio recorder project for use on FPV goggles.

### Features:
- Easy plug and forget fully automatic recording
- 24bit-44k audio recording
- Tunable DSP filtering with RMS AGC and clipping limiter
- Efficient and reliable dual core separation of audio and sd write engines
- Corruption resistant raw PCM recording format and sd write methods
- "Start new file" button
- Easy to use Raw to Wav file converter app for PC
- Printable enclosure

### Hardware:
*Prefer no pins installed if using the printable enclosure*
- ESP32 S3 Super Mini: https://www.amazon.com/dp/B0GFDSK5RD
- ICS43434 I2S Microphone: https://www.amazon.com/ICS43434-Microphone-Breakout-Module-Filter/dp/B0FMDGRM8F
- Micro SD Card Board (3.3V): https://www.amazon.com/WWZMDiB-Module-Adapter-Memory-Shield/dp/B0BV8ZQ81F
- 6mm Momentary NO Tactile Button: https://www.amazon.com/Momentary-Tactile-Through-Breadboard-Friendly/dp/B07WF76VHT
- 10mm Microphone Wind Screen: https://www.amazon.com/dp/B07BQ2LZDX

* Note that almost any i2s mic (like INMP441) could be substituted in place of an ICS43434. The ICS mic is recommended as it has high dynamic range and sensitivity for the price point. Also the printed enclosure is designed to fit the above linked ICS mic board (square, not round style).

* I recommend removing the 4 resistors on the SD card board. Leaving the resistors on may cause problems with certain SD cards.

### Operation
Simply plug a USB-C to USB-C cable between the recorder and your goggle charging jack, and fly as usual. 

Audio will start recording when you power on your goggles, and recording stops safely when you power them off. If you need to start a new file while recording, click the button and a new file will be started. This can be useful if for example you wait a while for GPS before launching, hit the button so you have an audio file that starts just before launching.

The code makes use of the onboard LED to indicate status. A blue LED to indicates boot status immediately after powering on. While recording, a green pulsating LED indicates proper operation, and an occasional red LED will indicate the DSP clipping limiter is engaged. After a button press, a blue LED will briefly light to indicate a new file is started. A repeated flashing red led indicates a failure (usually the SD card has come loose or otherwise failed). An orange flashing LED indicates the audio buffer is falling behind (extremely rare; usually caused from poor wiring or bad sd cards).

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/currentDraw.jpg" width="600">

The circuit averages ~50mA current draw while recording. So operating the recorder from a standard goggle battery will not noticeably decrease battery run time.

A `manifest.log` file is written to the sd card during operation for debugging purposes. This can be handy if you run in to troubles with recordings. Below is an example of lines written to the log:

```
OPEN,/rec_00000001_0001.24bit.raw,12405
CLOSE,/rec_00000001_0001.24bit.raw,132405
METADATA,/rec_00000001_0001.24bit.raw,PEAK=7842100,OVERRUNS=0,AVG_QUEUE=1.42,POOL_LEAKS=0,16586
```
- OPEN/CLOSE: This is the millisecond timer when a file starts and stops. Calculating adjacent `OPEN time - CLOSE time` can help diagnose time stretching/drifting within a file. Similarly, adjacent file `CLOSE time - OPEN time` calculation can pinpoint recording "blind spots" between files.
- PEAK: Tells you the highest dynamic audio value recorded in that file segment ($0$ to $8,388,607$). If this hits $8,300,000$, your mic is actively pushing up against the hard limiter ceiling by very loud noises.
- OVERRUNS: Isolates exactly how many buffer overruns happened inside that file only. If this value is non-zero, it is usually caused by your SD card having a prolonged FAT sector reallocation delay.
- AVG_QUEUE: Represents the running average depth of the queue over time. A healthy value sits between 0.5 and 3.0. If this numbers climbs up to 15.0 or 30.0 on a file, it indicates high SD card write latency, which may affect the reliability of recordings.
- POOL_LEAKS: Similar to OVERRUNS, but summed for the whole recording session instead of per file. This part is contains 2 values, X,Y. X represents the actual number of leaked blocks from the audio capture queue, and Y is the total number of blocks that have passed through the capture queue. X should remain zero, and Y will increase recording session duration.

### Recording Format:
Similar to how a dashcam/bodycam works, this code records audio to 24bit raw PCM files with routines that prevent data corruption when power is suddenly lost while recording. When power is cutoff, only the last seconds of the recording session will be lost. The files are named "rec_X-Y.24bit.raw", where X is the recording session, and Y is the audio segment. Both X and Y increment to allow easy reconstruction of wav files. The "manifest.txt" file keeps a log of segments that have been saved, as well as segments that were lost due to power cutoff. The newest segments will be at the top of the manifest file.

The code makes use of an RMS audio compressor with smooth clipping that has been somewhat optimized for the hardware and intended application. Voices and noises farther away will have a similar volume to the voice of the person wearing the mic. Due to "aggressive AGC parameters", there will be some noise underlying the audio, but it is minimal. This ESP32 version of my original fpvGoggleAudioRecorder project (for RP2040) has a more sophisticated DSP thanks to the faster MCU with a hardware floating point unit. So audio quality is even better than before.

### Post Processing:
<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/raw2wav/raw2wav.png" width="600">

For convenience I created `raw2wav.exe`, a Windows executable that converts raw files recorded with the fpvGoggleAudioRecorderESP32 into easy to use wav files (raw2wav.exe, in raw2wav/dist/). To use the converter, run the `raw2wav.exe`, select the input folder (your SD card) and optionally an output folder. If any of the loaded files do not need conversion, uncheck the box next to them in the file selector area. Click the "Start" button to convert the selected files. The log area near the bottom shows which files are being converted and where the converted files are saved. You can click an output path in the log area to open the output folder in explorer.

Alternatively, you can use an audio editing program like Audacity to "import raw data", 24bit unsigned, with "little endian", and "44000" bit rate, and export a wav file.

### Flashing/Compiling:
To flash your ESP32, you will need VSCode with the PlatformIO extension installed on your PC. The details on how to do this are beyond the scope of this repo. Download and unzip this repo to your `Projects` folder on your pc. Open the folder using the "Open Project" button in PlatformIO home. Connect your esp32 to USB, and click the right arrow button (on the bottom bar in VSCode) to compile+upload the firmware to your ESP32.

### Wiring:
Wire the hardware as shown in the table below.

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
BUTT |   A |   7
BUTT |   B  |   GND

### Printable Enclosure:

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/enclosureESPrender.png" width="600">

Files for a printable enclosure are provided in this repo. One of each part should be printed. Print the "agcRecorderTopWindscreen.stl" if you want to use a 10mm foam or furry cover for the mic. ABS or PETG is recommended for the top, bottom, and shim. Clear PETG filament or clear SLA resin should be used for the lens. I recommend using T-7000 adhesive on all of the glue joints described below.

Before wiring and assembling with glue, it's a good idea to test fit your button, boards, mic, lens, and case halves. Make sure you can fit a toothpick or similar insulated rod through the 2 button access holes on the top of the case; cut them larger if needed. Break off the unused pins on one side of your button to prevent shorting. Insert the button into the enclosure top with pins facing the mic hole. Insert the lens into the square hole next to the button. Insert the mic board into the enclosure top, pin holes first, with the mic hole facing outward. Insert the ESP32 board into the enclosure top. Insert the SD card into it's spot on the bottom of the case, with the bare side of the board pointing downward. Snap the top and bottom halves together, making sure they fully engage. There should be a ~0.5mm gap bewteen them when fully seated. If this gap is crooked or too large, adjust fit/reprint as needed. If everything fits well, move on to wire assembly.

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/wiring.jpg" width="600">

Tidy wire routing is important as the enclosure is very compact. Silicone stranded 30awg wire or smaller is recommended. Make the wires long enough to permit assembly without tension, but not so long that assembly may pinch a wire. Solder wires on the button then coat the joints with glue or heatshrink. Solder wires to the chip side of the microphone, adding a short jumper wire between the SEL and GND pins. Solder wires to the back side of the ESP32 board (opposite of boot/reset buttons). The 3 GND wires and 2 3v3 wires are best soldered together on the larger pads of the ESP32; it's easier to strip and twist these wires together before tinning and soldering the bundles on the pads.  It's a good idea to power up the circuit and verify proper operation after soldering is complete (DOA mic boards are more common than you may think). If everything is working properly and recordings sound good, proceed with the rest of the assembly.

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/assembly1.jpg" width="600">

 First add a fillet of glue around the inside edges of the lens you pressed in earlier. Next glue the button into place; use a small amount at first to avoid squirt out that may interfere with operation. Hold the button firmly in position until the glue tacks up. Add glue to the rails where the ESP32 will sit, and press the ESP into the rails. Add a fillet of additional glue on top of the board where it touches the rails, and hold it in place until the glue tacks up. Repeat this procedure to glue the SD card in the bottom half of the case. Insert the mic into the top half of the case, adding a small amount of glue to the rails to prevent movement. Now snap the case halves together taking care not to pinch any wires. Finally, add a thick bead of glue around the bottom of the windscreen basket where it touches the box and install the windscreen, pressing it firmly down into the glue.