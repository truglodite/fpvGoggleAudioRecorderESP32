/*
    fpvGoggleAudioRecorderESP32
    Production-oriented FPV goggle audio recorder

    Hardware:
    - ESP32-S3 Super Mini (v0.02 4MB Flash Revision)
    - ICS43434 I2S microphone (True 24-bit Audio Capture)
    - SPI microSD Slot (Configured on 100% safe edge pins)
    - Momentary Segment Button on GPIO 7 (Active Low, Internal Pullup)
    - Onboard WS2812 RGB LED on GPIO 48 (Hardware Driven via Freenove)

    Audio Architecture:
    - Dedicated FreeRTOS Audio Task pinned strictly to Core 1 (APP_CPU)
    - Disk Writing, File Operations, and UI handling locked to Core 0 (PRO_CPU)
    - Format: RAW 24-bit PCM, Signed little-endian (3 Bytes per sample), 44100 Hz mono
    - Hardware Floating-Point Unit (FPU) Optimized DSP chain
*/

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Freenove_WS2812_Lib_for_ESP32.h>
#include <math.h>
#include <atomic>

// ======================================================
// DSP & AGC (AUTOMATIC GAIN CONTROL) CONFIGURATION
// ======================================================

// High-Pass Filter: Prevents wind/breath rumble from falsely triggering the compressor.
// [Recommended Range: 0.980f to 0.995f]
// - 0.995f (~35Hz cut): Good for general audio, but lets wind rumble through.
// - 0.988f (~80Hz cut): Standard vocal mic cutoff. Blocks breath plosives.
// - 0.980f (~140Hz cut): Extreme wind reduction, but makes voices sound "thin" or tinny.
#define DSP_HPF_COEFF 0.988f

// Attack Speed (True Alpha): How fast the system clamps down on sudden loud peaks.
// [Recommended Range: 0.500f (Slower) to 0.990f (Brickwall)]
// - 0.990f: Instant clamping. Catches aggressive transients but can sound slightly clicky.
// - 0.900f: Sweet spot for vocal limiting. Fast enough to prevent clipping without artifacts.
// - 0.500f: Relaxed attack. Lets quick claps or sharp shouts pass through uncompressed.
#define DSP_ATTACK_COEFF 0.900f

// Release Speed (True Alpha): How smoothly volume fades back up to capture background noise.
// [Recommended Range: 0.005f (Very Slow) to 0.050f (Very Fast)]
// - 0.005f (~3-4 sec recovery): Slow, professional broadcast-style level gliding.
// - 0.015f (~1-2 sec recovery): Sweet spot. Background tracks up naturally between sentences. (default)
// - 0.050f (~200ms recovery): Fast pumping. Background noise rushes up between words.
#define DSP_RELEASE_COEFF 0.015f

// Threshold (dB): Audio levels above this are compressed down.
// [Recommended Range: -50.0f to -20.0f]
// - -20.0f: Standard peak limiter. Only affects your voice when you actively shout.
// - -40.0f: Sweet spot for AGC. Squashes normal voice so makeup gain lifts the background. (default)
// - -50.0f: Extreme sensitivity. Will start compressing ambient room noise and floor hiss.
#define DSP_COMP_THRESHOLD_DB -50.0f

// Ratio: How aggressively loud audio is squashed above the threshold.
// [Recommended Range: 2.0f to 20.0f]
// - 2.0f: Gentle leveling. Sounds natural, but loud shouts might still clip.
// - 5.0f: Standard AGC ratio. Keeps vocal tracking relatively flat. (default)
// - 20.0f: Hard brickwall limiting. Absolute volume ceiling; can sound crushed/distorted.
#define DSP_COMP_RATIO 5.0f

// Makeup Gain (dB): The volume boost applied to the entire signal after compression.
// [Recommended Range: 10.0f to 40.0f, default = 30]
// - 10.0f: Subtle boost. Good if you only care about your own voice.
// - 30.0f: Sweet spot. Pulls distant conversations up to sound nearby.
// - 40.0f: Extreme gain. Silent field sounds like a wall of white noise.
#define DSP_MAKEUP_GAIN_DB 41.0f

// Absolute brickwall limit to prevent 24-bit integer overflow/wrap-around.
// [Recommended Range: 8000000.0f to 8388600.0f]
// - Do not exceed 8388607.0f (theoretical 24-bit max).
// - Default 8300000.0f leaves a safety margin against digital wrap-around pops.
#define DSP_LIMIT_MAX 8300000.0f

// ======================================================
// SYSTEM CONFIGURATION
// ======================================================

#define SAMPLE_RATE            44100
#define BLOCK_SAMPLES          256
#define QUEUE_BLOCKS            128
#define QUEUE_WARN_THRESHOLD_HIGH (QUEUE_BLOCKS * 85 / 100)
#define QUEUE_WARN_THRESHOLD_LOW  (QUEUE_BLOCKS * 10 / 100)

#define SEGMENT_MS             120000UL
#define FLUSH_INTERVAL_MS      1000UL   // Must be <= SD_SPACE_CHECK_MS

#define SD_CONSECUTIVE_OK_CLEAR 16

#define SD_FREE_BYTES_MIN  (64ULL * 1024 * 1024)   // ~64 MB safety margin
#define SD_SPACE_CHECK_MS  5000UL

#define SD_MISO            1
#define SD_MOSI            2
#define SD_SCK             3
#define SD_CS              10

#define RGB_LED_PIN        48
#define BUTTON_PIN         7

#define I2S_BCLK_PIN       5
#define I2S_WS_PIN         4
#define I2S_DATA_PIN       6
#define I2S_NUM            I2S_NUM_0

#define CORE1_TIMEOUT_MS   2000

// ======================================================
// RUNTIME SYSTEM STATES
// ======================================================

enum LedMode {
    LED_BOOT,
    LED_RECORDING,
    LED_WARN,
    LED_FATAL
};

volatile LedMode ledMode = LED_BOOT;

std::atomic<uint32_t> globalPeak{0};
std::atomic<uint32_t> lastAudioMs{0};       // FIX 12: atomic — written Core 1, read Core 0
std::atomic<uint32_t> core1Heartbeat{0};    // FIX 11: all writes use explicit .store()
std::atomic<uint32_t> segmentPeak{0};
volatile uint32_t     lastFileSplitMs = 0;  // Core 0 only — volatile sufficient

volatile bool fatalState    = false;
volatile bool sdWarning     = false;
volatile bool bufferWarning = false;
volatile bool diskFull      = false;
std::atomic<bool> recording{false};
std::atomic<bool> rolloverPending{false};
std::atomic<bool> limiterActive{false};

Freenove_ESP32_WS2812 statusLED(1, RGB_LED_PIN, 0, TYPE_GRB);

// ======================================================
// OS-MANAGED DUAL-CORE AUDIO RING POOL
// ======================================================

struct AudioBlock {
    int32_t samples[BLOCK_SAMPLES];
};

AudioBlock    queueBuffer[QUEUE_BLOCKS];
QueueHandle_t freeBlockQueue   = NULL;
QueueHandle_t filledBlockQueue = NULL;

// ======================================================
// FILE I/O AND METADATA TELEMETRY FIELDS
// ======================================================

File audioFile;
File manifestFile;
char currentFilename[64];

uint32_t          sessionId      = 0;
uint32_t          segmentId      = 0;
volatile uint32_t segmentStartMs = 0;  // FIX 15: volatile — read in loop(), written in openSegment()

uint32_t segmentStartOverruns = 0;
std::atomic<uint32_t> segmentQueueSum{0};
std::atomic<uint32_t> segmentQueueSamples{0};

// FIX 10: File-scope so openSegment() can reset it at every segment boundary.
uint32_t consecutiveOkWrites = 0;

struct SystemHealth {
    std::atomic<uint32_t> bufferOverruns{0};
    std::atomic<uint32_t> i2sTimeouts{0};
    std::atomic<uint32_t> sdErrors{0};
    std::atomic<uint32_t> writes{0};
    std::atomic<uint32_t> poolLeaks{0};   // FIX 13: pool slots lost when both queues are full
};
SystemHealth sys;

TaskHandle_t AudioCoreTaskHandle = NULL;

struct DSPState {
    float hp       = 0.0f;
    float prev     = 0.0f;
    float inputEnv = 0.0f;
    std::atomic<float> env{0.0f};
};
DSPState dsp;

// ======================================================
// PEAK TRACKING
// ======================================================

inline void updatePeak24(int32_t sample) {
    uint32_t a = (sample < 0) ? -(uint32_t)sample : (uint32_t)sample;

    // FIX 6: Multiplicative decay — rate-independent of block size / call frequency.
    uint32_t peak = globalPeak.load(std::memory_order_relaxed);
    if (a > peak) {
        globalPeak.store(a, std::memory_order_relaxed);
    } else {
        globalPeak.store((uint32_t)(peak * 0.999f), std::memory_order_relaxed);
    }

    // Lock-free CAS loop — safely races with Core 0's openSegment() reset.
    uint32_t cur = segmentPeak.load(std::memory_order_relaxed);
    while (a > cur) {
        if (segmentPeak.compare_exchange_weak(cur, a, std::memory_order_relaxed)) break;
    }
}

// ======================================================
// LIGHTWEIGHT UI DRIVER
// ======================================================

void updateLED() {
    static uint32_t lastUpdate = 0;
    if ((millis() - lastUpdate) < 50) return;
    lastUpdate = millis();

    static uint32_t redAlertExpiryMs = 0;
    static float    rollingMax       = 100.0f;

    rollingMax *= 0.995f;
    if (rollingMax < 100.0f) rollingMax = 100.0f;

    // FIX 1: Explicit atomic load.
    float currentEnv = dsp.env.load(std::memory_order_relaxed);
    if (currentEnv > rollingMax) rollingMax = currentEnv;

    if (currentEnv > (rollingMax * 0.85f) && currentEnv > 500.0f) {
        redAlertExpiryMs = millis() + 500;
    }
    if (limiterActive.exchange(false, std::memory_order_relaxed)) {
        redAlertExpiryMs = millis() + 500;
    }

    if (fatalState || diskFull) {
        ledMode = LED_FATAL;
    } else if (sdWarning || bufferWarning) {
        ledMode = LED_WARN;
    } else if (recording) {
        ledMode = LED_RECORDING;
    }

    switch (ledMode) {
        case LED_BOOT:
            statusLED.setLedColor(0, 0, 0, 150);
            break;

        case LED_FATAL: {
            static bool toggle = false;
            toggle = !toggle;
            statusLED.setLedColor(0, toggle ? 255 : 0, 0, 0);
            break;
        }

        case LED_WARN: {
            static uint8_t slowToggle = 0;
            if (++slowToggle > 10) {
                slowToggle = 0;
                static bool wToggle = false;
                wToggle = !wToggle;
                statusLED.setLedColor(0, wToggle ? 200 : 0, wToggle ? 100 : 0, 0);
            }
            break;
        }

        case LED_RECORDING: {
            uint32_t now = millis();
            if ((now - lastFileSplitMs) < 600) {
                statusLED.setLedColor(0, 0, 0, 255);     // Blue on file split
            } else if (now < redAlertExpiryMs) {
                statusLED.setLedColor(0, 255, 0, 0);     // Red on high volume or clip
            } else if ((now - lastAudioMs.load(std::memory_order_relaxed)) < 250) {
                // FIX 12: .load() — lastAudioMs is atomic.
                statusLED.setLedColor(0, 0, (now % 1000 < 150) ? 150 : 20, 0);
            } else {
                statusLED.setLedColor(0, 50, 0, 50);     // Purple standby/idle
            }
            break;
        }
    }
}

void fatalError() {
    fatalState = true;
    if (audioFile)    audioFile.close();
    if (manifestFile) manifestFile.close();
    while (1) {
        updateLED();
        delay(50);
    }
}

// ======================================================
// MANIFEST AND FILE STORAGE LOGISTICS
// ======================================================

void openManifest() {
    manifestFile = SD.open("/manifest.log", FILE_APPEND);
    if (!manifestFile) fatalError();
    manifestFile.print("SESSION_START_24BIT,");
    manifestFile.println(millis());
    manifestFile.flush();
}

void logEvent(const char *type, const char *name) {
    if (!manifestFile) return;
    manifestFile.print(type);
    manifestFile.print(",");
    manifestFile.print(name);
    manifestFile.print(",");
    manifestFile.println(millis());
    manifestFile.flush();
}

uint32_t loadSessionCounter() {
    uint32_t id = 0;
    if (SD.exists("/session.dat")) {
        File f = SD.open("/session.dat", FILE_READ);
        if (f) {
            if (f.available() >= (int)sizeof(id)) {
                f.read((uint8_t *)&id, sizeof(id));
            }
            f.close();
        }
    }
    id++;
    File f = SD.open("/session.dat", FILE_WRITE);
    if (f) {
        f.write((uint8_t *)&id, sizeof(id));
        f.flush();
        f.close();
    }
    return id;
}

void openSegment() {
    segmentId++;
    snprintf(currentFilename, sizeof(currentFilename),
             "/rec_%08u_%04u.24bit.raw", sessionId, segmentId);
    audioFile = SD.open(currentFilename, FILE_WRITE);
    if (!audioFile) fatalError();

    uint32_t now      = millis();
    segmentStartMs    = now;
    lastFileSplitMs   = now;

    segmentPeak.store(0, std::memory_order_relaxed);
    segmentStartOverruns = sys.bufferOverruns.load(std::memory_order_relaxed);
    segmentQueueSum.store(0, std::memory_order_relaxed);
    segmentQueueSamples.store(0, std::memory_order_relaxed);

    consecutiveOkWrites = 0;

    logEvent("OPEN", currentFilename);
}

void closeSegment() {
    if (!audioFile) return;
    audioFile.flush();
    audioFile.close();
    logEvent("CLOSE", currentFilename);

    uint32_t overrunsInSegment =
        sys.bufferOverruns.load(std::memory_order_relaxed) - segmentStartOverruns;

    // FIX 7: Explicit .load() on atomics before arithmetic.
    uint32_t qs  = segmentQueueSamples.load(std::memory_order_relaxed);
    uint32_t qsu = segmentQueueSum.load(std::memory_order_relaxed);
    float    avgQueueDepth = (qs > 0) ? (float)qsu / qs : 0.0f;

    if (manifestFile) {
        manifestFile.print("METADATA,");
        manifestFile.print(currentFilename);
        manifestFile.print(",PEAK=");
        manifestFile.print(segmentPeak.load(std::memory_order_relaxed));
        manifestFile.print(",OVERRUNS=");
        manifestFile.print(overrunsInSegment);
        manifestFile.print(",AVG_QUEUE=");
        manifestFile.print(avgQueueDepth, 2);
        manifestFile.print(",POOL_LEAKS=");
        manifestFile.print(sys.poolLeaks.load(std::memory_order_relaxed));
        manifestFile.print(",");
        manifestFile.println(millis());
        manifestFile.flush();
    }
}

bool queueEmpty() {
    return (uxQueueMessagesWaiting(filledBlockQueue) == 0);
}

uint32_t queueUsed() {
    return uxQueueMessagesWaiting(filledBlockQueue);
}

// ======================================================
// DEDICATED CORE 1 HIGH-PRIORITY AUDIO CAPTURE LOOP
// ======================================================

void audioCoreTask(void *pvParameters) {
    size_t  bytesRead = 0;
    int32_t rawSamples[BLOCK_SAMPLES];

    while (1) {
        core1Heartbeat.store(millis(), std::memory_order_relaxed);

        if (!recording) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        AudioBlock *block = NULL;
        if (xQueueReceive(freeBlockQueue, &block, 0) != pdTRUE) {
            sys.bufferOverruns.fetch_add(1, std::memory_order_relaxed);  // FIX 16
            bufferWarning = true;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        esp_err_t err = i2s_read(I2S_NUM, &rawSamples, sizeof(rawSamples),
                                  &bytesRead, pdMS_TO_TICKS(20));

        if (err != ESP_OK || bytesRead == 0) {
            sys.i2sTimeouts.fetch_add(1, std::memory_order_relaxed);  // FIX 16
            memset(block->samples, 0, sizeof(block->samples));
        } else {
            size_t totalSamples = bytesRead / sizeof(int32_t);

            float maxBlockVal = 0.0f;
            for (size_t i = 0; i < totalSamples; i++) {
                float s = fabsf((float)(rawSamples[i] >> 8));
                if (s > maxBlockVal) maxBlockVal = s;
            }

            if (maxBlockVal > dsp.inputEnv) {
                dsp.inputEnv = (1.0f - DSP_ATTACK_COEFF)  * dsp.inputEnv
                             + DSP_ATTACK_COEFF  * maxBlockVal;
            } else {
                dsp.inputEnv = (1.0f - DSP_RELEASE_COEFF) * dsp.inputEnv
                             + DSP_RELEASE_COEFF * maxBlockVal;
            }

            float envdB = -100.0f;
            if (dsp.inputEnv > 0.0f) {
                envdB = 20.0f * log10f(dsp.inputEnv / 8388608.0f);
            }

            float gainReductionDB = 0.0f;
            if (envdB > DSP_COMP_THRESHOLD_DB) {
                gainReductionDB = (1.0f - (1.0f / DSP_COMP_RATIO))
                                * (DSP_COMP_THRESHOLD_DB - envdB);
            }

            float blockLinearGain = powf(10.0f,
                                         (gainReductionDB + DSP_MAKEUP_GAIN_DB) / 20.0f);

            float maxProcessedBlockVal = 0.0f;
            bool  blockClipped         = false;

            for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
                if (i < totalSamples) {
                    float x  = (float)(rawSamples[i] >> 8);
                    float hp = DSP_HPF_COEFF * dsp.hp + x - dsp.prev;
                    dsp.prev = x;
                    dsp.hp   = hp;

                    hp *= blockLinearGain;

                    if (hp > DSP_LIMIT_MAX) {
                        hp = DSP_LIMIT_MAX;
                        blockClipped = true;
                    } else if (hp < -DSP_LIMIT_MAX) {
                        hp = -DSP_LIMIT_MAX;
                        blockClipped = true;
                    }

                    block->samples[i] = (int32_t)hp;
                    updatePeak24(block->samples[i]);

                    float absHp = fabsf(hp);
                    if (absHp > maxProcessedBlockVal) maxProcessedBlockVal = absHp;
                } else {
                    block->samples[i] = 0;
                }
            }

            if (blockClipped) {
                limiterActive.store(true, std::memory_order_relaxed);
            }

            float prevEnv = dsp.env.load(std::memory_order_relaxed);
            dsp.env.store(0.9f * prevEnv + 0.1f * maxProcessedBlockVal,
                          std::memory_order_relaxed);
        }

        uint32_t currentQueueUsed = queueUsed();
        if (rolloverPending.load(std::memory_order_relaxed)) {
        } else if (currentQueueUsed > QUEUE_WARN_THRESHOLD_HIGH) {
            bufferWarning = true;
        } else if (currentQueueUsed < QUEUE_WARN_THRESHOLD_LOW) {
            bufferWarning = false;
        }

        if (xQueueSend(filledBlockQueue, &block, 0) != pdTRUE) {
            if (xQueueSend(freeBlockQueue, &block, 0) != pdTRUE) {
                sys.poolLeaks.fetch_add(1, std::memory_order_relaxed);
            }
            sys.bufferOverruns.fetch_add(1, std::memory_order_relaxed);
            bufferWarning = true;
        }

        segmentQueueSum.fetch_add(currentQueueUsed, std::memory_order_relaxed);
        segmentQueueSamples.fetch_add(1, std::memory_order_relaxed);

        // FIX 12: Explicit .store() — lastAudioMs is atomic.
        lastAudioMs.store(millis(), std::memory_order_relaxed);
    }
}

// ======================================================
// DISK WRITER SEQUENCER ENGINE
// ======================================================

bool writeAudioBlock24(AudioBlock &block) {
    uint8_t  packedBuffer[BLOCK_SAMPLES * 3];
    uint16_t packIndex = 0;

    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        int32_t s = block.samples[i];
        packedBuffer[packIndex++] = (uint8_t)( s        & 0xFF);
        packedBuffer[packIndex++] = (uint8_t)((s >>  8) & 0xFF);
        packedBuffer[packIndex++] = (uint8_t)((s >> 16) & 0xFF);
    }

    const size_t expectedBytes = sizeof(packedBuffer);

    for (int retry = 0; retry < 3; retry++) {
        // FIX 2: Capture position before write so partial writes rewind to the
        //        correct byte — not to the ambiguous post-partial-write position.
        uint32_t writeStart   = audioFile.position();
        size_t   bytesWritten = audioFile.write(packedBuffer, expectedBytes);

        if (bytesWritten == expectedBytes) return true;
        if (bytesWritten > 0)             audioFile.seek(writeStart);

        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

void checkSdFreeSpace() {
    if (diskFull) return;                                    // FIX 14
    static uint32_t lastSpaceCheckMs = 0;
    if ((millis() - lastSpaceCheckMs) < SD_SPACE_CHECK_MS) return;
    lastSpaceCheckMs = millis();

    if ((SD.totalBytes() - SD.usedBytes()) < SD_FREE_BYTES_MIN) {
        diskFull = true;
    }
}

void writeTaskStep() {
    static uint32_t lastFlush = 0;

    if (!rolloverPending && (millis() - segmentStartMs >= SEGMENT_MS)) {
        rolloverPending = true;
    }

    checkSdFreeSpace();

    AudioBlock *block = NULL;
    if (xQueueReceive(filledBlockQueue, &block, 0) != pdTRUE) {
        if (rolloverPending && queueEmpty()) {
            closeSegment();
            openSegment();
            rolloverPending = false;
        }
        return;
    }

    bool ok = writeAudioBlock24(*block);
    if (!ok) {
        sys.sdErrors.fetch_add(1, std::memory_order_relaxed);  // FIX 16
        consecutiveOkWrites = 0;   // FIX 3: Reset hysteresis on any error
        sdWarning = true;

        if (sys.sdErrors.load(std::memory_order_relaxed) > 10) {
            diskFull = true;
        }
    } else {
        sys.writes.fetch_add(1, std::memory_order_relaxed);  // FIX 16
        if (++consecutiveOkWrites >= SD_CONSECUTIVE_OK_CLEAR) {
            sdWarning = false;
        }
    }

    xQueueSend(freeBlockQueue, &block, 0);

    if ((millis() - lastFlush) > FLUSH_INTERVAL_MS) {
        if (audioFile)    audioFile.flush();
        if (manifestFile) manifestFile.flush();
        lastFlush = millis();
    }

    // Second rollover check: fires after the very last block before the deadline drains.
    // NOTE: if writeAudioBlock24 failed on this last block, the rollover still proceeds
    // and the failed block's data is silently dropped. sdWarning is set in that case.
    if (rolloverPending && queueEmpty()) {
        closeSegment();
        openSegment();
        rolloverPending = false;
    }
}

// ======================================================
// CORE 0 WATCHDOG AND FAULT MONITORS
// ======================================================

void checkButton() {
    static uint32_t lastDebounceTime = 0;
    static bool     lastButtonState  = HIGH;
    static bool     buttonState      = HIGH;

    bool reading = digitalRead(BUTTON_PIN);
    if (reading != lastButtonState) lastDebounceTime = millis();

    if ((millis() - lastDebounceTime) > 50) {
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
                if (recording && !rolloverPending && !diskFull) {
                    Serial.println(">>> MANUAL SPLIT REQUESTED <<<");
                    rolloverPending = true;
                }
            }
        }
    }
    lastButtonState = reading;
}

void initAudioHardware() {
    i2s_config_t i2s_config = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num   = I2S_BCLK_PIN,
        .ws_io_num    = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = I2S_DATA_PIN
    };
    if (i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL) != ESP_OK) fatalError();
    if (i2s_set_pin(I2S_NUM, &pin_config) != ESP_OK) fatalError();
}

void flushAllQueues() {
    AudioBlock *block = NULL;
    while (xQueueReceive(filledBlockQueue, &block, 0) == pdTRUE) {
        xQueueSend(freeBlockQueue, &block, 0);
    }
}

void checkCore1Health() {
    if ((millis() - core1Heartbeat.load(std::memory_order_relaxed)) > CORE1_TIMEOUT_MS) {
        Serial.println("!!! AUDIO CORE STALLED: ATTEMPTING RECOVERY !!!");

        vTaskDelete(AudioCoreTaskHandle);
        AudioCoreTaskHandle = NULL;

        i2s_driver_uninstall(I2S_NUM);

        flushAllQueues();  // FIX 5

        initAudioHardware();

        xTaskCreatePinnedToCore(audioCoreTask, "AudioCapture", 8192, NULL,
                                configMAX_PRIORITIES - 1, &AudioCoreTaskHandle, 1);

        core1Heartbeat.store(millis(), std::memory_order_relaxed);  // FIX 11
    }
}

void printStats() {
    static uint32_t lastPrint = 0;
    if ((millis() - lastPrint) < 5000) return;
    lastPrint = millis();

    Serial.printf("writes=%u overruns=%u i2s=%u sd=%u queued=%u leaks=%u\n",
                  sys.writes.load(std::memory_order_relaxed),
                  sys.bufferOverruns.load(std::memory_order_relaxed),
                  sys.i2sTimeouts.load(std::memory_order_relaxed),
                  sys.sdErrors.load(std::memory_order_relaxed),
                  queueUsed(),
                  sys.poolLeaks.load(std::memory_order_relaxed));
}

// ======================================================
// DEVICE INITIALIZATION SETUP
// ======================================================

void setup() {
    Serial.begin(115200);

    statusLED.begin();
    statusLED.setBrightness(40);
    statusLED.setLedColor(0, 0, 0, 150);

    pinMode(BUTTON_PIN, INPUT_PULLUP);

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 40000000)) {
        Serial.println("SD Initialization Failed!");
        fatalError();
    }

    freeBlockQueue   = xQueueCreate(QUEUE_BLOCKS, sizeof(AudioBlock *));
    filledBlockQueue = xQueueCreate(QUEUE_BLOCKS, sizeof(AudioBlock *));
    if (!freeBlockQueue || !filledBlockQueue) {
        Serial.println("Queue Allocation Failed!");
        fatalError();
    }

    for (int i = 0; i < QUEUE_BLOCKS; i++) {
        AudioBlock *ptr = &queueBuffer[i];
        xQueueSend(freeBlockQueue, &ptr, 0);
    }

    initAudioHardware();

    sessionId = loadSessionCounter();
    openManifest();
    openSegment();

    recording = true;

    xTaskCreatePinnedToCore(
        audioCoreTask,
        "AudioCapture",
        8192,
        NULL,
        configMAX_PRIORITIES - 1,
        &AudioCoreTaskHandle,
        1
    );

    core1Heartbeat.store(millis(), std::memory_order_relaxed);  // FIX 11
}

void loop() {
    checkButton();
    writeTaskStep();
    updateLED();
    checkCore1Health();
    if (diskFull) {
        closeSegment();
        fatalError();
    }
    printStats();
    vTaskDelay(pdMS_TO_TICKS(1));
}