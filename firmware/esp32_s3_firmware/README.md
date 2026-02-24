# ESP32-S3 Firmware — ESP32 AI Assistant (Wearable & Display)

Versão para **ESP32-S3** focada em fluidez, interações de áudio, conectividade e altíssima eficiência energética com hardware acessível.
Nesta nova versão, a plataforma suporta **Display LCD com Tela Touch**, **SD Card para armazenamento offline**, **Captive Portal**, **SNTP** e gestão inteligente de bateria (**Deep Sleep**).

## Hardware compatível

| Componente | Detalhe |
|---|---|
| SoC | ESP32-S3 (Wi-Fi nativo) + PSRAM (Recomendado 8MB) |
| Microfone | INMP441 ou integrado à placa via I2S |
| Armazenamento | Módulo Micro SD via SPI |
| Display | Painel LCD SPI (ex: ST7789) com Touch I2C (ex: CST816S) |
| Custo estimado | ~U$20 a U$35 dependendo do kit |

> Homologado para placas como **ESP32-S3-Touch-LCD-1.28** e similares.

## Diferenças em relação ao P4

| Feature | ESP32-S3 | ESP32-P4-EYE |
|---|---|---|
| Câmera | ❌ | ✅ 2MP |
| Display LVGL | ✅ ST7789/SPI | ✅ MIPI-DSI |
| SD Card | ✅ SPI | ✅ SDIO |
| Captive Portal | ✅ | ✅ |
| SNTP / RTC | ✅ | ✅ |
| Wi-Fi | Nativo STA | Via C6 (ESP-Hosted) |
| Deep Sleep | ✅ Otimizado (< microamperes) | ❌ |

## Arquitetura de Software e Robustez (Nota 8.8/10)

O firmware do ESP32-S3 foi construído com arquitetura profissional de altíssima resiliência, garantindo que o display, o processamento de áudio e a rede fluam sem gargalos ou *crashes*:

- **Gestão de PSRAM:** Todo o tráfego pesado (buffers de áudio PDM RAW de centenas de KB, encoding Base64 e strings JSON maiores) sofre `offloading` exclusivamente para a memória `PSRAM` externa (utilizando a flag `MALLOC_CAP_SPIRAM`). Isso bloqueia fragmentações e protege a limitada memória RAM interna do S3.
- **Concorrência e Filas (FreeRTOS):** A separação entre o Core da Aplicação e os Drivers (como o LVGL de interface gráfica) é orquestrada por uma *Event Queue* principal. Modificações visuais e lógicas de estado rodam seguras, e o acesso ao barramento SPI (por onde transita tanto o display de vídeo quanto o SD) é chaveado com mutex rígido (`bsp_lvgl_lock()`), impedindo conflitos.
- **Armazenamento Oportunista Offline:** A escrita de áudios brutos em arquivos `.WAV` e o persistir de Logs (`CHAT.TXT`) para o SD Card acontecem via filas secundárias assíncronas. Os *saves* ocorrem exatamente na janela ociosa do processamento (depois da resposta da IA ou em momentos oportunos), não impactando a velocidade do diálogo.
- **Push-to-Talk Perfeito e Resposta Preservada:** Acionamento de gravação amarrado 100% ao estado físico do botão. O envio das requisições detecta interrupções reais (borda de descida para gravar, de subida para enviar a LLM). A última resposta recebida do modelo é mantida travada no display para consulta do usuário até a próxima gravação.
- **Deep Sleep Inteligente:** Ao entrar em ociosidade por muito tempo (após garantir que os uploads e filas estouraram no SDCard e não há processamentos ativos), o firmware executa _deinit_ gracioso no Micro SD e Display, mergulhando no estado de Microamperes. O botão de gravação reativa o chip na mesma hora através de `Wake-On-Pin` Ext1 configurado.

## Build e Gravação

```bash
# Ativar ESP-IDF v5.5.1
. activate_esp_idf.ps1          # Windows
. $HOME/esp/esp-idf/export.sh   # Linux/Mac

# Compilação padrão
idf.py -p <PORTA> build flash monitor
```

---
← [Voltar ao projeto principal](../../README.md)
