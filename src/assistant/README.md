# Assistente push-to-talk — módulos reais

## Integração

`assistant_job.h` implementa a API pedida:

```cpp
AssistantJob assistant; // membro persistente do mainApp

// No acionamento da pressão longa:
assistant.start(settings, codec, connectAssistantNetwork);
// Enquanto a UI continua a consultar buttons.poll():
assistant.release(); // soltou; finaliza captura e envia activityEnd após o último PCM
assistant.cancel();  // cancelamento assíncrono, opcional

AssistantJob::Snapshot view;
assistant.snapshot(view);
if (view.finished) assistant.acknowledgeFinished();
```

`start`, `active`, `snapshot` e `acknowledgeFinished` pertencem à mesma task da
UI. `release` e `cancel` também podem ser chamados de outra task, **não de ISR**.
Não destruir o objeto com sessão pendente; mantê-lo vivo até o ACK. A captura e
a reprodução têm uso exclusivo do codec durante esse intervalo.

`start` só configura áudio/aloca memória/cria tasks; nenhuma operação de rede
ocorre na UI. O callback `ConnectFn` roda na task de rede, deve retornar em
**até 15 s**, e não pode acessar UI ou iniciar pareamento interativo. Ele escolhe
Wi-Fi/PhoneRelay e informa falha real. Como a assinatura não oferece cancelamento,
o módulo não consegue interromper à força um callback externo que nunca retorna.

Dependências de integração fornecidas pelo coordenador:

- `../net/network_client.h`: `NetworkClient`, compatível com `WiFiClientSecure`,
  inclusive os métodos herdados `setTimeout` e `setHandshakeTimeout`. Deve suportar
  dois canais TLS concomitantes (Live + gateway), também quando usa o relay BLE.
- `Settings`: `assistantEnabled`, `assistantModel`, `assistantGatewayUrl`,
  `assistantGatewayToken`, `assistantTimeZone`, `sttApiKey`,
  `geminiFreeConfirmed`, `micGainDb`, `speakerVolumeDb`.
- `HttpClient::readResponse(NetworkClient&, ...)` com limite explícito de corpo.
- `AudioCodec` existente: estéreo PCM16, leitura/escrita parcial e timeout.

Uma falha síncrona retorna `false` e fica disponível no snapshot. Se a criação
de uma task falhar após outra ter iniciado, `false` também exige consultar
`finished` antes do ACK: o encerramento parcial continua assíncrono.

## Fluxo e recursos

Três tasks independentes: áudio, Live WebSocket e gateway. A task de áudio captura
antes de conectar e posteriormente reproduz; não há acesso simultâneo de captura
e reprodução ao clock I2S. Antes de criar tasks, o codec recebe 16 kHz, enable,
ganho e volume das configurações. O canal 0 dos frames estéreo é preservado em
PCM mono little-endian. Não há arquivos temporários ou escrita de áudio em flash.

Buffers fixos em PSRAM:

| Buffer | Bytes | Política |
|---|---:|---|
| Captura | 960.000 | Prefixo imutável, até 30 s de PCM16 mono/16 kHz |
| Recepção WebSocket | 262.145 | Mensagem até 256 KiB mais NUL |
| Reprodução | 262.144 | Ring de PCM mono/24 kHz, aproximadamente 5,46 s |

Total fixo de 1.484.289 bytes (~1,42 MiB), além de cJSON/strings/decodificação e
34 KiB de stacks das tasks. JSON e resultados das ferramentas usam heap normal
do SDK; o módulo não instala hooks globais do cJSON. O contexto mantém no máximo
16 chamadas de ferramentas e seus pedidos/resultados até o ACK.

A captura termina ao soltar, ao atingir 30 s de tempo/amostras, cancelar ou
falhar. O limite de 30 s é informado também no snapshot final. Não há sobrescrita
de áudio nem descarte silencioso de overflow; erros de áudio/frames incompletos
são explícitos. O envio usa blocos de 2.048 bytes (64 ms), com cauda menor.

O setup espera `setupComplete`; depois envia `activityStart`, PCM preservado e
`activityEnd` somente após `captureDone` sincronizado e todo o PCM transmitido.
VAD automático fica desabilitado. Durante a captura a rede continua recebendo
mensagens, inclusive cancelamentos de ferramentas. Áudio antecipado é limitado
ao ring e só começa a tocar após terminar a captura. Overflow antecipado falha
explicitamente. Na reprodução, backpressure tem prazo de 6 s.

A saída é decodificada e reproduzida progressivamente em 24 kHz, duplicando cada
amostra mono nos dois canais, sem depender de transcrição. Escritas parciais
mantêm o offset exato; cada bloco tem prazo de 2 s e cancelamento. Há drenagem
final de 180 ms para os 8 x 512 frames DMA do codec atual. A tabela local
`lib/es8311/es8311.c` contém 16 kHz/MCLK 4.096.000 e 24 kHz/MCLK 6.144.000.

`finished` só é publicado depois de as três tasks saírem dos seus escopos de
trabalho e ficarem suspensas, os clientes TLS fecharem e o codec desligar.
O proprietário elimina as tasks suspensas antes de publicar o término. O ACK
libera o contexto/buffers e permite reutilizar o objeto, sem task acessando dados
já liberados. `snapshot`/`active` também fazem essa coleta, sem esperar rede.

## Modelo e política gratuita

É obrigatório configurar explicitamente um destes candidatos:

- `gemini-3.8-live`;
- `gemini-3.1-flash-live-preview`.

O prefixo `models/` é aceito, mas não obrigatório na configuração. A allowlist
é deliberadamente exata; não há discovery, seleção automática, REST/STT como
fallback ou tentativa em modelo pago. Modelos desconhecidos, o modelo vazio e
`geminiFreeConfirmed=false` são recusados antes de criar tasks ou conectar.
`gemini-3.8-live-extended-thinking` é recusado porque tem ciclo de turnos e
ferramentas diferente. A confirmação local não consulta faturamento/cota nem
modifica a conta do provedor; o usuário deve confirmar acesso gratuito para a
chave/projeto configurados. Nenhum controle adicional de UI é criado aqui.

O setup usa resposta `AUDIO`, transcrições de entrada/saída e instrução concisa
em pt-BR. Contexto temporal só é fornecido se o epoch estiver entre 2024-01-01
e 2100-01-01; envia UTC e o fuso IANA configurado, sem alterar o timezone global
do dispositivo. É uma checagem de plausibilidade, não uma sincronização NTP.
Com relógio inválido, instrui pedir data absoluta em vez de inventar hoje/amanhã.

Não é oferecida ferramenta de busca web. O sistema informa expressamente que
consulta web não está habilitada; eventual chamada `web_search` recebe
`TOOL_DISABLED`, nunca conhecimento do modelo disfarçado de pesquisa.

## WebSocket e limites de tempo

`ws_codec.*` é C++ puro, sem SDK. Valida framing RFC 6455, comprimentos canônicos
7/16/64 bits, RSV/opcodes, limites agregados, UTF-8 de texto/close, códigos de
close, continuação/fragmentação, controles intercalados e EOF truncado.
Não negocia compressão ou subprotocolos. Mensagens binárias também são aceitas,
mas o conteúdo precisa ser JSON UTF-8 válido. Falhas fecham a sessão.

`transport.*` faz HTTP Upgrade diretamente no `NetworkClient`, com nonce e masks
novos de `esp_fill_random`. Confere `Sec-WebSocket-Accept` usando
`mbedtls_sha1_ret` + base64. Cabeçalhos são limitados a 8 KiB. Responde ping/pong,
ecoa close e mascara todos os frames enviados pelo cliente. Não registra URL,
chave ou payload autenticado. Erros de provedor não são copiados crus para a UI;
texto exibido passa por redação de credenciais/URLs.

Endpoint implementado:

```text
wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=<chave>
```

Prazos: connect 10 s e handshake TLS 10 s no cliente Live; Upgrade 10 s; envio
de frame 3 s; mensagem fragmentada incompleta 15 s; setup 15 s; primeira resposta
de áudio **ou texto** até 45 s após envio da fala quando não há ferramenta pendente;
turno completo, incluindo captura e playback, 180 s. Não envia silêncio artificial
para terminar turnos. Um `turnComplete` com resposta e sem ferramenta pendente
encerra a sessão, após drenar o áudio. Turno vazio retorna erro.

Prazos dependem de `NetworkClient.connect/write/read/stop` respeitarem seus
timeouts; o módulo não elimina uma task enquanto ela está dentro de um driver.
A conexão direta mantém `setInsecure` da política existente. No transporte BLE,
a validação TLS pertence ao app PhoneRelay, conforme contrato do coordenador.

## Gateway, idempotência e cancelamento

`tool_schema.h` contém a cópia semântica literal das sete declarações de
`gateway/tool-schema.json`. `assistant_schema_test.py` compara os objetos JSON
inteiros, não apenas nomes. Ao alterar o gateway, atualizar esse literal e rodar
o teste. Para 3.8 Live, só a cópia usada no setup recebe `behavior: BLOCKING`,
para o modelo aguardar as respostas antes de prosseguir.

O gateway recebe exatamente `{version:1,token,id,tool,args}`. O ID usa nonce de
sessão + índice da chamada, apenas alfanuméricos/`_`/`-`, até 80 caracteres,
compatível com `core.js` (mais restritivo que os 96 solicitados). O ID original
do Gemini é mantido em `toolResponse.functionResponses[].id` e nunca é usado
diretamente como ID autenticado do gateway.

Somente origem `https://script.google.com/macros/s/<deployment>/exec`, sem query,
é aceita. O token vai exclusivamente no corpo JSON do POST. Cada execução HTTP
faz um POST; 302/303 viram GET sem corpo e sem token, exclusivamente para
`https://script.googleusercontent.com/`, no máximo três redirects. Não aceita
host com sufixo, userinfo, porta explícita, HTTP ou URL relativa. Um Location que
contém o token, inclusive percent-encoded, também é recusado. Não repete POST
para seguir redirecionamentos. Pedidos e respostas têm limite de 32 KiB; cada
execução tem orçamento de 25 s, connect de até 5 s e leitura de até 8 s.

Sucesso exige JSON válido, `version=1`, `ok` booleano e ID correspondente. Erros
autenticacionais do core com `id=null` são preservados como erro, nunca sucesso.
Erros de rede/HTTP/JSON/redirect viram resultados JSON para o modelo, sem abortar
a conversa inteira. Não anuncia ação concluída localmente; a instrução de sistema
exige `ok=true` antes de o modelo afirmar conclusão.

O worker faz no máximo uma reconciliação de `OUTCOME_UNKNOWN`/`REMOTE_UNCERTAIN`,
reutilizando **o mesmo corpo serializado e ID**, apoiado no ledger durável já
implementado no gateway. Erros de autenticação, redirects e JSON não disparam
esse retry. Chamadas repetidas com o mesmo ID Gemini reutilizam o resultado e
não repetem efeitos. Reutilização do ID com outro payload é erro de protocolo.

Cancelamentos são recebidos enquanto o worker está em HTTP. IDs cancelados são
lembrados mesmo se o cancelamento vier antes da chamada; efeitos ainda não
iniciados são bloqueados. Depois que um POST começou, cancelamento não pode
desfazer uma ação remota: devolve resultado confirmado quando disponível ou erro
com `uncertain=true`/`requestId`, sem novo POST após cancelamento. O ACK não deve
ser interpretado como cancelamento/reversão de ação remota. Não há diário local
persistente ou retomada de sessão após reboot; IDs/payloads locais vivem até ACK.

## Verificação executável

```powershell
python test/host/assistant_check.py
```

O runner usa cJSON oficial do cache existente, não baixa/instala bibliotecas e
grava somente artefatos `assistant_*` em `test/host/.build/`. Pode receber
`CJSON_DIR` e `CXX`. Protocolo/schema não dependem de SDK; o adapter de runtime
usa threads reais e SHA-1 real do Windows CNG, não SHA fake. A RNG desse adapter
é determinística e não testa entropia do ESP32. HTTP/TLS, I2S e FreeRTOS são
adapters: não há afirmação de validação em hardware, TLS de ponta a ponta ou
temporização de rádio/DMA real. O parser HTTP existente tem sua própria suíte.

Testes cobrem masks/vetor RFC, comprimentos, fragmentação, controles, limites,
truncamento, UTF-8, setup/VAD/PCM/base64, política de modelo/gratuidade, esquema,
redação, captura durante conexão, áudio sem texto, áudio antecipado sem troca
de clock, escrita parcial estéreo, falhas de tasks/PSRAM/codec/rede, ACK/reuso,
timeout de primeira resposta/turno e limite de captura, gateway real de produção
sobre adapter HTTP, redirects seguros/inseguros/limitados, token em Location,
cancelamento antes/depois de iniciar HTTP, deduplicação e reconciliação com
payload idêntico. O build usa C++11, `-Wall -Wextra -Werror` e cppcheck quando disponível.

## Fontes oficiais consultadas (2026-10-16)

```text
https://ai.google.dev/gemini-api/docs/live-guide
https://ai.google.dev/gemini-api/docs/live-tools
https://ai.google.dev/api/live
https://ai.google.dev/gemini-api/docs/pricing
https://www.rfc-editor.org/rfc/rfc6455.txt
```

Gemini: consulta via busca nas páginas oficiais, incluindo sintaxe manual de
atividade, PCM, modelos/camada gratuita e comportamento BLOCKING. O fetch direto
das páginas Google não respondeu neste ambiente. RFC 6455 foi consultada
diretamente; referência para handshake, masking, framing, controles e UTF-8:
seções 4.1, 5.2–5.5, 7 e 8. Os headers locais do core 2.0.17 confirmam que
`WiFiClientSecure::setTimeout` recebe **segundos** e a assinatura de connect
com timeout. Não há dependência de biblioteca externa de WebSocket.
