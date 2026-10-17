# Validação executada — 16/10/2026

## Escopo e ambiente

Somente arquivos de implementação/documentação dentro de `companion-android/`. Ferramentas e artefatos de build em pasta temporária externa preaprovada. Nenhuma execução ADB, instalação de aplicativo ou interação com dispositivo real.

- Windows 11 amd64.
- Microsoft OpenJDK 17.0.20.1; Python 3.12.10.
- Plataformas SDK instaladas: 35 e **36**; Build Tools: **35.0.0** e 36.0.0.
- Gradle **8.13**, distribuição oficial com SHA-256 validado e fixado no bootstrap.
- AGP **8.13.2**, resolvido efetivamente no build.

## Comandos e resultados

Executados a partir da raiz do repositório:

```powershell
python companion-android/build_android.py -- --version
python companion-android/build_android.py assembleDebug testDebugUnitTest lintDebug
python companion-android/test_protocol.py
git diff --check
```

| Verificação | Resultado |
| --- | --- |
| Bootstrap Gradle e SHA-256 oficial | OK; Gradle 8.13 / JDK 17 confirmados |
| `assembleDebug` | PASS; APK debug real gerado |
| `testDebugUnitTest` | PASS; 17 testes, sem falhas/erros |
| `lintDebug` | PASS; `No issues found.` com `warningsAsErrors=true` |
| `javac --release 17` + `JUnitCore`, sem Gradle/Android | PASS; `OK (17 tests)` |
| `git diff --check` | Sem diagnóstico; novos arquivos ainda não staged |

Foi necessário corrigir achados reais da primeira rodada de lint: guards de API 33 nos callbacks CDM, flags/permissão do receiver dinâmico, regras de extração de backup, ícone do app e strings de UI. A rodada final passou sem baseline ou desativação desses checks.

Avisos remanescentes fora do relatório de lint: uso dos callbacks CDM deprecated para compatibilidade com Android 12 e mensagem de recursos deprecated do Gradle/AGP incompatíveis com futura versão 9. A toolchain usada continua fixada em 8.13.

## Cobertura significativa do protocolo

**5 testes em `CodecPolicyWindowTest`:**

1. Layout little-endian, cópias defensivas e round-trip do codec.
2. Rejeição de envelopes, canais, comprimentos e controles inválidos.
3. Allowlist exata: URL, IP, hostname com sufixo, subdomínio, wildcard, Unicode e UTF-8 malformado.
4. Rejeição de DNS privado/local/CGNAT/multicast e aceitação de IPv4/IPv6 públicos, sem resolver nomes externos nos testes.
5. Janela oito, ACK individual da cabeça, rejeição de ACK duplicado e wrap de seq após 65.536 frames.

**12 testes em `RelayEngineTest`:**

1. Fragmentação TLS em payloads de 173 bytes, recomposição exata e bloqueio do nono DATA até ACK ESP.
2. ACK somente depois de write TLS retornar; outro canal progride durante write bloqueado.
3. DATA fora de sequência e ACK fora da cabeça encerram o canal.
4. Nono frame inbound não consumido aborta sem ACK/replay.
5. Troca de epoch fecha socket antigo e descarta DATA/CLOSE/OPEN antigos.
6. Cancelamento durante connect não permite OPENED tardio.
7. Deadlines de connect, write e ACK encerram o socket.
8. Quatro canais independentes, janela cheia de um não bloqueia outro; shutdown fecha todos.
9. Host não autorizado é rejeitado antes de criar socket; frame malformado é recusado.
10. EOF drena DATA/ACK pendentes antes de CLOSE.
11. Troca de epoch invalida DATA ainda na fila BLE; falha de callback antigo não fecha o canal novo.
12. DATA notificado antes do callback do write OPENED é aceito com TLS pronto.

Os testes usam socket factory, writer BLE, executor e relógio injetáveis; executam threads reais com latches/filas e deadlines curtos apenas para detectar bloqueio. Não são testes de Bluetooth Android nem de certificados públicos reais.

## Artefatos de evidência

Base: `%LOCALAPPDATA%\Temp\opencode\ideiarec-android-toolchain\build\app\`.

- `outputs\apk\debug\app-debug.apk`
- `test-results\testDebugUnitTest\TEST-br.com.ideiarec.companion.protocol.CodecPolicyWindowTest.xml`
- `test-results\testDebugUnitTest\TEST-br.com.ideiarec.companion.protocol.RelayEngineTest.xml`
- `reports\tests\testDebugUnitTest\index.html`
- `reports\lint-results-debug.txt` / `.xml` / `.html`

## Ainda depende de validação em hardware

Pareamento PIN real e bond, permissão/criptografia de RX/CCCD no ESP32, evento LE de criptografia API 36, MTU, retomada por CDM após reboot, políticas OEM com tela apagada, estabilidade BLE/TLS e execução completa do pipeline do firmware. As limitações de API 31–35 e DNS nativo estão descritas no README; não são cobertas pelo sucesso dos testes host.
