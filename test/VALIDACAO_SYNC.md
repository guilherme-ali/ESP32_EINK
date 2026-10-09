# Validação da sincronização — 09/10/2026

## Ambiente

- Waveshare ESP32-S3-PICO-1, flash 8 MB e PSRAM 8 MB, USB COM7.
- LittleFS interno, WAV mono 16-bit/16 kHz; reserva de 256 KiB.
- Firmware de depuração gravado somente na partição factory, offset 0x10000.
- Backup integral da flash preservado no diretório temporário aprovado:
  `C:/Users/guilh/AppData/Local/Temp/opencode/esp32-eink-before-sync-fix.bin`.
- A tabela lida do aparelho corresponde a `partitions.csv`. NVS e LittleFS
  foram preservados durante as gravações de firmware.
- Usuário confirmou projeto Gemini gratuito e autorizou novo pareamento Drive.

## Verificações automatizadas

- `tools/check_project.py --cppcheck`: 51/51 testes de transporte/JSON/STT e
  33/33 testes de armazenamento/Settings/Drive; whitespace e cppcheck aprovados.
- Build PlatformIO aprovado nos ambientes `esp32-s3-devkitc-1` e `battery`.
- Testes simulam transporte, NVS/filesystem e hash determinístico; TLS,
  criptografia MD5 e armazenamento físicos são verificados separadamente.

## Problemas observados e corrigidos no aparelho

1. OAuth Drive retornou `invalid_grant`, HTTP 400. Novo pareamento concluído
   pelo usuário. O firmware passa a invalidar autorização definitivamente
   recusada, permitindo reautorizar na conexão seguinte.
2. LittleFS recusou substituir `.ai` cujo destino permanecia aberto. O handle
   é fechado antes do rename; o mock ganhou a mesma restrição e regressão.
3. Foram observados erro 503, timeout e falhas de envio ao Gemini. O cliente
   captura respostas antecipadas, limita tentativas e preserva a nota para
   retomada. Também tenta Flash inline se Files estiver indisponível.

## Resultados físicos confirmados pela serial

| Nota | WAV | TXT/MD | Drive |
|---|---:|---|---|
| `20261005-165008` | 342060 bytes | Preexistentes, reutilizados | WAV/TXT/MD verificados; HTTP 200 |
| `20261005-165341` | 478252 bytes | Preexistentes, reutilizados | WAV/TXT/MD verificados; HTTP 200 |
| `20261009-101900` | 264236 bytes (~8 s) | Transcribe: 33 bytes; Flash: 73 bytes | WAV/TXT/MD verificados; HTTP 200 |
| `20261009-102928` | 699436 bytes (~22 s) | Transcribe: 232 bytes; Flash: 197 bytes | WAV/TXT/MD verificados; HTTP 200 |

Modelos efetivos das notas novas: `gemini-3.5-transcribe` para transcrição e
`gemini-3.8-flash` para Markdown. Valores efetivos ficam no irmão `.ai`.
A seleção automática permanece ativa; o valor manual anterior (`3.6 Flash`)
continua sendo apenas a configuração do modo manual.

Após repetir a sincronização da nota de 8 segundos, o firmware reutilizou os
textos e confirmou novamente os arquivos remotos; nenhum processamento IA
novo foi observado na serial. Os testes host também verificam reuso de IDs.

Estado final observado: **4 notas, 0 pendências**, todos os estados com
`fullySynced`, `wavUploaded`, `txtUploaded`, `mdUploaded` verdadeiros,
HTTP 200 e erro vazio. Espaço livre: 3014656 bytes; gravável após reserva:
2752512 bytes. A recuperação da nota de 22 segundos reutilizou o WAV salvo
na tentativa anterior, após reinício/gravação do firmware.

## Limites desta validação

Não foi feita uma reunião de uma hora (exige armazenamento maior), teste
físico esgotando toda a flash ou corte deliberado de Wi-Fi no meio de um chunk.
Esses cenários têm cobertura parcial nos testes simulados; o relatório não
os apresenta como testes físicos realizados.

## Encerramento acompanhado pelo usuário

- Usuário confirmou visualmente os arquivos no Google Drive.
- Firmware final `battery` gravado em 0x10000 com verificação de hash aprovada.
- Usuário desconectou o USB e confirmou funcionamento na bateria, navegação
  nos menus e preservação das quatro notas.
- Backup original preservado fora do repositório; nenhuma gravação apagou
  as partições NVS/LittleFS. Não houve commit automático das alterações.
