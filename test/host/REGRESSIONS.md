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
passa nos 155 testes, incluindo as regressões de fila, cache e contadores.
