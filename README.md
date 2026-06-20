# fpvGoggleAudioRecorderESP32
## An easy to use reliable audio recording device for FPV goggles

<img src="https://github.com/truglodite/fpvgoggleaudiorecorderesp32/blob/main/images/assembly3.jpg" width="600">

### Features:
- Easy to use "plug and forget" fully automated recording to sd card
- Powered via goggle USB charging jack
- 24bit-44khz audio pipeline
- DSP filter with RMS AGC and clipping limiter (pre-tuned, user adjustable)
- Robust and efficient dual core separated audio and sd write engines
- Corruption resistant raw PCM recording format and sd write methods
- "Start new segment file" button
- Easy to use Raw->Wav file converter app for PC
- Compact printable enclosure

### Hardware:
*Prefer no pins installed if using the printable enclosure*
- ESP32 S3 Super Mini Board: https://www.amazon.com/dp/B0GFDSK5RD
- ICS43434 I2S Microphone Board: https://www.amazon.com/ICS43434-Microphone-Breakout-Module-Filter/dp/B0FMDGRM8F
- Micro SD Card Board (3.3V): https://www.amazon.com/WWZMDiB-Module-Adapter-Memory-Shield/dp/B0BV8ZQ81F
- 6mm Momentary NO Tactile Button: https://www.amazon.com/Momentary-Tactile-Through-Breadboard-Friendly/dp/B07WF76VHT
- 10mm Microphone Wind Screen: https://www.amazon.com/dp/B07BQ2LZDX

* Note that almost any i2s mic (like INMP441) could be substituted in place of an ICS43434. The ICS mic is recommended as it has high dynamic range and sensitivity for the price point. Also the printed enclosure is designed to fit the above linked ICS mic board (square, not round style).

* I recommend removing the 4 resistors on the SD card board. Leaving the resistors on may cause problems with certain SD cards.

### Operation
Simply plug a USB-C to USB-C cable between the recorder and your goggle charging jack, and fly as usual. This has been tested with DJI goggles 1, 2, and 3. Fully automated recording should also work with any goggles that output 5V only while turned on.

Audio will start recording when you power on your goggles, and recording will stop when you power them off. If you want to start a new segment file while recording, click the button. This can be useful if for example you have to wait a while for GPS satellites before launching. You can hit the button just before launching so less jogging is required for A/V sync in your editor.

The code makes use of the onboard LED to indicate status. A blue LED to indicates boot status immediately after powering on. While recording, a green pulsating LED with an occasional red LED will indicate normal recording and clipping limiter operation. After a button press, a blue LED will briefly light to indicate a the current segment file is closed and a new segment file is started. A repeated flashing red led indicates a failure (usually the SD card has come loose or otherwise failed). An orange flashing LED indicates the audio buffer is falling behind (extremely rare; typically caused by poor wiring or slow/failing sd cards).

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/currentDraw.jpg" width="600">

The circuit averages ~50mA current draw while recording. Operating the recorder with a standard goggle battery will not appreciably decrease run time.

### Recording Format:
Similar to how a dashcam/bodycam works, this code records audio to 24bit 44.1khz raw PCM files with routines that prevent data corruption when power is suddenly lost while recording. When power is cutoff, typically only the last 0-200msec of audio are lost; in a rare worst case event the last ~1.7sec may be lost. The files are named "rec_X-Y.24bit.raw", where X is the recording session, and Y is the audio segment. Both X and Y increment to allow sequential reconstruction recordings. A new segment file is created within a session after every ~2min of recording, or after the new segment file button is pressed. New sessions are created after power cycles.

The code makes use of an DSP filter with HPF, RMS audio compressor, and clipping limiter that have are tuned for the intended "FPV ambient voice and noise" application. Noises farther away will have a similar volume to the voice of the person wearing the mic. The default DSP parameters are a bit aggressive. So there will be some underlying noise during quiet portions of a recording, but it is not very obtrusive. DSP parameters can be adjusted to reduce this noisefloor hiss, but doing so will come at the cost of not clearly hearing distant voices and noises. This ESP32 version of my original fpvGoggleAudioRecorder project (for RP2040) offers major improvements to the audio pipeline and DSP, thanks to the faster ESP32 with a hardware floating point unit. Users can modify DSP parameters to suit their specific needs by editing values of the 7 `#defines` located at the top of the code.

### Post Processing:
<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/raw2wav/raw2wav.png" width="600">

For convenience I created `raw2wav.exe`, a Windows executable that converts batches of raw files recorded with the fpvGoggleAudioRecorderESP32 into easy to use wav files (raw2wav.exe, in raw2wav/dist/). To use the converter, run the `raw2wav.exe`, select the input folder (your SD card) and optionally an output folder. If any of the loaded files do not need conversion, uncheck the box next to them in the file selector area. Click the "Start" button to convert the selected files. The log area near the bottom shows which files are being converted and where the converted files are saved. You can click an output path in the log area to open the output folder in explorer. 

The input file list has drop down boxes for audio resolution. 16bit is there for my older RP2040 based goggle recorder project. The app will automatically select the correct 16bit or 24bit resolution based on the presence or absence of "24bit" in the filename.

Note that raw2wav.exe does not actually modify the audio content; it simply adds wav headers to the raw data and saves the files with a `*.wav` extension. Alternatively, you can do your conversions using a program like Audacity to "import raw data", "24bit unsigned", "little endian", "mono", with "44100hz" bit rate, and export to a wav file.

### Logging:
A `manifest.log` file is saved to the sd card for troubleshooting purposes. Below is an example of lines that may be appended to the log (note that CLOSE and METADATA lines are written only when a segment last longer than ~2min, or when the new segment button is used):

```
OPEN,/rec_00000001_0001.24bit.raw,12405
CLOSE,/rec_00000001_0001.24bit.raw,132405
METADATA,/rec_00000001_0001.24bit.raw,PEAK=7842100,OVERRUNS=0,AVG_QUEUE=1.42,POOL_LEAKS=0,16586
```

- OPEN/CLOSE: The millisecond timer value when a segment file starts or stops. Calculating sequential `OPEN time - CLOSE time` can help diagnose time stretching/drifting within a file (often caused by overheating or an imprecise XTAL oscillator). Similarly, sequential file `CLOSE time - OPEN time` calculation can pinpoint recording "blind spots" between files (typically caused by poor SD card performance).
- PEAK: Tells you the highest 24bit dynamic audio value recorded in that file segment ($0$ to $8,388,607$). During typical use this should be roughly ~7,500,000, indicating proper AGC tuning for the application. If this value is very low (~300,000) or high (constently near or equal to $8,300,000$), it indicates poor utilization of the available dynamic range from the audio engine. Your DSP parameters (or choice of mic) may not be well suited to your application, and tweaking these items may help.
- OVERRUNS: The number of dropped audio blocks. This should remain zero. If this value is non-zero but small, and POOL_LEAKS is zero, it indicates a temporary overload the system recovered from. If this value is huge and POOL_LEAKS is zero, it indicates a slow SD card.
- AVG_QUEUE: Represents the running average depth of the queue over time. A healthy value sits between 0.5 and 3.0. If this numbers climbs up to 15.0 or 30.0 on a file, it indicates high SD card write latency, which may affect the reliability of recordings.
- POOL_LEAKS: The number of audio core crashes. This should remain zero. This part contains 2 values, X,Y. X represents the number of audio core crashes that resulted in a lost audio block, and Y is the total number of blocks that have passed through the capture queue. Similar to OVERRUNS, a non-zero X indicates a slow or defective SD card. POOL_LEAKS can also indicate very high DSP overhead; this has not been observed with the default DSP configuration and range of parameters recommended in the code comments.

### Flashing/Compiling:
I recommend using VSCode with the PlatformIO extension to compile and upload this code to your ESP32 board. Download and unzip this repo to your `Projects` folder. Use the "Open Project" button in PlatformIO home and select the `fpvgoggleaudiorecorderesp32-main` folder. Connect your esp32 to a USB port on your PC, and click the upload button to compile and upload the firmware to your ESP32 (right arrow button on the bottom bar in VSCode). Further details on setting up and using PlatformIO are beyond the scope this repo.

### Wiring:
Connect the hardware together as shown in the table below.

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

<img src="https://github.com/truglodite/fpvGoggleAudioRecorderESP32/blob/main/images/assembly2.jpg" width="600">