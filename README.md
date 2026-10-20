# Gravador de Ideias

Firmware para a placa **Waveshare ESP32-S3-ePaper-1.54**, com gravação local
de notas de voz, transcrição, sincronização com Google Drive e assistente de
voz. Gravar notas não depende de rede. Os serviços online exigem configuração
e internet por Wi-Fi direto ou pela ponte BLE do aplicativo Android.

Na inatividade, a tela mostra uma ilustração e o dispositivo entra em deep
sleep. O firmware encerra áudio e conexões, desliga os rails de e-paper/áudio
e configura o repouso da CPU, rádios e clocks. O **GPIO17 permanece HIGH,
retido como latch de alimentação da bateria**; não há corte de USB/VBUS.
O e-paper mantém a imagem sem refresh. Isso não significa consumo zero:
corrente e autonomia da placa ainda exigem medição elétrica na bateria.
Ensaios e limites: [validação de energia e wake](docs/VALIDACAO_ENERGIA_WAKE.md).

## Hardware

- **Placa:** [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm),
  versão V2 (chip ESP32-S3-PICO-1-N8R8: 8 MB flash + 8 MB PSRAM octal)
- **Tela:** e-paper 1.54" 200×200 preto/branco, controlador SSD1681
- **Áudio:** codec ES8311, conversor ADC/DAC ligado por I2S;
  microfone, amplificador e alto-falante são componentes separados
- **Relógio:** RTC PCF85063 (I2C), usado para carimbar hora nos arquivos
- **Armazenamento:** flash interna via LittleFS — o slot de cartão
  microSD existe na placa mas ainda não é usado pelo firmware
- **Botões:** BOOT e PWR (os únicos dois disponíveis)

A pinagem completa está em `src/config/pins.h`.

## Funcionalidades

### Celular BLE e assistente de voz

- **BOOT curto** continua gravando notas; **BOOT segurado**, na tela inicial,
  captura uma pergunta para a IA e responde por voz quando soltar.
- Wi-Fi direto tem prioridade. O aplicativo Android **IdeiaRec** fornece uma
  ponte TLS por BLE usando a conexão do telefone, sem ativar hotspot.
- Gateway Google Apps Script permite consultar/editar o vault Obsidian no
  GitHub e criar eventos/lembretes no Google Agenda, inclusive em Wi-Fi sem celular.
- **Enviar ao Obsidian**, no detalhe de uma nota, exporta seu Markdown/transcrição
  já gerado para `00_Inbox/Gravador/`, com commits idempotentes por conteúdo.
- Pesquisa web opcional com grounding e fontes, em provedor gratuito configurado.

Configuração, limites e validação física: [docs/ASSISTENTE_CELULAR.md](docs/ASSISTENTE_CELULAR.md).
Aplicativo: `companion-android/`; ferramentas: `gateway/`. As novas funções são
habilitadas pela configuração; instalação, pareamento e autorizações são iniciais.

- Gravação de voz em WAV PCM 16-bit mono, sem compressão, com taxas de
  8, 16, 32, 44,1 e 48 kHz
- Lista de notas na tela com horário e duração, navegável e reproduzível
- Transcrição de notas salvas via Gemini ou endpoint de transcrição
  OpenAI-compatível configurado (por exemplo, Groq ou OpenAI)
- Sincronização do áudio + transcrição/Markdown para uma pasta
  própria ("Gravador de Ideias") no Google Drive, usando o escopo
  `drive.file` (o app só enxerga o que ele mesmo cria — nada mais no
  seu Drive)
- Portal de configuração via Wi-Fi (Rede, chave de transcrição,
  credenciais do Google Drive) — nada fica fixo no código
- Tela de descanso com ilustrações geradas e deep sleep entre usos
- Sincronização manual ou automática após gravar, conforme a opção
  **Sincr. ao gravar**, desligada por padrão

## Como usar

| Contexto | BOOT curto | BOOT longo (aprox. 1 s) | PWR curto | PWR longo |
|---|---|---|---|---|
| **Home** | Inicia nota local | Inicia assistente PTT; solte para enviar | Abre menu principal | Solicita descanso/deep sleep |
| **Gravando nota** | Para e salva | Para e descarta, com confirmação na tela | Sem ação | Sem ação |
| **Assistente ativo** | Não inicia outra nota | Soltar o BOOT do PTT encerra a captura e envia | Cancela | Cancela |
| **Menu principal** | Abre item selecionado | Sem ação | Próximo item | Volta à Home |
| **Lista de notas** | Abre detalhe da nota | Sem ação | Próxima nota | Volta ao menu |
| **Detalhe da nota** | Executa item selecionado | Sem ação | Próximo item | Volta à lista |

Para ouvir uma nota, abra **Menu > Notas**, selecione a nota e escolha
**Reproduzir** no detalhe. O descarte por BOOT longo durante a gravação é
direto: a mensagem confirma o resultado, não pede uma segunda autorização.

Com **Sincr. ao gravar** desligada, salvar retorna à Home sem transcrever ou
enviar. Ative a opção para tentar o processamento da nova nota após salvar,
ou use **Sincronizar** no menu. Falta de rede/configuração mantém o áudio local
e a nota pendente; a UI informa o resultado da operação.

No deep sleep, **BOOT curto acorda e inicia uma nota no mesmo gesto**;
BOOT mantido inicia o PTT, e a soltura envia a pergunta. **PWR acorda na Home**,
sem iniciar captura nem reaproveitar a soltura para abrir o menu.
O timeout de inatividade é configurável e persistido em NVS, com padrão de
**120 segundos (2 min)**. Valores já salvos são preservados. Captura,
salvamento, reprodução e workers ativos impedem o sono prematuro.

### Captura, interface e volume

- A captura compartilhada por notas e PTT usa um buffer mono de cerca de
  **8 segundos em PSRAM**, dimensionado pela taxa de entrada entre 8 e 48 kHz.
  A escrita no filesystem tem worker separado; o PTT converte para 16 kHz
  com filtragem quando necessário. O buffer não grava durante o deep sleep
  nem recupera fala anterior à inicialização do microfone/ADC/I2S.
- **Ouvindo**, **Pensando** e **Falando** usam fonte de 40 px e feedback visual
  assíncrono, sem bloquear a captura ou a soltura do PTT. O primeiro refresh
  FULL já contém informação útil, sem uma tela branca intermediária.
- O painel continua tendo latência física: os ensaios do driver/BUSY registraram
  aproximadamente **0,6 s no parcial** e **2,1 s no FULL inicial**. Não são
  medições ópticas nem garantia de atualização instantânea ou de primeira
  sílaba capturada. A captura pode começar antes da imagem ficar pronta.
- O áudio reproduzido passa por **controle de dinâmica e de picos**, sem
  alterar os arquivos de notas. Parâmetros medidos e limites estão descritos
  em [docs/VALIDACAO_ENERGIA_WAKE.md](docs/VALIDACAO_ENERGIA_WAKE.md).
  Até 60%, a curva de saída anterior é mantida; acima disso, a reprodução
  de fala com picos limitados recebe reforço progressivo, chegando a **+4 dB
  adicionais em 100%**. Zero continua sendo mute. O reforço não é aplicado
  a PCM bruto sem a garantia de headroom, nem altera o ganho do microfone.
  Volume e ganho de microfone são ajustes distintos; 100% de volume não é
  uma especificação de potência acústica ou garantia de ausência de distorção.

## Primeira configuração

1. **Grave o firmware** (veja a seção de build abaixo) e ligue o
   dispositivo. Se já houver notas, use a atualização app-only descrita abaixo.
2. **Wi-Fi:** no primeiro boot, sem redes nem serviços configurados, ele sobe
   um Access Point próprio
   (`IdeiaRec-XXXX`, sem senha) e mostra o nome + IP na tela. Conecte
   um celular ou notebook nessa rede e acesse o endereço mostrado
   (`http://192.168.4.1`) para digitar o SSID e a senha da sua rede de
   casa. O dispositivo reinicia; depois, conecta sob demanda para os serviços
   online. BOOT longo permite sair do portal e usar offline.
3. **Transcrição:** abra **Menu > Wi-Fi > Portal (STT/Drive)** e preencha os
   campos abaixo no endereço mostrado na tela. O portal pode ser reaberto
   explicitamente, sem depender da ausência de redes salvas.
4. **Google Drive** (opcional, para sincronizar): crie um projeto no
   [Google Cloud Console](https://console.cloud.google.com), ative a
   *Google Drive API*, configure a tela de consentimento OAuth
   (externo, escopo `drive.file`, publicada em produção para o token
   não expirar a cada 7 dias) e crie uma credencial do tipo **"TVs e
   dispositivos de entrada limitados"**. Coloque o Client ID e Client
   Secret no mesmo portal. Após o reinício, solicite uma conexão pelo menu
   Wi-Fi ou por **Sincronizar** para iniciar o pareamento. Acesse o link
   indicado na tela em outro aparelho, faça login e digite o código para autorizar.

Campos de transcrição e política de modelos, conforme
`src/net/settings.h` e `src/net/gemini_client.cpp`:

- **Endpoint:** `https://generativelanguage.googleapis.com` para Gemini ou
  a URL de `.../audio/transcriptions` do provedor OpenAI-compatível.
- **Seleção automática:** tenta `gemini-3.5-transcribe`, depois
  `gemini-3.8-flash` e `gemini-3.5-flash-lite` para transcrição; para Markdown,
  tenta `gemini-3.8-flash` e depois `gemini-3.5-flash-lite`.
- **Seleção manual:** **Modelo STT** e **Modelo do Markdown** são independentes.
  Os valores iniciais são `gemini-3.5-transcribe` e `gemini-3.8-flash`.
  Para um endpoint Whisper, use o identificador aceito por esse provedor.
- **Projeto gratuito confirmado:** o gate Gemini exige confirmação explícita
  antes das requisições. A lista permitida inclui os três modelos acima e,
  apenas no modo manual, `gemini-2.5-flash` e `gemini-2.5-flash-lite`.
  Nomes e descoberta de modelos não comprovam gratuidade ou acesso da conta;
  verifique o plano/cotas no provedor. Sem opção permitida disponível, a nota
  fica pendente, sem recorrer a um modelo fora da política.
- **API key:** obtenha no [Google AI Studio](https://aistudio.google.com/apikey)
  ou no console do provedor escolhido; não a inclua no repositório.
- **Assistente:** vem desativado e sem modelo Live preenchido. Exige modelo
  explicitamente configurado e confirmação de acesso gratuito. A lista atual
  aceita `gemini-3.1-flash-live-preview` e `gemini-3.8-live`; esses modelos são
  distintos dos modelos de transcrição/Markdown. Consulte o
  [guia do assistente e celular](docs/ASSISTENTE_CELULAR.md) para BLE, gateway
  e autorizações.

## Estrutura do projeto

```
src/
  main.cpp              boot, wake, integração e entrada em deep sleep
  app/                  máquina de estados, botões e fluxos da interface
  config/pins.h         pinagem da placa, um lugar só
  board/                energia, amostragem de botões, wake e RTC PCF85063
  display/epaper.{h,cpp} driver do SSD1681 (LUTs de waveform portadas
                         do repositório oficial da Waveshare)
  ui/                   texto/formas sobre o framebuffer (canvas.cpp),
                         fontes e bitmaps da tela de descanso
  audio/                driver I2S/ES8311, captura compartilhada, DSP,
                         gravador e tocador de WAV (não usa PWM para áudio)
  storage/notes.{h,cpp}  listagem e nomeação das notas em /notes/
  storage/note_files.*   WAV canônico, payload PCM e backend LittleFS
  assistant/            PTT, protocolo Live, reprodução e exportação
  connectivity/         seleção de rota e ponte BLE
  sync/                 worker de sincronização, progresso e telemetria
  net/                  settings (NVS), portal Wi-Fi, cliente de
                         transcrição e cliente do Google Drive
lib/es8311/             driver do codec, com tabela de clock portada
                         do esp_codec_dev
tools/img2header.py     converte uma imagem em bitmap 1bpp para a
                         tela de descanso (dithering Floyd-Steinberg)
tools/power_test.ps1     ajuda, build por perfil e atualização app-only
companion-android/      aplicativo IdeiaRec para a ponte BLE
gateway/                ferramentas Google Apps Script e integrações
```

## Compilar e gravar

Projeto [PlatformIO](https://platformio.org/), framework Arduino. Os perfis
de `platformio.ini` têm finalidades diferentes:

| Ambiente | Uso | USB CDC | Firmware |
|---|---|---|---|
| `esp32-s3-devkitc-1` (padrão) | Depuração com cabo conectado | Ligado | `C:/pio-builds/ESP32_EINK/esp32-s3-devkitc-1/firmware.bin` |
| `battery` | Uso normal fora do cabo | Desligado | `C:/pio-builds/ESP32_EINK/battery/firmware.bin` |

```powershell
pio run -e esp32-s3-devkitc-1  # compila debug, sem gravar
pio run -e battery             # compila para bateria, sem gravar
pio device monitor -e esp32-s3-devkitc-1  # abre serial; aparelho acordado e USB conectado
```

O build fica em **`C:/pio-builds/ESP32_EINK`**, fora do caminho com acentos
e da pasta sincronizada pelo Google Drive. Isso evita falhas do linker MinGW
com caminhos acentuados no Windows. O wrapper abaixo exige esse diretório e
o layout atual; não altere o caminho sem revisar também a ferramenta.

### Atualização preservando notas

Use `tools/power_test.ps1` em PowerShell 7. Sem argumentos, ele só mostra
**Help**, sem Python, build ou hardware. `Build` compila apenas o perfil
selecionado; `Flash` exige porta explícita, recompila esse perfil e só prossegue
após sucesso. O script requer dependências já instaladas e cache Opus validado;
não instala nem baixa dependências. Não execute junto com outros builds/runners.

```powershell
pwsh -NoProfile -File tools/power_test.ps1
pwsh -NoProfile -File tools/power_test.ps1 -Action Build -Profile battery
# Simulação: não abre COM nem compila/grava.
pwsh -NoProfile -File tools/power_test.ps1 -Action Flash -Profile battery -Port COM7 -DryRun
```

Antes de uma gravação real, exporte as notas, preserve um binário baseline
compatível fora da pasta de build e confira porta, layout e alimentação.
Exemplo para execução deliberada, com um Python confiável que tenha **esptool v5**:

```powershell
$esptoolPython = 'C:/CAMINHO_REAL/Scripts/python.exe'
pwsh -NoProfile -File tools/power_test.ps1 -Action Flash -Profile battery -Port COM7 -EsptoolPython $esptoolPython
```

`COM7` é exemplo, não autodetecção. O parâmetro `-EsptoolPython` recebe o
executável Python, não o executável esptool. A atualização escreve **somente
a aplicação em `0x10000`** e executa `verify-flash` no mesmo endereço, sem
gravar/apagar NVS, LittleFS, bootloader ou tabela de partições. Há apagamento
setorial normal na faixa da aplicação, não erase completo.

**Não use `pio run -t upload` como atualização de um aparelho com notas:**
o upload normal pode gravar bootloader/tabela de partições e outros artefatos,
não apenas a aplicação. Layout diferente exige análise própria, não migração
automática. Procedimento, rollback e limites de validação estão em
[docs/VALIDACAO_ENERGIA_WAKE.md](docs/VALIDACAO_ENERGIA_WAKE.md).

Instale **battery antes de usar sem cabo**. O perfil debug inicializa o USB
CDC nativo e não é o perfil para testar operação ou consumo na bateria.
No perfil battery, não espere console USB.

O particionamento (`partitions.csv`) reserva 3 MiB para o firmware e
4,75 MiB na versão V2 da placa para o LittleFS, onde ficam
as gravações — sem cartão SD isso dá poucos minutos de áudio no total.
Existe uma reserva para manter o filesystem e a recuperação operacionais:
espaço livre não é necessariamente espaço gravável. Ao atingir a reserva,
novas notas são recusadas sem apagar as anteriores. A sincronização não remove
automaticamente as cópias locais; exclusão é uma ação explícita do usuário.

## Sincronização resiliente

### Salvamento de gravações maiores

- Novas notas armazenam um cabeçalho de 44 bytes em `.wav` e o áudio em
  `.pcm`, escrito apenas por append. Parar a gravação atualiza atomicamente
  somente o cabeçalho; não reescreve megabytes de áudio no LittleFS.
- Reprodução, transcrição, hash, Drive e download HTTP recebem um WAV completo
  e canônico por `NoteFiles::openRead`. Copiar só o `.wav` físico de uma imagem
  da flash não exporta o áudio novo: use o download HTTP/Drive ou conserve
  também o `.pcm`. Notas antigas com WAV completo continuam funcionando.
- O writer chama `flush` a cada aproximadamente um segundo de áudio escrito.
  Após interrupção, a leitura reconstrói duração/cabeçalho a partir das amostras
  completas persistidas. A cauda ainda em RAM/I2S pode ser perdida; checkpoints
  não equivalem à proteção de todas as amostras capturadas contra falta de energia.
- WAVs antigos com cabeçalho placeholder e PCM persistido são recuperados na
  leitura, sem reescrever o original. Um arquivo apenas com 44 bytes não contém
  áudio recuperável. Arquivos inválidos aparecem como `sem audio`, fora do lote
  normal; uma falha de escrita confirmadamente vazia é limpa ao finalizar.
- A tela `Salvando` acompanha o dreno sem bloquear o loop principal. Sono,
  operações concorrentes de notas e atendimento HTTP ficam suspensos enquanto
  captura/finalização possuem o arquivo; o writer cede CPU entre blocos.
- Falta de espaço encerra e preserva o prefixo. Saturação do buffer interrompe
  explicitamente a sessão, sem continuar descartando trechos silenciosamente.
- Exclusões usam `.wav.delete` e são retomadas no boot caso tenham sido interrompidas.

Regressões e limites de validação:
[test/VALIDACAO_GRAVACAO_LONGA.md](test/VALIDACAO_GRAVACAO_LONGA.md).

- O WAV é finalizado e validado antes de transcrever. TXT/Markdown são
  escritos em arquivos temporários, verificados e promovidos ao destino.
- A transcrição pronta é reutilizada quando falta apenas Markdown. O
  modelo efetivamente utilizado e o último erro ficam no irmão `.ai`.
- O Drive usa IDs pré-gerados persistidos, sessões completas e progresso
  salvo para WAV/TXT/MD no irmão `.sync` (schema v3). Após queda de conexão,
  consulta o servidor antes de reenviar bytes. Conclusão exige GET remoto
  com tamanho e MD5 iguais ao local, além de persistência local bem-sucedida.
- Marcadores antigos `.snc`/`.sync` são reconciliados por nome, pasta,
  tamanho e hash. Nova autorização exige nova verificação do destino.
- **Sincronizar** processa somente notas pendentes. O estado, tamanho e hash
  locais selecionam a fila; notas já confirmadas não são consultadas no Drive
  nem incluídas no contador. Se apenas uma nota estiver pendente, a barra
  mostra **1 / 1** na transcrição, no Markdown e no envio.
- **Sincronizar esta**, no detalhe de uma nota, permite conferir novamente o
  backup remoto daquela nota e recuperar arquivos/pasta apagados.
- O portal oferece **Refazer autorização do Drive**. Alterar as credenciais
  OAuth invalida a autorização anterior; PWR cancela a espera de pareamento.
- A tela final informa a etapa/erro e reinicia o contador de inatividade.

### Performance e duas barras

- Operações de IA/Drive executam em um worker FreeRTOS. A UI continua atendendo
  botões e atualiza o e-paper fora do caminho crítico da transferência.
- A barra superior mostra o progresso ponderado da nota atual. Os pesos usam
  bytes/duração do áudio e um histórico local por modelo/velocidade. `~` indica
  percentual estimado; 100% exige a confirmação final dos arquivos.
- A barra inferior mostra **notas concluídas / notas pendentes no lote**.
  As antigas já sincronizadas não entram no lote. PWR longo pede pausa após
  a etapa atual; uma chamada HTTPS em andamento termina ou atinge seu prazo.
- As regiões de texto/barras são fixas no painel 200x200. A UI de sincronização
  atualiza a cada 2 segundos ou mudança de fase/nota, sem refresh por bloco de rede.
- O Drive reserva os IDs de arquivos em lote, usa multipart até 5 MiB,
  reutiliza HTTPS/token/pasta e verifica MD5 na resposta final (GET em caso
  incompleto/ambíguo). Arquivos maiores usam blocos retomáveis de 1 MiB.
- Áudios até 2 MiB tentam Transcribe inline em base64 por streaming. Se a API
  recusar especificamente esse formato, o firmware usa Files URI no mesmo
  modelo e guarda a incompatibilidade por 24 horas. Chave/cota inválida não
  é tratada como incompatibilidade de formato.
- Capacidades/modelos são armazenados no NVS por fingerprint da chave e
  configuração, com validade de 24 horas; nenhuma chave original vai ao cache.
- Gemini 3.8 Flash usa `thinkingLevel: LOW`; Lite/legados não recebem esse campo.
- Wi-Fi tenta a favorita/última rede diretamente antes do scan completo.
  O NTP é iniciado sem a espera bloqueante de até 8 segundos e aplicado pelo loop.
- Temporários Files são limpos separadamente, com orçamento total de 15 segundos
  por lote. A fila cheia tenta liberar capacidade; falha de limpeza não apaga
  transcrição/Markdown nem altera a confirmação dos arquivos já entregues.
- Estados/metadados locais são lidos por blocos com tamanho conhecido, evitando
  o custo de `readString()` byte a byte. Falha de envio inline tenta Files URI
  no mesmo modelo; bloqueio temporário desse transporte não vira incompatibilidade
  permanente nem dispara a mesma tentativa inline em todos os modelos.

Cada nota recebe um irmão **`.perf`** com tempos por etapa, bytes, requests HTTP,
handshakes TLS e modelos efetivos, sem chaves/tokens/corpos de áudio. A serial
também publica `[Perf]`. Esse diagnóstico e o histórico são auxiliares: uma
falha em salvá-los não provoca nova inferência de uma nota já entregue.
Validação em hardware: [test/VALIDACAO_PERFORMANCE.md](test/VALIDACAO_PERFORMANCE.md)
(tempos medidos e limites da comparação, incluindo variação de latência do
Markdown no serviço).

### Memória interna e futuro microSD

`src/storage/note_files.*` centraliza o backend das notas (LittleFS nesta
versão). São reservados 256 KiB para textos/metadados; o tempo de gravação
considera o espaço disponível e a gravação para antes de consumir essa
reserva. WAV mono 16-bit a 16 kHz consome 32.000 bytes/s. A partição inteira
equivale a aproximadamente 2,6 minutos antes da reserva, arquivos existentes
e overhead. PSRAM guarda buffers temporários, não substitui armazenamento.
**Pode não haver espaço gravável mesmo com notas já sincronizadas:** sincronizar
não apaga os arquivos locais. Exporte/confira o backup e exclua notas pelo menu
para liberar espaço; a reserva de metadados não está disponível para áudio novo.

O backend permite integração futura de SD, que ainda não está implementada.
A transcrição das **notas** ocorre depois da gravação; o assistente PTT é um
fluxo Live separado, não uma transcrição contínua das notas.
Textos têm capacidade de 128 KiB em PSRAM, com resposta HTTP limitada a
256 KiB. O buffer do visualizador é alocado sob demanda em PSRAM. Isso permite
processar textos maiores, mas não comprova gravação de uma hora na flash nem
uma implementação futura de microSD ou compressão.

### Verificações e diagnóstico USB

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/check_project.py" --cppcheck
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/check_integrations.py"
# Opcional: inclui build, testes e lint do aplicativo Android.
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/check_integrations.py" --android
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1 -e battery
```

`check_project.py` aceita `--cjson-dir CAMINHO` opcional, apontando uma pasta
com **`cJSON.c` e `cJSON.h` oficiais da versão 1.7.17** já disponíveis.
Exemplo, se esse cache existir: `--cjson-dir test/host/.build/v1.7.17`.
Sem o parâmetro, procura fontes locais e pode baixar a versão fixada se faltar.
Os runners são verificações de software, não ensaios de hardware ou de corrente.

Os testes host exercitam parsers reais, arquivos/estado, persistência NVS,
captura/DSP e protocolo Drive com rede simulada; detalhes em
[test/host/README.md](test/host/README.md). Leia também os guias de
[gravação longa](test/VALIDACAO_GRAVACAO_LONGA.md),
[sincronização](test/VALIDACAO_SYNC.md),
[performance](test/VALIDACAO_PERFORMANCE.md) e
[energia/wake](docs/VALIDACAO_ENERGIA_WAKE.md) para os limites dos ensaios.

Para testes com o firmware de depuração e cabo conectado, **acorde o ESP32
antes de cada sessão**; o console não tenta reconectar indefinidamente:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" "stayawake 1" --timeout 3
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" status --timeout 3
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" sync-one --timeout 420
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" "stayawake 0" --timeout 3
```

`status` mostra espaço, modelos, flags e erros sem chaves/tokens. Durante um
lote, consulta o snapshot do worker e não acessa arquivos sendo escritos. `sync-one`
processa a nota mais recente; `sync` processa somente as pendentes. As chamadas são
assíncronas e têm tentativas e prazos limitados; `[DiagDone] sync`/`sync-one`
somente aparece ao terminar o worker. O botão não cancela inferência TLS em
andamento. O ambiente `battery` é o destinado a uso fora do cabo.

## Limitações conhecidas

- **Sem cartão SD ainda**: o slot existe (pinagem em `src/config/pins.h`),
  mas o firmware só grava na flash interna; o espaço total de áudio é pequeno.
- **Sem cartão SD**, cada nota compete por espaço com as demais — o
  tempo gravável depende da taxa, espaço livre, reserva e overhead; não há
  capacidade ilimitada por usar PSRAM ou sincronizar com o Drive.
- **Refresh token do Google expira em 7 dias** se a tela de
  consentimento OAuth do seu projeto ficar em modo "Testing" — publique
  em produção (passo 4 acima) para evitar reautorizar toda semana.
- **TLS sem verificação de certificado** (`setInsecure()`): como o
  endpoint de transcrição é configurável pelo usuário, o firmware não
  fixa uma CA específica nesses clientes. A conexão é cifrada, mas não
  autentica o servidor por certificado; há risco de interceptação ativa,
  inclusive de credenciais e áudio. Não trate esse modo como TLS validado.
- **Validação física limitada:** feedback assíncrono e testes host não provam
  resposta óptica instantânea, captura da primeira sílaba, corrente em deep
  sleep ou autonomia. Consulte os registros de bancada antes de extrapolar.
