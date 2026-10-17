# Ponte socket TLS pelo telefone

## Integração

- Interface em `ble_relay.h`, namespace `PhoneRelay`: `begin`, `waitReady`,
  `ready`, `active`, `stop`, `open`, `write`, `read`, `available`, `peek`,
  `connected`, `close`, `lastError` e o opcional terminal `shutdown`.
- Alvo: Arduino ESP32 **2.0.17**, APIs NimBLE-Arduino verificadas no tag
  **2.5.1** (release upstream de 30/07/2026). O coordenador deve fixar exatamente
  a versão escolhida no gerenciamento de dependências. Não foi alterado o build.
- O adaptador `Client`/HTTP e a seleção Wi-Fi/telefone pertencem ao coordenador.
  O telefone abre TLS verificado em 443 e transporta bytes da aplicação já
  decifrados. Sync, OAuth, IDs e parsers HTTP continuam no firmware existente.
- `begin` inicializa somente na primeira chamada e anuncia sob demanda. O PIN
  vem do portal/NVS do coordenador: número 0..999999 apresentado com seis dígitos.
  Este módulo não persiste nem imprime PIN, host ou respostas. Mudança do PIN
  durante a vida da stack é recusada; reprovisionamento/revogação de bonds fica
  com o coordenador. Não habilitar logs de debug do stack SMP no produto.
- Lifecycle chamado serialmente em task do coordenador, fora de callbacks.
  `open`, `waitReady` e `write` podem esperar: executá-los no worker HTTP do
  coordenador, nunca no loop MAIN. Cada espera pública é limitada a 60 segundos.
  O worker interno só envia frames GATT e monitora ACKs; não executa HTTP/OAuth.
- Um escritor por canal. Escrita concorrente é recusada. Pode haver leitor
  simultâneo e escritores em outros canais. `write` retorna bytes enfileirados,
  não bytes confirmados por HTTP/TLS. Crédito exige ACK individual em ordem.
  Timeout/falha aborta e não há replay. ACK sem progresso por 30 s aborta mesmo
  se o último `write` já retornou.
- `read` não bloqueia e retorna no máximo um frame/parcela, ou -1 sem dados;
  `available` retorna o total em bytes; `peek` não consome. `CLOSE` normal preserva
  RX até EOF, incluindo ACK após a leitura final. `connected` continua verdadeiro
  enquanto esse EOF tiver bytes pendentes. `ERROR`/desconexão/sequência incorreta
  abortam e invalidam buffers. O erro fica em `lastError` como literal estático,
  sem eco do payload remoto.
- `close` é cancelamento explícito: libera o slot e invalida operações antigas.
  Havendo bytes pendentes, registra esse cancelamento em `lastError`. Canais com
  erro/EOF continuam reservados até `close`; uma falha de `open` libera seu slot.
- Falta de recursos, perda de assinatura, falha de notify e desconexão não causam
  perda/retransmissão silenciosa. Falhas do link invalidam todos os canais.

## Contrato GATT preservado

| Item | Valor |
| --- | --- |
| Service | `7fd00001-6f6e-4d69-9b18-8e8c49b5c100` |
| RX / write com resposta | `7fd00002-6f6e-4d69-9b18-8e8c49b5c100` |
| TX / notify | `7fd00003-6f6e-4d69-9b18-8e8c49b5c100` |
| MTU mínimo | 183 |
| Frame / header / payload máximos | 180 / 7 / 173 bytes |
| Canais / janela por direção | 4 / 8 |

Header: `[1 u8, type u8, channel u8, epoch u16LE, seq u16LE]`.
O tamanho da operação GATT determina o payload; não há length adicional ou CRC.
A validação detecta corrupção estrutural, não alterações arbitrárias em DATA
binário (não foi adicionado checksum ao contrato).

`OPEN=1` leva porta u16LE (apenas 443) e hostname UTF-8 de até 171 bytes, sem
NUL/controles/espaços. `OPENED=2` não tem payload e usa seq 0. `DATA=3` aceita
0..173 bytes. `ACK=4` não tem payload e identifica exatamente um DATA totalmente
consumido, sem comportamento cumulativo. `CLOSE=5` não tem payload. `ERROR=6`
contém até 173 bytes UTF-8. OPEN/OPENED não consomem créditos de DATA. OPEN sai
com seq 0; as duas sequências DATA iniciam em 0 e avançam independentemente
módulo 65536. Epoch avança a cada abertura, módulo 65536, e não zera no stop.
Frames com epoch diferente são descartados antes da interpretação de sequência.
Tokens locais de geração adicionais cancelam tarefas/frames já enfileirados.
Epoch não é identidade persistente: reinício ocorre com link BLE desconectado.

Cada canal tem oito slots RX próprios. Leitura parcial não libera slot/crédito;
somente o consumidor que drena o frame agenda ACK. DATA vazio é trivialmente
consumido, mas nunca ultrapassa bytes anteriores ainda não lidos. Janela cheia,
duplicata ou lacuna abortam; não existe reorder/replay. CLOSE recebido com TX
sem ACK é falha explícita, não EOF bem-sucedido.

Ready exige um único telefone, TX subscribed, MTU >=183, criptografia,
autenticação MITM, bond e chave de 128 bits. `setSecurityAuth(true,true,true)`
mais `ble_hs_cfg.sm_sc_only=1` exige Secure Connections, sem fallback legado.
O bond armazenado também precisa indicar SC e autenticação, evitando aceitar
bond legado que tenha restado de outro firmware. RX tem WRITE_ENC/WRITE_AUTHEN;
TX tem READ_ENC/READ_AUTHEN e envio explicitamente condicionado a ready.
Segundo telefone é desconectado. Não se reinicia advertising após desconexão;
outra chamada `begin` pode solicitar uma nova tentativa.

## Rádio, memória e shutdown

`stop` cancela canais, para anúncios e desconecta. A stack e os objetos NimBLE
permanecem inicializados: custo de RAM retida, em troca de reutilização sem ciclos
inseguros de reinit. O worker e as arenas estáticas também ficam retidos; sem
alocação proporcional ao volume de HTTP. Ponteiro de frame da stack nunca é
armazenado e callbacks não esperam mutex/ACK/crédito. O lock de estado é curto;
chamadas NimBLE e esperas ficam fora dele.

`shutdown(timeoutMs)` é **terminal**: desliga rádio, encerra/deleta worker,
executa `deinit(false)` e, apenas após parada confirmada, `deinit(true)`. Isso
evita que o upstream apague objetos ainda em uso caso a parada falhe. Callbacks
estáticos são registrados sem transferência de ownership. Arenas estáticas do
codec/fila permanecem no BSS (não são vazamento), bonds permanecem em NVS.
Em falha/timeout, é permitido tentar shutdown novamente; begin é recusado após
iniciar shutdown. Não recomeçar a stack com esses ponteiros. Os timeouts limitam
esperas próprias: init/deinit síncronos do upstream não oferecem timeout público.

## Validação

Teste puro C++11, sem Arduino, SDK, mocks ou dependências adicionais:

```powershell
Test-Path -LiteralPath "test/host/.build"
& "C:\Strawberry\c\bin\g++.exe" -std=c++11 -O1 -g -Wall -Wextra -Wpedantic -Werror "src/connectivity/relay_protocol.cpp" "test/host/relay_protocol_tests.cpp" -o "test/host/.build/relay_protocol_tests.exe"
& "test/host/.build/relay_protocol_tests.exe"
```

A suíte é independente de `tools/check_project.py`; executar ambos. Inclui wire
golden LE, limites, falha de decode atômica, controles malformados, UTF-8, porta,
oito créditos/slots, cópia de payload, ACK só após consumo, cancelamento, erro,
EOF, quatro canais, epochs antigos/wrap, 65540 DATA+ACK por direção e 6000
entradas pseudoaleatórias. Não substitui validação física de pairing, bond
restaurado, MTU, CCCD, congestionamento, quedas e ciclo stop/begin/shutdown.
O build firmware compartilhado não foi executado.

## Upstream consultado

- Release: https://api.github.com/repos/h2zero/NimBLE-Arduino/releases/latest
- Tag/API: https://github.com/h2zero/NimBLE-Arduino/tree/2.5.1/src
- Headers: `NimBLEDevice.h`, `NimBLEServer.h`, `NimBLECharacteristic.h`,
  `NimBLEAdvertising.h`, `NimBLEConnInfo.h`, `NimBLEAddress.h`, `NimBLECppVersion.h`.
- Segurança: `nimble/nimble/host/include/host/ble_hs.h`, `ble_store.h`,
  `nimble/nimble/host/src/ble_sm.c` no mesmo tag.
- Implementações: `NimBLEDevice.cpp` (deinit/ownership),
  `NimBLECharacteristic.cpp` (notify copia para mbuf), `NimBLEServer.cpp`
  (assinaturas/dispatch de callbacks) e `NimBLEAdvertising.cpp` (stop idempotente).
- Em 2.5.1, NOTIFY_TX chama também o overload novo `onStatus` com peerInfo não
  preenchido; usa-se o overload antigo de dois argumentos para não filtrar por
  handle incorreto. `onWrite`, `onSubscribe` e callbacks do server usam ConnInfo.
