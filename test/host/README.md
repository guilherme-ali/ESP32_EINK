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
  conservadoras e jobs reais com OAuth, reserva de ID, upload retomável simples,
  PATCH de fonte alterada e GET final de tamanho/hash. Pareamento testa
  capacidades de token e falha de persistência.

As duas suítes são executáveis independentes: `host_tests.cpp` mantém os 48
casos anteriores; `storage_tests.cpp` acrescenta 32 casos. Ambas compilam os
sete `.cpp` reais acima, sem cópias das implementações de produção. Não há
medição percentual de cobertura de linhas/branches.

O link usa `-ffunction-sections -fdata-sections -Wl,--gc-sections`.
Fixtures são bytes HTTP/JSON independentes; não existem cópias dos parsers de
produção nem testes por regex sobre código-fonte. Novos comportamentos no HTTP
devem ganhar casos e exigir nova execução do checker: ele recompila sempre e
detecta alterações concorrentes nos `.cpp` de produção durante a execução.

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
  JSON nem faz TLS real.
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
- `Host::onConnect` permite inspecionar estado persistido antes de requests:
  reserva antes do POST, sessão antes de PUT e ACK antes do GET final; falhas
  de commit bloqueiam a próxima mutação remota.
- `heap_caps`: malloc/free host, sem PSRAM real.
- `mbedtls_sha256_ret`: **falha explicitamente**, sem criptografia simulada.
  Os parsers Gemini são reais; descoberta, ativação de chave, inferência remota
  e política de cache do cliente Gemini não são exercitadas end-to-end.

## Regressões cobertas

O checker exige sucesso em 51 testes HTTP/JSON/Gemini e 33 testes de
armazenamento/Drive/Settings. As falhas de framing HTTP, query `pageSize=100`,
persistência de autorização/política e migração de confirmação gratuita foram
corrigidas na produção; os testes mantêm suas expectativas originais.
Inclui o formato Interactions `steps[].content[]` e o formato legado `outputs[]`.
cppcheck analisa os sete arquivos reais; não há `xfail` nem supressão dessas
regressões. O checker retorna 1 diante de qualquer falha.
O fake de filesystem também recusa rename de arquivos abertos, conforme
observado no LittleFS físico durante a atualização dos metadados `.ai`.
Testes de resposta antecipada preservam HTTP 403/429 após envio interrompido
e impedem tratar resposta 200 como confirmação de um corpo não finalizado.

Não exercitados end-to-end: slow_down/cancelamento/expiração OAuth, criação e
paginação de pastas, 401 reativo, 308/Range e retomadas complexas de Drive,
chunks acima de 256 KiB, upload simultâneo do trio WAV/TXT/MD, concorrência real,
TLS, MD5 real, NVS/LittleFS físicos e hardware. Setters antigos de Wi-Fi/áudio/UI
não têm cobertura de persistência nesta suíte.

Executar novamente o checker depois de alterações nessas áreas.
