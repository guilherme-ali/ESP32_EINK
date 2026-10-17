# IdeiaRec Companion — Android Java 17

Aplicativo Android nativo real, independente, em `companion-android/`. Faz a ponte **GATT ↔ TLS** para o ESP32. O firmware continua executando exatamente seu pipeline de transcrição, Drive, OAuth e IA. O app não interpreta HTTP, não faz transcrição, não mantém credenciais desses serviços e não implementa OAuth. HTTP/WebSocket/WSS são bytes de aplicação transportados pelo `SSLSocket`.

## Compilar e testar

Ambiente validado em **16/10/2026**:

| Componente | Versão |
| --- | --- |
| Java (linguagem e JDK) | 17 / Microsoft OpenJDK 17.0.20.1 |
| Android Gradle Plugin | 8.13.2 |
| Gradle | 8.13 |
| minSdk | 31 (Android 12) |
| compileSdk / targetSdk | 36 (Android 16) |
| SDK Build Tools | 35.0.0, instalado e compatível com AGP 8.13 |
| SDK existente | `C:\Users\guilh\AppData\Local\Android\Sdk` |
| Dependências do app em runtime | Somente Android/Java; nenhuma biblioteca externa |
| Dependências de teste | JUnit 4.13.2 / Hamcrest Core 1.3 |

Na raiz do repositório, em PowerShell:

```powershell
Test-Path -LiteralPath "C:\Users\guilh\AppData\Local\Temp\opencode"
python companion-android/build_android.py assembleDebug testDebugUnitTest lintDebug
python companion-android/test_protocol.py
```

`build_android.py`:

1. Obtém Gradle **8.13** de `services.gradle.org` por HTTPS. Segue o redirecionamento oficial da distribuição.
2. Compara o `.sha256` oficial com o valor fixado no script e verifica o ZIP antes da extração:

   ```text
   20f1b1176237254a6fc204d8434196fa11a4cfb387567519c61556e8710aed78
   ```

3. Usa o SDK existente (`ANDROID_HOME` pode sobrescrever o caminho). Exige plataforma 36; não instala SDK silenciosamente.
4. Copia somente este projeto para caminho ASCII temporário e mantém Gradle, caches e saídas nessa pasta. Isso evita o caminho Unicode/Google Drive em AGP/aapt.
5. Executa as tarefas solicitadas com `--no-daemon`. Não instala APK e não executa ADB.

Pasta base das saídas:

```text
C:\Users\guilh\AppData\Local\Temp\opencode\ideiarec-android-toolchain\
  build\app\outputs\apk\debug\app-debug.apk
  build\app\reports\tests\testDebugUnitTest\index.html
  build\app\reports\lint-results-debug.html
  build\app\reports\lint-results-debug.txt
  build\app\test-results\testDebugUnitTest\TEST-*.xml
```

O APK debug é assinado com a chave debug padrão do Android. APK, ZIP, JAR, caches e arquivos de assinatura não estão versionados no projeto. O Gradle pode criar a chave debug no local padrão do usuário (`~/.android`), se ainda não existir.

O runner `test_protocol.py` usa `javac --release 17` e `org.junit.runner.JUnitCore` **sem `android.jar`, sem Gradle e sem mocks Android**. Obtém os JARs históricos do Maven Central por HTTPS e verifica seus SHA-1 oficiais fixados (esses artefatos não publicam `.sha256` no Maven). Saídas em `%LOCALAPPDATA%\Temp\opencode\ideiarec-protocol-tests\`.

Com Gradle próprio já disponível, também é possível executar na pasta `companion-android/`:

```powershell
$env:ANDROID_HOME = "C:\Users\guilh\AppData\Local\Android\Sdk"
gradle -PexternalBuildDir=C:\Users\guilh\AppData\Local\Temp\opencode\ideiarec-direct-build assembleDebug testDebugUnitTest lintDebug
```

Prefira o bootstrap para evitar a validação de caminhos Unicode feita pelo AGP. Não foi necessário usar `android.overridePathCheck`.

## Uso no telefone

1. No portal do ESP32, configure **PIN de seis dígitos**, nome anunciado `IdeiaRec…` e o serviço GATT deste contrato.
2. No app, toque em **Permissões**: conceda dispositivos próximos (`BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`) e, no Android 13+, notificações.
3. Toque em **Parear / associar IdeiaRec**. A seleção é do Companion Device Manager (CDM), filtrada simultaneamente por nome e service UUID.
4. Confirme a associação CDM. O app solicita `createBond()` quando necessário. **Digite/confirme o PIN somente no diálogo Bluetooth do Android.** O app não recebe o PIN do portal, não chama `setPin()` ou `setPairingConfirmation()` e não aceita pareamento automaticamente.
5. Aguarde o estado **Pronto**. Fechar a Activity não encerra uma conexão ativa.
6. **Parar e desativar automático**, ou a ação **Parar** da notificação, encerra canais/GATT e desativa a observação automática persistida.
7. **Reassociar** remove/refaz a associação CDM. Isso não apaga automaticamente o bond Bluetooth: para trocar um PIN com bond antigo, remova o pareamento nas configurações Bluetooth do Android e refaça-o.

Se a tela de seleção for interrompida por recriação da Activity/processo, a abertura seguinte reconcilia a associação já registrada pelo Android. O app mantém somente um dispositivo configurado.

### Hosts

A UI permite editar a lista completa, um **domínio DNS exato** por linha. Salvar encerra as conexões existentes e, se o automático estava ativo, inicia uma sessão nova com a política atualizada. Uma lista vazia bloqueia todos os OPEN.

Padrões:

```text
generativelanguage.googleapis.com
www.googleapis.com
oauth2.googleapis.com
accounts.google.com
script.google.com
script.googleusercontent.com
api.github.com
api.groq.com
api.openai.com
```

Aceita apenas porta **443**. Rejeita URLs, curingas, sufixos implícitos, endereços IP, espaços e formas alternativas de IPv4. Nomes são ASCII, normalizados em minúsculas; para IDN, use a forma punycode exata. O tamanho do host no OPEN continua limitado pelo payload GATT (171 bytes após a porta).

A resolução DNS é validada contra endereços não públicos, incluindo privados, loopback, link-local, multicast, CGNAT e faixas de documentação. O IP escolhido é fixado na conexão TCP. O `SSLSocket` recebe **o hostname original**, SNI explícito e `SSLParameters.setEndpointIdentificationAlgorithm("HTTPS")`, com trust manager padrão Android. Não há trust-all, troca por hostname de IP nem redirecionamento HTTP automático. Um redirect do firmware para outro domínio precisa de outro OPEN autorizado.

## Lifecycle e execução em background

```text
Activity → associação CDM + runtime permissions + observar presença
                    ↓
CompanionDeviceService → presença → RelayService connectedDevice
                                       ↓
                       conectar / MTU / serviços / bond / segurança / CCCD
                                       ↓
                              relay durante link ativo
                                       ↓
                           desconexão / falha / parada → cleanup

BOOT_COMPLETED / MY_PACKAGE_REPLACED → apenas re-observar presença
```

- `PresenceService` usa `onDevicePresenceEvent()` + association ID no Android 16; usa os callbacks compatíveis nas APIs 31–35.
- `RelayService` promove imediatamente a foreground `connectedDevice`, somente ao tentar conectar ou manter o link de comunicação. Não fica como FGS esperando presença com Bluetooth desconectado.
- Declara `REQUEST_COMPANION_RUN_IN_BACKGROUND`, `REQUEST_COMPANION_START_FOREGROUND_SERVICES_FROM_BACKGROUND` e `REQUEST_COMPANION_USE_DATA_IN_BACKGROUND`, além de `REQUEST_OBSERVE_COMPANION_DEVICE_PRESENCE` e das permissões FGS/Bluetooth apropriadas.
- Usa `START_NOT_STICKY`: uma falha termina a sessão. Uma nova presença CDM ou início manual abre outro GATT, sem replay. Não há loop infinito de reconnect nem FGS iniciado indiscriminadamente no boot.
- Uma indicação de desaparecimento do anúncio BLE não derruba um link já conectado: o periférico pode deixar de anunciar durante a conexão. Desconexão Bluetooth encerra o relay.
- Notificação negada não é tratada como permissão Bluetooth; continuam valendo as regras do sistema para visibilidade de FGS. O botão permite solicitar notificações novamente.
- A configuração fica em um único `SharedPreferences`: MAC, ID de associação, automático, allowlist e último status sem conteúdo de tráfego. Backup/transferência dessa configuração são excluídos. A UI distingue serviço ativo de último estado persistido.

### Bond e criptografia: dependência real do periférico

**Associação CDM não é bond e bond não é uma prova de criptografia do link corrente.**

- Em API **36**, o receiver é registrado **antes** de conectar. Antes de qualquer write de CCCD/dados, aguarda bond `BONDED` e `ACTION_ENCRYPTION_CHANGE` LE com status de sucesso e criptografia habilitada. Perda de criptografia aborta o GATT. Sem esse evento, não libera writes e encerra por timeout.
- Em APIs **31–35**, não existe uma API pública equivalente para observar diretamente esse estado. O app aguarda o bond e usa o procedimento GATT protegido pelo Android; a garantia de criptografia depende de o ESP32 exigir **encryption/authenticated pairing em RX e CCCD TX**. O sucesso de CCCD só comprova o requisito de segurança se o servidor o exigir. O app não consegue inspecionar remotamente essas permissões ATT.
- Em reconexão já bonded, o ESP32 precisa iniciar a segurança do link, especialmente na API 36: o app não faz um write inseguro para provocar criptografia. Sem iniciativa do periférico/evento, o app falha fechado.

O firmware não foi alterado nem validado em hardware nesta entrega. Portanto, a condição de segurança acima é uma dependência explícita de integração, e não uma garantia de comportamento do ESP32 sem teste.

## Organização

```text
app/src/main/java/br/com/ideiarec/companion/
  MainActivity.java       UI nativa, permissões e associação
  Config.java             configuração única e status sanitizado
  Presence.java            observar presença com APIs por versão
  PresenceService.java     entrada do sistema CDM
  RestoreReceiver.java     re-observação após reboot/update
  RelayService.java        FGS, ownership e cleanup
  GattTransport.java       fila serial BLE, MTU, bond, callbacks e timeout
  TlsSocketFactory.java    DNS público + TCP/TLS nativo, SNI/hostname
  protocol/
    Frame.java             codec independente de Android
    HostPolicy.java        política exata de destino
    SendWindow.java        janela de 8 e ACK individual FIFO
    RelayEngine.java       canais, fluxo, deadlines e cancelamento
app/src/test/java/.../protocol/
  CodecPolicyWindowTest.java
  RelayEngineTest.java
```

Detalhes de wire protocol, concorrência e tempos em [PROTOCOL.md](PROTOCOL.md). Resultados efetivamente executados em [VALIDACAO.md](VALIDACAO.md).

## Limites reais

- Não houve ADB, instalação, emulador, telefone ou ESP32 conectado. Build/testes não substituem validação de PIN, MTU, criptografia, callbacks de presença, tela apagada ou transferência real em Android 12–16/OEMs.
- Android pode interromper o processo por force-stop, revogação de permissões, desligamento do Bluetooth ou políticas do fabricante. Associação/presença não prometem execução ininterrupta. Depois de force-stop, é necessário abrir o app novamente; um reboot não deve contornar isso.
- DNS Java nativo pode não responder imediatamente a interrupt/close. Cancelamento invalida o canal e impede ressuscitar a conexão; o worker DNS pode permanecer ocupado até o resolver do sistema retornar. O pool e sua fila são limitados e o cancelamento fecha o socket antes de qualquer connect tardio.
- O socket tenta o primeiro IP público retornado; não implementa Happy Eyeballs/fallback nem proxy, CONNECT, túnel TCP arbitrário ou retry de bytes. Sem rede, certificado inválido, host negado ou timeout resultam em fechamento do canal.
- O app é uma ponte TLS de aplicação: os bytes entre ESP32 e telefone dependem da segurança BLE descrita acima. Não há gravação de áudio no app, logs de payload, chaves ou tokens, nem inspeção de protocolos.
- Existem avisos de depreciação do compilador pelos callbacks CDM legados necessários ao minSdk 31 e aviso de compatibilidade futura do AGP/Gradle com Gradle 9. A combinação está fixada; não atualizar Gradle isoladamente.

## Fontes oficiais consultadas

Documentação consultada antes de fixar toolchain/lifecycle; detalhes de API também foram conferidos com `javap` no `android-36/android.jar` instalado:

- [AGP 8.13 — compatibilidade e patch 8.13.2](https://developer.android.com/build/releases/past-releases/agp-8-13-0-release-notes)
- [Gradle 8.13 — release notes](https://docs.gradle.org/8.13/release-notes.html)
- [Comunicação BLE em background](https://developer.android.com/develop/connectivity/bluetooth/ble/background)
- [Restrições para iniciar FGS em background e exceções companion](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)
- [Tipo connectedDevice e pré-requisitos](https://developer.android.com/develop/background-work/services/fgs/service-types#connected-device)
- [CompanionDeviceService e presença API 36](https://developer.android.com/reference/android/companion/CompanionDeviceService)
- [CompanionDeviceManager — associação e observação](https://developer.android.com/reference/android/companion/CompanionDeviceManager)
- [BluetoothDevice — bond e ACTION_ENCRYPTION_CHANGE](https://developer.android.com/reference/android/bluetooth/BluetoothDevice)
- [BluetoothGatt — MTU e writes com resposta](https://developer.android.com/reference/android/bluetooth/BluetoothGatt)
- [Broadcast receivers e broadcasts de Bluetooth](https://developer.android.com/develop/background-work/background-tasks/broadcasts)
- [Regras de backup/transferência Android 12+](https://developer.android.com/about/versions/12/backup-restore#xml-changes)

As páginas Android não responderam ao fetch direto neste ambiente; a consulta foi realizada pelo mecanismo de busca com análise das URLs oficiais. O artefato AGP foi efetivamente resolvido pelo Google Maven e utilizado no build.
