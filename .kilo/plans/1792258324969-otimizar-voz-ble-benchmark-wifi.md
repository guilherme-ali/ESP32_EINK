**Plano BLE**
Otimizar a espera da resposta falada pelo celular BLE, preservando o funcionamento e a qualidade do Wi-Fi direto. Implementar por etapas medidas, sem trocar qualidade por um resultado isolado de cronometro.

**Decisoes**
- Meta escolhida: mediana da espera acustica BLE ate 1 s acima do Wi-Fi, comparando condicoes equivalentes. Apresentar partida fria e perguntas consecutivas separadamente; nao esconder uma com a outra.
- Opus permitido somente se nao houver perda perceptivel na comparacao de escuta. Primeiro tentar otimizar PCM sem perdas.
- Permitir reutilizacao BLE por ate 30 s enquanto o dispositivo ja estiver acordado, sem prolongar o prazo atual de deep sleep. Encerrar ao dormir, desativar, cancelar ou perder o telefone.
- Continuidade de contexto entre perguntas nao foi decidida e fica fora do escopo: preservar perguntas independentes. Reutilizar a sessao Gemini somente se a API oferecer semantica equivalente documentada; caso contrario, reutilizar apenas BLE e recriar Live. Nao inventar um reset de contexto.
- Nao mudar modelo, voz, sample rates, volume, ganho, politica Auto, PCM/protocolo direto ou prebuffer Wi-Fi de 500 ms para melhorar artificialmente a comparacao.
- Preservar `tx_desc_auto_clear`, cauda DMA, captura integral, limites de 30/180 s, seguranca, notas, NVS e sincronizacao Drive. Nenhum fallback pago, alteracao de credenciais ou permissao ampla entra neste plano.

**Estado Atual**
- BLE: frame 180 B, payload 173 B, janela oito; worker ESP32 envia um item e espera aproximadamente 3 ms; Android serializa writes com resposta. PHY 2M/DLE 251 ja sao solicitados, nao comprovados como negociados.
- PCM de resposta demanda 48.000 B/s; em base64, aproximadamente 64.000 B/s antes de JSON/cabecalhos. Link nominal nao comprova vazao suficiente.
- Ring de saida 2 MiB. BLE normalmente espera `networkDone`, publicado depois de `socket.close()`, ou cerca de 39,6 s acumulados; Wi-Fi toca progressivamente. Separar resposta pronta de encerramento da rede oferece um primeiro experimento de baixo risco.
- `audio_rx_ms`/`audio_tx_ms` sao duracoes de PCM, nao latencia nem prova de som audivel. O teste anterior de 1,91 s nao constitui baseline de tempo de resposta.

**Medicoes**
| Medida | Metodo e interpretacao |
|---|---|
| Espera percebida, principal | Fim acustico da pergunta ate primeiro fonema da resposta, extraidos da mesma gravacao externa. |
| Espera apos release | Release efetivamente recebido no ESP32 ate primeiro PCM e primeiro aceite I2S, no relogio do ESP32; correlacionar inicio acustico com incerteza explicitada. |
| Conexao | Inicio, rota pronta, abertura TLS, upgrade WebSocket e `setupComplete`; podem ocorrer durante a captura, antes do release. |
| Upload | Backlog capturado menos enviado no release, ultimo envio e `activityEnd`; aceitacao local nao significa consumo pelo Gemini. |
| Resposta | Primeira mensagem de audio, primeiro PCM validado, `turnComplete`, resposta pronta sem ferramentas pendentes e rede encerrada. |
| Reproducao | Preparo do codec, primeiro/ultimo aceite I2S, fim da alimentacao e cauda acustica. Aceite do driver nao e primeiro som. |
| Transporte/qualidade | Vazao util por sentido, jitter, ACK/callbacks, espera por credito, filas, erros, rebuffer, amostras e duracao; PHY/MTU/intervalo/DLE efetivos. |

Usar `boot_id`/`turn_id`, rota solicitada e rota efetiva do socket, variante e resultado em todos os registros. ESP32: `esp_timer_get_time()`; PC: `perf_counter_ns()` e timestamps ADC/DAC do stream; Android: relogio monotono local. Nao subtrair diretamente relogios de maquinas diferentes nem usar chegada da linha serial como instante do evento. Estimar offset/incerteza por ping/RTT quando necessario; a gravacao unica continua sendo a referencia acustica.

**1. Congelar Baseline**
- Guardar hashes de firmware/APK, configuracao e versoes atuais. Confirmar backup recuperavel antes de gravar; nao limpar flash, NVS ou LittleFS. Nao incluir backup de flash/segredos no pacote distribuivel.
- Registrar rotas, volume, distancias, microfone, modelo/voz, internet, tela Android, condicao fria/quente e duracao das falas.
- Enumerar microfones/saidas na implementacao. A enumeracao por terminal nao foi concluida nesta sessao de planejamento. Preferir microfone local do PC e saida cabeada/integrada, nao microfone ou alto-falante Bluetooth.
- Primeiro comparar ESP32 Wi-Fi e telefone BLE usando o mesmo acesso de internet. Depois repetir BLE com dados moveis como cenario real separado; nao atribuir a diferenca de operadora ao transporte BLE.

**2. Instrumentar Bancada**
- Em `assistant_job.*`, `transport.*`, `app.cpp` e `platform.h`, adicionar marcos limitados por turno, sem prints/bloqueios nos caminhos de audio e radio. Copias de timestamps 64-bit entre cores precisam de sincronizacao. Medir custo da instrumentacao contra baseline original.
- Registrar release uma unica vez, distinguindo botao, serial, limite e cancelamento. A soltura reconhecida pela aplicacao nao e a borda eletrica do botao; polling/e-paper podem atrasar sua entrega.
- Acrescentar politica diagnostica volatil `Auto`/`WifiOnly`/`PhoneOnly`, fixa durante o turno. Cobrir `network_route.*` E os fallbacks/recover de `network_client.*`; forcar so o seletor nao basta. `PhoneOnly` nao tenta Wi-Fi; `WifiOnly` nao abre BLE. Restaurar Auto apos fechar clientes e juntar workers, sem persistir NVS.
- Expor rota real de `NetworkClient::isPhone()` pelo LiveSocket. Rota global/label nao basta. Rota divergente invalida a comparacao; falha de rota entra no relatorio.
- Criar `tools/benchmark_voice.py`, reutilizando o console sem transforma-lo em gravador complexo. Manter uma porta serial aberta, sem reset DTR/RTS, esperar confirmacao de turno/captura pronta e temporizar release independentemente da leitura serial.
- Pre-gerar WAVs de perguntas com voz do PC e hashes; nao incluir geracao TTS online no tempo medido. Abrir captura antes do estimulo e manter ate depois do fim/cauda da resposta. Fixar posicao, volume e ruido ambiente; desabilitar AGC/supressao se possivel e registrar se nao for.
- Usar `sounddevice`/PortAudio com buffers pre-alocados e timestamps ADC/DAC; callbacks apenas copiam buffers, sem arquivos, rede ou serial. Guardar WAV continuo, eventos JSONL e resumo CSV em diretorio de resultados ignorado pelo Git, sem tokens/payloads de usuario.
- Detectar o fim do estimulo por alinhamento com o WAV conhecido e o inicio da voz por energia/janelas acima do ruido. Excluir estalos do amplificador; revisar onda e escuta em casos ambiguos. Relatar resolucao/incerteza, nao precisao ficticia.
- Celular pode fazer gravacao/video externa de confericao ou substituir o microfone PC, com marcadores acusticos para alinhamento. Nao adicionar captacao de microfone ao relay Android nem usar SCO: alteraria carga e permissoes do sistema medido.
- Cronometro manual/video sera confirmacao perceptiva, nao medida primaria. Timeout, erro do firmware, rota errada e overflow da gravacao precisam produzir status distinto de sucesso. Captura invalida e registrada e refeita; falhas/lentidoes reais nao sao descartadas.

**3. Comparar Corretamente**
- Executar piloto de tres pares Wi-Fi/BLE para validar roteiro e detectores antes de gastar cota com series extensas.
- Separar dois experimentos: corpus PCM fixo para integridade/fidelidade e Gemini real para latencia ponta a ponta. Diferentes respostas geradas nao permitem comparar qualidade de codec de maneira justa.
- Corpus: voz curta, media e longa, numeros, sibilantes, plosivas, pausas naturais e cauda; respostas maiores que o ring. Mesmo PCM original deve alimentar referencia e candidato em modo exclusivo de bancada, sem enfraquecer TLS/allowlist de producao. Comparar samples/hashes no ramo sem perdas e gravacao pelo mesmo alto-falante/ganho.
- Perguntas reais: curtas, medias e proximas do limite de 30 s; resposta curta padronizada e uma mais longa. Modelo/voz/ferramentas constantes, sem ferramentas no teste principal; gateway separado.
- Fazer triagem curta por mudanca e, para baseline/finalista, pelo menos 20 pares por condicao principal, com ordem Wi-Fi/BLE alternada ou aleatoria, mesma pergunta e condicoes comparaveis. Reportar tamanho da amostra e incerteza; ampliar serie se a decisao ficar perto do limite. p95 com amostra pequena e exploratorio.
- Separar primeira pergunta com radio/sessao encerrados, perguntas consecutivas dentro da janela e retorno apos expirar/dormir. Comparar contra o Wi-Fi existente, sem adicionar reuso ao Wi-Fi para a bancada.
- Publicar mediana, p95, maximo, duracao da resposta, erros e deltas BLE-Wi-Fi. Nao escolher apenas melhor rodada nem excluir timeouts. Respeitar cotas: diante de 429, parar/retomar com registro, sem trocar por modelo pago.

**4. Otimizar Sem Perdas**
Aplicar uma mudanca por rodada, medir e manter somente ganhos repetiveis. Parar o desenvolvimento de alternativas quando uma variante atender latencia, qualidade e regressao.
- Liberar reproducao BLE integral em `response_ready` antes do teardown, usando a condicao atual de sucesso sem ferramentas pendentes. Manter a espera de workers para liberar memoria; nao substituir `networkDone` na seguranca do lifecycle.
- Em `transport.*`, experimentar staging/leitura em blocos somente para LiveSocket BLE. Preservar bytes restantes, frames fragmentados, ping/close, `inputPending()`, cancelamento e orcamento limitado de trabalho. Nao alterar globalmente `writeBytes()` nem o caminho Wi-Fi/gateway por conveniencia.
- Em `ble_relay.cpp`, substituir pausa fixa por pacote por worker orientado a eventos e pequenos lotes com limite de quantidade/tempo. Preencher creditos disponiveis com fairness, mantendo ACKs oportunos, FIFO e controles. Sem espera sob mutex, spin ou retry cego de notify de resultado ambiguo.
- Em `RelayEngine.java`, agrupar DATA do mesmo canal ja disponiveis em writes TLS limitados, sem esperar encher lote. ACKs continuam individuais e ordenados, depois do write bem-sucedido; slots em voo contam na janela. Falha parcial/ambigua aborta sem replay.
- Testar separadamente perfil MTU 247 / frame 244 / payload 237, mantendo janela oito. Atualizar limites C++/Java e negociar capability explicitamente, usando 180/173 para pares legados. MTU alto sozinho nao identifica versao do parser; nao concatenar frames GATT.
- So depois experimentar DATA sem resposta GATT, com propriedade `WRITE_NR` autenticada, suporte do peer, fila proprietaria e creditos. Diferenciar aceito localmente, callback GATT e ACK de consumo. Nao disparar writes concorrentes nem remover confiabilidade para obter velocidade.
- Reutilizar link por ate 30 s conforme decisao; testes de expirar/cancelar/dormir obrigatorios. Reuso Live depende da regra de contexto acima e verificacao da API. Distinguir ganho de reuso de ganho de transporte.

**5. Antecipar Reproducao**
- Somente depois de comprovar margem de vazao/jitter, habilitar politica exclusivamente BLE com prebuffers candidatos de 200/400/800/1200 ms, ajustados pelas medicoes. Wi-Fi permanece em 500 ms.
- Para PCM/base64, usar como margem inicial de bancada vazao equivalente a pelo menos 1,25 vezes o consumo: aproximadamente 60 kB/s PCM ou 80 kB/s base64, antes dos demais headers. Medir sustentacao e gaps, nao apenas rajada inicial ou PHY nominal.
- Se a chegada sustentada continuar abaixo do consumo, nao vender prebuffer curto como solucao. Manter acumulacao segura, rebuffer com histerese e limites, sem repetir, descartar, acelerar ou reduzir sample rate. Variante que gagueja nao e aprovada por comecar mais cedo.

**6. Canal De Midia**
Somente se o ramo anterior nao atingir a meta: criar caminho de voz especifico do BLE no companion, preservando Wi-Fi direto e relay transparente de HTTP/Drive.
- Celular assume enquadramento da sessao Live e troca envelopes de controle/texto/ferramentas e audio binario com o ESP32. Preservar ordenacao, atividade manual, cancelamento, erros e semantica de ferramentas; isto nao e simplesmente retirar base64 de strings.
- Primeiro medir PCM binario exato, mantendo 16 kHz de entrada e 24 kHz de saida: elimina aproximadamente 25% do volume base64, mas a saida ainda demanda 48 kB/s.
- Se insuficiente, experimentar Opus primeiro em 64 kbit/s mono, frames de 20 ms; avaliar 48/32 kbit/s apenas se necessario. Priorizar qualidade, nao o menor bitrate. Confirmar backend Android, decoder ESP32, disponibilidade/licenca/versoes e APIs oficiais antes de escolher dependencias; nao presumir suporte do Gemini a Opus.
- Comprimir primeiro a resposta; incluir entrada somente se backlog de upload justificar o encoder no ESP32. Celular converte para os formatos da API. Medir custo CPU, heap/PSRAM, stacks e latencia de codec, inclusive carga concorrente de microfone/radio.
- Negociar protocolo/capabilities de midia e codec antes do turno. Incluir sequencia, numero de amostras, timestamps de audio e fim explicito; tratar lookahead/padding e cauda. Buffers limitados; perdas/PLC/erros devem ser contados, nunca mascarados.
- Usar novo canal/contrato de midia, sem reinterpretar silenciosamente frames do relay legado. Pareamento SC/MITM e TLS seguro continuam obrigatorios. Incompatibilidade permite modo seguro anterior antes do turno; nao trocar codec ou repetir dados/ferramentas no meio de uma operacao.
- Opus so pode virar padrao BLE apos teste cego A/B ou ABX com o MESMO corpus original, volume alinhado e configuracao fixa. Fazer 20 comparacoes aleatorizadas com referencia PCM, avaliando sibilancia, naturalidade, distorcao, inteligibilidade, cortes e cauda. Se o usuario perceber piora, subir bitrate ou descartar Opus. Ausencia de diferenca neste conjunto nao prova transparencia universal.

**7. Aceite E Regressao**
- Latencia: alvo BLE mediana ate Wi-Fi +1 s em condicoes equivalentes; informar separadamente se foi alcancado no frio e na janela de reuso. p95 deve melhorar ou permanecer dentro da variacao do BLE anterior; nao aceitar mediana melhor a custa de travamentos. Se a meta nao for atingida, documentar ganho parcial e limitador, sem declarar sucesso completo.
- Qualidade: nenhuma gagueira/repeticao/corte novo; PCM sem perdas byte a byte no ramo correspondente, nenhum pacote/amostra perdido/reordenado, cauda completa e cancelamento correto. No Opus, integridade do transporte mais aprovacao de escuta sao obrigatorias; hashes PCM nao podem ser exigidos como se o codec fosse lossless.
- Wi-Fi: sem mudanca funcional, mesmo PCM/sample rate/ganho/prebuffer. Repetir baseline apos cada finalista; variacoes acima de maximo entre 10% e 100 ms na mediana ou 10% e 200 ms no p95 exigem investigacao e repeticao controlada antes de aprovar.
- Radio: telefone com tela bloqueada, reconexao, distancia/interferencia, versao antiga/nova, ACK lento, callbacks ausentes/tardios, fila cheia, EOF com cauda, epoch velho, stop durante credito e cancelamento com ferramentas concorrentes. Preservar hashes e uploads de notas sem modificar notas pessoais para o benchmark.
- Testes host/Android: rotas realmente fixas sem fallback; release idempotente; staging e sobras; timers; PCM atrasado/exato e maior que ring; pipeline abaixo/acima de tempo real; perfis 180/244; sequencias/wrap/creditos; writes rejeitados ou ambiguos; timeout da captura; resultados acusticos sinteticos com onset conhecido, silencio, ruido, estalo e overflow. Testes de pacing nao podem depender do sleep de 1 ms do Windows.
- Rodar `python tools/check_project.py --cppcheck`, `python tools/check_integrations.py --android` e `pio run -e esp32-s3-devkitc-1 -e battery`. Acrescentar testes nos runners existentes. Host nao substitui radio/I2S reais.

**8. Entrega Segura**
- Entregar tabela baseline Wi-Fi / BLE anterior / candidatos / escolhido, com frio/quente e topologia de internet separados, WAVs de escuta, CSV/JSONL e graficos de latencia/jitter/filas. Guardar manifesto de versoes/configuracoes e as limitacoes do ensaio.
- Novos arquivos provaveis: `tools/benchmark_voice.py`, testes do harness e, apenas se necessario, modulos especificos de midia BLE/Opus. Modificar os testes existentes e documentacao pertinente; evitar refactor geral do assistente.
- Instrumentacao e fixture exclusiva de bancada ficam desligadas por padrao. Flags experimentais separadas; promover uma variante so depois de passar nos gates. Rollback: retornar ao BLE PCM acumulado validado, nao reintroduzir repeticao DMA.
- Antes de encerrar: restaurar Auto, descanso, janela aprovada e configuracoes temporarias do telefone. Atualizar pacote/hash, gravar somente aplicacao em `0x10000`, preservar particoes e instalar firmware `battery` para uso sem cabo. Validar acusticamente tambem em bateria: CDC fica desabilitado e os numeros USB nao substituem esse ensaio.
- Registrar duty cycle/tempo acordado; so alegar consumo em mA com medidor fisico adequado. Nao aumentar silenciosamente o prazo de deep sleep. Nenhum commit/push sem pedido.

**Referencias**
- Sounddevice: https://python-sounddevice.readthedocs.io/en/latest/api/streams.html (timestamps ADC/DAC, callbacks, latencia e overflow).
- Opus: https://opus-codec.org/ (codec interativo, bitrates e frames).
- ITU-T P.800: https://www.itu.int/rec/T-REC-P.800/en (referencia de avaliacao subjetiva; a escuta proposta nao e certificacao MOS).
