# Regressão corrigida — integração de telemetria Drive

## `drive_observer_byte_events_identify_asset_filename`

- A API publica `current,total` **por arquivo**, enquanto o modelo agrega
  WAV/TXT/MD. Para atribuir bytes sem contar retries, `detail` deve identificar
  o arquivo (`test.wav`, `test.txt`, `test.md`) nos eventos de transferência.
- O defeito original publicava apenas `"enviando arquivo"` em `detail`, sem
  identificar o asset. A produção agora conserva o nome em `Job::transferName`
  e o publica nos eventos iniciais/ACK e incrementais de `wrote`.
- Reprodução: `tools/check_project.py --cppcheck`.
- O caso exige nome nos eventos DriveUpload com bytes e passou após a correção.
  Não há `xfail`, remoção do caso ou expectativa que aceite o defeito.

Fases literais sem bytes conservam mensagens descritivas. O conjunto atual
passa nos 196 testes, incluindo as regressões de fila, cache, contadores e gravação.

## Gravação longa: finalização e recuperação

- O gravador antigo fazia `seek(0)` e atualizava o cabeçalho de um arquivo
  LittleFS grande. A cadeia CTZ exige copiar o restante, podendo falhar por
  falta de espaço. Não havia checkpoints reais de `File::flush` durante a captura.
- O gravador real agora escreve PCM append-only e um cabeçalho separado. A suíte
  compara todas as 960.000 amostras do canal esquerdo de 60 s a 16 kHz, além de
  gravações consecutivas, overflow terminal, falhas de tasks/escrita/flush,
  preservação de prefixo e recuperação somente leitura.
- Drive recebe o WAV canônico de 1.920.044 bytes; hash e confirmação usam a mesma
  view, incluindo recuperação de placeholder legado. Escrita zero não é WAV válido.
- Uma suíte adicional usa LittleFS upstream real em NOR simulado. A atualização
  de header do novo desenho não programa nem apaga blocos pertencentes ao PCM.
  Checkpoints e rename são verificados por remontagem de snapshots.
- Exclusão com falha retém tombstone e pode ser repetida, sem perder o caminho
  para limpar PCM. Colisões não sobrescrevem sessões anteriores.
- Detalhes e o que ainda requer hardware: `test/VALIDACAO_GRAVACAO_LONGA.md`.
