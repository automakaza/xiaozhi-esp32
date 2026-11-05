# Automakaza AUDIN (ESP_S3-AUDIN RE1.1)

Placa adaptadora de áudio para módulo ESP32-S3-WROOM-1 **N16R8** (16 MB de flash,
8 MB de PSRAM octal), com codec **ES8388**, amplificador **HT6872**, dois
microfones de eletreto, três botões e conector para display.

## Pinagem

| Função | GPIO |
|---|---|
| I2C do ES8388 (SDA / SCL), endereço 0x10 | 14 / 47 |
| I2S MCLK / BCLK / LRCK | 8 / 3 / 9 |
| I2S saída para o codec (DSDIN) | 46 |
| I2S entrada do microfone (ASDOUT, entrada LINE1) | 10 |
| Botão BOOT do módulo | 0 |
| Botões esquerdo / meio / direito | 12 / 11 / 13 |
| LED verde | 21 |
| Display OLED I2C (SDA / SCL), endereço 0x3C | 5 / 4 |

Observações:

- O HT6872 não tem enable em GPIO: fica sempre ligado. Para não haver chiado,
  as saídas analógicas do DAC são desligadas quando o áudio fica ocioso.
- A saída analógica do ES8388 fica em `AUDIO_ANALOG_OUTPUT_DB` (-15 dB) em vez
  de 0 dB: com o HT6872 a escala original era forte demais (10% já soava alto).
- **Alimente pela USB-C da placa principal.** Pela USB do módulo o ES8388 fica
  sem alimentação (todas as linhas dele ficam presas em nível baixo).
- O display é opcional: sem resposta em 0x3C, a placa segue sem tela.
- No OLED 128x64 a carinha é substituída por **olhos grandes animados**
  (`eyes_oled_display.cc`): piscam, olham em volta e mudam com a emoção
  (feliz, triste, bravo, surpreso, pensando, sono, piscadinha...). O texto da
  conversa rola numa linha embaixo. Ícones de sistema (aviso, Wi-Fi,
  atualização) continuam aparecendo como no original. Para voltar à carinha
  original, `DISPLAY_ROBOT_EYES 0` no `config.h`.

- Ao ouvir a palavra de ativação, a placa toca um bipe e já fica escutando
  (`CONFIG_SEND_WAKE_WORD_DATA=n`): o áudio da palavra não vai para o servidor,
  então a assistente não responde ao próprio nome e escuta o pedido em seguida.

## Botões

- **BOOT ou meio**: inicia/encerra a conversa. Durante a partida, entra na
  configuração de Wi-Fi.
- **Segurar o meio**: entra na configuração de Wi-Fi a qualquer momento
  (rede e, em "avançado", o endereço OTA do servidor).
- **Direita**: volume +10 (segurar = máximo).
- **Esquerda**: volume -10 (segurar = mudo).

## Compilar

Variantes por tipo de display:

- `automakaza-audin-128x64` (SSD1306 128x64)
- `automakaza-audin-128x32` (SSD1306 128x32)
- `automakaza-audin-sh1106` (SH1106 128x64)

```bash
python scripts/build.py automakaza-audin --name automakaza-audin-128x64 --language pt-BR
```
