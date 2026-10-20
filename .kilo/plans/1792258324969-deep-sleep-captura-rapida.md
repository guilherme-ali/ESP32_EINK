# Deep sleep com captura rapida

## Objetivo e decisoes

- Minimizar energia de espera da placa Waveshare ESP32-S3-ePaper-1.54 V2 e a latencia entre pressionar BOOT e capturar audio valido.
- BOOT curto acorda e inicia nota; BOOT longo conserva assistente PTT. Captura antecipada atende ambos. PWR apenas acorda para a interface; consumir o gesto inicial sem abrir menu ou voltar a dormir acidentalmente.
- Manter timeout, opcoes e valor NVS existentes nas configuracoes. Nao impor 30 segundos nem alterar o padrao de 120 segundos.
- Manter deep sleep como repouso principal, sem light sleep intermediario, conexao BLE permanente, wake periodico ou microfone sempre ligado.
- Preservar qualidade, taxas configuradas, notas, configuracoes, funcionamento offline, Wi-Fi/BLE e semantica das operacoes existentes.
- Escopo inicial somente firmware e testes. Alteracoes de hardware, bootloader e migracao de framework ficam fora desta implementacao.

## Diagnostico confirmado

- Ja existe deep sleep por BOOT/GPIO0 ou PWR/GPIO18: `src/main.cpp:244-285`. E-paper e audio sao desligados; GPIO17 HIGH sustenta o latch da bateria.
- O boot tem `delay(2000)` incondicional e prepara e-paper, sensores, armazenamento e interface antes de atender botoes: `src/main.cpp:288-357`.
- `App::begin()` varre notas; a verificacao de sincronizacao pode ler arquivos completos para MD5: `src/app/app.cpp:98-129`, `src/storage/notes.cpp:21-119`, `src/net/gdrive.cpp:1541-1559`.
- O construtor global do display ja aloca buffers e configura SPI/GPIO: `src/display/epaper.cpp:49-98`. Adiar apenas `epd.init()` nao remove todo esse trabalho.
- `readBusy()` nao tem timeout. O refresh pode bloquear tanto boot quanto entrada em sleep: `src/display/epaper.cpp:100-104`.
- O driver habilita PA durante abertura, mas seu estado logico inicial pode fazer `enable(false)` retornar sem desliga-lo. Suspender codec tambem nao para I2S/APLL: `lib/es8311/es8311.c:560-609`, `src/audio/codec.cpp:69-153`.
- Clique curto so e publicado na soltura. Polling tardio perde gestos e o catch-up atual repete o nivel presente como se fosse historico: `src/board/buttons.cpp:21-69`.
- Gravador prepara arquivos antes de criar captura; assistente so captura depois da classificacao longa e de suas alocacoes: `src/audio/recorder.cpp:28-117`, `src/assistant/assistant_job.cpp:253-315`.

## Limites fisicos

- Deep sleep reinicia o caminho de boot; nao retoma RAM/PSRAM comum. Nao ha audio recuperavel antes de microfone/ADC/I2S estarem operantes.
- Consumo da placa nao e o consumo nominal do chip. Regulador, carregador, RTC, resistores, sensor e eventual microSD estabelecem um piso. Nao prometer corrente quase zero ou autonomia numerica sem medicao.
- Percentual de bateria e a referencia assumida de 4,086 V nao medem corrente nem comprovam economia.
- GPIO17 nunca deve ser desligado neste plano. USB pode mascarar queda do latch e consumo dos rails.

## Implementacao Ordenada

1. **Estabelecer baseline e marcos de medicao.** Instrumentar em RAM causa de reset/wake, mascara EXT1, primeira observacao do botao, audio ligado, I2S pronto, primeiro bloco recebido, audio analogicamente valido, classificacao do gesto, primeiro checkpoint e UI pronta. Emitir logs depois, apenas no perfil diagnostico. Separar botao ate primeira captura valida, primeira persistencia e disponibilidade visual. Registrar tambem heap interno/DMA, PSRAM, overflow e energia por ciclo. Nao usar timestamps de boots diferentes como um unico relogio.

2. **Robustecer o repouso e eliminar consumo evitavel.** Exigir ausencia de captura, Saving, playback, sync/exportacao, escrita FS/NVS e workers ativos antes de dormir. Encerrar rotas/radios de forma limitada e verificar resultados. Corrigir PA para permanecer LOW desde abertura e durante captura; habilita-lo somente para reproducao. Separar captura ADC de reproducao DAC no wrapper/driver, com mute e ganhos restaurados corretamente. Parar/desinstalar I2S e liberar APLL/DMA quando nao houver consumidor. Desligar o rail de audio quando ocioso, reinicializando o codec de forma explicita na proxima utilizacao. Evitar recriar handles sem destruir os anteriores.

3. **Revisar GPIOs e transicao para deep sleep.** Conferir esquema/revisao fisica e pulls dos botoes; preparar RTC IO e restaurar GPIO digital no wake conforme SDK instalado. Manter latch HIGH sem pulsos ao aplicar/liberar holds, inclusive em construtores globais. Colocar sinais de perifericos desligados em estados sem backfeed, apenas conforme esquema; nao isolar indiscriminadamente wake, latch ou memoria. Esperar soltura estavel antes de armar wake por LOW. Se botao permanecer preso, nao entrar em loop de wake nem esperar em busy-loop: manter estado seguro de baixo consumo acordado e reavaliar soltura; nao declarar deep sleep alcancado. Conferir retornos EXT1/holds/configuracoes. Conservar wallpaper configurado, mas adicionar timeout/fallback de BUSY que permita cortar EPD e dormir mesmo com painel defeituoso. Evitar refresh redundante quando seguramente dispensavel.

4. **Criar boot minimo orientado a captura.** Ler causa/mascara de wake no primeiro ponto seguro. Remover espera fixa de 2 s do perfil de bateria; eventual espera serial e opcional e limitada ao diagnostico. Tornar inicializacao de display realmente tardia, incluindo trabalho dos construtores. Priorizar botoes, I2C, configuracao de audio necessaria e captura; sensores, Home, catalogo, hashes, portal e OAuth nao precedem captura solicitada. Carregar taxa/ganho/configuracao de audio do NVS sem valores default temporarios incorretos e sem migracoes/escritas no caminho critico quando evitavel. Montar LittleFS sem formatacao e resolver metadados depois que a captura em RAM estiver rodando. RTC/calendario pode ser obtido antes de nomear a nota, sem preceder a primeira amostra. Separar inicializacao minima de `App` da inicializacao visual/completa. No wake por PWR nao ligar audio especulativamente.

5. **Modelar o gesto de wake explicitamente.** BOOT EXT1 e uma intencao latched mesmo se o botao ja estiver solto no inicio do app. Comecar a observar niveis cedo, preservar transicoes e medir duracao real disponivel; nao fabricar tempo por repeticao de polls atrasados. Pressao ja encerrada antes da primeira observacao e tratada como curta, pois sua duracao nao pode ser reconstruida. Pressao mantida usa limiar longo atual a partir da primeira observacao valida; nao afirmar duracao fisica exata anterior a esse ponto. Ao classificar longo, encaminhar ao assistente; soltura encerra PTT mesmo se a rede ainda estiver conectando. Consumir exatamente uma vez o gesto inicial para que sua soltura nao pare a nota recem-aberta. Proximo BOOT curto para e salva; longo durante nota mantem descarte atual. Dois botoes simultaneos seguem prioridade segura de PWR, sem gravacao especulativa persistida. Assistente desativado/invalido gera resultado explicito, sem converter silenciosamente a pressao longa em nota.

6. **Introduzir captura antecipada com dono unico de I2S.** Extrair somente o mecanismo necessario de ingresso de audio para uma sessao compartilhada entre gravador e assistente, com uma task produtora, buffers prealocados e consumidor unico selecionado pelo gesto. Capturar imediatamente em PSRAM enquanto FS, destino e interface ficam prontos. Orcamento inicial: fila mono PCM16 de 8 segundos na taxa de ingresso, entre 256.000 e 768.000 bytes; confirmar folga com buffers existentes do assistente/Opus e evitar duplicar rings grandes. Fila e limitada, preserva o prefixo e nunca sobrescreve audio inicial silenciosamente. Cheia, falta de memoria, erro de codec/task ou destino indisponivel encerram com falha explicita. Transferir prefixo e fluxo em ordem, sem segunda task lendo I2S, reinstalacao do driver, reset de codec ou buraco na troca. Distinguir fila temporaria de captura duravel. Na nota, somente drenar apos admissao de espaco, nome sem colisao, placeholder validado e PCM aberto. No assistente, integrar a captura ja iniciada sem reinicializa-la e preservar limite de duracao, cancelamento, release, politicas de rota e protocolo.

7. **Resolver taxas sem mudar configuracoes ou cortar a fala.** Usar taxa de ingresso `max(audioSampleRateHz, 16000)` durante classificacao. No caso padrao 16 kHz, nota e assistente usam passthrough. Notas a 32/44,1/48 kHz mantem captura nativa e taxa do WAV; para assistente converter continuamente a 16 kHz. Nota configurada a 8 kHz recebe decimacao de 16 para 8 kHz. Usar conversao streaming com filtro antialias, estado e fase preservados entre prefixo e fluxo, buffers limitados e sem heap por bloco; nao usar apenas descarte/duplicacao de amostras ou interpolacao sem filtro. Contabilizar atraso do filtro e cauda, validar resposta e duracao antes de liberar as taxas alternativas. A unica dona do hardware continua sendo a captura; Playback so assume apos seu encerramento. Nao trocar a taxa de I2S no meio da entrada de voz. Se a conversao nao passar criterios de qualidade/tempo real, a etapa fica incompleta, sem reduzir silenciosamente a qualidade ou esconder perda de prefixo.

8. **Adiar servicos sem criar concorrencia insegura.** Depois da captura estar estavel, inicializar display com timeout e mostrar estado real; continuar atendendo botoes durante operacoes lentas. Catalogo completo, recuperacoes e hashes ficam preferencialmente depois de Recording/Saving, nao em worker concorrente com escrita sem exclusao comprovada. Nao eliminar verificacao MD5 de sincronizacao: retira-la somente do caminho ate captura. FS sem montagem/espaco gera erro de gravacao, nunca sucesso aparente ou formatacao automatica. Portal/pareamento inicial sao adiados ate concluir a intencao de gravacao. Preservar auto-sync configurado e sua ordem apos salvamento. Encerramento de rede ou display com falha nao deve prender a maquina indefinidamente; nenhuma finalizacao de audio pode ser abreviada pelo timeout de inatividade.

## Testes e Criterios de Aceite

1. Rodar suites existentes `tools/check_integrations.py` e `tools/check_project.py --cppcheck`, alem de builds `esp32-s3-devkitc-1` e `battery`. Acrescentar testes integrados para boot/gesto, power/audio, timeout BUSY, fila/ownership e conversao de taxas.
2. Cobrir BOOT breve ja solto, mantido, bounce, proximidade do limiar longo, release durante boot/conexao, pressao simultanea, PWR preso e segundo clique imediato. Nenhum gesto duplicado, gravacao acidental por PWR ou ciclo sleep/wake continuo.
3. Injetar sequencia PCM identificavel: prefixo e fluxo devem aparecer uma unica vez, em ordem, sem gaps; passthrough deve ser exato. Conversores devem passar testes de contagem/duracao, continuidade por blocos, saturacao, alias e comparacao espectral com referencia offline filtrada. Fazer escuta comparativa para todas as taxas e testar carga simultanea de display/rede.
4. Preservar testes de gravacoes longas, todas as taxas, armazenamento cheio, falha de alocacao/tasks, colisao, checkpoint e power loss. Nao dormir durante Saving; nao modificar formato append-only, recuperacao, tombstones ou chave/configuracao existente sem necessidade.
5. Validar fisicamente boot frio e wake real separadamente, bateria sem USB, FS com muitas notas grandes/sincronizadas, sensores ausentes, BUSY preso e rede indisponivel. Fazer pelo menos 30 ciclos por caminho principal e relatar mediana, p95, maximo, falhas e tentativas invalidas.
6. Medir botao ate audio valido com referencia externa comum e estimulo acustico deterministico nos offsets 0/50/100/200/500/1000 ms, refinando o limite. Analisar primeira silaba, transientes, continuidade e duracao. Primeiros bytes DMA ou tela "gravando" nao sao prova suficiente. O benchmark de resposta de voz existente nao mede essa janela e precisa de ensaio especifico.
7. Comparar corrente/energia na bateria: Home inicial, apos captura/playback, janela BLE, entrada em repouso e deep sleep estabilizado. Usar instrumento adequado a microampere e picos de boot sem queda de tensao excessiva; verificar rails/PA e latch com instrumento adequado. Comparar com/sem microSD. Registrar tensao, revisao da placa, firmware e condicoes. Sem instrumento, marcar consumo quantitativo como nao validado; nao inferir por percentual de bateria.
8. Aceitar somente se espera nao aumentar, energia por ciclo e latencia de captura melhorarem frente ao baseline acima da incerteza, nao houver perda adicional de audio apos captura valida, corrupcao ou regressao de UI/assistente. Nao prometer valor absoluto em ms ou microampere antes da bancada; publicar piso observado e gargalos restantes.

## Entrega e Reversao

- Implementar por etapas verificaveis: baseline, repouso seguro, boot minimo, gesto/captura 16 kHz, demais taxas, bancada completa. Nao declarar concluido somente com o caso padrao.
- Manter binario atual para retorno; validar primeiro sem modificar particoes/NVS/LittleFS. Na instalacao autorizada, gravar somente aplicacao em `0x10000` e verificar digest, preservando notas e configuracoes. Nao fazer erase completo.
- Perfil final de uso na bateria permanece sem USB CDC. Binario diagnostico e temporario; logs nao devem alongar caminho critico nem mudar comparacao de consumo.
- Sem commit/push automatico: a autorizacao anterior se referia a entrega anterior.

## Otimizacoes Deliberadamente Adiadas

- Light sleep pode dar retomada mais rapida, mas consome mais em espera e nao e o foco escolhido. Reavaliar somente se o deep sleep otimizado nao atender ao uso real.
- Wake stub customizado exigiria RTC FAST retida, hoje explicitamente desligada. So estudar apos medir boot residual, com ganho energetico comprovado; nao inicializar Arduino/I2S completo em stub nem combinar com RTC FAST OFF.
- Skip de verificacao no bootloader altera integridade/seguranca e depende de reconstruir artefatos do SDK Arduino precompilado. Nao habilitar por simples `-D`, nao trocar bootloader nesta fase nem desabilitar validacao de firmware para ganhar latencia.
- Remocao de indicadores, mudanca de regulador/carregador, corte de alimentacao total e alteracao do circuito de wake dependem de avaliacao eletrica separada. Se o piso de consumo for de hardware, firmware sozinho nao o elimina.

## Referencias Oficiais

- ESP32-S3, ESP-IDF 4.4.7, modos de sono, EXT1, RTC IO e flash: https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/sleep_modes.html
- Limites e memoria dos wake stubs: https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/deep-sleep-stub.html
- Startup, logging, medicao e riscos de skip de validacao: https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/performance/speed.html
- Esquema oficial a conferir contra a revisao fisica: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54/blob/main/04_Hardware/Schematics/ESP32-S3-Touch-ePaper-1.54-Schematic.pdf
