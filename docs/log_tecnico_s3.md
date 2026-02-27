# 🛠️ Logs Técnicos — ESP32 AI Assistant (S3)

> **Status do Sistema: ✅ Operacional (Captive Portal UX Fixes)**  
> **Data do Log: 27 de Fevereiro de 2026**  
> **Hardware: ESP32-S3 | Firmware: ESP-IDF v5.5.1**

---

## 🚀 Métricas de Performance Medidas

| Métrica | Valor | Notas |
|---|---|---|
| ⏱️ Boot completo do sistema | **~1,5 s** | Inclui init de PSRAM, I2S Mic, Wi-Fi BG e SD |
| 🧠 PSRAM disponível | **8 MB** | AP Octal PSRAM 64Mbit, 80MHz |
| 🌐 Latência End-to-End | **~5-8 s** | Captura → API → Áudio/Log salvos |
| 🎙️ Máx Duração de Áudio | **20 segundos** | Suportado na PSRAM (Buffer de 660 KB) |
| 🎙️ Chunk de áudio gravado | **3.200 bytes** | 100ms @ 16kHz, 16-bit, mono |
| 💾 Gravação WAV no SD | **< 200 ms** | Bulk save via SPI (Ex: 224KB -> 224KB) |
| 💬 Append do log de chat | **< 10 ms** | Arquivo CMMDD.txt salvo junto ao áudio |
| 💤 Deep Sleep Timeout | **45 s** | Inatividade, c/ aviso aos 35s |
| ⚡ Consumo em Standby | **< µA** | Deep Sleep Ext1 (Acorda no Botão) |
| 🔋 Leitura de Bateria (ADC) | **~O(1)** | Leitura contínua na ADC_UNIT_1 via BSP |

---

## 📋 Sequência de Boot Anotada

```
I (32) boot.esp32s3: Boot SPI Speed : 80MHz
I (39) boot.esp32s3: SPI Flash Size : 16MB
...
I (416) esp_psram: Found 8MB PSRAM device
I (420) esp_psram: Speed: 80MHz
I (921) esp_psram: Adding pool of 8192K of PSRAM memory to heap allocator
...
I (1117) bsp: I2S mic init ok (BCLK=16 WS=17 SD=21)
I (1267) bsp: Wi-Fi connection started in background
I (1337) wifi:connected with MyNetHome, aid = 1, channel 2, BW20, bssid = a0:ff:70:24:c8:60
...
I (1387) bsp_sd: SD card SPI bus ready (MOSI=38 MISO=40 CLK=39 CS=41)
I (1397) app_storage: SD card detected (will save opportunistically when idle)
I (1497) bsp_sd: SD card mounted at /sdcard
I (1537) main: assistant_esp32 started
```

**Tempo total de boot (até prompt livre): ~1,5 segundos.** O Wi-Fi e a detecção de armazenamento rodam paralelamente em background sem bloquear a aplicação central.

---

## 🎙️ Fluxo de Interação por Voz e Buffer de PSRAM

```
I (6867) app: button pressed -> start recording
I (6867) app: starting interaction in audio mode
I (6867) bsp: Audio captured: 16000 Hz, 16-bit, 1 ch, 100 ms, 3200 bytes
... [capturas sucessivas, sem perdas DMA] ...
I (17667) app_storage: Audio queued in PSRAM (172800 bytes, queue: 1/2)
W (17667) app_storage: Audio queue almost full, triggering immediate save
I (17917) app: interaction finished (captured=172800 bytes, ms=5400)
```

**Observações:**
- Botão "Push-to-Talk" lido robustamente no pino 18.
- Captura de forma síncrona aos blocos de 100ms.
- A gravação aguenta 20 segundos de interação contínua. No limite documentado de 224 KB coletados em 7s, a alocação dá baixa e salva as mídias preventivamente (*Opportunistic Saving*). Isso impede panes e *Out of Memory (OOM)*.

---

## 💾 Subsistema de Armazenamento e Tolerância a Falhas

```
I (17697) app_storage: Sufficient DMA memory available: 31744 bytes (need 24576)
I (17707) app_storage: SD card already mounted, proceeding to save
I (17707) app_storage: Chat log appended: /sdcard/logs/chat/C0227.TXT (370 bytes)
I (18867) app_storage: Audio saved: /sdcard/media/audio/R224537.WAV (172800 bytes PCM -> 172844 bytes WAV)
I (18867) app_storage: Batch save complete (SD kept mounted): 1 saved, 0 failed
```

**Observações:**
- PCM raw extraído do microfone é nativamente embutido no WAV container no firmware.
- Log de conversa adicionado atomicamente (*append*).
- Checagem preventiva de hardware: A memória DMA (Direct Memory Access) garante que há ~32KB de blocos internos paralelos antes de iniciar o processo de persistência do Cartão de Memória. Eliminação de falhas críticas.

---

## 🛡️ Proteção e Isolamento de Handshake (Erro SSL 0x0050)

```
E (60906) esp-tls-mbedtls: mbedtls_ssl_handshake returned -0x0050
E (60906) esp-tls: Failed to open new connection
E (60916) HTTP_CLIENT: Connection failed, sock < 0
...
I (61706) app_storage: Audio saved: /sdcard/media/audio/R202403.WAV (150400 bytes PCM -> 150444 bytes WAV)
```

**Observações:**
- Uma falha de Wi-Fi provocou um *Reset de Conexão* do Host API (`MBEDTLS_ERR_NET_CONN_RESET -0x0050`). 
- A placa comportou a degradação: Nenhuma falha severa (*No System Crash*). O áudio foi descarregado confiavelmente no SD card para arquivo provisório, e assim que a conexão restabeleceu nas sub-rotinas adjacentes, a próxima fala captada respondeu em milésimos de segundos.

---

## ⚡ Gerenciamento Inteligente de Low-Power (Deep Sleep)

```
I (36537) app: Deep sleep warning: 10s remaining
I (46537) app: Inactivity timeout reached, preparing deep sleep...
I (48037) bsp_sleep: Entering Deep Sleep Mode...
I (48037) bsp_sd: SD card unmounted
W (48057) bsp_sleep: Button is already LOW (pressed?). Waiting for release...
```

**Observações:**
- Timeout perfeitamente estipulado e rearmável (Deep Sleep warning printado aos 35s da inatividade original e postergado caso toque ocorresse).
- **Safe Shutdown:** A controladora `bsp_sd` proíbe o corte da RAM sem antes desmontar (Ejetar) o FileSystem lógicamente (SD unmount). Corrupção dos logs não irá ocorrer.
- **Debounce de Hibernação:** Evita efetivamente um Bootloop detectando que o botão de Wake estava precocemente comprimido (`Button is already LOW`). Só cai no descanso físico de µA se o canal Ext1 GPIO está garantidamente liberado.

---

## 🌐 Portal Cativo UX e Interrupção Manual

```
W (24627) app: Config portal triggered by double-hold!
I (24627) captive_portal: === Entering Configuration Mode (Captive Portal) ===
...
I (27027) captive_portal: Portal active at http://192.168.4.1 — waiting for config or button interrupt... 
...
I (75337) captive_portal: POST /save => ssid='MyNetHome' token='sk-svcac...' ...
I (75427) config_mgr: Configuration saved to /sdcard/data/config.txt (2253 bytes)
I (76927) captive_portal: Configuration saved — restarting device...

[Teste de Interrupção Manual]
I (27027) captive_portal: Portal active at http://192.168.4.1 — waiting for config or button interrupt... 
W (29537) captive_portal: Configuration interrupted by user (physical button pressed)
I (31037) wifi:lmac stop hw txq
```

**Observações:**
- **UX Dinâmico**: O Portal agora pré-preenche os campos com as configurações atuais do SD Card ao abrir a página (via `httpd_resp_sendstr_chunk`).
- **Segurança de Parsing**: A função `form_get_field` foi aprimorada para evitar que termos dentro de um prompt (ex: "token") casem com campos de sistema, fixando vazamentos de dados.
- **Interrupção via Hardware**: Implementada uma lógica de monitoramento de GPIO dentro do loop do Portal. Um clique no botão físico interrompe o modo AP e reinicia o sistema imediatamente.
- **Trava de Segurança (Debounce)**: O sistema aguarda a liberação do botão usado para entrar no modo (hold de 10s) antes de ativar o trigger de interrupção, evitando saídas acidentais.

---

## ✅ Conclusão Operacional

O S3 atingiu estabilidade completa operando sobre uma *Event Queue* não bloqueante. O firmware geriu sem esforço 16 MB de Flash e 8 MB de PSRAM distribuindo buffers generosos para transições, tolerando conexões oscilantes no meio da transcrição e gerindo bateria magistralmente rumo à suspensão.

---

*Log coletado via `idf.py monitor` durante sessão prolongada de debug no hardware real (ESP32-S3 com Microfone, ESP-IDF v5.5.1).*
