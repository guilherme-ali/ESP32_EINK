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

O protocolo atual transporta PCM/base64 do Live pelo canal BLE. A velocidade
e a latência dependem do rádio, do Android e da rede; não há garantia de
“resposta instantânea” nem codec Opus nesta versão. O rádio negocia PHY/intervalo
de sessão ativa e se desconecta ao terminar. O deep sleep desliga a conexão BLE.

### Correcao de repeticao de audio (17/10/2026)

O I2S agora usa `tx_desc_auto_clear`: se faltarem amostras, transmite silencio
em vez de repetir o conteudo antigo do DMA. Comportamento documentado no
[driver I2S do ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v4.4.8/esp32s3/api-reference/peripherals/i2s.html).
Nao se apaga o DMA inteiro durante a reproducao, preservando a cauda ainda em voo.

O ring de saida usa 2 MiB de PSRAM, cerca de 43,7 s de PCM mono a 24 kHz/16 bits.
Pelo BLE, espera-se a resposta completa; se ela exceder a reserva, a reproducao
comeca antes da saturacao para permitir que a rede continue recebendo. Caso
esvazie, acumula novamente. Respostas longas podem ter pausas entre blocos;
o limite global continua em 180 s. Pelo Wi-Fi, o prebuffer inicial e de 500 ms.
Essa politica prioriza fala continua em vez de menor latencia inicial pelo BLE.

Validacao fisica: pergunta emitida pelo alto-falante do PC e capturada no ESP32,
com resposta Gemini Live via celular BLE. Log: `buffered=1`, `audio_rx_ms=1910`,
`audio_tx_ms=1910`, `rebuffer=0`, sem erro. O usuario confirmou a frase
"Ola, tudo bem com voce?" fluida, sem repeticoes. Os contadores medem PCM
recebido/entregue ao codec; nao substituem medicao eletrica do I2S.

As regressoes host incluem entrega atrasada, Wi-Fi progressivo, cancelamento
enquanto acumula, resposta curta e resposta maior que o ring, com comparacao
exata das amostras. O teste host nao simula o DMA real.
