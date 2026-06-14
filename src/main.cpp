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

// ======================================================
// DSP & AGC (AUTOMATIC GAIN CONTROL) CONFIGURATION
// ======================================================

#define DSP_HPF_COEFF 0.988f 
#define DSP_ATTACK_COEFF 0.900f 
#define DSP_RELEASE_COEFF 0.015f 
#define DSP_COMP_THRESHOLD_DB -40.0f
#define DSP_COMP_RATIO 5.0f
#define DSP_MAKEUP_GAIN_DB 30.0f
#define DSP_LIMIT_MAX 8300000.0f 

// ======================================================
// SYSTEM CONFIGURATION
// ======================================================

#define SAMPLE_RATE            44100
#define BLOCK_SAMPLES          256
#define QUEUE_BLOCKS           128

#define SEGMENT_MS             120000UL
#define FLUSH_INTERVAL_MS      1000UL

#define SD_MISO                1   
#define SD_MOSI                2   
#define SD_SCK                 3   
#define SD_CS                  10  

#define RGB_LED_PIN            48  
#define BUTTON_PIN             7   

#define I2S_BCLK_PIN           5
#define I2S_WS_PIN             4   
#define I2S_DATA_PIN           6
#define I2S_NUM                I2S_NUM_0

#define CORE1_TIMEOUT_MS       2000

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

volatile uint32_t globalPeak = 0;
volatile uint32_t segmentPeak = 0; // Tracks absolute raw peak within the current segment
volatile uint32_t lastAudioMs = 0;
volatile uint32_t core1Heartbeat = 0;
volatile uint32_t lastFileSplitMs = 0; 

volatile bool fatalState = false;
volatile bool sdWarning = false;
volatile bool bufferWarning = false;
volatile bool diskFull = false;
volatile bool recording = false;
volatile bool rolloverPending = false;
volatile bool limiterActive = false;

Freenove_ESP32_WS2812 statusLED(1, RGB_LED_PIN, 0, TYPE_GRB);

// ======================================================
// OS-MANAGED DUAL-CORE AUDIO RING POOL
// ======================================================

struct AudioBlock {
    int32_t samples[BLOCK_SAMPLES]; 
};

AudioBlock queueBuffer[QUEUE_BLOCKS];
QueueHandle_t freeBlockQueue = NULL;
QueueHandle_t filledBlockQueue = NULL;

// ======================================================
// FILE I/O AND METADATA TELEMETRY FIELDS
// ======================================================

File audioFile;
File manifestFile;
char currentFilename[64];

uint32_t sessionId = 0;
uint32_t segmentId = 0;
uint32_t segmentStartMs = 0;

// Segment-specific tracking states for manifest output
uint32_t segmentStartOverruns = 0;
uint64_t segmentQueueSum = 0;
uint32_t segmentQueueSamples = 0;

struct SystemHealth {
    volatile uint32_t bufferOverruns = 0;
    volatile uint32_t i2sTimeouts    = 0;
    volatile uint32_t sdErrors       = 0;
    volatile uint32_t writes         = 0;
};
SystemHealth sys;

TaskHandle_t AudioCoreTaskHandle = NULL;

struct DSPState { 
    float hp;        
    float prev;      
    float inputEnv;  
    float env;       
};

DSPState dsp = {0.0f, 0.0f, 0.0f, 0.0f};

inline void updatePeak24(int32_t sample) {
    uint32_t a = (sample < 0) ? -(uint32_t)sample : (uint32_t)sample;
    uint32_t peak = globalPeak;
    if (a > peak) globalPeak = a;
    else if (peak > 0) globalPeak = peak - 10; 
    
    // Telemetry capture: Track the true maximum peak reached inside this file boundary
    if (a > segmentPeak) segmentPeak = a;
}

// ======================================================
// LIGHTWEIGHT UI DRIVER
// ======================================================

void updateLED() {
    static uint32_t lastUpdate = 0;
    if ((millis() - lastUpdate) < 50) return;
    lastUpdate = millis();

    static uint32_t redAlertExpiryMs = 0;
    static float rollingMax = 100.0f; 
    
    rollingMax *= 0.995f; 
    if (rollingMax < 100.0f) rollingMax = 100.0f;

    float currentEnv = dsp.env;

    if (currentEnv > rollingMax) {
        rollingMax = currentEnv;
    }

    if (currentEnv > (rollingMax * 0.85f) && currentEnv > 500.0f) {
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
            if (toggle) statusLED.setLedColor(0, 255, 0, 0); 
            else statusLED.setLedColor(0, 0, 0, 0);
            break;
        }
        
        case LED_WARN: {
            static uint8_t slowToggle = 0;
            if (++slowToggle > 10) {
                slowToggle = 0;
                static bool wToggle = false;
                wToggle = !wToggle;
                if (wToggle) statusLED.setLedColor(0, 200, 100, 0); 
                else statusLED.setLedColor(0, 0, 0, 0);
            }
            break;
        }
        
        case LED_RECORDING:
            if ((millis() - lastFileSplitMs) < 600) {
                statusLED.setLedColor(0, 0, 0, 255); 
            } 
            else if (millis() < redAlertExpiryMs) {
                statusLED.setLedColor(0, 255, 0, 0); 
            } 
            else if ((millis() - lastAudioMs) < 250) {
                if (millis() % 1000 < 150) {
                    statusLED.setLedColor(0, 0, 150, 0); 
                } else {
                    statusLED.setLedColor(0, 0, 20, 0);  
                }
            } else {
                statusLED.setLedColor(0, 50, 0, 50);     
            }
            break;
    }
}

void fatalError() {
    fatalState = true;
    if (audioFile) audioFile.close();
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
            if (f.available() >= sizeof(id)) {
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
    snprintf(currentFilename, sizeof(currentFilename), "/rec_%08u_%04u.24bit.raw", sessionId, segmentId);
    audioFile = SD.open(currentFilename, FILE_WRITE);
    if (!audioFile) fatalError();
    segmentStartMs = millis();
    
    // Clear and establish telemetry markers for this distinct boundary block
    segmentPeak = 0;
    segmentStartOverruns = sys.bufferOverruns;
    segmentQueueSum = 0;
    segmentQueueSamples = 0;
    
    lastFileSplitMs = millis();
    logEvent("OPEN", currentFilename);
}

void closeSegment() {
    if (!audioFile) return;
    audioFile.flush();
    audioFile.close();
    logEvent("CLOSE", currentFilename);

    // Process out structural performance data metrics for the completed timeline block
    uint32_t overrunsInSegment = sys.bufferOverruns - segmentStartOverruns;
    float avgQueueDepth = (segmentQueueSamples > 0) ? (float)segmentQueueSum / segmentQueueSamples : 0.0f;

    // Append standard parsing format metadata injection row to tracking logs
    if (manifestFile) {
        manifestFile.print("METADATA,");
        manifestFile.print(currentFilename);
        manifestFile.print(",PEAK=");
        manifestFile.print(segmentPeak);
        manifestFile.print(",OVERRUNS=");
        manifestFile.print(overrunsInSegment);
        manifestFile.print(",AVG_QUEUE=");
        manifestFile.print(avgQueueDepth, 2); 
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
// DEDICATED CORE 1 HIGH-PRIORITY AUDIO INTERRUPT LOOP
// ======================================================

void audioCoreTask(void *pvParameters) {
    size_t bytesRead = 0;
    int32_t rawSamples[BLOCK_SAMPLES]; 

    while (1) {
        core1Heartbeat = millis();

        if (!recording) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        AudioBlock *block = NULL;
        if (xQueueReceive(freeBlockQueue, &block, 0) != pdTRUE) {
            sys.bufferOverruns++;
            bufferWarning = true;
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        esp_err_t err = i2s_read(I2S_NUM, &rawSamples, sizeof(rawSamples), &bytesRead, pdMS_TO_TICKS(5));
        
        if (err != ESP_OK || bytesRead == 0) {
            sys.i2sTimeouts++;
            memset(block->samples, 0, sizeof(block->samples));
        } else {
            size_t totalSamples = bytesRead / sizeof(int32_t);
            
            float maxBlockVal = 0.0f;
            for (size_t i = 0; i < totalSamples; i++) {
                float s = fabsf((float)(rawSamples[i] >> 8));
                if (s > maxBlockVal) maxBlockVal = s;
            }

            if (maxBlockVal > dsp.inputEnv) {
                dsp.inputEnv = (1.0f - DSP_ATTACK_COEFF) * dsp.inputEnv + DSP_ATTACK_COEFF * maxBlockVal; 
            } else {
                dsp.inputEnv = (1.0f - DSP_RELEASE_COEFF) * dsp.inputEnv + DSP_RELEASE_COEFF * maxBlockVal; 
            }

            float envdB = -100.0f;
            if (dsp.inputEnv > 0.0f) {
                envdB = 20.0f * log10f(dsp.inputEnv / 8388608.0f);
            }

            float gainReductionDB = 0.0f;
            if (envdB > DSP_COMP_THRESHOLD_DB) {
                gainReductionDB = (1.0f - (1.0f / DSP_COMP_RATIO)) * (DSP_COMP_THRESHOLD_DB - envdB);
            }

            float totalGainDB = gainReductionDB + DSP_MAKEUP_GAIN_DB;
            float blockLinearGain = powf(10.0f, totalGainDB / 20.0f);

            float maxProcessedBlockVal = 0.0f;
            for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
                if (i < totalSamples) {
                    float x = (float)(rawSamples[i] >> 8);
                    
                    float hp = DSP_HPF_COEFF * dsp.hp + x - dsp.prev;
                    dsp.prev = x;
                    dsp.hp = hp;

                    hp *= blockLinearGain;

                    if (hp > DSP_LIMIT_MAX)  hp = DSP_LIMIT_MAX;
                    if (hp < -DSP_LIMIT_MAX) hp = -DSP_LIMIT_MAX;

                    block->samples[i] = (int32_t)hp;
                    updatePeak24(block->samples[i]);

                    float absHp = fabsf(hp);
                    if (absHp > maxProcessedBlockVal) {
                        maxProcessedBlockVal = absHp;
                    }
                } else {
                    block->samples[i] = 0; 
                }
            }
            
            dsp.env = 0.9f * dsp.env + 0.1f * maxProcessedBlockVal;
        }

        uint32_t currentQueueUsed = queueUsed();
        if (currentQueueUsed > 108) { 
            bufferWarning = true;
        } else if (currentQueueUsed < 12) {
            bufferWarning = false;
        }

        if (xQueueSend(filledBlockQueue, &block, 0) != pdTRUE) {
            xQueueSend(freeBlockQueue, &block, 0); 
            sys.bufferOverruns++;
            bufferWarning = true;
        }

        lastAudioMs = millis();
    }
}

// ======================================================
// DISK WRITER SEQUENCER ENGINE
// ======================================================

bool writeAudioBlock24(AudioBlock &block) {
    uint8_t packedBuffer[BLOCK_SAMPLES * 3];
    uint16_t packIndex = 0;

    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        int32_t sample = block.samples[i];
        packedBuffer[packIndex++] = (uint8_t)(sample & 0xFF);
        packedBuffer[packIndex++] = (uint8_t)((sample >> 8) & 0xFF);
        packedBuffer[packIndex++] = (uint8_t)((sample >> 16) & 0xFF);
    }

    const size_t expectedBytes = sizeof(packedBuffer);
    size_t bytesWritten = 0;

    for (int retry = 0; retry < 3; retry++) {
        bytesWritten = audioFile.write(packedBuffer, expectedBytes);
        if (bytesWritten == expectedBytes) return true;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return false;
}

void writeTaskStep() {
    static uint32_t lastFlush = 0;
    static uint32_t lastQueueSampleTime = 0;

    // --- TIME-SERIES UNBIASED QUEUE TELEMETRY CAPTURE (100 Hz Sample Window) ---
    if (recording && (millis() - lastQueueSampleTime >= 10)) {
        lastQueueSampleTime = millis();
        segmentQueueSum += queueUsed();
        segmentQueueSamples++;
    }

    if (!rolloverPending && (millis() - segmentStartMs >= SEGMENT_MS)) {
        rolloverPending = true;
        portMEMORY_BARRIER();
    }

    AudioBlock *block = NULL;
    if (xQueueReceive(filledBlockQueue, &block, 0) != pdTRUE) {
        if (rolloverPending && queueEmpty()) {
            closeSegment();
            openSegment();
            rolloverPending = false;
            portMEMORY_BARRIER();
        }
        return;
    }

    bool ok = writeAudioBlock24(*block);
    if (!ok) {
        sys.sdErrors++;
        sdWarning = true;
        if (sys.sdErrors > 10) diskFull = true;
    } else {
        sys.writes++;
        sdWarning = false; 
    }

    xQueueSend(freeBlockQueue, &block, 0);

    if ((millis() - lastFlush) > FLUSH_INTERVAL_MS) {
        if (audioFile) audioFile.flush();
        if (manifestFile) manifestFile.flush();
        lastFlush = millis();
    }

    if (rolloverPending && queueEmpty()) {
        closeSegment();
        openSegment();
        rolloverPending = false;
        portMEMORY_BARRIER();
    }
}

// ======================================================
// CORE 0 WATCHDOG AND FAULT MONITORS
// ======================================================

void checkButton() {
    static uint32_t lastDebounceTime = 0;
    static bool lastButtonState = HIGH;
    static bool buttonState = HIGH;

    bool reading = digitalRead(BUTTON_PIN);
    if (reading != lastButtonState) lastDebounceTime = millis();

    if ((millis() - lastDebounceTime) > 50) { 
        if (reading != buttonState) {
            buttonState = reading;
            if (buttonState == LOW) {
                if (recording && !rolloverPending) {
                    Serial.println(">>> MANUAL SPLIT REQUESTED <<<");
                    rolloverPending = true;
                    portMEMORY_BARRIER();
                }
            }
        }
    }
    lastButtonState = reading;
}

void checkCore1Health() {
    if ((millis() - core1Heartbeat) > CORE1_TIMEOUT_MS) {
        Serial.println("FATAL: AUDIO CAPTURE CORE PIPELINE STALLED");
        closeSegment();
        esp_restart(); 
    }
}

void printStats() {
    static uint32_t lastPrint = 0;
    if ((millis() - lastPrint) < 5000) return;
    lastPrint = millis();

    Serial.printf("writes=%u overruns=%u i2s=%u sd=%u queued=%u\n", 
                  sys.writes, sys.bufferOverruns, sys.i2sTimeouts, sys.sdErrors, queueUsed());
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
    if (!SD.begin(SD_CS, SPI, 20000000)) { 
        Serial.println("SD Initialization Failed!");
        fatalError();
    }

    freeBlockQueue = xQueueCreate(QUEUE_BLOCKS, sizeof(AudioBlock*));
    filledBlockQueue = xQueueCreate(QUEUE_BLOCKS, sizeof(AudioBlock*));
    if (freeBlockQueue == NULL || filledBlockQueue == NULL) {
        Serial.println("Queue Allocation Failed!");
        fatalError();
    }

    for (int i = 0; i < QUEUE_BLOCKS; i++) {
        AudioBlock *ptr = &queueBuffer[i];
        xQueueSend(freeBlockQueue, &ptr, 0);
    }

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, 
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 64,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DATA_PIN
    };

    if (i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL) != ESP_OK) fatalError();
    if (i2s_set_pin(I2S_NUM, &pin_config) != ESP_OK) fatalError();

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

    core1Heartbeat = millis();
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