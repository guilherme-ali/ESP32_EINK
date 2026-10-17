# Testes de NetworkClient / guard host de NetworkRoute

Executar na raiz em PowerShell, sem PlatformIO e sem modificar runners:

```powershell
& "C:\Strawberry\c\bin\g++.exe" -std=c++17 -O1 -g -Wall -Wextra -Werror -ffunction-sections -fdata-sections -DARDUINO_ARCH_ESP32 -Itest/host/network_route_tests_stubs -Itest/host/stubs src/net/network_client.cpp src/net/http_client.cpp test/host/network_route_tests.cpp "-Wl,--gc-sections" -o test/host/.build/network_route_tests.exe
& "test/host/.build/network_route_tests.exe"

& "C:\Strawberry\c\bin\g++.exe" -std=c++17 -O1 -Wall -Wextra -Werror -Itest/host/network_route_tests_stubs -Itest/host/stubs test/host/network_route_tests_host.cpp -o test/host/.build/network_route_tests_host.exe
& "test/host/.build/network_route_tests_host.exe"
```

A pasta `test/host/.build` deve existir (criada pelo checker do projeto).

## Escopo

- `network_route_tests.cpp`: compila **o ramo ESP32 real** de
  `src/net/network_client.cpp` e o `HttpClient` real. Stubs somente nas fronteiras
  SDK, seleção da rota e API PhoneRelay; não há alias/falso NetworkClient.
- 15 casos: TLS direto/CA, sem Wi-Fi, falha TLS antes de body, DNS assíncrono
  expirado/callback tardio, escrita BLE parcial sem replay, escritas curtas,
  telefone já selecionado, rota fixa por socket, canais/RAII, fallback desabilitado
  e prazo esgotado, segundos versus milissegundos/contexto TLS intocado no relay,
  quatro DNS pendentes, canais esgotados, wrap de millis e write nativo sem migração.
- `network_route_tests_host.cpp`: alias host e status/queda do Wi-Fi stub,
  `recover`, label e fallback desabilitado, sem link com PhoneRelay.

Não exercita NimBLE/TLS/Android reais, tarefas concorrentes do ESP32 nem a política
ESP32 de `network_route.cpp`/scan de `WifiManager`. A compilação dessa política
depende dos campos `Settings::bleFallbackEnabled` e `Settings::blePasskey` fornecidos
pelo coordenador. Os 196 testes antigos continuam no checker existente.

## Contrato da integração

Configurar com clientes/threads encerrados; chamar `NetworkRoute::connect` antes
de iniciar os workers. Chamadas paralelas de `fallbackToPhone` são serializadas;
cada socket permanece no transporte em que foi aberto até `stop()`.
Fechar STT, Drive, Live e gateway/juntar workers antes de `NetworkRoute::finish()`.

O orçamento de seleção padrão é 15 s: associação Wi-Fi até 5 s, probe HTTPS/204
até 2 s, restante para BLE. O connect do socket reparte DNS/TCP/TLS e BLE no prazo
recebido; com fallback habilitado, a parte nativa usa até metade/5 s. Writes usam
ms; `setTimeout` público mantém segundos. Scan síncrono e init NimBLE não são
preemptíveis. O probe com a política `setInsecure` existente detecta portais comuns,
mas não autentica a identidade Google. DNS/TLS no relay pertencem ao Android.

Referências primárias consultadas para unidades/URL (além do SDK local 2.0.17):

- `https://raw.githubusercontent.com/espressif/arduino-esp32/2.0.17/libraries/WiFiClientSecure/src/WiFiClientSecure.cpp`
- `https://android.googlesource.com/platform/packages/modules/NetworkStack/+/refs/heads/main/res/values/config.xml`
