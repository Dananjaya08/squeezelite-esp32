# Project Progress & Roadmap

## Status Overview

| Phase | Description | Status | Evidence / Artifacts |
|---|---|---|---|
| **Phase 0** | Investigation & Architectural Analysis | **DONE** | [phase0_investigation.md](file:///C:/Users/DANAN/.gemini/antigravity-ide/brain/ac64d50f-77ac-4ae3-aad1-5663607ac2e2/phase0_investigation.md) |
| **Phase 1** | Standalone USB DAC Test Component & CLI | **DONE** | `components/usb_audio_test/`, CLI command `usbtest` |
| **Phase 2** | Squeezelite USB Backend (`-o USB`) | **DONE** | `components/squeezelite/output_usb.c`, `output_embedded.c` |
| **Phase 3** | Hardware Flash & Field Validation | **READY** | Build instructions documented below |

---

## Detailed Task Breakdown

### Phase 0: Investigation (Completed)
- [x] Analyzed repository target IDF version: Verified **ESP-IDF v4.4.8** used across Dockerfile and CI.
- [x] Evaluated UAC host drivers: Selected `espressif/usb_host_uac` component compatible with v4.4+.
- [x] Mapped audio pipeline: Identified `output_embedded.c`, `_output_frames()`, `output.write_cb` mechanism.
- [x] Assessed USB/WiFi/PSRAM coexistence: Confirmed pin separation (GPIO19/20 for USB OTG, internal DMA requirement for USB buffers).
- [x] Formulated concrete implementation plan.

### Phase 1: Standalone USB DAC Test (Completed)
- [x] Created `components/usb_audio_test/include/usb_audio_test.h`.
- [x] Created `components/usb_audio_test/idf_component.yml` with `espressif/usb_host_uac: "^1.5.0"` dependency.
- [x] Created `components/usb_audio_test/CMakeLists.txt` (conditional compilation for ESP32-S3).
- [x] Created `components/usb_audio_test/usb_audio_test.c`:
  - Installs USB Host stack and UAC host driver.
  - Enumerates USB DAC and logs full descriptors and alternate settings.
  - Generates 440Hz sine wave in internal DMA RAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`).
  - Streams 44.1kHz / 16-bit stereo PCM for 10 seconds.
  - Exposes interactive CLI command `usbtest [-d duration] [-f freq]`.
- [x] Registered `usbtest` command in `components/platform_console/app_squeezelite/cmd_squeezelite.c`.

### Phase 2: Squeezelite USB Backend (Completed)
- [x] Created `components/squeezelite/output_usb.c`:
  - Full output backend with `output_init_usb()`, `output_close_usb()`, `output_volume_usb()`.
  - Implemented `output_thread_usb` FreeRTOS task handling locked ring-buffer consumption.
  - Implemented `_usb_write_frames` callback supporting both 16-bit and 32-bit internal sample depths.
  - Implemented automatic hotplug / disconnect / reconnect recovery.
- [x] Updated `components/squeezelite/output_embedded.c`:
  - Added `-o USB` dispatch in `output_init_embedded()`.
  - Added 44.1kHz rate declaration in `test_open()`.
- [x] Updated `components/squeezelite/CMakeLists.txt` and `idf_component.yml`:
  - Added target requirements for `usb` and `espressif__usb_host_uac` on ESP32-S3.
- [x] Updated `components/platform_console/cmd_config.c`:
  - Added `USB` to command-line documentation for `-o`.
- [x] Updated `build-scripts/S3-i2s-sdkconfig.defaults`:
  - Configured `CONFIG_USB_HOST_HW_BUFFER_BIAS_PERIODIC_OUT=y`.

---

## Next Steps (Phase 3 & Beyond)

1. **Physical Device Flashing**:
   - Compile firmware:
     ```bash
     cp build-scripts/S3-i2s-sdkconfig.defaults sdkconfig
     idf.py set-target esp32s3
     idf.py build -DDEPTH=16
     idf.py -p COM_PORT flash monitor
     ```
2. **Phase 1 Validation**:
   - Run `usbtest` in console to verify TRN TE DAC enumeration, hub stability, and sine tone playback.
3. **Phase 2 Validation**:
   - Launch Squeezelite with `-o USB`:
     ```text
     squeezelite -o USB -b 500:2000 -d all=info
     ```
   - Connect to Logitech Media Server and test full track playback.
4. **Phase 4 Expansion (Future)**:
   - Add support for 48kHz / 24-bit and 96kHz / 24-bit tracks.
   - Add USB DAC option into the Squeezelite Web UI audio configuration page.
