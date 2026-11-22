/*
 * TinyMPGDecoder + TinyGPU (https://github.com/pschatzmann/TinyGPU)
 * display example: decodes a small embedded MPEG-1 clip and streams each
 * decoded picture to an ILI9341 SPI TFT, band by band, looping forever -
 * the MPEG-1 counterpart of TinyH264's own DecodeToDisplay example.
 * Requires the TinyGPU library (installed the same way as TinyMPG itself
 * - see its README) in addition to TinyMPG.
 *
 * Board used for development and testing: a common 3.5" 240x320 ILI9341
 * SPI TFT -
 * https://github.com/pschatzmann/arduino-audio-tools/wiki/Audio-Boards#esp32-arduino-lvgl-wifibluetooth-development-board-24inch-lcd-tft-module
 *
 * Decodes at 176x144 (QCIF) and centers it in the 320x240 panel, rather
 * than decoding at the panel's full resolution or the project's CIF
 * compile-time default - on a plain ESP32 (no PSRAM), CIF's 456192-byte
 * requirement (3 resident picture buffers, see docs/mpg_config.h's own
 * comment) plus this sketch's own display objects exceeds real free heap.
 * 176x144 specifically, not TinyH264's own DecodeToDisplay's 256x192 -
 * this decoder always holds 3 resident picture buffers (curFrame_/
 * refPast_/refFuture_, even for an all-I-frame stream - Phase A/B/C share
 * one decode path, see mpg_config.h), not TinyH264's 2, so the same pixel
 * dimensions cost 50% more heap here (256x192 measured 221184 bytes for
 * all 3 buffers - real hardware testing found this exhausted a plain
 * ESP32's largest free contiguous block by the 2nd or 3rd picture, once
 * TinyGPU's own SPI/display buffers were also resident, even though total
 * free heap looked sufficient - the classic fragmentation trap
 * docs/memory-budget.md already discusses for other buffers). 176x144
 * (114048 bytes for all 3 buffers) was confirmed working on real hardware
 * via examples/DecodeFromProgmem before being adopted here too. See
 * docs/memory-budget.md for the full picture-buffer budget model;
 * decoder.setMaxDimension() in setup() below overrides the project-wide
 * default for this sketch's decoder instance only, at runtime (an
 * alternative to `#define MPG_MAX_WIDTH`/`MPG_MAX_HEIGHT` before the
 * #include). If your board has PSRAM, remove that call and switch the
 * decoder to PSRAMAllocatorESP32<uint8_t> instead (see
 * TinyMPGDecoder<PSRAMAllocatorESP32<uint8_t>> in docs/decoding.md) to
 * decode at full CIF (or a larger custom size than this sketch uses).
 *
 * Avoids a full-screen RGB565 framebuffer (one ~150KB+ contiguous
 * allocation, which a plain ESP32's split internal DRAM often can't
 * satisfy) the same way TinyGPU's own bouncing-ball example does:
 * render/push the screen in small horizontal bands instead of one
 * whole-frame buffer, via TinyMPGDecoder::toRGB565()'s windowed overload
 * (see docs/decoding.md).
 *
 *   TFT_MOSI -> GPIO13   TFT_MISO -> GPIO12   TFT_SCLK -> GPIO14
 *   TFT_CS   -> GPIO15   TFT_DC   -> GPIO2    TFT_RST  -> not connected (-1)
 *   TFT_BL   -> GPIO27 (backlight)
 * (same pinout as TinyGPU's bouncing-ball example and TinyH264's own
 * DecodeToDisplay; adjust for your own wiring.)
 *
 * If the screen stays blank: check the backlight pin and SPI pins first.
 * If it lights up but the image is rotated, mirrored, or has swapped
 * colors: try a different kRotationLandscape value below (0-3, see
 * ILI9341Driver's constructor in TinyGPU) - exact MADCTL bit meaning
 * varies by panel vendor even for the same controller IC.
 *
 * ESP32-only: uses ESP32-Arduino's 4-argument SPI.begin() pin-remap
 * overload, which RP2040-Arduino's SPI class doesn't have.
 *
 * Measures decode time and convert+SPI-push time separately with
 * micros() (excluding the deliberate pacing delay() below), printing a
 * per-frame line plus a rolling avg/min/max summary once per clip loop -
 * same on-device-measurement approach as DecodeFromProgmem's benchmark.
 */

#if !defined(ARDUINO_ARCH_ESP32)
#error \
    "DecodeToDisplay is ESP32-only (see the file header comment above) - it uses ESP32-Arduino's 4-argument SPI.begin() pin-remap API."
#endif

#include <TinyGPU.h>
#include <TinyGPU/DisplayDriverSPI.h>
#include <TinyMPGDecoder.h>

#include "mpg_display_test_clip.h"

using namespace tinympg;
using namespace tinygpu;

// --- display geometry -----------------------------------------------------
// Panel is 320x240; the decoded picture is 176x144 (QCIF), centered via
// kOffsetX/kOffsetY (see the file header comment above for why this size,
// not TinyH264's own 256x192).
constexpr int kPanelWidth = 320;
constexpr int kPanelHeight = 240;
constexpr int kDisplayWidth = 176;
constexpr int kDisplayHeight = 144;
constexpr int kOffsetX = (kPanelWidth - kDisplayWidth) / 2;
constexpr int kOffsetY = (kPanelHeight - kDisplayHeight) / 2;

// --- band buffer geometry ---------------------------------------------------
// kDisplayHeight and kPanelHeight must both be evenly divisible by
// kBandHeight.
constexpr int kBandHeight = 16;
constexpr int kBandCount = kDisplayHeight / kBandHeight;

// The embedded clip was encoded at 25 fps, 10 frames total (see
// mpg_display_test_clip.h) - paces playback to match and marks where one
// full loop of the clip ends, for the periodic timing summary below.
constexpr int kClipFps = 25;
constexpr int kClipFrameCount = 10;

// --- SPI / display pins -----------------------------------------------------
constexpr int8_t kPinMosi = 13;
constexpr int8_t kPinMiso = 12;
constexpr int8_t kPinSclk = 14;
constexpr int8_t kPinCs = 15;
constexpr int8_t kPinDc = 2;
constexpr int8_t kPinRst = -1;
constexpr int8_t kPinBacklight = 27;

TinyMPGDecoder<> decoder;
ILI9341Driver tftDriver(SPI, kPinCs, kPinDc, kPinRst, ILI9341Driver::Rotation::kLandscape);
SurfaceRGB565 band(kDisplayWidth, kBandHeight, FontRGB565);

static uint8_t clipBuf[kDisplayTestClipSize];
static int frameCount = 0;

/*
 * Timing state. decodeCheckpointUs is reset at the very end of onFrame()
 * (after the pacing delay()), so the elapsed time until the *next*
 * onFrame() fires - measured at that call's start - is exactly the time
 * decoder.write() spent decoding that next picture, with neither the
 * delay() nor this frame's own convert+push work counted against it
 * (same reset-after-the-non-decode-work technique DecodeFromProgmem
 * uses, adapted for the extra delay() this sketch has and it doesn't).
 * displayUs is measured directly around the band conversion+SPI-push
 * loop, so it isn't diluted by decode or the pacing delay either.
 */
static uint32_t decodeCheckpointUs = 0;
static uint64_t totalDecodeUs = 0, totalDisplayUs = 0;
static uint32_t minDecodeUs = 0xFFFFFFFF, maxDecodeUs = 0;
static uint32_t minDisplayUs = 0xFFFFFFFF, maxDisplayUs = 0;
static int statsFrameCount = 0;

// One-time full-panel clear so the border around the centered picture
// isn't leftover power-on garbage. Uses its own transient, panel-width
// band, freed right after (unlike `band` above, which stays resident).
static void clearScreen() {
  SurfaceRGB565 clearBand(kPanelWidth, kBandHeight, FontRGB565);
  clearBand.begin();
  clearBand.clear(RGB565(0, 0, 0));
  for (int y = 0; y < kPanelHeight; y += kBandHeight) {
    tftDriver.writeData(clearBand, 0, y);
  }
  clearBand.end();
}

// Also prints the largest free contiguous block, not just the free-heap
// total - see docs/memory-budget.md for why that distinction matters.
static void printFreeHeap(const char* label) {
  Serial.print(label);
  Serial.print(": ");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" bytes free, largest block ");
  Serial.print(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.println(" bytes");
}

// Called once per display-ready picture, from inside decoder.write()
// below. Converts and pushes the picture to the display one band at a
// time.
void onFrame(TinyMPGDecoder<>& d, void* /*userData*/) {
  uint32_t decodeUs = micros() - decodeCheckpointUs;

  uint32_t displayStartUs = micros();
  for (int bandY = 0; bandY < kDisplayHeight; bandY += kBandHeight) {
    // TinyGPU's Surface<RGB565>::pixels() returns tinygpu::RGB565* (a
    // single uint16_t member, no vtable) - reinterpret_cast to the
    // uint16_t* toRGB565() writes, matching this project's established
    // "same binary layout, safe to alias" reasoning for RGB565 buffers.
    size_t written = d.toRGB565(0, bandY, kDisplayWidth, kBandHeight,
                                reinterpret_cast<uint16_t*>(band.pixels()),
                                band.pixelCount());
    if (written == 0) {
      Serial.println("toRGB565() failed (buffer too small?) - stopping.");
      while (true) delay(1000);
    }
    tftDriver.writeData(band, kOffsetX, kOffsetY + bandY);
  }
  uint32_t displayUs = micros() - displayStartUs;

  frameCount++;
  // The very first frame overall also pays for one-time sequence_header
  // parsing and the picture buffers' first allocation - excluded from the
  // running stats, matching DecodeFromProgmem's identical exclusion.
  if (frameCount > 1) {
    totalDecodeUs += decodeUs;
    if (decodeUs < minDecodeUs) minDecodeUs = decodeUs;
    if (decodeUs > maxDecodeUs) maxDecodeUs = decodeUs;
    totalDisplayUs += displayUs;
    if (displayUs < minDisplayUs) minDisplayUs = displayUs;
    if (displayUs > maxDisplayUs) maxDisplayUs = displayUs;
    statsFrameCount++;
  }

  uint32_t totalUs = decodeUs + displayUs;
  Serial.print("Frame ");
  Serial.print(frameCount);
  Serial.print(": decode=");
  Serial.print(decodeUs);
  Serial.print("us, convert+push=");
  Serial.print(displayUs);
  Serial.print("us, total=");
  Serial.print(totalUs);
  Serial.print("us (");
  Serial.print(totalUs > 0 ? 1000000.0f / totalUs : 0.0f, 1);
  Serial.print(" fps unpaced)");
  Serial.print(" - free heap: ");
  Serial.println(ESP.getFreeHeap());

  // Rolling avg/min/max over just the last kClipFrameCount (10) frames -
  // reset after each print below, so this reflects one clip loop at a
  // time rather than diluting with the whole session's history, letting
  // drift over a long-running session (thermal throttling, WiFi/SPI
  // contention, ...) actually show up instead of being averaged away.
  if (statsFrameCount > 0 && frameCount % kClipFrameCount == 0) {
    Serial.print("  avg decode=");
    Serial.print((uint32_t)(totalDecodeUs / statsFrameCount));
    Serial.print("us (min=");
    Serial.print(minDecodeUs);
    Serial.print(", max=");
    Serial.print(maxDecodeUs);
    Serial.print("), avg convert+push=");
    Serial.print((uint32_t)(totalDisplayUs / statsFrameCount));
    Serial.print("us (min=");
    Serial.print(minDisplayUs);
    Serial.print(", max=");
    Serial.print(maxDisplayUs);
    Serial.print(") - ");

    // Unpaced avg fps over this window, i.e. excluding the delay() above -
    // same convention as the per-frame "fps unpaced" figure, just averaged.
    uint64_t avgTotalUs = (totalDecodeUs + totalDisplayUs) / statsFrameCount;
    Serial.print("  avg fps (unpaced, last ");
    Serial.print(statsFrameCount);
    Serial.print(" frames): ");
    Serial.println(avgTotalUs > 0 ? 1000000.0f / avgTotalUs : 0.0f, 1);

    totalDecodeUs = 0;
    totalDisplayUs = 0;
    minDecodeUs = 0xFFFFFFFF;
    maxDecodeUs = 0;
    minDisplayUs = 0xFFFFFFFF;
    maxDisplayUs = 0;
    statsFrameCount = 0;
  }

  delay(1000 / kClipFps);
  decodeCheckpointUs = micros();
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("TinyMPGDecoder DecodeToDisplay example");
  printFreeHeap("Free heap before decode");

  if (kPinBacklight >= 0) {
    pinMode(kPinBacklight, OUTPUT);
    digitalWrite(kPinBacklight, HIGH);
  }

  SPI.begin(kPinSclk, kPinMiso, kPinMosi, kPinCs);
  tftDriver.begin();
  clearScreen();
  band.begin();

  // See the file header comment: sizes buffers for kDisplayWidth x
  // kDisplayHeight instead of the project-wide CIF compile-time default.
  // Unlike TinyH264Decoder, there's no separate setMaxRefFrames() knob to
  // also call here - MPEG-1 always needs exactly its fixed refPast_/
  // refFuture_ pair regardless of GOP structure (see mpg_config.h).
  decoder.setMaxDimension(kDisplayWidth, kDisplayHeight);
  decoder.setCallback(onFrame);

  // Clip is in PROGMEM (flash); copy to RAM since the decoder reads
  // directly from the pointer it's given.
  memcpy_P(clipBuf, kDisplayTestClip, kDisplayTestClipSize);

  printFreeHeap("Free heap after setup");
  decodeCheckpointUs = micros();
}

void loop() {
  /*
   * The embedded clip is short (10 frames, ~0.4s at kClipFps) and
   * entirely I-frames (see mpg_display_test_clip.h) - each write() call
   * decodes and displays the whole clip once, always a clean,
   * self-contained decode. Looping this call for as long as the sketch
   * runs turns it into a continuously-repeating animation.
   */
  decoder.write(clipBuf, kDisplayTestClipSize);
  if (decoder.hasError()) {
    Serial.println(decoder.lastStatus() == TinyMPGDecoder<>::Status::kUnsupported
                        ? "Decoder: unsupported stream feature"
                        : decoder.lastStatus() == TinyMPGDecoder<>::Status::kAllocationError
                              ? "Decoder: allocation failure (out of memory)"
                              : "Decoder: bitstream error");
    while (true) delay(1000);
  }
}
