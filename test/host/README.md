# Testes host executáveis

Na raiz do projeto, em PowerShell:

```powershell
& "C:\Users\guilh\.platformio\penv\Scripts\python.exe" "tools/check_project.py"
# Análise estática adicional, se cppcheck estiver disponível:
& "C:\Users\guilh\.platformio\penv\Scripts\python.exe" "tools/check_project.py" --cppcheck
```

O checker executa `git diff --check`, compila e executa os testes. Retorna `1`
para falha de whitespace, compilação, teste ou lint solicitado. O projeto não
possuía script de linter ou runner host antes deste checker. Não usa PlatformIO,
upload serial, nem instala pacotes Python.

## Código efetivamente testado

- `src/net/http_client.cpp`: `readResponse`, `retryDelayMs`, `writeAll` e
  `writeFileChunk`, com fragmentação e passagem determinística do tempo.
- `src/net/json_utils.cpp`: `jsonGetString` e `jsonGetLong` com cJSON real.
- `src/net/gemini_client.cpp`, **arquivo inteiro**: parsers `SttNet`, transporte,
  limites, UTF-8, Flash (`extractGemini`, `interaction=false`), Interactions,
  compatibilidade, retry e escrita de WAV/base64.
- `src/net/stt.cpp`, **arquivo inteiro**: chamadas públicas de `SttClient` para
  provedores compatíveis, usando filesystem e rede em memória.
- `src/net/settings.cpp`: carga/migrações, política gratuita, persistência de
  STT/Drive/política AI, limites 512/513 do refresh token e geração de autorização.
- `src/storage/note_files.cpp`: implementação real de `fs`, espaço/reserva,
  `readText`, `validText`, `validWav`, `writeAtomic` e `recoverText`.
- `src/net/gdrive.cpp`: save/load/migração de `NoteSyncState` v3, consultas
  conservadoras, OAuth, IDs em lote, multipart binário, retomável em blocos de
  1 MiB, PATCH, TTL de token/pasta e confirmação estrita de tamanho/hash.
- `src/sync/telemetry.cpp`: relógio, wrap, fases, esperas e contadores reais.
- `src/sync/progress_model.cpp`: modelo real em build separado **sem SDK/mocks**.

São quatro suítes: HTTP/STT/telemetria, Storage/Drive, integração Gemini e
progresso portátil. As três primeiras compilam os nove `.cpp` reais acima.
Gemini executa cada caso em processo novo: `endSession()` fecha TLS, mas preserva
cache/fila de produção; os testes de reutilização fazem múltiplas chamadas no
mesmo caso. Não há reset fictício nem alteração nos clientes. `main`/app não são
compilados. Não há medição percentual de cobertura de linhas/branches.

O link usa `-ffunction-sections -fdata-sections -Wl,--gc-sections`.
Fixtures são bytes HTTP/JSON independentes; não existem cópias dos parsers de
produção nem testes por regex sobre código-fonte. Novos comportamentos no HTTP
devem ganhar casos e exigir nova execução do checker: ele recompila sempre e
detecta alterações concorrentes em **todos** os `.cpp`/`.h` sob `src/`, inclusive
adição/remoção de arquivos, durante a execução.

## Dependência oficial

O SDK instalado tem o cabeçalho cJSON 1.7.17, mas o host precisa do código C,
não de uma biblioteca pré-compilada para ESP32. A descoberta procura um par
`cJSON.c`/`cJSON.h` em `test/host/vendor/cJSON`, `.pio`, `sdk` e frameworks locais
do PlatformIO. Se não encontrar, baixa **v1.7.17 do upstream DaveGamble/cJSON**
para `test/host/.build/v1.7.17/`. O cache permite executar offline depois.
Os arquivos oficiais mantêm os avisos/licença originais.

Para fornecer outra cópia oficial local, inclusive offline:

```powershell
& "C:\Users\guilh\.platformio\penv\Scripts\python.exe" "tools/check_project.py" --cjson-dir "C:\caminho\cJSON"
```

`--cxx` ou `CXX` substitui o compilador; `CJSON_DIR` substitui a dependência.

## Escopo dos fakes

- `String`: bytes e operações usadas por esses arquivos, incluindo `concat`
  com comprimento explícito. A alocação host não simula falta de memória.
- `WiFiClientSecure`: liberação de fragmentos programada pelo relógio fake,
  EOF separado de conexão persistente, timeout, escritas curtas e captura dos
  pedidos. Respostas de sucesso aguardam os bytes declarados no Content-Length
  da requisição; erros podem chegar antes de terminar o áudio. Não interpreta
  JSON nem faz TLS real. Cada request captura sua própria entrada em
  `Host::requests`, independente do socket TLS. A próxima Wire é ativada somente
  após consumir a resposta e escrever o próximo header. `connectionHosts` conta
  handshakes reais do mock; `disconnectAfterRead` força EOF de leitura parcial.
- `File`/`fs::FS`/`LittleFS`: bytes compartilhados entre handles, reads/escritas
  curtos, seek, flush, existência, rename com replacement indivisível e espaço
  contabilizado em memória. `NoteFiles::fs()` agora é a implementação real.
  `faults` permite falhar open/rename por caminho, limitar reads/write,
  interromper leitura com orçamento e corromper bytes no flush. Não simula
  journal, desgaste nem perda física de energia; capacidade controla os
  checks de admissão, não a alocação física de blocos.
- `Preferences`: fake NVS tipado por namespace, `isKey`, strings vazias,
  contagens de bytes dos puts e limite das strings lidas sem truncamento.
  `Host::failPuts` usa chaves `cfg/nome`; `failNvsBegin` falha abertura.
  `rebootNvs()` invalida handles mantendo dados; `resetNvs()` limpa tudo.
  Sem transação multi-chave fake: falha numa chave mantém puts anteriores.
- `MD5Builder`: **não é MD5 nem criptografia**. FNV-1a 32-bit repetido quatro
  vezes em hexadecimal; vetor explícito `abc` →
  `1a47e90b1a47e90b1a47e90b1a47e90b`. `Host::mockFileHash` gera expectativas
  conhecidas, inclusive para fontes modificadas sem alterar o tamanho. Testa
  comparação/persistência/verificação de hash, nunca validade criptográfica.
- `Host::onConnect` (nome legado, hook **por request**) inspeciona estado persistido:
  reserva antes do POST, sessão antes de PUT e ACK antes do GET final; falhas
  de commit bloqueiam a próxima mutação remota.
- `heap_caps`: malloc/free host, sem PSRAM real.
- `mbedtls_sha256_ret`: **falha por padrão**; integração habilita explicitamente
  `Host::fakeShaEnabled` e fingerprint determinístico por chave, **não SHA real**.
  Exercita associação do cache à chave e snapshot NVS sem persistir a chave.

## Regressões cobertas

O checker exige sucesso em todos os casos registrados e informa contagens reais.
Resultado atual: HTTP/STT 56, Storage/Drive 58, Gemini integrado 26 e progresso
portatil 15: **155 testes**. Gemini tambem cobre fila Files com quatro uploads,
fila restaurada com epoch zero/socket fechado, orcamento agregado de limpeza,
troca de fingerprint, timestamp futuro e separacao de bytes por fase.
Leitura de metadados em blocos e exercitada com arquivo parcial/grande e contador
de chamadas, evitando `readString()` byte a byte. O fake representa timeout zero
de File no core instalado, nao o timeout padrao de Stream. Ha cobertura de WAV
base64 com muitos blocos e fallback de falha inline para URI no mesmo modelo.
Os casos anteriores foram preservados, com fixtures ajustadas à nova sequência
multipart/IDs em lote. Um trio WAV/TXT/MD usa **9 requests**, conservando os três
IDs antes do primeiro POST e recusando hash errado até na última resposta.
Inclui resposta perdida → GET do mesmo ID; fontes alteradas durante/depois do
upload; falhas de commit; 401 reativo; invalid_grant/NVS; 308/Range; keep-alive
misto 2xx/4xx/5xx e erros antecipados de mídia. `cppcheck` analisa os nove arquivos
reais; não há `xfail` nem supressão dessas regressões. Uma falha retorna 1.
O fake de filesystem também recusa rename de arquivos abertos, conforme
observado no LittleFS físico durante a atualização dos metadados `.ai`.
Testes de resposta antecipada preservam HTTP 403/429 após envio interrompido
e impedem tratar resposta 200 como confirmação de um corpo não finalizado.

Gemini cobre inline REST/Interactions com JSON e base64 reais; fallback de inline
400 ou 200 vazio para Files URI no mesmo modelo preferido; LOW somente em 3.8;
cache de discovery/NVS/TTL, rotação de chave e bloqueio de conta; limpeza adiada
ou limitada a 15 s após a entrega sem alterar diagnóstico. O modelo cobre pesos
80% divididos entre destinos, retries sem bytes duplicados, cap estimado 95%,
esperas congeladas, ETA/overdue/wrap, reutilização TXT/MD e 99% até `verified()`.

Não exercitados end-to-end: slow_down/cancelamento/expiração OAuth, criação e
paginação de pastas, concorrência real, TLS/MD5/SHA reais, NVS/LittleFS físicos e
hardware. Setters antigos de Wi-Fi/áudio/UI não têm cobertura nesta suíte.
Regressões de produção pendentes estão em `test/host/REGRESSIONS.md`.

Executar novamente o checker depois de alterações nessas áreas.
