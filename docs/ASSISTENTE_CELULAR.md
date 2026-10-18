# Voz, celular BLE, Obsidian e Agenda

## O que está implementado

- O gravador preserva seu clique curto para iniciar/parar notas. Na tela inicial
  e no resultado do assistente, **BOOT segurado** captura uma pergunta; soltar
  finaliza a fala. **PWR cancela** a conversa. Cada fala tem limite explícito de
  30 segundos e cada turno, 180 segundos. No Wi-Fi a resposta toca progressivamente;
  pelo BLE ela e acumulada antes de tocar para evitar cortes por falta de dados.
- Wi-Fi conhecido é a primeira rota. Sem rede/internet, o firmware anuncia BLE
  e tenta o Android associado. O celular usa sua conexão ativa, Wi-Fi ou dados
  móveis; não exige habilitar hotspot.
- O app Android é uma ponte TLS: transcrição, Markdown, OAuth e controle dos
  IDs/hashes do Drive continuam no firmware. Uma conexão interrompida não é
  reaberta no meio de um corpo HTTP. O pipeline existente reconcilia os uploads.
- O gateway Apps Script executa ferramentas de consulta/criação/edição de notas
  no GitHub, eventos/lembretes de Agenda e pesquisa web opcional com fontes.
  Ele é acessível por ambas as rotas: o Wi-Fi funciona sem celular por perto.
- No detalhe de uma nota, **Enviar ao Obsidian** exporta o resumo `.md` ou a
  transcrição `.txt` já gerada para `00_Inbox/Gravador/<id>.md`, com frontmatter.
  O limite do texto de origem é 15.000 bytes; textos maiores são recusados,
  preservando o original. A criação usa identidade SHA-256 estável para retentativas.

## 1. Aplicativo no Galaxy S26

1. Instale `dist/IdeiaRec-debug.apk`. É um APK de desenvolvimento assinado com
   a chave debug local, não uma publicação na Play Store.
2. Abra **IdeiaRec** e conceda **Dispositivos próximos/Bluetooth** e notificações.
3. No ESP32, abra **Menu → Parear celular (BLE)**. Ele habilita o fallback e
   mostra o PIN de seis dígitos. A janela dura dois minutos.
4. No app, use **Parear**, selecione o dispositivo e informe o PIN no diálogo
   do Android. Aguarde a indicação de celular conectado no ESP32.
5. Volte ao menu do ESP32. Nas próximas operações, o anúncio BLE poderá acionar
   o app em segundo plano. Bluetooth e internet precisam estar habilitados no
   telefone. Durante a sessão há uma notificação de serviço conectado.

Não é necessário manter a tela do aplicativo aberta. Detecção/reconexão são
feitas por `CompanionDeviceManager`/`CompanionDeviceService`; encerramento
forçado do aplicativo pelo usuário e políticas do fabricante precisam ser
considerados nos testes do aparelho. Consulte `companion-android/README.md`.

O app aceita TLS na porta 443 apenas para os domínios permitidos. Os serviços
Google usados pelo firmware já constam da lista. Um endpoint STT personalizado
precisa ter seu hostname exato adicionado nas configurações do app.

## 2. Assistente de voz

No **Menu → Wi-Fi → Portal**, configure a seção **Assistente por voz**:

1. Habilite o assistente.
2. Informe um modelo Live permitido e disponível na sua conta:
   `gemini-3.1-flash-live-preview` ou `gemini-3.8-live`.
3. A chave é a mesma do campo STT. A confirmação de projeto gratuito precisa
   estar marcada. Não há descoberta/fallback automático para modelos pagos.
4. Mantenha o fuso correto, por exemplo `America/Sao_Paulo`.
5. Salve e reinicie. Na tela inicial, segure BOOT, aguarde **Ouvindo**, fale e
   solte. Clique curto continua pertencendo ao gravador.

O campo de modelo começa vazio; não presume disponibilidade de um preview na
conta. A lista local limita candidatos, mas não consulta faturamento. Cota,
permissões e disponibilidade são determinadas pela API do provedor. Um erro
não habilita faturamento nem seleciona outro serviço pago.

## 3. Obsidian/GitHub e Google Agenda

Prepare o gateway seguindo `gateway/README.md`. O pacote inclui os dois arquivos
gerados necessários para o editor Apps Script:

- `dist/gateway-Code.gs` → conteúdo de `Code.gs`;
- `dist/gateway-appsscript.json` → manifesto `appsscript.json`.

Configure as propriedades do script, autorize a conta proprietária e publique
como Web App. O GitHub PAT fica no serviço, restrito ao repositório do vault.
Copie a URL HTTPS terminada em `/exec` e o token compartilhado (32–128 caracteres)
para o portal do ESP32. O token deve ser o mesmo `GATEWAY_TOKEN` do serviço.

Exemplos de comandos:

- “Procure minha lista de compras e adicione café.”
- “Leia a nota sobre a reunião do projeto.”
- “Crie uma nota com esta ideia na minha caixa de entrada.”
- “Marque uma reunião amanhã às 14h, por meia hora, com aviso 15 minutos antes.”

Ferramentas recebem datas absolutas com fuso. O firmware fornece seu relógio
ao modelo; quando inválido, a instrução pede data absoluta. Um lembrete de
Agenda é um evento com notificação, não uma tarefa do Google Tasks.

As edições são commits no GitHub. O Obsidian recebe a versão no próximo pull do
plugin Git. Arquivos ainda não enviados ao GitHub não entram nas consultas.
Busca é literal, com orçamento explícito e resultado `truncated`; não é um
índice semântico completo de todo o vault. Edite a lista existente por caminho
e seção, preservando YAML e wikilinks.

## 4. Pesquisa na internet

O gateway oferece `web_search` usando Gemini REST + `google_search`. Para
habilitar, configure no Apps Script:

| Propriedade | Valor |
|---|---|
| `GEMINI_SEARCH_API_KEY` | Chave do projeto sem faturamento |
| `GEMINI_SEARCH_MODEL` | `gemini-2.5-flash` ou `gemini-2.5-flash-lite`, disponível na conta |
| `SEARCH_FREE_CONFIRMED` | `true` |

Voz e pesquisa usam endpoints/cotas distintos. A confirmação é do operador,
não uma detecção de plano de cobrança. Pesquisa sem fontes e metadados de
grounding é recusada; erro/cota não vira resposta inventada de pesquisa.

## 5. Compilação e verificações

```powershell
python tools/check_project.py --cppcheck
python tools/check_integrations.py --android
pio run -e esp32-s3-devkitc-1 -e battery
python tools/package_assistant.py
```

O bootstrap Android usa o SDK 36 local e Gradle fixado com SHA-256 oficial.
`dist/` é ignorado pelo Git e contém APK, firmwares, gateway e manifesto de hashes.
O pacote não contém configurações, tokens ou imagem da flash do aparelho.

Gravar somente a aplicação em `0x10000` preserva NVS e LittleFS. Use o firmware
`battery` para uso fora do cabo; o debug USB é destinado à bancada. O processo
de gravação deve incluir backup da flash e conferência da partição existente.

## 6. Validação física

Build, testes host e lint não comprovam comportamento do rádio ou do Android
Samsung. Validar no conjunto real:

1. Gravação curta, salvamento e sincronização por Wi-Fi.
2. Pareamento inicial com PIN e reconexão com a tela do celular bloqueada.
3. Nota por BLE, interrupção e retomada, sem duplicar arquivos no Drive.
4. Pergunta/resposta falada por Wi-Fi e BLE; medir primeiro áudio e perdas.
5. Edição de nota e evento, incluindo resposta perdida e reconciliação.
6. Mesmo teste por Wi-Fi com celular desligado.
7. Retorno ao deep sleep e comparação de consumo.

O caminho legado, disponível para diagnóstico, transporta PCM/base64 do Live pelo canal BLE. A velocidade
e a latência dependem do rádio, do Android e da rede; não há garantia de
“resposta instantânea”. O link pode ser retido por até 30 s enquanto o aparelho
já estiver acordado, sem prolongar o prazo de deep sleep. A sessão Gemini continua
independente por pergunta. O deep sleep desliga a conexão BLE.

### Medição de latência e limites de vazão Wi-Fi vs BLE (17/10/2026)

Instrumentação com relógios monotônicos de microssegundos no ESP32 (`[AssistantMetrics]` e `[AssistantMedia]`),
rotas exclusivas sem fallback (`benchmark-route wifi` e `benchmark-route phone`), janela de 8 créditos com `WRITE_NR`
e perfis comparativos de mídia (`benchmark-media default|legacy|pcm|opus64|opus48|opus32`).

Registros históricos exploratórios (ESP32-S3 + Galaxy S26 + Gemini Live).
Os turnos têm perguntas/respostas e condições diferentes; não são uma comparação
pareada de latência nem demonstram qualidade perceptiva equivalente:

| Perfil / Transporte | Dados TX | Dados RX | Tempo Upload | Tempo Download | Tempo Total do Turno | Início Reprodução I2S | Rebuffers | Estado |
|---|---|---|---|---|---|---|---|---|
| **Wi-Fi direto (PCM base64)** | ~152 KB | ~100 KB | ~4,7 s (tempo real) | ~3,8 s (progressivo) | **~8,5 s** | **~1,68 s** após soltura | 0 | OK |
| **BLE Legado (PCM base64)** | 110,6 KB | 240,0 KB | 28,4 s (~4,0 KB/s) | 48,6 s (~5,2 KB/s) | **78,05 s** | **78,08 s** (acumulado) | 0 | OK |
| **BLE Mídia v1 `pcm` (PCM binário)** | 87,7 KB | 92,0 KB | 16,4 s (~5,3 KB/s) | 27,5 s (~5,2 KB/s) | **44,01 s** | **44,03 s** (acumulado) | 0 | OK |
| **BLE Mídia v1 `opus64` (64 kbit/s)** | 25,5 KB | 18,6 KB | 7,2 s (~3,5 KB/s) | 5,1 s (~3,6 KB/s) | **14,82 s** | **12,93 s** (progressivo) | 0 | OK |
| **BLE Mídia v1 `opus32` (32 kbit/s)** | 14,5 KB | 13,5 KB | 4,8 s (~3,0 KB/s) | 4,1 s (~3,3 KB/s) | **10,55 s** | **8,94 s** (progressivo) | 0 | OK |
| **BLE Mídia v1 `opus48` (48 kbit/s)** | 19,8 KB | 12,7 KB | 4,7 s (~4,2 KB/s) | 3,1 s (~4,1 KB/s) | **9,33 s** | **8,74 s** (progressivo) | 0 | OK |

**Limites da interpretação:**
1. A alegação anterior de meta atingida por uma diferença de 0,83 s estava incorreta: subtraía tempos totais de turnos distintos, não a espera após a pergunta. A meta BLE até Wi-Fi +1 s permanece não comprovada.
2. Contadores de mídia não representam todo o tráfego no rádio. Comparar percentuais de bytes de respostas diferentes também não isola o ganho do codec.
3. `rebuffer=0` indica ausência de esvaziamento detectado naquele turno, não aprovação de escuta, ausência universal de cortes ou latência acústica validada.
4. A vazão observada nesses pilotos não é teto físico do BLE. Rádio, créditos, callbacks, framing, TLS e codec precisam ser separados na medição.
5. No piloto legado `pilot-ble-ready/trial-001-phone-short.json`, os marcos do firmware indicam 74,631 s entre soltura e primeiro aceite I2S. O detector acústico marcou esse ensaio como inválido; o número não deve ser publicado como latência acústica.

### Correção do perfil normal (18/10/2026)

O código usa `NetworkRoute::kDefaultPhoneMedia = Opus48` no boot e ao terminar
uma operação, inclusive na retenção BLE. Antes, Opus só era escolhido no benchmark,
e o BOOT normal voltava para `legacy`. `benchmark-media default` restaura a seleção
normal; o harness também usa esse comando ao encerrar, para não deixar um override
legado na próxima pergunta. O Wi-Fi continua enviando PCM sem compressão.

O companion passa a agrupar as escritas de cada quadro WebSocket antes do flush
TLS e a preencher DATA com registros de mídia já disponíveis, sem esperar por
novos registros. O ESP32 solicita intervalo de conexão de 7,5 ms (mínimo e máximo
de seis unidades de 1,25 ms); o Galaxy S26 confirmou esse intervalo nos pilotos,
em vez dos 15 ms anteriores. Trata-se de negociação, não garantia para todo telefone.
O prebuffer comprimido passa a 500 ms, sem alterar o prebuffer PCM do Wi-Fi.
Na entrada, registros Opus já disponíveis são agrupados em lotes pré-alocados de
1384 bytes. O worker pode consumir até 8192 bytes de PCM já capturado por rodada,
sem esperar novas amostras. O fim de `sendAudio`, JSON e `END_INPUT` drenam o lote;
cancelamento o descarta. O fluxo de bytes, posições, trimming e bitrate não mudam.
Essas alterações têm testes de stream e lifecycle. A qualidade perceptiva de
Opus48 foi aprovada pelo usuário como "Sem piora audível" nos testes desta sessão;
isso não é ABX cego nem prova de transparência universal.

Ensaio final curto: `test/results/voice-benchmark/ble-batched-final/20261018T035133Z-f9ea3d2729f4`.
Foram feitos três pares, com ordem Wi-Fi/BLE alternada. Todas as seis respostas
terminaram com `rebuffer=0`; três gravações acústicas foram inválidas por captar
energia antes do início possível da reprodução. Elas permanecem nos registros.
O único par completo acusticamente válido foi Wi-Fi 1,825 s e BLE 2,250 s,
diferença de 0,425 s. Uma pergunta BLE consecutiva teve 1,655 s. Esses números
são pilotos, não mediana pareada validada para todas as condições.

Os tempos de soltura até primeiro aceite I2S foram 2,211 s no primeiro BLE,
1,619 s e 1,613 s nos dois BLE seguintes. Houve também um turno Wi-Fi com
11,015 s nesse marco, incluindo espera pelo servidor; ele não foi descartado.
Não confundir esses tempos internos com latência acústica.

Uma pergunta média gerou 7,020 s de áudio BLE completos, também sem rebuffer.
O onset de 0,925 s inicialmente aceito nesse teste médio precedia o I2S em
mais de 100 ms e não deve ser usado como resultado de latência: motivou o
critério assimétrico mais conservador descrito abaixo. O relatório de origem
preserva a versão e os critérios usados na captura, sem reescrita retroativa.

A equivalência estatística Wi-Fi/BLE, ensaios com dados móveis fora de casa e
vinte pares por condição continuam fora do que estes pilotos comprovam.

### Canal de mídia BLE v1

O Wi-Fi direto continua usando o PCM e o WebSocket existentes. HTTP, sincronização
Drive e gateway permanecem no relay transparente. A voz BLE comprimida usa um
canal lógico reservado, aberto como `voice-v1.ideiarec.invalid:443`: esse nome é um
serviço local explícito, nunca uma consulta DNS nem uma exceção geral à allowlist.
Companions antigos recusam esse serviço antes do envio da chave ou de áudio.

O companion novo termina a sessão Gemini com TLS verificado e limite de mensagem,
converte mídia e mantém controle JSON, transcrições, ferramentas e cancelamento.
A chave permanece em memória durante a sessão e não é salva em logs/configuração.
É necessário atualizar firmware e APK; não existe troca silenciosa de codec no turno.

| Perfil volátil | Transporte BLE da voz | Uso |
|---|---|---|
| `default` | Seleção normal do firmware (`opus48`) | Restaura comportamento do BOOT |
| `legacy` | WebSocket/JSON/base64 originais | Diagnóstico sem compressão, resposta acumulada e alta latência |
| `pcm` | PCM16 binário, 16 kHz entrada / 24 kHz saída | Referência sem perdas, bitrate negociado zero |
| `opus64` | Opus mono, 64 kbit/s, frames de 20 ms | Primeiro candidato de qualidade |
| `opus48` / `opus32` | Opus mono, 48 / 32 kbit/s, frames de 20 ms | `opus48` no código normal; `opus32` para comparação |

Cada registro usa cabeçalho de 12 bytes: magic `56 31`, tipo, flags, comprimento
u32 LE e posição u32 LE em amostras PCM originais. Tipos: HELLO=1, JSON=2, OPUS=3,
PCM=4, END_INPUT=5, ERROR=6. HELLO confirma versão, perfil, taxas, frame e lookahead.
OPUS contém `skip:u16 LE`, `take:u16 LE` e pacote raw de até 1276 bytes, sem Ogg.
A flag 1 reinicializa apenas o decoder no início de um segmento; posições continuam
globais no turno. O flush/remove-delay é explícito e preserva a cauda original.
O relay mantém epochs, sequências, ACK por consumo e limites de memória.

Libopus oficial 1.5.2 tem fonte validada por SHA-256; a licença acompanha APK e pacote.
No ESP32 são usados fixed-point, estados preferencialmente em PSRAM e nenhuma
alocação no hot path. A task de rede reserva 32 KiB quando pode usar Opus, devido
ao scratch C99; o watermark real é registrado em `AssistantMedia`. A seleção
diagnóstica exclusiva Wi-Fi não precisa dessa stack adicional.

Opus não é lossless. A seleção padrão no código não comprova transparência:
comparação de escuta com o mesmo PCM e aprovação de qualidade continuam pendentes.
A taxa nominal não inclui
enquadramento, ACKs ou VBR; ficar abaixo da vazão média não garante ausência de cortes.
PCM binário mantém a política acumulada BLE; Opus permite prebuffer progressivo,
sem retirar o auto-clear do DMA e sem acelerar/descartar amostras para ocultar atrasos.

### Bancada reproduzível

```powershell
python -m venv tools/.venv-benchmark
tools/.venv-benchmark/Scripts/python.exe -m pip install -r tools/requirements-benchmark.txt
pwsh -File tools/create_benchmark_wavs.ps1
tools/.venv-benchmark/Scripts/python.exe tools/benchmark_voice.py --list-devices
tools/.venv-benchmark/Scripts/python.exe tools/benchmark_voice.py --wav test/results/voice-benchmark/corpus/short.wav --routes wifi,phone --media default --repeats 3 --input-device 15 --output-device 12
```

Os índices de áudio acima são exemplos: confirmar o microfone físico e a saída
cabeada/local. Usar posições/volume constantes. Cada execução cria pasta exclusiva
com WAV, JSON, CSV e eventos JSONL. O detector correlaciona o estímulo gravado e
procura a resposta no mesmo eixo de amostras, sem descartar arbitrariamente os
primeiros 350 ms. Reverberação, estalos, áudio fraco, overflow e ausência de vínculo
de turno/perfil tornam o ensaio ambíguo/inválido, não uma latência favorável.

`AssistantMetrics` informa estágios monotônicos do ESP32; `AssistantMedia` inclui
perfil real, bytes de mídia, tempo do codec e watermark de stack. Aceite I2S não é
som audível. O harness exige `turn` no status/resultado e restaura Auto/ready/default.
Falhas devem aparecer nos resultados e no exit code. Um piloto não substitui as
20 comparações pareadas, partida fria/quente e escuta cega previstos no plano.

Em Windows, `--wasapi-exclusive` abre microfone e alto-falante WASAPI em modo
exclusivo. Os ensaios compartilhados anteriores tiveram falsos onsets por supressão
de áudio. A correlação mínima agora é 0,35, com piso de energia de três LSB PCM16.
Uma checagem adicional compara durações relativas ao release e rejeita contradições
com antecipação maior que 100 ms ou atraso maior que 750 ms em relação ao primeiro
aceite I2S; nunca substitui onset acústico por
timestamp do firmware. Um ensaio inválido salva latência nula e mantém a candidata
somente como diagnóstico. `--playback full` com voz comprimida BLE é recusado:
esse comparativo de buffer integral só se aplica a `legacy` e `pcm`.
