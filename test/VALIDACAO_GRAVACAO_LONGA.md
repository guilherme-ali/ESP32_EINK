# Correção de salvamento de gravações maiores

Data: 2026-10-09. Estado: código corrigido, testes host e builds USB/bateria
aprovados; **firmware desta correção ainda não gravado nem validado no aparelho**.

## Relato e evidência

Dois áudios de aproximadamente 5 segundos salvaram; um áudio de cerca de
1 minuto permaneceu em salvamento por aproximadamente 2 minutos, houve reinício,
e a nota apareceu com 0 segundos, sem permitir envio.

O caminho anterior fazia `seek(0)` e escrevia o cabeçalho de 44 bytes no
arquivo WAV grande ao encerrar. No LittleFS, a organização CTZ copy-on-write
copia os blocos seguintes ao alterar o início. Isso requer trabalho e espaço
adicionais proporcionais ao arquivo. A gravação não fazia checkpoints reais:
a lambda chamada `flush` apenas chamava `write`. A duração/validação dependia
do tamanho e cabeçalho comprometidos antes do reinício.

O custo estrutural foi reproduzido com LittleFS upstream real. A causa exata
do reset relatado (watchdog, alimentação ou outra) **não foi confirmada sem
log do dispositivo**. O firmware agora registra `[Boot] reset_reason/wakeup`
e `[Record] finalize_ms/checkpoint/full/failed` para a validação física.

## Implementação

1. `.wav` guarda somente o cabeçalho; `.pcm` recebe as amostras mono por append.
   O placeholder é validado e promovido antes de iniciar captura. O cabeçalho
   final passa por `.wav.tmp`, flush, releitura e rename; somente 44 bytes mudam.
2. `NoteFiles::openRead` expõe header + PCM como um `File` WAV canônico. Player,
   STT, Drive, checksums, seleção do worker, catálogo e HTTP usam essa leitura.
   O áudio não é duplicado para criar outro WAV físico.
3. Checkpoints ocorrem a cada aproximadamente um segundo **de PCM escrito**.
   A cauda ainda no ring/I2S não tem persistência garantida. Perda de energia
   recupera o prefixo sincronizado, não necessariamente toda a fala capturada.
4. A leitura reconstrói tamanho/duração a partir das amostras persistidas.
   Um WAV legado com placeholder e payload também é corrigido em leitura,
   sem reescrever o original. Cabeçalhos de formato desconhecido ou corrupção
   arbitrária não são tratados como PCM recuperável. Um arquivo só com 44 bytes
   não possui áudio para recuperar.
5. `requestStop` retorna sem esperar o dreno. A UI mantém `Saving` até ambos os
   workers terminarem; `finish` libera o ring. O writer bloqueia por um tick
   entre escritas, permitindo executar tarefas de menor prioridade.
6. Saturação interrompe a captura explicitamente e drena o prefixo já aceito.
   Falhas de I/O interrompem escritas; falhas de metadados conservam o PCM.
   Fim de espaço finaliza no limite de amostras inteiras, preservando a reserva.
7. Não há timeout de sono nem atendimento do portal durante captura/finalização.
   O contador de inatividade é reiniciado ao terminar. Escrita comprovadamente
   vazia é limpa; arquivos ilegíveis são preservados e exibidos como `sem audio`,
   fora das pendências normais de sincronização.
8. `.wav.delete` permite retomar exclusão de PCM interrompida. Boot conclui a
   limpeza de PCM e todos os associados antes de remover o tombstone; geração
   de nomes evita também transcrições/estados órfãos, sem reutilizar seu conteúdo.

## Testes executados

`tools/check_project.py --cppcheck`: **196/196**.

| Suíte | Casos |
|---|---:|
| HTTP/STT | 56 |
| Storage/Drive | 67 |
| Gemini integrado | 26 |
| Progresso portátil | 15 |
| Gravador de produção com codec/RTOS/FS host | 23 |
| LittleFS upstream em flash NOR simulada | 9 |

O teste do gravador alimenta 60 s de amostras determinísticas a 16 kHz,
gerando 1.920.000 bytes mono; compara **cada byte** do canal esquerdo e do WAV
virtual final, não apenas o tamanho. Os testes host executam mais rápido que
tempo real e não medem captura física de microfone, I2S, watchdog ou alimentação.

Drive foi exercitado com o WAV virtual de 1.920.044 bytes, confirmação remota
simulada e verificação de hash consistente. O hash fake testa contratos, não
a correção criptográfica do MD5. Recuperação de legado e alterações no PCM
invalidando confirmação também foram exercitadas.

### Finalização no LittleFS real — contagens, não tempo no ESP32

LittleFS **v2.9.3** oficial, sources com SHA-256 fixado, compilado como C99.
NOR em memória: 4.864 KiB, blocos de 4 KiB. Dois WAVs anteriores de 5 s
presentes em todos os cenários. Essa versão não foi identificada como a
versão binária embutida no SDK do ESP32.

| Áudio | Desenho | Bytes programados ao finalizar | Erases |
|---|---|---:|---:|
| 5 s | Antigo, reescreve cabeçalho no WAV grande | 161.024 | 40 |
| 5 s | Novo, PCM + cabeçalho separado | 1.280 | 1 |
| 60 s | Antigo | 1.924.352 | 470 |
| 60 s | Novo | 1.280 | 0 |

Em cenários com pouco espaço, o antigo falhou com `NOSPC` esperado e o novo
concluiu. A atualização do header novo programou **zero bytes nos blocos do PCM**.
Remontagens exercitaram 62 snapshots de checkpoints, duas caudas sem sync e
cinco pontos de interrupção de metadados. São operações NOR simuladas; esses
números não autorizam prometer tempo de salvamento ou ausência de resets no hardware.

## Compilação

`pio run -e esp32-s3-devkitc-1 -e battery`: ambos aprovados.

Artefatos em `C:/pio-builds/ESP32_EINK/{esp32-s3-devkitc-1,battery}/firmware.bin`.
Não houve acesso serial, leitura da flash, gravação ou reinício do dispositivo
nesta etapa. Não houve commit/push desta correção.

## Validação física no ESP32-S3 (COM7)

Realizada em 2026-10-09 com o dispositivo conectado e acordado pelo usuário.

### 1. Diagnóstico da nota problemática anterior (`20261009-202013.wav`)
- Montagem da partição LittleFS em backup bruto (`0x310000` a `0x7D0000`):
  - Entrada de diretório: tamanho 0 bytes (`lfs_stat` size = 0, EOF imediato na leitura).
  - Sem PCM associado e cabeçalho nunca finalizado (devido ao reinício do hardware no `seek(0)` antes do `close()`).
  - Classificada corretamente pelo firmware como `sem audio` (`sampleRateHz = 0`), excluída da contagem de pendências e sem bloquear o lote.

### 2. Novas gravações realizadas no aparelho
- Três notas gravadas fisicamente no dispositivo:
  1. `20261009-214516.wav`: 324.652 bytes (~10,1 s mono 16 kHz), finalização normal.
  2. `20261009-214536.wav`: 226.348 bytes (~7,1 s mono 16 kHz), finalização normal.
  3. `20261009-214600.wav`: **2.766.892 bytes (86,46 s mono 16 kHz — ~2,77 MB)**:
     - Dreno e finalização concluídos em **589 ms** (contra ~2 minutos de travamento e reinício na versão anterior).
     - **0 overflows** no ring buffer e **0 falhas de escrita**.
     - Checkpoint completo comitado na flash (`checkpointBytes = 2766848`).
- Download HTTP de todos os WAVs do aparelho via Wi-Fi:
  - Formato canônico WAV PCM 16-bit 16 kHz mono íntegro em todos os arquivos.
  - O WAV virtual gerado por `NoteFiles::openRead` foi validado como **idêntico** ao PCM gravado na flash (`payload_identical: true`).

### 3. Sincronização em lote (STT + Markdown + Google Drive)
- Execução de `sync` pelo console serial:
  - `20261009-214600` (86,5 s): Gemini STT (`gemini-3.5-transcribe`, 2,45 s) + Markdown (fallback automático em 429 para `gemini-3.5-flash-lite`, 1,60 s) + Drive (14,27 s). Total: 53,8 s.
  - `20261009-214536` (7,1 s): Gemini STT (1,58 s) + Markdown (`gemini-3.5-flash-lite`, 1,05 s) + Drive (5,91 s). Total: 26,5 s.
  - `20261009-214516` (10,1 s): Gemini STT (retry automático em HTTP 503, 2,44 s) + Markdown (0,77 s) + Drive (7,12 s). Total: 59,4 s.
- Resultado final: **IA: 3 | Drive: 3 | Falhas: 0**.
- Conteúdo transcrito e estruturado em Markdown conferido no dispositivo e no Drive.
- Status pós-sincronização: **6 notas cadastradas, 0 pendências**, com resposta imediata *"Nenhuma nota pendente"* em novas tentativas.

## Limite de reuniões longas e próximo passo

A partição LittleFS interna tem ~4,75 MiB compartilhados por todas as notas (~1,1 MiB livres após os testes). Gravações acima de 2 a 3 minutos ou reuniões de longa duração exigem o armazenamento em cartão **microSD**, cujo suporte em hardware existe na placa mas o backend ainda não foi implementado.

## Referências consultadas

- LittleFS upstream: `DESIGN.md` (CTZ/copy-on-write), `README.md` e `lfs.h`
  da tag v2.9.3 (sync, persistência e rename).
- Arduino-ESP32 instalado, core 2.0.17: `libraries/FS/src/FSImpl.h`,
  `FS.h` e `FS.cpp` (contrato FileImpl, compartilhamento e close).
