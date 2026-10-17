# Contrato GATT e implementação da ponte

## Wire format fixo

| Item | Valor |
| --- | --- |
| Service UUID | `7fd00001-6f6e-4d69-9b18-8e8c49b5c100` |
| RX ESP32, write **com resposta** | `7fd00002-6f6e-4d69-9b18-8e8c49b5c100` |
| TX ESP32, notify | `7fd00003-6f6e-4d69-9b18-8e8c49b5c100` |
| CCCD TX | `00002902-0000-1000-8000-00805f9b34fb` |
| Frame / payload máximo | 180 / 173 bytes |
| MTU mínimo antes de subscrever | 183 (ATT overhead: 3 bytes) |
| Canais | IDs `0..3`, quatro simultâneos |
| Janela DATA | 8 por canal **em cada direção** |

Um valor de characteristic corresponde a **um frame completo**. Não se concatenam frames no mesmo valor ATT e não se fragmenta um frame entre notificações.

```text
offset  tamanho  campo
0       1        version = 1
1       1        type
2       1        channel
3       2        epoch, uint16 little-endian
5       2        seq, uint16 little-endian
7       0..173   payload
```

| Type | Direção | Payload / seq |
| --- | --- | --- |
| OPEN = 1 | ESP → Android | `port u16LE` + hostname UTF-8 estrito; `seq=0` |
| OPENED = 2 | Android → ESP | vazio, `seq=0`; somente após connect + handshake TLS |
| DATA = 3 | ambas | 1..173 bytes de aplicação; seq independente por direção |
| ACK = 4 | ambas | vazio; `seq` do DATA **consumido**, ACK individual |
| CLOSE = 5 | ambas | vazio |
| ERROR = 6 | ambas | mensagem UTF-8 ≤173; app envia apenas texto genérico sem dados de exceção |

OPENED/ACK/CLOSE não aceitam payload; DATA vazio, tipo/canal inválido, versão incorreta, frame curto ou >180 bytes abortam o protocolo. Um envelope inválido invalida toda a sessão BLE porque não é possível confiar em seu canal/epoch. Falhas restritas a um canal usam ERROR e fecham seu socket.

## Epoch e sequência

- Cada OPEN precisa de uma epoch renovada para aquele canal. Uma nova epoch cancela o canal anterior antes de conectar seu substituto.
- Frames de epochs anteriores são descartados. Todo write BLE pendente carrega um predicado de validade, reavaliado antes de iniciar sua operação GATT; frames antigos ainda enfileirados não são enviados para um canal novo.
- Para distinguir inclusive um **OPEN antigo**, o engine memoriza as epochs já usadas por canal durante a sessão BLE (bitset, 8 KiB por canal). Não presume que epochs aleatórias sejam monotônicas. **Não reutilize uma epoch no mesmo link BLE.** Ao esgotar as 65.536 epochs, reconecte o link para começar nova sessão. Esta regra evita ambiguidade entre reuse e replay de OPEN.
- Seq DATA começa em zero em cada direção/novo canal e incrementa com wrap de 16 bits.
- ACK é **individual FIFO**: deve ser exatamente a cabeça da janela. ACK duplicado, fora de ordem ou sem DATA pendente fecha o canal. Não é ACK cumulativo.
- DATA fora da sequência esperada, duplicado ou além da capacidade inbound encerra o canal; não há recuperação nem replay de bytes.

## Fluxo Android → ESP

1. Um reader por canal lê até 173 bytes do InputStream TLS.
2. Reserva seq/crédito na janela e enfileira DATA no writer BLE.
3. Até oito frames podem estar pendentes mesmo que cada write GATT já tenha sido confirmado.
4. O callback de write GATT **não libera crédito**. Apenas ACK recebido por TX notify, depois de o ESP consumir DATA, libera um crédito.
5. Quando a janela enche, somente o reader TLS espera no monitor; o handler GATT continua atendendo notificações e outras operações/canais.
6. Em EOF, o reader termina e o canal drena os ACKs dos DATA já enviados antes de CLOSE. Se esses ACKs não chegarem, o timeout termina o canal. Não descarta o final de uma resposta por fechar antes da drenagem.

## Fluxo ESP → Android

1. O callback TX valida seq/epoch e enfileira o frame na fila inbound do canal, capacidade oito **incluindo o frame em escrita**.
2. Um writer por canal retira logicamente a cabeça, mas a mantém contabilizada enquanto executa `SSLSocket.getOutputStream().write(payload)`.
3. Somente depois do retorno bem-sucedido desse write remove o frame e enfileira seu ACK.
4. Não há flush exigido para cada pacote: o OutputStream de SSLSocket não é um BufferedOutputStream da aplicação. ACK significa aceitação pelo OutputStream TLS, não resposta HTTP nem confirmação do servidor remoto.
5. Socket lento/travado, exceção ou fila perdida/excedida encerram o canal. Dados não são repetidos.

O ESP pode notificar DATA logo após consumir OPENED, antes de Android entregar o callback do write correspondente. O engine aceita esse DATA assim que TLS está pronto, preservando FIFO na fila BLE; o reader phone→ESP só começa após concluir o write OPENED.

## GATT estritamente serial

`GattTransport` possui um único Handler proprietário e uma operação ATT em voo:

```text
connect → requestMtu(183) → validar MTU ≥183 → discoverServices
    → aguardar bond + segurança → habilitar notify local + escrever CCCD
    → ready → fila de writes RX com resposta
```

Android 14+ pode negociar 517 no primeiro pedido de MTU. O app aceita qualquer MTU negociado ≥183, mas **sempre mantém frames ≤180**.

A fila é limitada a 80 operações aguardando (mais uma em voo). Inclui controle/ACK/DATA; o máximo normal das quatro janelas cabe nesse limite. Toda operação tem deadline de 10 s. Timeout, erro ATT ou callback incompatível fecham `BluetoothGatt`, falham futures pendentes e terminam a sessão. Não se avança para a próxima operação depois de timeout, pois Android não oferece um ID de request nos callbacks.

Callbacks são vinculados ao objeto `BluetoothGatt` da sessão; o objeto nunca é reciclado. Depois de cancelamento, callbacks antigos não podem completar uma nova operação. Notificações recebidas entre o write CCCD e seu callback são armazenadas temporariamente (até oito), e entregues somente após o engine ser criado.

## Concorrência e fechamento

- O lock do engine protege somente estado/fila. `connect`, handshake, `read`, `write` e espera de crédito acontecem em workers de socket.
- A factory cria um handle cancelável antes de iniciar DNS/conexão. Cancelar fecha TCP primeiro (interrompe handshake/write/read), depois TLS. Um connect atrasado verifica o cancelamento antes de continuar.
- Pool: 12 threads, fila limitada a 32 tarefas. Com quatro canais há no máximo quatro readers e quatro writers ativos, além das aberturas/cancelamentos transitórios.
- `tick()` usa relógio monotônico injetado; a aplicação chama a cada segundo pelo handler do serviço. Testes avançam o relógio sem esperar os deadlines reais.
- Timeout/erro gera ERROR genérico; CLOSE/ERROR remoto encerra e responde CLOSE uma vez. O estado fechado ignora os frames seguintes, impedindo loop de fechamento.
- A parada remove tick/timeouts, encerra engine/canais, fecha e libera GATT, desregistra o receiver, cancela futures, interrompe workers e encerra HandlerThread/FGS. Estado de ownership impede cleanup de uma instância antiga de sobrescrever status de uma nova.

### Deadlines implementados

| Operação | Prazo |
| --- | --- |
| Inicialização BLE, incluindo diálogo de bond/evento de criptografia | 90 s |
| Cada operação GATT | 10 s |
| OPEN lógico (DNS + connect + handshake) | 20 s |
| TCP connect | 10 s |
| Read durante handshake TLS | 15 s; OPEN lógico limita o total |
| DATA enviado aguardando ACK da cabeça | 20 s desde o envio/enfileiramento |
| Write TLS / fila de writer | 20 s |
| Inatividade do canal / read TLS | 90 s |

Os deadlines do engine têm granularidade de até um tick (1 s). Timeout de write é implementado fechando o socket, pois SSLSocket/Socket Java não expõem timeout de escrita. DNS nativo tem a limitação de cancelamento descrita no README.
