# ESP32-S3 Firmware — ESP32 AI Assistant (versão acessível)

Versão simplificada do assistente para **ESP32-S3**. Sem câmera, sem display dedicado — focada em voz e wearables de baixo custo.

## Hardware compatível

| Componente | Detalhe |
|---|---|
| SoC | ESP32-S3 (Wi-Fi nativo, sem co-processador) |
| Microfone | INMP441 (I2S) ou compatível |
| Display | Opcional (não obrigatório) |
| Custo estimado | ~U$20–35 dependendo do kit |

> Qualquer placa ESP32-S3 com microfone I2S é compatível.

## Componentes

```
components/
├── app/    # Lógica do assistente (modo Voz)
├── bsp/    # Board Support Package — áudio I2S, Wi-Fi STA
└── gui/    # Stub / adaptação para display opcional
```

## Configuração

Edite `components/bsp/include/secret.h`:
```c
#define SECRET_WIFI_SSID      "sua_rede"
#define SECRET_WIFI_PASS      "sua_senha"
#define SECRET_OPENAI_API_KEY "sk-..."
```

## Build

```bash
# Ativar ESP-IDF v5.5.1
. activate_esp_idf.ps1          # Windows
. $HOME/esp/esp-idf/export.sh   # Linux/Mac

idf.py -p <PORTA> build flash monitor
```

## Diferenças em relação ao P4

| Feature | ESP32-S3 | ESP32-P4-EYE |
|---|---|---|
| Câmera | ❌ | ✅ 2MP |
| Display LVGL | ❌ | ✅ |
| SD Card | ❌ | ✅ |
| Captive Portal | ❌ | ✅ |
| SNTP | ❌ | ✅ |
| Wi-Fi | Nativo STA | Via C6 (ESP-Hosted) |
| Custo | ~U$20 | ~U$33 |
| Wearable | ✅ compacto | ⚠️ placa de dev |

## LLM

OpenAI GPT-4o via `secret.h`. Para trocar de provedor, altere o endpoint no código e recompile.

---
← [Voltar ao projeto principal](../../README.md)
