# Validacao de performance — 09/10/2026

## Implementacao validada

- Worker unico no Core 0, pilha de 24 KiB; UI exclusivamente no loop principal.
- Duas barras: progresso ponderado da nota atual e concluidas/pendentes do lote.
- Modelos efetivos: `gemini-3.5-transcribe` e `gemini-3.8-flash` com LOW.
- Transcribe inline comprovado em hardware; alternativa Files URI implementada
  e testada em mocks, inclusive para falha de transporte no mesmo modelo.
- Drive com IDs em lote, multipart binario, keep-alive e checksum de resposta.
- Cache de capacidades persistente, cache OAuth/pasta, tentativa direta Wi-Fi,
  NTP assincrono, metadados locais lidos por blocos e TXT/MD ate 128 KiB na PSRAM.

## Verificacoes locais

`tools/check_project.py --cppcheck`: **155 testes aprovados** (56 HTTP/STT,
58 Storage/Drive, 26 Gemini, 15 progresso portatil). Ambos os ambientes
PlatformIO, depuracao e bateria, compilam. O modelo de progresso nao depende
do SDK; o worker/UI sao verificados por build, revisao e dispositivo fisico.

## Backup e dados do usuario

Antes da primeira gravacao foi feito backup completo dos 8 MiB, com tabela
compativel com `partitions.csv`:
`C:/Users/guilh/AppData/Local/Temp/opencode/esp32-eink-before-perf-v3.bin`.
Somente a particao factory em 0x10000 foi atualizada. O usuario confirmou que
apagou intencionalmente as notas antigas para liberar espaco e gravou tres
novas, que foram preservadas e processadas.

## Medicoes reais do worker

Tempos excluem o boot e a conexao Wi-Fi anterior ao inicio do worker. Versoes
intermediarias serviram para localizar gargalos; audios diferentes NAO sao
uma comparacao controlada de velocidade total.

| Nota / versao | Audio | Total | Upload IA | Transcricao | Markdown | Drive | Verificacao | HTTP / TLS |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `193514`, antes da leitura local em blocos | 9,63 s | 60,42 s | 0,68 s | 1,59 s | 22,25 s | 11,23 s | 8,86 s | 12 / 3 |
| `193816`, leitura em blocos | 3,55 s | 32,71 s | 1,08 s | 1,76 s | 9,93 s | 6,14 s | 4,32 s | 11 / 3 |
| `193804`, lote final | 4,42 s | **26,72 s** | 1,46 s | 1,61 s | 3,01 s | 5,74 s | 5,46 s | **11 / 3** |
| `193754`, segunda do mesmo lote | 4,32 s | **62,63 s** | 0,38 s | 1,57 s | **41,61 s** | 5,85 s | 5,89 s | **10 / 0** |

O total inclui Preparing: 15,81 / 9,48 / 9,44 / 7,33 segundos, respectivamente.
As duas notas do lote final tiveram zero quota_wait e zero retry_wait. A
segunda reutilizou todos os sockets TLS. O ultimo estado fisico mostrou
**3 notas, 0 pendencias**, com WAV/TXT/MD confirmados, HTTP 200 e erro vazio.

## Recuperacao e interface

- Teste intermediario encontrou falhas HTTP 0 no envio inline. WAVs e estado
  pendente foram preservados; o lote final retomou ambos sem nova gravacao.
- Recuperacao foi ajustada para Files URI no mesmo modelo apos falha de
  transporte inline, com bloqueio temporario de 60 s e diagnostico de bytes.
  A rota de fallback e coberta pelo host; no lote final o inline funcionou.
- O usuario confirmou as duas barras e os textos legiveis, sem sobreposicao.
- Nota ja concluida fica fora do lote. Pausa ocorre em checkpoint, nunca
  atraves de remocao forcada da task durante escrita de arquivo.

## Precisao do diagnostico

O primeiro diagnostico levantou a possibilidade de timeout EOF de um segundo.
A leitura do SDK mostrou que `File` configura timeout zero: o gargalo corrigido
era leitura por caractere/montagem de String, nao espera EOF. A regressao host
mede chamadas de leitura por blocos e nao simula artificialmente esse timeout.

## Interpretacao dos resultados

Requisicoes Drive de um trio novo caem estruturalmente de 21 para 9, ou menos
com caches; isso e exercitado no host e consistente com os 11/10 requests
totais observados (2 Gemini + 9/8 Drive). O tempo total ainda varia conforme
o servico Gemini: no segundo audio, Markdown representou cerca de dois terços
do ciclo. Nao se promete um limite de tempo que dependa da fila do provedor.

Uma hora continua sendo a etapa futura com microSD/captura/compressao. Nao
houve teste fisico de arquivo de uma hora nem uso de servidor intermediario.

## Encerramento

- Usuario conferiu os arquivos TXT/MD no Drive e confirmou o conteudo correto.
- Com todas concluidas, `sync` retornou imediatamente "Nenhuma nota pendente",
  sem conectar Wi-Fi nem iniciar um job de rede.
- Firmware final `battery` gravado somente em 0x10000, com hash verificado.
- Usuario confirmou funcionamento sem USB e preservacao das tres notas.
