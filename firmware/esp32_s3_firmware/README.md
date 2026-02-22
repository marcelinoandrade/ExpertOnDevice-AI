# assistant_esp32 (ESP-IDF)

Arquitetura inicial em camadas:

- `bsp`: acesso a hardware (display, touch, audio, rede)
- `app`: maquina de estados do assistente
- `gui`: camada de interface (atualmente stub para logs/display)

## Estrutura

- `main/main.c`: bootstrap do sistema
- `components/bsp`: abstracoes de placa
- `components/app`: fluxo da aplicacao
- `components/gui`: interface e atualizacao de tela

## Fluxo atual

1. boot
2. captura de audio fake (stub)
3. transcricao fake
4. resposta fake
5. retorno ao estado pronto

## Proximos passos

1. Substituir `bsp_audio_capture_blocking` por I2S real (INMP441)
2. Ligar `gui` ao LVGL/ST7789/CST816 usando exemplos em `docs/ESP-IDF`
3. Integrar chamadas HTTP para STT e LLM
