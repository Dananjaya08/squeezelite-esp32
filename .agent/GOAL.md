# Project Goal & Target Specifications

## 1. Project Vision
Build a dedicated USB Audio Class (UAC 1.0) digital audio transport backend for **squeezelite-esp32** on the **ESP32-S3**. This allows an ESP32-S3 development board to act as a standalone Logitech Media Server (LMS) player, streaming bit-perfect digital audio over its native USB OTG port directly to external USB DACs (such as the **TRN TE** USB DAC dongle) instead of traditional I2S DAC chips.

---

## 2. Hardware Environment & Topology

```
+-------------------------------------------------------------+
|                     ESP32-S3 (N16R8)                        |
|  - 16MB Flash, 8MB Octal PSRAM                              |
|  - UART USB-C Port  -----> PC (Flashing & Serial Logs)      |
|  - Native USB OTG   -----> External USB Hub (D+ / D-)       |
+-------------------------------------------------------------+
                               |
                               v
               +-------------------------------+
               |    Self-Powered USB Hub       | <--- 5V External Power
               +-------------------------------+
                               |
                               v
               +-------------------------------+
               |      TRN TE USB DAC Dongle    | (UAC 1.0 Mode)
               +-------------------------------+
                               |
                               v
                     Analog Audio Out (3.5mm)
                     To Headphones / Amplifier
```

### Hardware Facts & Constraints
1. **Dual USB Ports**:
   - Port 1: USB-UART / CP2102 / CH340 / USB-Serial-JTAG used for firmware flashing, monitoring, and interactive CLI console (115200 baud).
   - Port 2: Native USB OTG (GPIO19 = D-, GPIO20 = D+) operating in USB Host mode.
2. **USB Power Management**:
   - DAC is powered through an external self-powered USB hub positioned between the ESP32-S3 and the DAC.
   - VBUS power boost from the ESP32-S3 board is NOT required.
3. **Octal PSRAM Constraints**:
   - Octal PSRAM occupies GPIO35, GPIO36, and GPIO37. These GPIOs must remain untouched.
   - **Critical**: PSRAM is **not DMA-capable**. Any buffer touched by the USB DMA engine (including isochronous OUT transfer buffers and staging buffers) must reside strictly in internal SRAM allocated via `MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`.
4. **Target Audio Formats**:
   - **Phase 1 & Phase 2 Target**: 44.1 kHz / 16-bit Stereo PCM (Red Book CD audio standard).
   - **Future Expansion**: 48 kHz / 24-bit and up to 96 kHz / 24-bit.

---

## 3. Success Criteria
- [x] Phase 0: Complete architecture analysis, verify IDF v4.4.8 compatibility, and evaluate UAC options.
- [x] Phase 1: Standalone USB DAC test with sine wave generation and descriptor logging.
- [x] Phase 2: Integrated Squeezelite `-o USB` backend streaming audio from LMS to TRN TE DAC.
- [ ] Phase 3: Field verification with physical board and DAC dongle.
