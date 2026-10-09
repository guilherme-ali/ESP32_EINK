# Gravador de Ideias

Firmware para transformar a placa **Waveshare ESP32-S3-ePaper-1.54** num
gravador de notas de voz de bolso: aperta um botão, fala, e a nota é
salva, transcrita automaticamente e sincronizada com uma pasta no seu
Google Drive — tudo sem precisar de celular ou computador por perto.

Quando parado por um tempo, a tela mostra uma ilustração de fundo e o
dispositivo entra em deep sleep de verdade (a alimentação do e-paper e
do USB são desligadas; a imagem continua visível porque e-paper não
gasta energia para manter o que já está desenhado).

## Hardware

- **Placa:** [Waveshare ESP32-S3-ePaper-1.54](https://www.waveshare.com/esp32-s3-epaper-1.54.htm),
  versão V2 (chip ESP32-S3-PICO-1-N8R8: 8 MB flash + 8 MB PSRAM octal)
- **Tela:** e-paper 1.54" 200×200 preto/branco, controlador SSD1681
- **Áudio:** codec ES8311 (microfone e alto-falante no mesmo chip),
  ligado em I2S
- **Relógio:** RTC PCF85063 (I2C), usado para carimbar hora nos arquivos
- **Armazenamento:** flash interna via LittleFS — o slot de cartão
  microSD existe na placa mas ainda não é usado pelo firmware
- **Botões:** BOOT e PWR (os únicos dois disponíveis)

A pinagem completa está em `src/config/pins.h`.

## Funcionalidades

- Gravação de voz em WAV PCM 16-bit, 8 kHz mono, sem compressão
- Lista de notas na tela com horário e duração, navegável e reproduzível
- Transcrição automática (Gemini por padrão; qualquer API compatível
  com o formato da OpenAI também funciona — Groq, OpenAI, etc.)
- Sincronização automática do áudio + transcrição para uma pasta
  própria ("Gravador de Ideias") no Google Drive, usando o escopo
  `drive.file` (o app só enxerga o que ele mesmo cria — nada mais no
  seu Drive)
- Portal de configuração via Wi-Fi (Rede, chave de transcrição,
  credenciais do Google Drive) — nada fica fixo no código
- Tela de descanso com ilustrações geradas e deep sleep entre usos

## Como usar

| Botão | Na lista de notas | Gravando |
|---|---|---|
| **BOOT** (clique) | Começa a gravar uma nota nova | Para e salva a nota |
| **PWR** (clique) | Passa para a próxima nota da lista | — |
| **PWR** (segurar) | Reproduz a nota selecionada | — |

Depois de gravar, se houver Wi-Fi configurado, a tela mostra
"Transcrevendo..." e depois "Sincronizando..." antes de voltar para a
lista — cada etapa é pulada silenciosamente se não houver rede ou
configuração (a nota sempre fica salva localmente).

Sem interação por alguns minutos (padrão: 2 min), o dispositivo mostra
uma ilustração de fundo e desliga; qualquer clique em BOOT acorda de
volta para a lista de notas.

## Primeira configuração

1. **Grave o firmware** (veja a seção de build abaixo) e ligue o
   dispositivo.
2. **Wi-Fi:** sem rede salva, ele sobe um Access Point próprio
   (`IdeiaRec-XXXX`, sem senha) e mostra o nome + IP na tela. Conecte
   um celular ou notebook nessa rede e acesse o endereço mostrado
   (`http://192.168.4.1`) para digitar o SSID e a senha da sua rede de
   casa. O dispositivo reinicia e conecta sozinho da próxima vez.
3. **Transcrição:** com o Wi-Fi já configurado, o mesmo portal
   continua acessível pelo IP que a tela mostra na lista de notas
   (`config: <ip>`). Preencha:
   - **Endpoint**: `https://generativelanguage.googleapis.com` (Gemini)
     ou a URL de `.../audio/transcriptions` de um provedor
     OpenAI-compatível (ex.: Groq)
    - **Seleção automática**: transcrição com `gemini-3.5-transcribe`,
      seguida de `gemini-3.8-flash` e `gemini-3.5-flash-lite` quando
      necessário; Markdown com 3.8 Flash e alternativa Flash-Lite.
      No modo manual, os campos **Modelo STT** e **Modelo do Markdown**
      são independentes. Para Groq, use um modelo Whisper para STT.
    - **Projeto gratuito confirmado**: confirme que a chave pertence
      ao tier gratuito, sem faturamento pago. Os nomes dos modelos não
      comprovam o plano da conta; o firmware utiliza uma lista restrita
      de modelos e deixa a nota pendente ao esgotar as opções permitidas.
   - **API key**: gerada em [aistudio.google.com/apikey](https://aistudio.google.com/apikey)
     (Gemini, gratuito) ou no console do provedor escolhido
4. **Google Drive** (opcional, para sincronizar): crie um projeto no
   [Google Cloud Console](https://console.cloud.google.com), ative a
   *Google Drive API*, configure a tela de consentimento OAuth
   (externo, escopo `drive.file`, publicada em produção para o token
   não expirar a cada 7 dias) e crie uma credencial do tipo **"TVs e
   dispositivos de entrada limitados"**. Coloque o Client ID e Client
   Secret no mesmo portal — o dispositivo reinicia e mostra um código
   de pareamento na tela; acesse o link indicado em outro aparelho,
   faça login e digite o código para autorizar.

## Estrutura do projeto

```
src/
  main.cpp            máquina de estados do app e integração de tudo
  config/pins.h        pinagem da placa, um lugar só
  board/                energia, botões (multi_button), RTC PCF85063
  display/epaper.{h,cpp} driver do SSD1681 (portado do repo oficial
                          da Waveshare — LUTs de waveform do painel)
  ui/                   texto/formas sobre o framebuffer (canvas.cpp),
                         fonte 5x7 e os bitmaps da tela de descanso
  audio/                codec ES8311 (I2S), gravador e tocador de WAV
  storage/notes.{h,cpp} listagem e nomeação das notas em /notes/
  net/                  settings (NVS), portal Wi-Fi, cliente de
                         transcrição e cliente do Google Drive
lib/es8311/             driver do codec (tabela de clock portada do
                         esp_codec_dev — a parte arriscada de reescrever)
tools/img2header.py     converte uma imagem em bitmap 1bpp para a
                         tela de descanso (dithering Floyd-Steinberg)
```

## Compilar e gravar

Projeto [PlatformIO](https://platformio.org/), framework Arduino.

```
pio run                 # compila
pio run -t upload        # grava (porta configurada em platformio.ini)
pio device monitor        # abre o monitor serial
```

Se o caminho do repositório tiver acentos (como neste caso, dentro de
"Meu Drive"), o linker do MinGW pode falhar ao gravar `firmware.map` —
por isso o `platformio.ini` redireciona o diretório de build para
`C:/pio-builds/ESP32_EINK` (fora do caminho acentuado). Ajuste ou
remova essa linha se não for o seu caso.

O particionamento (`partitions.csv`) reserva ~3 MB para o firmware e
o resto (~4,75 MB na versão V2 da placa) para o LittleFS, onde ficam
as gravações — sem cartão SD isso dá poucos minutos de áudio no total.

## Sincronização resiliente

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
  nem incluídas no contador. Se seis notas estão prontas e uma é nova, a barra
  mostra **1 / 1** na transcrição, no Markdown e no envio.
- **Sincronizar esta**, no detalhe de uma nota, permite conferir novamente o
  backup remoto daquela nota e recuperar arquivos/pasta apagados.
- O portal oferece **Refazer autorização do Drive**. Alterar as credenciais
  OAuth invalida a autorização anterior; PWR cancela a espera de pareamento.
- A tela final informa a etapa/erro e reinicia o contador de inatividade.

### Performance e duas barras

- Operacoes de IA/Drive executam em um worker FreeRTOS. A UI continua atendendo
  botoes e atualiza o e-paper fora do caminho critico da transferencia.
- A barra superior mostra o progresso ponderado da nota atual. Os pesos usam
  bytes/duracao do audio e um historico local por modelo/velocidade. `~` indica
  percentual estimado; 100% exige a confirmacao final dos arquivos.
- A barra inferior mostra **notas concluidas / notas pendentes no lote**.
  As antigas ja sincronizadas nao entram no lote. PWR longo pede pausa apos
  a etapa atual; uma chamada HTTPS em andamento termina ou atinge seu prazo.
- As regioes de texto/barras sao fixas no painel 200x200. A UI atualiza a cada
  2 segundos ou mudanca de fase/nota, sem refresh por cada bloco de rede.
- O Drive reserva os IDs de arquivos em lote, usa multipart ate 5 MiB,
  reutiliza HTTPS/token/pasta e verifica MD5 na resposta final (GET em caso
  incompleto/ambiguo). Arquivos maiores usam blocos retomaveis de 1 MiB.
- Audios ate 2 MiB tentam Transcribe inline em base64 por streaming. Se a API
  recusar especificamente esse formato, o firmware usa Files URI no mesmo
  modelo e guarda a incompatibilidade por 24 horas. Chave/cota invalida nao
  e tratada como incompatibilidade de formato.
- Capacidades/modelos sao armazenados no NVS por fingerprint da chave e
  configuracao, com validade de 24 horas; nenhuma chave original vai ao cache.
- Gemini 3.8 Flash usa `thinkingLevel: LOW`; Lite/legados nao recebem esse campo.
- Wi-Fi tenta a favorita/ultima rede diretamente antes do scan completo.
  O NTP deixa de bloquear a conexao por ate 8 segundos e e aplicado pelo loop.
- Temporarios Files sao limpos separadamente, com orcamento total de 15 segundos
  por lote. A fila cheia tenta liberar capacidade; falha de limpeza nao apaga
  transcricao/Markdown nem altera a confirmacao dos arquivos ja entregues.
- Estados/metadados locais sao lidos por blocos com tamanho conhecido, evitando
  o custo de `readString()` byte a byte. Falha de envio inline tenta Files URI
  no mesmo modelo; bloqueio temporario desse transporte nao vira incompatibilidade
  permanente nem dispara a mesma tentativa inline em todos os modelos.

Cada nota recebe um irmao **`.perf`** com tempos por etapa, bytes, requests HTTP,
handshakes TLS e modelos efetivos, sem chaves/tokens/corpos de audio. A serial
tambem publica `[Perf]`. Esse diagnostico e o historico sao auxiliares: uma
falha em salva-los nao provoca nova inferencia de uma nota ja entregue.
Validacao em hardware: `test/VALIDACAO_PERFORMANCE.md` (tempos medidos e limites
da comparacao, incluindo variacao de latencia do Markdown no servico).

### Memória interna e futuro microSD

`src/storage/note_files.*` centraliza o backend das notas (LittleFS nesta
versão). São reservados 256 KiB para textos/metadados; o tempo de gravação
considera o espaço disponível e a gravação para antes de consumir essa
reserva. WAV mono 16-bit a 16 kHz consome 32.000 bytes/s. A partição inteira
equivale a aproximadamente 2,6 minutos antes da reserva, arquivos existentes
e overhead. PSRAM guarda buffers temporários, não substitui armazenamento.

O backend permite integração futura de SD, que ainda não está implementada.
Não há transcrição Live; o arquivo é processado depois da gravação.
Textos agora tem capacidade de 128 KiB em PSRAM, com resposta HTTP limitada a
256 KiB. O buffer do visualizador e alocado sob demanda em PSRAM. Isso prepara
o processamento de texto longo; a gravacao de uma hora ainda depende da etapa
microSD e validacao de captura/compressao, que nao faz parte do teste em flash.

### Verificações e diagnóstico USB

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/check_project.py" --cppcheck
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -e esp32-s3-devkitc-1 -e battery
```

Os testes host exercitam os parsers reais, arquivos/estado, persistência NVS
e protocolo Drive com rede simulada; detalhes em `test/host/README.md`.

Para testes com o firmware de depuração e cabo conectado, **acorde o ESP32
antes de cada sessão**; o console não tenta reconectar indefinidamente:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" "stayawake 1" --timeout 3
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" status --timeout 3
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" sync-one --timeout 420
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "tools/device_console.py" "stayawake 0" --timeout 3
```

`status` mostra espaço, modelos, flags e erros sem chaves/tokens. Durante um
lote, consulta o snapshot do worker e nao acessa arquivos sendo escritos. `sync-one`
processa a nota mais recente; `sync` processa somente as pendentes. As chamadas são
assíncronas e têm tentativas e prazos limitados; `[DiagDone] sync`/`sync-one`
somente aparece ao terminar o worker. O botão não cancela inferência TLS em
andamento. O ambiente `battery` é o destinado a uso fora do cabo.

## Limitações conhecidas

- **Sem cartão SD ainda**: o slot existe (pinagem em `pins.h`) mas o
  firmware só grava na flash interna, então o espaço total de áudio é
  pequeno. Trocar por gravação em SD é o próximo passo natural quando
  o cartão chegar.
- **Sem cartão SD**, cada nota compete por espaço com as demais — o
  app mostra o tempo livre restante durante a gravação.
- **Refresh token do Google expira em 7 dias** se a tela de
  consentimento OAuth do seu projeto ficar em modo "Testing" — publique
  em produção (passo 4 acima) para evitar reautorizar toda semana.
- **TLS sem verificação de certificado** (`setInsecure()`): como o
  endpoint de transcrição é configurável pelo usuário, o firmware não
  fixa uma CA específica. Aceitável para uso doméstico; não é o ideal
  para uma rede não confiável.
