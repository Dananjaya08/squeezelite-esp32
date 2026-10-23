# .agent Documentation Directory

Selamat datang di direktori dokumentasi agent untuk proyek **USB Audio Class (UAC 1.0) DAC Backend pada Squeezelite-ESP32 (ESP32-S3)**.

Direktori ini berfungsi sebagai pusat pengetahuan (*knowledge base*), dokumentasi tujuan (*goal*), dan pencatatan kemajuan (*progress*) yang diperoleh selama pengembangan proyek:

1. **[GOAL.md](file:///e:/vibe%20code/esp%20dap/squeezelite-esp32/.agent/GOAL.md)**:
   * Visi dan target proyek.
   * Topologi perangkat keras (ESP32-S3 N16R8, Dual USB-C, self-powered USB hub, DAC TRN TE).
   * Batasan hardware (Octal PSRAM vs DMA internal RAM, pinout GPIO19/20).
   * Kriteria keberhasilan proyek.

2. **[KNOWLEDGE.md](file:///e:/vibe%20code/esp%20dap/squeezelite-esp32/.agent/KNOWLEDGE.md)**:
   * Insight teknis mengenai ESP-IDF v4.4.8 dan `idf-component-manager`.
   * Arsitektur memori ESP32-S3 & aturan alokasi internal DMA RAM.
   * Pipeline audio Squeezelite (`output.write_cb`, `_output_frames`, `outputbuf` vs `obuf`).
   * Detail driver `espressif/usb_host_uac`, isochronous OUT FIFO bias, dan pemulihan hotplug/reconnect.

3. **[PROGRESS.md](file:///e:/vibe%20code/esp%20dap/squeezelite-esp32/.agent/PROGRESS.md)**:
   * Status setiap tahapan (Phase 0, Phase 1, Phase 2, Phase 3).
   * Rincian file dan komponen yang telah dibuat atau dimodifikasi.
   * Panduan langkah selanjutnya untuk kompilasi, flashing, dan pengujian audio riil di perangkat.
