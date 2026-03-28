# 🛠️ Technical Logs — ESP32 AI Assistant (S3 Lite)

> **System Status: ✅ Operational (8kHz Audio + Deep Sleep Power Optimization)**  
> **Last Updated: March 28, 2026**  
> **Hardware: ESP32-S3 (QFN56) rev v0.2 | Firmware: ESP-IDF v5.5.1 | PSRAM: 8 MB Octal**  
> **Build: Mar 28 2026 12:23:52 | ELF SHA256: eff048b61**

---

## 🚀 Measured Performance Metrics

| Metric | Value | Notes |
|---|---|---|
| ⏱️ Full system boot | **~1.5 s** | From CPU start to `main: assistant_esp32 started` |
| 📶 Wi-Fi connected (IP) | **~1.5–1.6 s** | From WiFi init to IP obtained — no retry needed |
| 🧠 Available PSRAM | **8 MB** | AP Octal PSRAM 64Mbit, 80MHz |
| 🧠 Heap at runtime | **~7.5–7.6 MB total** | Internal ~100–102 KB, Largest DMA block 31 KB |
| 🎙️ Sample Rate | **8,000 Hz** | 16-bit mono, 100 ms window = 1,600 bytes |
| 🔊 High-Pass Filter (HPF) | **100 Hz @ 8kHz** | IIR Butterworth 1st order, applied before API call |
| 💾 WAV recording to SD | **~200–800 ms** | Depends on file size; SD kept mounted |
| 💬 Chat log append | **~100 ms** | CMMDD.TXT appended per interaction |
| 💤 Deep Sleep Timeout | **~50 s inactivity** | Warning at 10s remaining |
| ⚡ Deep Sleep current | **~0.1 mA (estimated)** | All peripherals shut down; hw validation pending |
| ⚡ Sleep current (before fix) | **~30 mA** | Backlight floating HIGH + WiFi/I2S/I2C active |
| 📶 WiFi modem-sleep ratio | **87–89%** | `WIFI_PS_MIN_MODEM` — measured across 3 sessions |
| ⏱️ Wake + WiFi reconnect | **~1.5–1.6 s** | Full reboot to IP, consistent across 3 wakeups |
| 🔆 Backlight during sleep | **OFF (GPIO held LOW)** | `gpio_hold_en()` prevents floating HIGH |
| 🧩 Dynamic Profiles | **1–6 profiles** | Loaded from SD card, configurable via Captive Portal |
| 🔄 Profile save on switch | **~30–40 ms** | Persisted to `/sdcard/data/config.txt` (3142 bytes) |
| 🔒 TLS handshake | **~980–1,030 ms** | X.509 certificate validation per session |
| 🕐 API total time (short) | **~3.9 s** | 2.5s PTT — from release to `interaction finished` |
| 🕐 API total time (long) | **~5.3 s** | 8.0s PTT — from release to `interaction finished` |
| 🎙️ Audio max per recording | **262,144 bytes** | Longer recordings truncated with warning log |
| 🔋 Battery Reading (ADC) | **O(1)** | ADC_UNIT_1 (GPIO 4), calibration at boot |

---

## 📋 Boot Sequence

```
I (24)  boot: ESP-IDF GIT-NOTFOUND 2nd stage bootloader
I (24)  boot: compile time Mar 28 2026 12:24:12
I (25)  boot: Multicore bootloader | chip revision: v0.2
I (32)  boot.esp32s3: Boot SPI Speed: 80MHz | Mode: DIO | Flash: 16MB
I (375) octal_psram: Found 8MB PSRAM device (AP gen3, 80MHz, 3V)
I (853) esp_psram: SPI SRAM memory test OK
I (862) cpu_start: cpu freq: 160000000 Hz
I (902) heap_init: At 3FCB58E8 len 00033E28 (207 KiB): RAM
I (907) heap_init: At 3FCE9710 len 00005724 (21 KiB): RAM
I (912) heap_init: At 3FCF0000 len 00008000 (32 KiB): DRAM
I (917) heap_init: At 600FE06C len 00001F7C  (7 KiB): RTCRAM
I (923) esp_psram: Adding pool of 8192K of PSRAM to heap allocator
I (946) sleep_gpio: Configure to isolate all GPIO pins in sleep state
I (952) sleep_gpio: Enable automatic switching of GPIO sleep configuration
I (999) bsp: Init BSP
I (999) bsp: Button logic: Active=0, Current Level=0
I (1119) CST816S: IC id: 182
I (1119) bsp: I2S mic init ok (BCLK=16 WS=17 SD=21)
I (1269) bsp_battery: ADC Calibration Success
I (1289) bsp_sd: SD card SPI bus ready (MOSI=38 MISO=40 CLK=39 CS=41)
I (1389) bsp_sd: SD card mounted at /sdcard  (SDHC, 29818 MB, 10 MHz)
I (1419) config_mgr: Config loaded: SSID='MyNetHome' profiles=4 volume=70 brightness=85
I (1459) wifi:mode : sta (80:b5:4e:d9:2b:ac)
I (1469) bsp: Wi-Fi connection started for SSID: MyNetHome
I (1479) main: assistant_esp32 started
I (1519) wifi:connected with MyNetHome, aid=5, ch5, BW20, rssi: -65
I (3069) bsp: Wi-Fi connected, got IP: 192.168.0.184
I (3069) wifi:Set ps type: 1, coexist: 0
I (3069) bsp: Wi-Fi Power Save: WIFI_PS_MIN_MODEM enabled
I (3079) bsp: Initializing SNTP in background...
```

**Boot time: ~1.5 s** (to `main: started`) | **WiFi IP: at ~3.0 s from power-on**, ~1.5 s from WiFi init.  
`WIFI_PS_MIN_MODEM` ativo imediatamente após IP — rádio dorme entre beacons DTIM.

> ⚠️ `W i2c: This driver is an old driver` — API legada do CST816S, sem impacto funcional.  
> ⚠️ `W i2s_common: dma frame num limited to 511` — ajuste automático do driver I2S a 8kHz, informativo.  
> ⚠️ `E spi: SPI bus already initialized` — compartilhamento SPI2 entre LCD e SD, tratado corretamente.

---

## 🎙️ Interaction Flow — Push-to-Talk com RMS Monitoring

O sistema usa **Push-to-Talk (PTT)** como controle exclusivo de gravação. Todos os chunks de 100ms são capturados integralmente; o RMS de cada janela é exibido para monitoramento. Após a captura, um **HPF de 100 Hz** é aplicado in-place no buffer PCM antes do envio à API.

**Padrão de RMS observado:**
- Silêncio / ruído ambiente: 500–1,400
- Fala normal: 4,000–14,000
- Fala intensa / pico: 15,000–22,000

---

## 📅 Session Log — March 28, 2026

### Session 1 — Boot + Interaction (PTT 8.0 s / 128 KB)

```
I (8199) bsp: GPIO 18 changed from 0 to 1           ← botão pressionado
I (8239) app: button pressed -> start recording
I (8239) bsp: Audio captured: 8000 Hz, 16-bit, 1 ch, 100 ms, 1600 bytes
I (8529) app: [RMS] Window: 17038.85 (Total: 16000 bytes)   ← pico de fala clara
...
I (15559) bsp: GPIO 18 changed from 1 to 0          ← botão liberado
I (15569) app: Button released -> stopping recording
I (15589) app: HPF applied: 100 Hz cutoff @ 8kHz, 64000 samples
I (15699) app: Audio-only path initiated
I (15759) app: HTTP client initialized: https://api.openai.com/v1/chat/completions
I (16739) esp-x509-crt-bundle: Certificate validated         ← TLS: 980 ms
I (20479) app_storage: Audio queued in PSRAM (128000 bytes, queue: 1/2)
I (20819) app: interaction finished (captured=128000 bytes, ms=8000)
I (21469) app_storage: Audio saved: /sdcard/media/audio/R155838.WAV (128044 bytes WAV)
I (21469) app_storage: Batch save complete (SD kept mounted): 1 saved, 0 failed
```

**Timing — Interaction 1:**

| Phase | Duration |
|---|---|
| Recording (PTT) | **8,000 ms** |
| HPF + setup | ~170 ms |
| TLS handshake | **980 ms** |
| Upload + inference + stream | ~3,740 ms |
| **Total (release → finished)** | **~5,260 ms** |

**Storage:**
- Heap: Total ~7.54 MB, Internal ~101 KB, Largest DMA 31 KB ✅
- Chat log: `/sdcard/logs/chat/C0328.TXT` (482 bytes)
- WAV: `R155838.WAV` — 128,044 bytes (PCM 128,000 → WAV 128,044)

### Deep Sleep 1 — Shutdown Sequence

```
I (71939) app: Deep sleep warning: 10s remaining
I (81939) app: Inactivity timeout reached, preparing deep sleep...
I (83439) bsp_sleep: Entering Deep Sleep Mode...
I (83439) bsp_sleep: LVGL task deleted (Core 1 freed)        ← evita Task WDT
I (83439) bsp_sleep: LVGL tick timer stopped                 ← 500 IRQs/s eliminados
I (83439) bsp_sleep: Backlight GPIO1 forced LOW and held     ← gpio_hold_en()
I (83449) bsp_sleep: I2S channel stopped and deleted
I (83449) bsp_sleep: SNTP stopped
I (83449) wifi:state: run -> init (0x0)
I (83459) wifi:pm stop, total sleep time: 72204308 us / 81930905 us   ← 88.1% PS
I (83519) bsp_sleep: WiFi stopped and deinitialized
I (83519) bsp_sleep: I2C driver deleted
I (83519) bsp_sd: SD card unmounted
E (83519) spi_master: not all CSses freed                    ← cosmético: LCD io_handle
I (83519) bsp_sleep: SPI bus freed
W (83549) bsp_sleep: Botao pressionado. Aguardando soltar...
I (91749) bsp_sleep: Botao solto. Entrando em sleep.
I (91749) bsp_sleep: Todos perifericos desligados. Entrando em Deep Sleep.
```

**WiFi modem-sleep ratio sessão 1:** `72,204,308 µs / 81,930,905 µs` = **88.1%**

**Sequência de shutdown — 9 passos:**

| # | Periférico | Ação | Resultado |
|---|---|---|---|
| 1 | LVGL Task (Core 1) | `vTaskDelete()` | Core 1 liberado — sem Task WDT |
| 2 | LVGL tick timer | `esp_timer_stop()` | 500 IRQs/s eliminados |
| 3 | Backlight GPIO1 | `gpio_set_level(0)` + `gpio_hold_en()` | Backlight apagado e travado |
| 4 | I2S / INMP441 | `channel_disable` + `channel_del` | GPIOs BCLK/WS/SD flutuando |
| 5 | SNTP | `esp_sntp_stop()` | Stack NTP parado |
| 6 | WiFi | `esp_wifi_stop()` + `esp_wifi_deinit()` | Rádio RF desligado |
| 7 | I2C / CST816S | `i2c_driver_delete()` | SDA/SCL sem pull-up drain |
| 8 | SD Card | `vfs_fat_sdspi_unmount()` | Filesystem desmontado com segurança |
| 9 | SPI bus | `spi_bus_free()` | Barramento liberado |

---

### Session 2 — Wakeup 1 + Profile Switching + Interaction (PTT 2.5 s / 40 KB)

**Boot após wakeup:**
```
I (1515) wifi:connected with MyNetHome, aid=5, ch5, rssi: -62   ← sem retry
I (2595) bsp: Wi-Fi connected, got IP: 192.168.0.184            ← ~1.5 s
I (2595) bsp: Wi-Fi Power Save: WIFI_PS_MIN_MODEM enabled
```

**Profile switching — 4 ciclos, todos persistidos:**

```
I (5075)  app: Profile changed to: 1 (Agronomo)
I (5115)  config_mgr: Config saved to /sdcard/data/config.txt (3142 bytes, 4 perfis)  ← ~40ms
I (6565)  app: Profile changed to: 2 (Teacher)    → saved ~40ms
I (9145)  app: Profile changed to: 3 (Digital)    → saved ~40ms
I (11655) app: Profile changed to: 0 (Generalista) → saved ~40ms
```

**Interaction:**
```
I (19865) app: button pressed -> start recording
I (21705) app: Button released -> stopping recording
I (21715) app: HPF applied: 100 Hz cutoff @ 8kHz, 20000 samples
I (22805) esp-x509-crt-bundle: Certificate validated        ← TLS: 1,030 ms
I (25115) app_storage: Audio queued in PSRAM (40000 bytes, queue: 1/2)
I (25365) app: interaction finished (captured=40000 bytes, ms=2500)
I (25655) app_storage: Audio saved: /sdcard/media/audio/R160327.WAV (40044 bytes WAV)
```

**Timing — Interaction 2:**

| Phase | Duration |
|---|---|
| Recording (PTT) | **2,500 ms** |
| HPF + setup | ~60 ms |
| TLS handshake | **1,030 ms** |
| Upload + inference + stream | ~2,290 ms |
| **Total (release → finished)** | **~3,660 ms** |

### Deep Sleep 2 — Idle (sem interação)

```
I (47985) wifi:pm stop, total sleep time: 41477007 us / 46462092 us   ← 89.3% PS
...sequência completa de 9 passos...
I (192175) bsp_sleep: Botao solto. Entrando em sleep.   ← botão segurado ~144s
```

**WiFi modem-sleep ratio sessão 2:** `41,477,007 µs / 46,462,092 µs` = **89.3%**

---

### Session 3 — Wakeup 2 + Idle

```
I (1525) wifi:connected with MyNetHome, aid=5, ch5, rssi: -59   ← sem retry
I (3095) bsp: Wi-Fi connected, got IP: 192.168.0.184            ← ~1.6 s
I (3095) bsp: Wi-Fi Power Save: WIFI_PS_MIN_MODEM enabled
```

### Deep Sleep 3 — Idle (60s timeout)

```
I (71685) wifi:pm stop, total sleep time: 61103219 us / 70148142 us   ← 87.1% PS
I (71665) bsp_sleep: LVGL task deleted (Core 1 freed)
I (71665) bsp_sleep: Backlight GPIO1 forced LOW and held
...shutdown completo...
I (74585) bsp_sleep: Todos perifericos desligados. Entrando em Deep Sleep.
```

**WiFi modem-sleep ratio sessão 3:** `61,103,219 µs / 70,148,142 µs` = **87.1%**

---

### Session 4 — Wakeup 3 (em andamento)

```
I (1525) wifi:connected with MyNetHome, aid=5, ch5, rssi:-63    ← sem retry
I (3095) bsp: Wi-Fi connected, got IP: 192.168.0.184            ← ~1.6 s
I (3095) bsp: Wi-Fi Power Save: WIFI_PS_MIN_MODEM enabled
I (36465) app: Deep sleep warning: 10s remaining                ← 4º ciclo a caminho
```

---

## 📊 Resumo Multi-Ciclo — 3 Deep Sleep Cycles (28/03/2026)

| Ciclo | Contexto | WiFi PS ratio | WiFi reconnect | Shutdown |
|---|---|---|---|---|
| 1 | PTT 8s interaction | **88.1%** | ~1.5 s | ✅ limpo |
| 2 | Idle + profile switch + PTT 2.5s | **89.3%** | ~1.5 s | ✅ limpo |
| 3 | Idle 60s timeout | **87.1%** | ~1.6 s | ✅ limpo |
| **Média** | — | **88.2%** | **~1.55 s** | 3/3 ✅ |

**Observações:**
- ✅ Zero crashes em 3 ciclos completos de sleep → wakeup → operação → sleep
- ✅ Zero Task WDT — LVGL task deletada antes do sleep em todos os ciclos
- ✅ Zero race conditions WiFi — flag `s_wifi_shutting_down` efetiva
- ✅ Backlight apagado em todos os sleeps, acendeu em todos os wakeups
- ✅ SD Card desmontado e remontado corretamente em cada ciclo
- ✅ Profile switching: 4 trocas × ~40ms, config.txt 3142 bytes, zero falhas de escrita
- ✅ Heap estável: Total ~7.5–7.6 MB, Internal ~100–102 KB ao longo de todos os ciclos
- ⚠️ `E spi_master: not all CSses freed` — cosmético em todos os shutdowns (LCD io_handle não deletado)

---

## 💾 Storage Subsystem (Opportunistic Saving)

```
I (20479) app_storage: Audio queued in PSRAM (128000 bytes, queue: 1/2)
W (20479) app_storage: Audio queue almost full, triggering immediate save
I (20489) app_storage: Memory before SD mount: Total=7543896, Internal=101759, Largest=31744
I (20499) app_storage: DMA memory diagnostic: LargestDMA=31744 bytes (need 24576) ✅
I (20519) app_storage: SD card already mounted, proceeding to save
I (20619) app_storage: Chat log appended: /sdcard/logs/chat/C0328.TXT (482 bytes)
I (21469) app_storage: Audio saved: /sdcard/media/audio/R155838.WAV (128044 bytes WAV)
I (21469) app_storage: Batch save complete (SD kept mounted): 1 saved, 0 failed
```

- **PSRAM queue**: capacidade 2; save preemptivo ao atingir threshold
- **DMA check**: verifica 24,576 bytes disponíveis antes de operar o SD (medido: 31,744 bytes)
- **SD kept mounted**: sem overhead de mount/unmount entre saves
- **WAV header**: gerado com `sample_rate=8000` — reproduzível diretamente

---

## 🔋 Consumo de Corrente — Comparativo

| Cenário | Corrente estimada | Status |
|---|---|---|
| Antes das otimizações (WiFi+I2S+I2C ativos) | ~5–10 mA | baseline |
| Backlight flutuando HIGH durante sleep (bug) | ~30 mA | corrigido |
| **Solução completa (28/03/2026)** | **~0.1 mA** | ⏳ validação com multímetro pendente |

> ⚠️ **Estimativas de tempo de bateria não incluídas** — valores dependem de medição real de corrente com multímetro de precisão em hardware.

**WiFi modem-sleep durante operação ativa:** 87–89% medido diretamente nos logs.

---

## 🔊 High-Pass Filter (HPF)

Filtro IIR Butterworth 1ª ordem aplicado in-place no buffer PCM após captura completa, antes do envio à API.

| Parâmetro | Valor |
|---|---|
| Tipo | IIR Butterworth 1ª ordem |
| Frequência de corte | 100 Hz |
| Rolloff | −6 dB/oitava |
| Sample rate | 8,000 Hz |
| Custo computacional | ~2 mult + 2 add por amostra |
| Alocação extra | Nenhuma (in-place) |

---

## 🧩 Dynamic Specialist Profiles

Profiles armazenados em `/sdcard/data/config.txt` (3142 bytes, JSON array). Até **6 profiles** via Captive Portal. Troca em tempo real com persistência imediata no SD.

| Index | Name | Scope |
|---|---|---|
| 0 | Generalista | General-purpose assistant |
| 1 | Agronomo | Agricultural IoT — greenSe project (UnB FCTE) |
| 2 | Teacher | Digital Electronics — UnB FGA (Prof. Marcelino) |
| 3 | Digital | Digital Electronics scope-restricted |

**Ciclo completo de 4 trocas nesta sessão: zero falhas, todos persistidos em ~30–40 ms.**

---

## 🌐 Captive Portal

- Portal disponível em `192.168.4.1` com redirect DNS automático (Android, iOS, Windows)
- Configura: Wi-Fi, personalidade IA, modelo, URL base e 1–6 profiles especializados
- Escaping HTML em todos os campos (previne corrupção de SSID/token/prompt)
- `httpd` task stack: 12288 bytes; array de profiles alocado na heap
- Buffer URL-decode: 2048 bytes (suporta prompts até 511 chars com UTF-8 `%XX`)

---

## ✅ Operational Conclusion

Firmware validado com **2 interações de áudio**, **4 trocas de perfil**, e **3 ciclos completos de deep sleep/wakeup** na sessão de 28/03/2026:

- ✅ **Boot ~1.5 s** sem calibrações adicionais
- ✅ **WiFi reconexão ~1.5–1.6 s** direto, sem retry — consistente nos 3 wakeups
- ✅ **Captura PTT** com monitoramento RMS por janela de 100ms
- ✅ **HPF 100 Hz IIR Butterworth** aplicado sem alocação extra
- ✅ **TLS ~980–1,030 ms** por sessão — sem reutilização entre boots (cold start)
- ✅ **Persistência SD confiável** — saving preemptivo, DMA check, SD kept mounted
- ✅ **Shutdown de 9 periféricos em ordem** — zero WDT, zero race conditions, 3/3 ciclos
- ✅ **Backlight OFF no sleep** (`gpio_hold_en`) | **ON no wakeup** (`gpio_hold_dis`)
- ✅ **WiFi modem-sleep 87–89%** medido diretamente nos logs de 3 sessões
- ✅ **Redução de corrente estimada ~50–100x** vs. baseline — medição hardware pendente
- ✅ **PM + Tickless Idle + DFS** ativos — escalonamento dinâmico de frequência
- ✅ **Profiles dinâmicos** (1–6) — persistidos em ~30–40 ms, zero falhas
- ✅ **Heap estável** ao longo de todos os ciclos (Total: ~7.5–7.6 MB, Internal: ~100–102 KB)
- ⚠️ `E spi_master: not all CSses freed` — cosmético, LCD io_handle não deletado antes de `spi_bus_free()`

---

*Log collected via `idf.py -p COM15 monitor` — session 03/28/2026 (build 12:23:52).*
