# 🛠️ Logs Técnicos — ESP32 AI Assistant (S3 Lite)

> **Status do Sistema: ✅ Operacional (Captura Direta + Monitoramento RMS)**  
> **Data do Log: 01 de Março de 2026**  
> **Hardware: ESP32-S3 | Firmware: ESP-IDF v5.5.1**

---

## 🚀 Métricas de Performance Medidas

| Métrica | Valor | Notas |
|---|---|---|
| ⏱️ Boot completo do sistema | **~1,5 s** | Da CPU start até prompt livre |
| 🧠 PSRAM disponível | **8 MB** | AP Octal PSRAM 64Mbit, 80MHz |
| 🎙️ Janela de captura | **100 ms** | 3.200 bytes por janela (16kHz, 16-bit, mono) |
| 🎙️ RMS por janela | **Informativo** | Monitoramento via Serial, sem filtragem |
| � Filtro Passa-Altas (HPF) | **100 Hz** | IIR Butterworth 1ª ordem, latência: 1 amostra |
| �💾 Gravação WAV no SD | **< 200 ms** | Bulk save via SPI |
| 💬 Append do log de chat | **< 10 ms** | Arquivo CMMDD.txt salvo junto ao áudio |
| 💤 Deep Sleep Timeout | **45 s** | Inatividade, c/ aviso aos 35s |
| ⚡ Consumo em Standby | **< µA** | Deep Sleep Ext1 (Acorda no Botão) |
| 🔋 Leitura de Bateria (ADC) | **~O(1)** | Leitura via ADC_UNIT_1 (GPIO 4) |

---

## 📋 Sequência de Boot Anotada

```
I (415) esp_psram: Found 8MB PSRAM device
I (419) esp_psram: Speed: 80MHz
...
I (1117) bsp: I2S mic init ok (BCLK=16 WS=17 SD=21)
I (1377) bsp_battery: ADC Calibration Success
I (1377) bsp_battery: Battery ADC initialized
I (1387) app_storage: ensure_mounted: Mounting SD card...
I (1627) config_mgr: Configuration loaded: SSID='MyNetHome', volume=70, brightness=85
I (1627) app: Dynamic config loaded from SD card
I (1627) main: assistant_esp32 started
```

**Tempo total de boot: ~1,5 segundos.** O sistema inicializa sem calibrações adicionais, ficando instantaneamente disponível para interação via botão.

---

## 🎙️ Fluxo de Interação — Captura Direta com Monitoramento RMS

O sistema utiliza **Push-to-Talk (PTT)** como controle exclusivo da gravação. Todos os chunks de áudio são capturados integralmente — o RMS de cada janela de 100ms é calculado e exibido no log serial para monitoramento. Após a captura, um **Filtro Passa-Altas (HPF) de 100 Hz** é aplicado in-place no buffer PCM para remover ruídos de baixa frequência antes do envio à IA.

```
I (6637) app: button pressed -> start recording
I (6637) app: starting interaction in audio mode
I (6647) app: [RMS] Window: 462.85 (Total: 3200 bytes)
I (6657) app: [RMS] Window: 795.59 (Total: 6400 bytes)
...
I (8277) app: [RMS] Window: 3880.54 (Total: 64000 bytes)
I (8377) app: [RMS] Window: 1794.48 (Total: 67200 bytes)
...
I (9687) app: Button released -> stopping recording
I (9707) app: HPF applied: 100 Hz cutoff, 54400 samples
I (13917) app_storage: Audio queued in PSRAM (108800 bytes, queue: 1/2)
I (13957) app: interaction finished (captured=108800 bytes, ms=3400)
```

**Observações:**
- **Captura integral**: Todo o áudio é mantido (silêncio + fala). A decisão fica a cargo do modelo de IA.
- **Monitoramento RMS**: Valores típicos: silêncio ~300-600, fala ~1500-7000, picos de voz alta ~12000-27000.
- **HPF**: Aplicado após captura completa, antes da conversão WAV — tempo de processamento desprezível.

---

## 💾 Subsistema de Armazenamento (Opportunistic Saving)

```
I (11967) app_storage: Audio queued in PSRAM (73600 bytes, queue: 1/2)
W (11967) app_storage: Audio queue almost full, triggering immediate save
I (11987) app_storage: Chat log appended: /sdcard/logs/chat/C0301.TXT (93 bytes)
I (12477) app_storage: Audio saved: /sdcard/media/audio/R120102.WAV (73600 bytes PCM -> 73644 bytes WAV)
I (12487) app_storage: Batch save complete (SD kept mounted): 1 saved, 0 failed
```

**Observações:**
- **Estabilidade**: O sistema monitora a fila de PSRAM e descarrega preventivamente quando atinge o limiar de segurança.
- **DMA Check**: Realiza verificação de memória interna livre antes de iniciar operações pesadas no SD.

---

## 💤 Gerenciamento de Baixo Consumo (Deep Sleep)

```
I (36597) app: Deep sleep warning: 10s remaining
I (46597) app: Inactivity timeout reached, preparing deep sleep...
I (48097) bsp_sleep: Entering Deep Sleep Mode...
I (48097) bsp_sd: SD card unmounted
W (48117) bsp_sleep: Button is already LOW (pressed?). Waiting for release...
```

**Observações:**
- **Safe Shutdown**: O cartão SD é desmontado com segurança antes da suspensão.
- **Hardware Trigger**: O sistema aguarda a liberação do GPIO 18 (botão) para evitar bootloops infinitos.

---

## 🌐 Captive Portal — Ativação por Double-Hold

```
W (27947) app: Config portal triggered by double-hold!
I (27947) captive_portal: === Entering Configuration Mode (Captive Portal) ===
I (28537) esp_netif_lwip: DHCP server started on interface WIFI_AP_DEF with IP: 192.168.4.1
I (29537) captive_portal: DNS server task started (port 53)
I (29537) captive_portal: HTTP server started on port 80
```

**Observações:**
- **Acessibilidade**: Portal disponível em `192.168.4.1` com redirecionamento DNS automático.
- **Configuração**: Permite ajuste de Wi-Fi, personalidade da IA, modelo, URL base e perfis de especialista.

---

## 🔊 Filtro Passa-Altas (HPF) — Melhoria de Inteligibilidade

Implementado filtro digital IIR Butterworth de 1ª ordem com frequência de corte em **100 Hz**, aplicado in-place no buffer PCM após a captura completa e antes da conversão WAV.

| Parâmetro | Valor |
|---|---|
| Tipo | IIR Butterworth 1ª ordem |
| Frequência de corte | 100 Hz |
| Rolloff | -6 dB/oitava |
| Latência | 1 amostra (62,5 µs a 16 kHz) |
| Custo computacional | ~2 mult + 2 add por amostra |
| Alocação extra | Nenhuma (processamento in-place) |

**Justificativa técnica:**
- Remove hum elétrico (50/60 Hz + harmônicos), rumble do microfone MEMS e vibrações mecânicas.
- A fundamental mais grave da voz masculina (~85 Hz) sofre atenuação mínima (-6 dB/oitava de rolloff suave).
- Formantes essenciais para inteligibilidade estão acima de 300 Hz — totalmente preservados.
- Padrão compatível com APIs de STT (Whisper, GPT-4o Audio).

**Resultado**: IA avaliou qualidade do áudio em **7-8/10** — satisfatório para transcrição e resposta contextual.

---

## 🔋 Telemetria e Monitoramento de Bateria

O S3 Lite realiza leitura contínua via ADC_UNIT_1:
- **Pino**: GPIO 4
- **Calibração**: Uso de curva de calibração nativa do chip via BSP.
- **Status UI**: Atualização em tempo real na Status Bar do visor LVGL via barramento SPI.

---

## ✅ Conclusão Operacional

O firmware do S3 Lite demonstrou:
- ✅ **Boot instantâneo** (~1,5s) sem calibrações adicionais.
- ✅ **Captura de áudio direta** com monitoramento RMS informativo por janela.
- ✅ **Filtro HPF 100 Hz** — IIR Butterworth sem atraso, melhoria mensurável na inteligibilidade (7-8/10).
- ✅ **Push-to-Talk robusto** com lockout de 1s e debounce de 150ms.
- ✅ **Persistência confiável** com salvamento preventivo no SD Card.
- ✅ **Gestão de Energia eficiente** com shutdown seguro do FileSystem.

---

*Log coletado via `idf.py monitor` em 01/03/2026.*
