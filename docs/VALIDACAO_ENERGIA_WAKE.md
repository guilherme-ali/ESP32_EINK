# Validacao de energia e wake

## Estado e limites

Firmware **candidato, ainda nao medido nesta etapa**. Este guia prepara a bancada;
nao certifica latencia, corrente, autonomia, qualidade de audio ou aprovacao fisica.
A preparacao de 18/10 foi feita sem placa. A bancada USB de 19/10, com
instalacao app-only e ensaios reais, esta registrada na secao final desta pagina.
Compilacoes e testes de software nao substituem medicao eletrica ou acustica.
Resultados historicos de outras versoes nao validam este candidato.

Referencia de escopo: [plano de deep sleep e captura rapida](../.kilo/plans/1792258324969-deep-sleep-captura-rapida.md).
Manter timeout e opcoes de Config, inclusive o valor salvo em NVS: nao impor
30 segundos nem trocar o padrao existente de 120 segundos. Registrar o valor
real antes e depois. O sono nao pode interromper Recording, Saving, playback,
sync/exportacao, escrita FS/NVS ou workers ativos.

Deep sleep reinicia o boot; nao conserva captura na RAM/PSRAM comum.
**Nao ha audio durante o sleep nem antes de microfone/ADC/I2S estarem operantes.**
Primeiro bloco DMA, tela de gravacao e checkpoint nao provam audio analogico valido.
Nao prometer 1 ms, zero microampere ou autonomia numerica sem ensaio.
O consumo do MCU nao e o consumo da placa: regulador, carregador, sensores,
RTC, pulls e eventual microSD podem estabelecer o piso.

## Ferramenta segura

Executar da raiz do projeto em PowerShell 7. O script e
[`tools/power_test.ps1`](../tools/power_test.ps1), em ASCII.

| Acao | Efeito |
|---|---|
| sem argumentos / `Help` | Ajuda offline, sem Python e sem hardware |
| `Check` | Confere layout, Python/PlatformIO locais e inventario do binario; sem build/serial |
| `Build` | Build somente do perfil escolhido; nao executa full runners ou upload |
| `Flash` | Unica acao com hardware; exige porta explicita, build selecionado nesta chamada e esptool v5 |
| `Metrics` | Apenas imprime comando serial para uso posterior separado; exige debug e porta |
| `-DryRun` | Plano textual, sem Python, build, serial ou flash; nao certifica ferramentas/binario |

```powershell
pwsh -NoProfile -File tools/power_test.ps1 -Action Help
pwsh -NoProfile -File tools/power_test.ps1 -Action Check -Profile battery
pwsh -NoProfile -File tools/power_test.ps1 -Action Build -Profile battery -DryRun
pwsh -NoProfile -File tools/power_test.ps1 -Action Flash -Profile debug -Port COM7 -DryRun
pwsh -NoProfile -File tools/power_test.ps1 -Action Metrics -Profile debug -Port COM7
```

COM7 e exemplo, nao autodeteccao. Nenhum desses exemplos acessa hardware.
`Check` avisa se nao encontrar esptool v5; inventario nao e aprovacao para flash.
Sem argumentos o padrao e `Help`, com perfil `battery`.

O Python do PlatformIO e procurado em `$HOME/.platformio/penv/Scripts/python.exe`
e no ambiente local existente `tools/.venv-benchmark/Scripts/python.exe`;
`-PlatformIOPython` aceita caminho explicito. O script verifica importacao isolada
e Core v6, sem usar `pio` ou Python arbitrario do PATH.
Para esptool, sonda esses ambientes conhecidos e exige modulo **v5**;
se nao existir, fornecer `-EsptoolPython` apontando um `python.exe` confiavel ja
instalado. Nao ha download/instalacao automatica nem novo caminho em `.opencode`.
Importacao isolada evita usar um modulo `esptool.py` do diretorio atual.

Build requer dependencias ja instaladas e cache Opus autenticado em
`C:/pio-builds/ESP32_EINK/voice-opus/opus-1.5.2.tar.gz`.
O wrapper recusa pacotes ausentes e conexoes de rede no processo PlatformIO;
o cache e conferido antes do filho SCons para que `setup_opus.py` nao baixe fontes.
Se faltar algo, para com erro; preparar dependencias separadamente, nao nesta tarefa.
O bloqueio usa o Core v6 instalado e deve ser revisto ao migrar de versao.

| Perfil | Ambiente PlatformIO | Aplicacao |
|---|---|---|
| `battery` | `battery`, USB CDC desligado | `C:/pio-builds/ESP32_EINK/battery/firmware.bin` |
| `debug` | `esp32-s3-devkitc-1`, USB CDC ligado | `C:/pio-builds/ESP32_EINK/esp32-s3-devkitc-1/firmware.bin` |

Nao usar somente existencia/data de `firmware.bin` como prova de build atualizado.
`Flash` executa primeiro o build selecionado, exige sucesso e arquivo nao vazio
de ate 3 MiB, registra SHA-256 e mantem o binario protegido contra alteracao
durante write/verify. Falha de build nao permite gravar o binario antigo.
O lock coordena apenas chamadas deste script: reservar janela exclusiva e
**nao executar em paralelo com PlatformIO, checkers ou outro agente**.

## Instalacao posterior, somente autorizada

Antes: exportar notas/configuracoes por meios ja conhecidos, guardar binario
baseline compativel e seu SHA-256 fora da pasta de build. Nao sobrescrever o
unico baseline ao compilar candidato. Conferir revisao da placa, particoes e
alimentacao estavel; fechar consoles e monitores. Nao usar `pio ... -t upload`.

Somente na bancada futura, com aparelho conectado e porta confirmada:

```powershell
# Apenas build; nao grava. Repetir depois para o outro perfil.
pwsh -NoProfile -File tools/power_test.ps1 -Action Build -Profile debug
# Exemplo de Python confiavel ja instalado: substituir pelo caminho local real.
$esptoolPython = 'C:/CAMINHO_REAL/Scripts/python.exe'
pwsh -NoProfile -File tools/power_test.ps1 -Action Flash -Profile debug -Port COM7 -EsptoolPython $esptoolPython
```

`Flash` usa somente `write-flash 0x10000 firmware.bin`, seguido de
`verify-flash 0x10000 firmware.bin`, via `python -I -m esptool` v5,
`--chip esp32s3 --port COMn --baud 460800`. Mantem bootloader, partitions,
NVS e LittleFS. Nao ha erase completo, `--erase-all`, `--force`, OTA ou
gravacao de imagem mesclada. O apagamento setorial inerente ao `write-flash`
fica na faixa da app. O layout local exigido e app em `0x10000`, tamanho 3 MiB.
Se o layout real diferir, parar; nao migrar particoes nesta validacao.
Write deixa o chip em bootloader (`--after no-reset`); a verificacao seguinte
reconecta e so entao pede `--after hard-reset`. Falha nunca e declarada sucesso.
USB pode reenumerar; nao insistir em outra porta sem conferencia manual.

Neste computador foi conferido offline um esptool **5.5.0** ja instalado em
`C:/Users/guilh/AppData/Local/Temp/opencode/esptool-ble-toolchain/Scripts/python.exe`.
O caminho e de uma ferramenta preexistente, nao uma configuracao nova nem
instalacao feita nesta entrega. Pode ser fornecido a `-EsptoolPython`; por estar
em Temp, sua existencia nao e garantida em sessoes futuras. A sondagem nao abriu COM.

## Diagnostico USB versus bateria

No perfil **debug**, USB conectado e aparelho acordado, `Metrics` imprime:

```powershell
& "$HOME/.platformio/penv/Scripts/python.exe" tools/device_console.py boot-metrics --port COM7 --timeout 10
```

Esse comando **abre serial se executado separadamente**. Nao executa-lo nesta
preparacao offline. Guardar `[Boot]`, `[BootMetrics]` e
`[DiagDone] boot-metrics` do mesmo boot. Os marcos sao relativos ao boot, nao
ao instante fisico do botao; nao subtrair relogios de boots/hosts diferentes.
`valid_audio_us=unmeasured` e um limite explicito, nao valor zero.
Marcos ausentes/zero nao devem virar uma latencia aprovada por inferencia.

Para consumo, instalar depois o perfil **battery**, desligar USB e medir na
alimentacao de bateria. Nao desconectar USB de firmware debug para alegar
medicao de battery: sao configuracoes diferentes. Nao esperar console USB
no perfil battery. Logs/USB nao fazem parte da comparacao energetica final.
Nao usar percentual, tensao isolada ou referencia de 4,086 V como corrente.

## Roteiro de bancada

1. Registrar baseline e candidato: revisao fisica, SHA-256/binario/perfil,
   versoes, tensao/estado da bateria, timeout Config, taxa/ganho, FS ocupado,
   rede/rota do assistente, microSD presente/ausente e condicoes do instrumento.
2. Smoke test separado de boot frio e wake EXT1 real. BOOT curto no sleep
   deve iniciar **nota local** e consumir o gesto inicial uma vez; o proximo
   BOOT curto para/salva. BOOT longo conserva **PTT**; soltura encerra captura
   mesmo com rede conectando. Assistente indisponivel/desativado deve produzir
   resultado explicito, nao nota silenciosa. PWR acorda para **Home**, sem
   ligar captura nem abrir menu/dormir de novo por reaproveitar a soltura.
3. Ensaiar gesto ja solto antes do app, mantido, bounce, perto do limiar longo,
   segundo clique imediato, release durante boot/conexao, BOOT+PWR simultaneos
   (prioridade segura de PWR) e botao preso. Nao aceitar wake loop nem registrar
    deep sleep se permaneceu acordado aguardando soltura estavel.
    Durante PTT pressionado a inicializacao e os refreshes da tela sao adiados,
    priorizando captura e despacho da soltura. O wallpaper pode permanecer ate
    soltar BOOT. Wake por PWR permanece Home; portal automatico so no boot frio.
4. Fazer pelo menos **30 ciclos por caminho principal**, para baseline e
   candidato, separando boot frio, BOOT curto, BOOT longo e PWR. Registrar cada
   tentativa/falha/invalida, mediana, p95 e maximo, sem excluir falhas silenciosamente.
5. Validar audio a 8/16/32/44,1/**48 kHz**, taxa real do WAV e duracao; comparar
   escuta e prefixo/fluxo sem repeticao, lacuna ou corte de silaba. Dar foco a
   48 kHz com display/rede ativos e ao PTT convertido a 16 kHz. Fila cheia,
   alocacao/tarefa/codec falhando devem interromper explicitamente, nunca
   sobrescrever prefixo ou trocar taxa/qualidade sem informar.
6. FS com muitas notas grandes/sincronizadas, espaco insuficiente, falha de
   montagem/escrita, checkpoint e recuperacao apos power loss controlado,
   somente com backup. Nao formatar automaticamente. Em gravacao longa e
   **Saving**, deixar vencer timeout e tentar sono: finalizacao deve concluir
   sem sleep prematuro, nota vazia aparente, corrupcao ou perda do prefixo duravel.
7. Ensaiar sensores ausentes, rede indisponivel e **BUSY do e-paper preso**
   somente com procedimento eletrico seguro apropriado. Exigir timeout/fallback
   limitado, operacao dos botoes e tentativa segura de repouso, sem espera eterna.
8. Medir audio valido com referencia externa comum entre botao e estimulo
   acustico deterministico nos offsets 0/50/100/200/500/1000 ms, refinando o
   limite. Registrar transiente/primeira silaba e duracao do arquivo, separando
   primeira captura valida, primeira persistencia e UI pronta. Benchmark de
   pergunta/resposta do assistente nao substitui este ensaio de wake.
9. Com instrumento adequado a microampere **e picos de boot**, medir Home,
   apos captura/playback, janela BLE, entrada em sono e deep sleep estabilizado;
   integrar energia por ciclo. Conferir queda introduzida pelo instrumento,
   rails/PA e latch GPIO17 HIGH, com/sem microSD. USB pode mascarar consumo e
   falha de latch; nunca desligar GPIO17 para simular economia.

**Sem instrumento de corrente:** executar somente ensaios funcionais/acusticos
possiveis e registrar `consumo quantitativo nao validado`. Nao estimar microampere,
energia por ciclo ou autonomia por percentual. Sem referencia acustica comum,
registrar tambem `latencia ate audio valido nao validada`.

## Artefatos e aceite

Guardar por execucao: identificador/data, configuracao acima, binarios baseline
e candidato com hashes, saidas de build/write/verify, logs debug por boot,
CSV de cada tentativa (caminho, taxa, resultado, motivo invalido/falha), WAVs
originais/estimulados, capturas eletricas e resumo mediana/p95/maximo/incerteza.
Nao publicar credenciais nem backups contendo NVS com segredos.

Aceitar apenas com espera sem aumento, latencia e energia por ciclo melhores
que baseline acima da incerteza, sem perda adicional de audio apos captura
valida, corrupcao, regressao UI/PTT ou alteracao do timeout salvo. Ensaios host,
parser PowerShell e builds nao substituem essas evidencias. Sem bancada,
o estado permanece **candidato nao medido**.

## Rollback app-only

Separar console e alimentacao segura. Usar o **binario baseline arquivado**,
compatibilidade do mesmo layout e hash previamente anotado; nao usar `Flash`
do script para rollback, pois ele recompila o candidato atual. Somente na
bancada autorizada, com esptool v5 ja instalado, executar separadamente:

```powershell
# Substituir ambos os caminhos e a porta por valores reais conferidos.
$esptoolPython = 'C:/CAMINHO_REAL/Scripts/python.exe'
$baseline = 'C:/BACKUP_REAL/baseline-battery-firmware.bin'
Get-FileHash -LiteralPath $baseline -Algorithm SHA256
& $esptoolPython -I -m esptool --chip esp32s3 --port COM7 --before usb-reset --after no-reset write-flash 0x10000 $baseline
# Prosseguir somente se write-flash terminou com codigo 0.
& $esptoolPython -I -m esptool --chip esp32s3 --port COM7 --before no-reset --after hard-reset verify-flash 0x10000 $baseline
```

Nao executar verificacao/retorno ao uso se a escrita falhou; nao declarar sucesso
se verify falhar. Nunca apagar flash, gravar bootloader/partitions/NVS/LittleFS
ou restaurar dump completo. Conferir notas/configuracoes e smoke test apos retorno.

## Validacao desta entrega

Verificacao offline em 2026-10-18, a partir da raiz deste projeto. Comandos reais:

```powershell
pwsh -NoProfile -File tools/power_test.ps1 -Action Help
pwsh -NoProfile -File tools/power_test_test.ps1
git diff --no-index --check -- NUL tools/power_test.ps1
git diff --no-index --check -- NUL tools/power_test_test.ps1
git diff --no-index --check -- NUL docs/VALIDACAO_ENERGIA_WAKE.md
```

Ajuda executada sem dependencias. Suite offline: **22 verificacoes/grupos
aprovados**, cobrindo parser PowerShell,
ASCII, ajuda/padrao seguro, DryRun Check/Build/Flash nos dois perfis, Metrics
somente textual, rejeicao de porta ausente/invalida, Flash posicional e acao
invalida, propagacao de erro do wrapper de build e links locais deste guia.
As sondagens usam caminhos Python deliberadamente inexistentes em DryRun,
para detectar execucao indevida. O teste confere a presenca de `sys.exit(main())`
no wrapper; nao simula uma compilacao nem comprova funcionamento do Core.
Foram conferidos os cinco links locais: plano, script, `platformio.ini`,
`partitions.csv` e `tools/device_console.py`. Whitespace sem erros; Git apenas
avisa sobre conversao futura de LF para CRLF.

Na integracao final, tambem foram executados, sem hardware:

```powershell
& "$HOME/.platformio/penv/Scripts/python.exe" tools/check_project.py --cppcheck
& "$HOME/.platformio/penv/Scripts/python.exe" tools/check_integrations.py
pwsh -NoProfile -File tools/power_test.ps1 -Action Build -Profile debug
pwsh -NoProfile -File tools/power_test.ps1 -Action Build -Profile battery
pwsh -NoProfile -File tools/power_test.ps1 -Action Check -Profile battery
pwsh -NoProfile -File tools/power_test.ps1 -Action Check -Profile debug -EsptoolPython 'C:/Users/guilh/AppData/Local/Temp/opencode/esptool-ble-toolchain/Scripts/python.exe'
```

Ambos os runners finais e ambos os builds passaram. O checker inicial foi
repetido porque detectou alteracao de fontes durante a integracao; somente a
repeticao com fontes estaveis e considerada valida.

| Verificacao de software | Resultado final |
|---|---|
| Host geral / storage-Drive / Gemini / progresso | 56/56, 67/67, 26/26, 15/15 |
| Recorder e captura compartilhada | 41/41, incluindo erros, prefixo e timeout |
| LittleFS upstream em NOR simulado | 9/9; nao e medicao de tempo real |
| Botoes / codec real lifecycle-player | 13 grupos / 10 grupos |
| Conversor PCM FIR | 4 grupos; pior alias na grade testada: -75,06 dB |
| Assistente runtime | 18 grupos, 3.772 verificacoes |
| Kit PowerShell offline | 22 verificacoes/grupos |
| cppcheck, whitespace, builds debug e battery | Aprovados |

Os binarios finais estao nos caminhos da tabela de perfis. SHA-256 registrado
em 2026-10-18, valido somente para estes artefatos (novo build pode mudar o hash):

| Perfil | SHA-256 de firmware.bin |
|---|---|
| debug | `68F6479AE3F94BFB685197185380C03E8E1EC549D558D9B1492B658CFB86074F` |
| battery | `F8252B331A3D9C6A69F0FFF1E0FD89AD57EE7582830B038BD1245BEE32CCA12D` |

`Check` confirmou PlatformIO Core 6.2.0 e, com o parametro explicito acima,
esptool 5.5.0. Os dois caminhos `Build` do wrapper foram ensaiados de verdade,
com dependencias locais e sem upload. **Flash, verify no dispositivo, coleta
serial, escuta, latencia acustica e corrente nao foram executados.**
O firmware continua sendo candidato de bancada, nao versao fisicamente aprovada.

## Bancada Real em 19/10/2026

Placa identificada em COM7: ESP32-S3-PICO-1 v0.2, MAC
`70:04:1d:d7:ae:a4`, flash/PSRAM de 8 MB. Antes da primeira gravacao foi lida
somente a particao de aplicacao para retorno, sem ler nem apagar NVS/LittleFS:
`C:/Users/guilh/AppData/Local/Temp/kilo/esp32-eink-app-baseline-20261019.bin`.
A imagem anterior foi validada offline; por estar em Temp, preservar fora dessa
pasta caso seja necessario manter o rollback por longo prazo.

### Captura e Limpeza

- `power-selftest 30`: **240/240 casos aprovados**, 30 sessoes para cada um dos
  oito caminhos de taxa. Nao sao 240 ciclos de deep sleep.
- Nenhum overflow, timeout, arquivo invalido, storage-full ou falha de limpeza.
- Em todas as 240 leituras pos-sessao, heap interno = **198.960 bytes** e PSRAM
  livre = **8.386.051 bytes**. Isto demonstra restauracao dos recursos nesse
  ensaio, nao mede corrente nem cobre todos os caminhos da aplicacao.
- Matriz curta apos a mudanca de feedback visual: **8/8 aprovada**, novamente
  com todos os temporarios removidos e estado de software quiescente.
- Cinco notas originais preservadas, todas sincronizadas; espaco livre ao final
  = **3.035.136 bytes**, identico ao inicio. Nenhuma nota de teste foi mantida.
- Configuracoes existentes mantidas: audio 16 kHz, ganho 24 dB, volume 100%,
  timeout de descanso **600 s** e auto-sync ligado. O teste nao gravou novos
  valores nas configuracoes.

Arquivos da bancada em `test/results/power-wake-20261019/`:

- `capture-30.log`: texto serial integral da matriz prolongada.
- `capture-30-summary.json`: veredito e estatisticas por taxa.
- `capture-final-smoke.log`: oito casos finais.
- `final-status.log` e `final-boot-state.log`: estado antes do perfil battery.

Reproducao no perfil debug, com pasta de resultados ja existente:

```powershell
python tools/device_console.py 'power-selftest 30' --timeout 600 --log caminho/novo-capture.log
python tools/summarize_power_test.py caminho/novo-capture.log --output caminho/novo-summary.json
```

`--log` e `--output` recusam sobrescrever arquivos. O console escolhe 600 s
por padrao para 30 repeticoes e 120 s para uma; falha/incompleto retorna codigo 2.
O resumo distingue o preparo do codec da latencia do botao ou do audio valido.

### Retomada e Interface

- Usuario confirmou PWR acordando na Home, sem iniciar nota.
- Usuario confirmou BOOT curto acordando e iniciando gravacao no mesmo gesto.
- Telemetria desse wake: reset **8** (deep sleep), wake **3** (EXT1), mascara
  **0x1** (GPIO0), com contador RTC de entradas em sono preservado.
- Nesse boot, primeiro bloco I2S em **183.645 us** do relogio do firmware,
  entrada no setup em **91.226 us**, armazenamento pronto em **1.177.495 us**.
  O bloco chegou aproximadamente **994 ms antes** do armazenamento ficar pronto.
  **Nao e medicao fisica de botao ate fala valida**, nem uma distribuicao p95
  de wakes: inclui somente marcos internos de uma observacao.
- Primeiro relato de descarte foi inconclusivo. O estado posterior ja tinha as
  cinco notas e espaco original, sem captura ativa. Nao foi comprovado defeito
  no classificador da longa; sua politica nao foi alterada.
- Adicionada confirmacao visivel de descarte antes da varredura de notas/hash.
  Usuario repetiu o gesto e confirmou a mensagem. Estado posterior novamente
  com cinco notas, nenhum worker ativo e mesmo espaco livre.
- Logs debug de botoes agora distinguem observacao, despacho, gaps e resultado
  da exclusao. O observador `discard-gesture.log` expirou antes da acao humana;
  esse arquivo vazio nao serve como evidencia do gesto. A confirmacao e humana
  e pelo estado posterior, nao por uma sequencia serial inexistente.

### Assistente e Limites

O assistente completou o fluxo direto e o de adocao da captura antecipada por
Wi-Fi, com estado final 5 e zero rebuffer. No caminho adotado foram registrados
4.416 ms de entrada e 2.470 ms de audio recebido/reproduzido.

Os ensaios acusticos permanecem **invalidos para latencia**: o direto falhou
alinhamento do estimulo; o adotado detectou energia antes do playback do
firmware. Resultados estao nas subpastas `voice/` e `voice-adopted/`. Nao relaxar
esses gates nem apresentar a energia detectada como fala verificada.

Sem medidor de corrente na bateria, referencia externa do botao, atuador,
injecao de falha eletrica ou arranjo acustico calibrado, **nao foram aprovados**:
corrente em microampere, autonomia, primeira silaba, 30 wakes por caminho,
comparacao energetica antes/depois e testes fisicos de BUSY preso/sensor ausente.
Essas lacunas nao sao substituidas pela matriz de captura nem por percentual
de bateria. Os gestos basicos testados nao constituem homologacao completa.

### Ajustes da Bancada

`CORE_DEBUG_LEVEL=0` nos dois perfis elimina logs do SDK em sondagens normais de
arquivos ausentes. Diagnosticos explicitos da aplicacao continuam no perfil USB;
o perfil battery nao inicializa CDC. Evita-se poluir e atrasar os ensaios com
mensagens que nao representam falha da nota.

O runner focado do codec passou a criar executaveis temporarios em Temp/kilo
no Windows, fora da pasta sincronizada pelo Drive, apos uma falha de limpeza
`WinError 145`. Nao foi ignorada nenhuma falha de teste. Referencia da API:
https://docs.python.org/3/library/tempfile.html#tempfile.TemporaryDirectory

Imagens mais recentes substituem os hashes historicos de 18/10 acima:

| Perfil | SHA-256 de firmware.bin |
|---|---|
| debug | `28795EFA3E3A92AD53CB4C96B11BF8C3BD02693CE818CBA9FEDA20E50595944C` |
| battery | `0A6222ACE0F28CF24F35BD13AA5EF09296E55E6E595A2AFF43170B5B6665F730` |

### Entrega Final

- Perfil **battery instalado**, imagem de 1.758.496 bytes, gravacao somente em
  `0x10000` e `verify-flash` concluido com digest correspondente. Nenhum erase
  de NVS/LittleFS, bootloader ou tabela de particoes.
- Usuario confirmou o ciclo completo **sem USB**: Home alimentada pela bateria,
  descanso por PWR, BOOT curto acordando e iniciando captura e descarte por
  BOOT longo com confirmacao. Isso valida funcionamento basico e retencao da
  alimentacao, nao corrente nem autonomia.
- Testes de integracao finais aprovados, incluindo 9 testes do console e 4 do
  consolidador. `check_project.py --cppcheck` final aprovado usando a fonte
  cJSON oficial em cache via `--cjson-dir test/host/.build/v1.7.17`, evitando
  nova varredura de todo o SDK na pasta sincronizada.
- Nenhum commit ou push nesta entrega.

O aparelho ficou no perfil sem CDC para uso normal. O timeout existente de
600 segundos foi preservado; PWR longo na Home permite dormir manualmente
sem esperar esse intervalo. Nao confundir a confirmacao funcional sem cabo com
medicao de microampere, prova de primeira silaba ou homologacao energetica.

## Revisao de Interface e Som em 19/10

O pedido posterior foi mostrar escuta sem esperar a soltura do BOOT e avaliar
som baixo a 100%. A bancada encontrou **seis notas existentes**, todas
sincronizadas; as seis foram preservadas. Configuracoes de ganho, volume,
timeout e auto-sync nao foram redefinidas.

### Feedback Visual

O display passou a ter worker exclusivo, prioridade inferior a botoes/captura.
A captura publica `Ouvindo` antes do filesystem e da classificacao curta/longa.
O primeiro FULL recebe o conteudo util, sem refresh branco intermediario.
`Ouvindo`, `Pensando` e `Falando` usam a fonte de 40 px, microfone vetorial e
uma dica curta; nao redesenham contadores nem diagnosticos a cada segundo.

O framebuffer nunca e escrito simultaneamente pelo worker e pelos menus.
Cancelamento restaura a Home, e repouso aguarda o refresh em andamento. O
rollback de um sono rejeitado reconstroi o controlador apos cortar o rail.

Um ensaio real registrou o refresh parcial de `Ouvindo` entre **168.512.866 e
169.105.438 us**, aproximadamente **593 ms**. O pedido foi publicado 26 us
antes de iniciar o trabalho visual. A escuta continuou durante o refresh e a
soltura nao foi bloqueada por ele. Sao marcos de software/BUSY, nao medicao
optica nem promessa de resposta instantanea no painel desligado.

### Diagnostico de Volume

Fonte oficial dos registradores: https://files.waveshare.com/wiki/common/ES8311.user.Guide.pdf

Tons de 24 kHz, mono duplicado LR, volume 100%, registro DAC `0x32 = 0xBA`,
microfone do PC via WASAPI exclusivo. Nao foi alterado ganho global do Windows.
Valores abaixo sao da cadeia **alto-falante + sala + microfone**, nao SPL nem
THD isolada do amplificador. Ensaios com SNR insuficiente foram rejeitados.

| Perfil | Fonte pico dBFS | Hz | Fundamental no microfone dBFS | THD da cadeia |
|---|---:|---:|---:|---:|
| DAC | -12 | 1000 | -44,53 | 1,48% |
| BOTH, mesmo TX-only | -12 | 1000 | -44,59 | 1,43% |
| DAC | -6 | 1000 | -38,87 | 2,65% |
| DAC | -3 | 1000 | -35,97 | 4,22% |
| DAC | -12 | 2000 | -50,03 | 2,28% |

A diferenca DAC/BOTH de 0,06 dB nao sustenta atribuir uma perda grande de ganho
ao perfil DAC-only nas condicoes testadas. Nao foi feito boost analogico ou
alteracao arbitraria de registradores para contornar o relato.

Uma resposta real concluiu com zero rebuffer, volume solicitado/cache = 100%
e registro `0xBA`. A fonte PCM oferecida tinha RMS **-19,104 dBFS**, maior RMS
de 100 ms **-13,699 dBFS**, pico **-3,840 dBFS** e nenhum extremo digital. Isso
mostra pouca margem para boost fixo dos picos; nao significa silencio na fonte.

O ensaio `voice-source-meter-20261019/20261019T181349Z-a04629dcbd32` passou os
gates do benchmark acustico, com 2.225 ms de resposta nessa tentativa. Nao e
distribuicao de latencia nem teste do wake fisico.

Durante a instrumentacao ocorreu um reset com motivo PANIC. A task de audio
recebeu margem adicional de stack; o mesmo caminho concluiu depois e registrou
minimo livre de 3.420 bytes antes do resumo. Esse ajuste de robustez nao e
medicao de consumo e nao dispensa a repeticao final apos integrar o nivelador.

Ferramenta de um ensaio limitado, que nunca toca som no alto-falante do PC:

```powershell
& "tools/.venv-benchmark/Scripts/python.exe" tools/calibrate_speaker.py --mode dac --amplitude-dbfs -12 --frequency 1000 --duration-ms 1500 --volume 100 --wasapi-exclusive --output-dir test/results
```

Ela grava WAV bruto e JSON em pasta UTC/UUID exclusiva. `--mode both` isola o
perfil de codec com TX-only mantido; `--mode drc` exercita o nivelador de fala,
sem mudar registradores/volume ou arquivos do usuario. Este ultimo perfil e
validado separadamente antes da entrega final.

### Nivelador e Interface Finais

O primeiro limitador foi rejeitado: na MESMA resposta, RMS RAW -18,289 dBFS
virava -18,964 dBFS, embora os picos estivessem protegidos. Outro candidato
com janela fixa tambem falhou em dez fixtures de cristas densas. Esses testes
nao foram removidos nem tiveram seu criterio relaxado para aprovar a entrega.

A versao final usa duas janelas de pico: completa para conteudo de baixo fator
de crista e curta de pelo menos 1,25 ms para cristas de fala. Release adaptativo
recupera o corpo entre as cristas, sem elevar o teto. Parametros:

- Makeup maximo +6 dB; threshold RMS -18 dBFS, ratio 3:1 e soft knee de 6 dB.
- Gate -50 dBFS; detector RMS de 20 ms; ataque/release de ganho 5/120 ms.
- Teto de pico conservador 0,5 FS, aproximadamente **-6,02 dBFS**.
- Delay de aproximadamente 5 ms, com cauda preservada: N+L amostras na saida.
- Filas monotonicamente ordenadas, sem heap por amostra; dados de usuario/WAV
  permanecem inalterados. O processamento e somente na reproducao.
- Taxas de Player preservadas, incluindo 11.025, 12.000 e 22.050 Hz; falha de
  reproducao agora e indicada na interface, em vez de aparentar sucesso.

Prova numerica final: **23.239.104 verificacoes**, sem overshoot e sem uso do
clamp de emergencia; 20/20 fixtures vocais passaram o requisito de +1 dB.
Esse numero descreve testes sinteticos, nao uma amostra de usuarios ou audios
gravados. Os casos densos ganharam 1,615 a 2,050 dB e os silabicos 4,504 a
4,573 dB. Testes de tons tambem verificam sidebands/AM, nao apenas harmonicos.

Na resposta REAL final em
`test/results/voice-final-level-20261019/20261019T191046Z-d78b80365344/`:

| Medida | RAW oferecido | Processado oferecido |
|---|---:|---:|
| Amostras mono | 29.521 | 29.641 |
| RMS | -19,603 dBFS | -17,231 dBFS |
| Maior RMS de 100 ms | -14,130 dBFS | -14,043 dBFS |
| Pico | -2,992 dBFS | -6,021 dBFS |

Ganho medio de aproximadamente **2,37 dB na mesma resposta**, sem comparar
falas geradas independentemente. Diferenca de 120 amostras = 5 ms do delay/EOF.
Volume solicitado/cache 100%, REG32 `0xBA`, guards zero, estado final 5,
rebuffer zero e 2.096 bytes livres no watermark da task de audio. O resumo em
ponto flutuante e emitido pelo owner apos suspensao das tasks, fora dos mutexes.
O benchmark acustico dessa tentativa foi valido e mediu 1.220 ms de resposta;
nao atribuir a variacao do servidor a uma melhoria causal da interface.

Comparacao acustica final, com todas as linhas abaixo validas pelos gates de
SNR/protocolo/overflow e sem clipping no microfone:

| Perfil | Fonte pico dBFS | Hz | Fundamental MIC dBFS | THD da cadeia |
|---|---:|---:|---:|---:|
| Sem nivelador | -18 | 1000 | -52,94 | 0,48% |
| Nivelador final | -18 | 1000 | -46,61 | 0,87% |
| Sem nivelador | -3 | 1000 | -38,92 | 5,84% |
| Nivelador final | -3 | 1000 | -40,93 | 2,58% |

A captura sugere cerca de 6 dB a mais no tom fraco e menor distorcao no forte.
Nao e SPL calibrado, estimativa isolada do amplificador ou garantia para toda
frequencia/posicao. Leituras anteriores rejeitadas por ruido continuam nos
artefatos, sem serem promovidas a resultados validos.

Pastas desses quatro ensaios em `test/results/`:

- `speaker-20261019T191512.866967Z-35c25abe23024a78a8f9b69fd377953e`
- `speaker-20261019T191425.009877Z-1f7b066d99964a1ea809e5a1e84dde8a`
- `speaker-20261019T191528.880624Z-2ee19c3aa5df486bac387bfc9b962c61`
- `speaker-20261019T191440.348011Z-a287e24d2b9344f48f24b1f2cfec877c`

O probe do controlador reinicializado, sem esperar soltura/classificacao/rede,
concluiu em **2.143.510 us**: pedido 466.696.458 us, inicio 466.696.487 us e fim
468.839.968 us. Log: `test/results/feedback-cold-20261019-complete.log`.
O conteudo util ja estava no primeiro FULL; o parcial pronto continuou perto
de 0,59 s. Nao e teste optico nem prova de que e-paper possa atualizar em 0 ms.

O console foi corrigido para aguardar o refresh real em vez de encerrar no
ACK do comando. Duas tentativas anteriores encerradas cedo nao sao falhas do
painel nem resultados de latencia; foram mantidas como diagnostico do harness.

### Instalacao da Revisao de Interface/Audio

Revisao final de codigo concluida sem veto P1. As suites finais passaram:
`tools/check_integrations.py` e `tools/check_project.py --cppcheck --cjson-dir
test/host/.build/v1.7.17`, incluindo as novas grades de DSP, Player e o worker
real nas variantes CDC 0/1. A ultima coleta de status confirmou seis notas,
todas sincronizadas, mesmo espaco livre e nenhum trabalho de audio/FS ativo.

O perfil **battery** desta revisao foi instalado e verificado por digest,
somente em `0x10000`. Imagem de **1.765.088 bytes**, sem formatacao ou escrita
em NVS/LittleFS/particoes. A verificacao ocorreu na placa de MAC
`70:04:1d:d7:ae:a4`. Os binarios finais substituem os hashes historicos acima:

| Perfil | SHA-256 de firmware.bin |
|---|---|
| debug | `DEB3B6196817DE98BA0B165F89E7D1E272B88FFC2B34C9E4A95C97B921B72F63` |
| battery | `C05949CF999A4A7C80C2FA664EA4FD29920F635DA2694F9DAAA4591D8F4D4928` |

Backup de aplicacao anterior a esta revisao:
`C:/Users/guilh/AppData/Local/Temp/kilo/esp32-eink-before-ui-audio-20261019-1735.bin`.
Arquivo de Temp nao e armazenamento permanente; nao e necessario usa-lo para
o funcionamento normal. Nenhum commit ou push foi realizado nesta tarefa.

A validacao nova foi feita por USB e em probes do controlador, sem uma nova
homologacao optica ou medicao de corrente. O ciclo sem USB relatado na secao
anterior e da entrega anterior. O perfil atual mantem a mesma alimentacao,
GPIOs de wake e preservacao de dados, mas nao afirmar que houve uma nova
confirmacao humana sem cabo nesta rodada.

## Margem de Volume e README, 19-20/10/2026

O usuario aprovou a interface e pediu mais saida em 100%, citando os antigos
20-30 como referencia subjetiva. Esses valores antigos eram encaminhados como
dB, nao percentuais; reproduzir ganho de +30 dB nao seria uma conversao segura.
A referencia de audibilidade foi considerada sem restaurar a escala saturada.

O README foi revisado: botoes por contexto, wake no mesmo gesto, captura,
feedback visual, limites do e-paper, armazenamento WAV/PCM, perfis de build,
atualizacao app-only, modelos/configuracao, testes e limites da bancada.

### Varredura da Saida

Instrumentado trim absoluto somente no diagnostico. Mesmo PCM de 1 kHz,
pico -6 dBFS, 24 kHz, 100% de volume e 1 s por tom. Antes de qualquer PCM,
o firmware verifica com REG32 REAL a margem digital pos-ganho, recusando
pico acima de -0,25 dBFS. Nenhum ganho global do Windows foi modificado.

| Trim | REG32 | Fundamental MIC | THD da cadeia |
|---|---|---|---|
| 0 dB, referencia recente | `0xBA` | -43,22 dBFS | 2,60% |
| +2 dB, captura anterior | `0xBE` | -40,80 dBFS | 5,86% |
| +4 dB | `0xC2` | -39,09 dBFS | 6,68% |
| +6 dB, rejeitado para producao | `0xC6` | -38,31 dBFS | 12,68% |

Nas capturas, +4 acompanhou aproximadamente o ganho pedido; o degrau de +4
para +6 produziu apenas cerca de +0,78 dB e elevou bastante os harmonicos.
Foi escolhido **+4 dB como teto adicional**, nao +6/+8 ou o antigo +30.
Comparacao descritiva, sem alegacao de ganho acustico/SPL calibrado ou de uma
distribuicao estatistica. As capturas nao foram simultaneas.

Outros tons de estresse com trim +4 foram validos pelo protocolo/SNR:
500 Hz resultou em THD de cadeia de 14,59% e 2 kHz em 16,58%. Sao resultados
altos e nao foram ocultados nem declarados "sem distorcao". Incluem resposta
do alto-falante, sala e microfone; nao isolam saturacao do amplificador. O ajuste
nao constitui certificacao hi-fi ou ausencia de distorcao em toda frequencia.
Nenhuma captura usada acima apresentou clipping do ADC do microfone.

### Curva Final

- Volume zero: mute e PA desligado, inclusive com o perfil calibrado.
- Ate 60%: curva anterior, sem reforco adicional.
- Acima de 60%: reforco progressivo, +2 dB em 80% e **+4 dB em 100%**.
- Aplicacao somente em `preparePlayback(..., boundedSpeech=true)`, apos configurar
  e utilizar o `SpeechLeveler`. PCM bruto/generic/Legacy permanece nominal.
- A precondicao e pico <=0,5 FS. Em 100%, REG32 `0xC2` corresponde a +1,5 dB
  real no DAC apos a compensacao existente; resta cerca de **4,52 dB de margem
  digital**. Isto nao garante margem eletrica/analogica em qualquer carga.
- O ganho de microfone, compensacao `hw_gain=6`, CTRL do NS4150B, arquivos de
  notas e NVS nao foram alterados. O reforco pode consumir mais durante a
  reproducao; o desligamento de audio/deep sleep permanece o mesmo.
- Resume/troca de taxa preservam o perfil; `end()` limpa o opt-in. Diagnostico
  continua trim absoluto e nao soma a curva calibrada uma segunda vez.

A resposta REAL em
`test/results/voice-max-volume-20261020/20261020T004509Z-1e5bb04a87d8/`
concluiu em estado 5, zero rebuffer e benchmark acustico valido (1.660 ms nessa
tentativa, sem inferir melhoria causal de rede). O hardware confirmou volume
solicitado/cache 100%, **REG32=194 (`0xC2`)**, source RAW -20,052 dBFS,
processado -18,012 dBFS com pico -6,021 dBFS, 120 amostras de delay e guards 0.
O reforco final de +4 e posterior a esse PCM; nao aparece como aumento na
telemetria `PlaybackProcessed`, que descreve dados oferecidos antes do DAC.

Artefatos de referencia em `test/results/`:

- `speaker-20261020T003100.479717Z-69e5603a668b42f48f771d5179967e10` (0 dB)
- `speaker-20261019T203935.034054Z-745362da4830439fbd3ca148d5f81165` (+2 dB)
- `speaker-20261020T001043.041200Z-404a95801ad34258be28a72f2a0a9676` (+4 dB)
- `speaker-20261020T003048.183616Z-a39edb654a7e4093823ad5ba0b2629b1` (+6 dB)
- `speaker-20261020T003135.789141Z-883db97c7312475ea96c06673c27b7cd` (500 Hz)
- `speaker-20261020T003157.289818Z-5a8207c53b914de6a6747574b82fd871` (2 kHz)

O aparelho tinha **sete notas sincronizadas**, incluindo uma nota longa criada
pelo usuario, e apenas 245.760 bytes livres dentro da reserva: zero gravavel
para novas notas. Nenhuma foi apagada. A calibracao usou RAM e saida controlada,
nao gravacoes adicionais no LittleFS. Backup de aplicacao anterior a esta rodada:
`C:/Users/guilh/AppData/Local/Temp/kilo/esp32-eink-before-max-volume-20261019.bin`.

### Entrega da Curva Calibrada

Os testes finais de integracao, `check_project.py --cppcheck` com cJSON 1.7.17
em cache e os builds debug/battery passaram. Revisao focada nao encontrou P1;
permanece a precondicao de usar o reforco apenas com PCM limitado pelo DSP.
O README foi atualizado e links locais conferidos. Sem commit ou push.

Ultima coleta: sete notas sincronizadas, 245.760 bytes livres, zero gravavel,
nenhum worker ativo, volume 100%, taxa 16 kHz, ganho do microfone 24 dB,
timeout 600 s e auto-sync ligado. Valores preservados; nao foram criadas nem
apagadas notas durante os ensaios. Log: `test/results/max-volume-final-status-20261020.log`.

O perfil **battery** foi instalado app-only em `0x10000` e verificado por
digest. Tamanho **1.765.232 bytes**. Artefatos finais desta rodada:

| Perfil | SHA-256 de firmware.bin |
|---|---|
| debug | `3D335184243F29C4DCADC1940F948E7206338FBF41B986021E893A6046F1476A` |
| battery | `05427CDF42305AE2A5F184B21E39094D398D32AA34204D9947049F6DC13BDD7F` |

Nenhuma escrita em NVS/LittleFS/bootloader/tabela de particoes foi feita pela
atualizacao. A instalacao e os testes por USB nao substituem nova medicao de
corrente na bateria, SPL calibrado, teste de todos os alto-falantes ou toda
condicao de alimentacao; nao se promete ausencia absoluta de distorcao analogica.

## Referencias

- [Configuracao atual de perfis e build_dir](../platformio.ini).
- [Layout de particoes](../partitions.csv) e [console existente](../tools/device_console.py).
- [Espressif: write-flash e apagamento setorial](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/basic-commands.html).
- [Espressif: verify-flash](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-commands.html).
- [Espressif: modos de reset](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/esptool/advanced-options.html).
