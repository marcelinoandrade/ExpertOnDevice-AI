# ESP32-P4-EYE Firmware — ESP32 AI Assistant

Firmware principal do projeto. Roda no **ESP32-P4** com co-processador Wi-Fi **ESP32-C6** via ESP-Hosted (SDIO 4-bit).

## Hardware

| Componente | Detalhe |
|---|---|
| SoC principal | ESP32-P4 @ 360 MHz, 32 MB PSRAM |
| Wi-Fi | ESP32-C6 (co-processador, SDIO 4-bit) |
| Câmera | OV2710 2MP com ISP (AWB, AGC, AE) |
| Microfone | PDM integrado |
| Display | LCD com interface LVGL |
| Armazenamento | SD Card via SDMMC |

## Componentes

```
components/
├── app/          # Lógica principal, dois modos (Voz / Foto+Voz)
│   ├── app.c             # Máquina de estados + loop principal
│   ├── app_storage.c     # Gravação assíncrona (WAV, JPEG, chat log)
│   ├── config_manager.c  # Leitura de settings.json do SD card
│   └── captive_portal.c  # SoftAP + HTTP + DNS para configuração zero-touch
├── bsp/          # Board Support Package — câmera, áudio, Wi-Fi, SD
├── gui/          # Interface LVGL (scroll de resposta, modos)
└── esp32_p4_eye/ # Definições de hardware da placa
```

## Configuração

**Opção 1 — `secret.h` (recompilação necessária):**
```bash
cp components/bsp/include/secret.h.example components/bsp/include/secret.h
# Edite secret.h com SSID, senha e token da API
idf.py -p COM12 build flash monitor
```

**Opção 2 — Captive Portal (sem recompilar):**
1. Segure `ENCODER + BTN1` por **10 segundos**
2. Conecte ao Wi-Fi `Assistant-Config-P4` (sem senha)
3. Acesse `http://192.168.4.1` no browser
4. Preencha SSID, senha e token → dispositivo reinicia

**Opção 3 — `settings.json` no SD card:**
```
/sdcard/data/settings.json
```
Ver template em [`sd_card_template/data/settings.json`](sd_card_template/data/settings.json).

## O que é salvo no SD Card

```
/sdcard/
├── media/images/   → IMG_YYYYMMDD_HHMMSS.jpg
├── media/audio/    → REC_YYYYMMDD_HHMMSS.wav
└── logs/chat/      → CHAT_YYYYMMDD.txt
```

## Build

```bash
# Ativar ESP-IDF v5.5.1
. activate_esp_idf.ps1          # Windows
. $HOME/esp/esp-idf/export.sh   # Linux/Mac

idf.py -p <PORTA> build flash monitor
```

## sdkconfig.defaults relevante

```
CONFIG_IDF_TARGET=esp32p4
CONFIG_SPIRAM=y
CONFIG_FATFS_LFN_HEAP=y
CONFIG_LWIP_MAX_SOCKETS=16
```

## LLM

Hoje: OpenAI GPT-4o via `secret.h`. Para trocar de provedor, altere `APP_AI_ENDPOINT` e `APP_AI_HOST` em `app.c` e recompile.
Roadmap: troca dinâmica via `settings.json` sem recompilação.

---
← [Voltar ao projeto principal](../../README.md)
