# Gateway HTTP JSON — Google Apps Script

Implementação isolada, sem dependências npm, para usar ferramentas de vault GitHub
e Google Calendar diretamente de um cliente HTTPS. Não depende de celular. Usa os
serviços das contas do proprietário e está sujeito às cotas e políticas dessas
contas; não inclui assinatura, mecanismo de cobrança ou promessa de disponibilidade.

## Arquivos e execução local

Requer Node 22 ou posterior. A partir de `gateway/`:

```sh
npm run lint
npm test
npm run build
```

- `src/core.js`: núcleo síncrono UMD/CommonJS, validação, patches e idempotência.
- `src/apps-script.js`: PropertiesService, ScriptLock, CacheService, GitHub REST e
  Google Calendar REST por UrlFetchApp e ScriptApp.getOAuthToken().
- `src/entry.js`: `doGet`, `doPost` e autorização manual do proprietário.
- `tool-schema.json`: declarações `functionDeclarations` Gemini compartilháveis.
- `appsscript.json`: runtime V8, escopos explícitos e configuração do web app.
- `scripts/`: lint com `node --check` e build determinístico por concatenação.
- `test/`: testes Node de protocolo HTTP com serviços simulados e execução do bundle
  em VM, incluindo recuperação de operações cujo resultado não chegou ao ledger.

Não há instalação npm obrigatória nem dependências de execução. `npm test` executa
`node --test test/*.test.cjs`. Os testes não usam rede, contas ou credenciais reais.
O lint verifica sintaxe JavaScript, JSON e espaços no fim das linhas; não substitui
um analisador estático de segurança nem um teste de integração na plataforma GAS.

O build gera somente `dist/Code.gs` e `dist/appsscript.json`, ignorados pelo Git.
O artefato não usa `require`, módulos ES ou APIs Node. O build lê somente os fontes
e o schema deste diretório: não lê variáveis de ambiente, arquivos `.env` ou tokens.
Não mantenha cópia manual do bundle versionada. Não coloque segredos nos fontes.

## Configuração e deploy manual

1. Prepare um repositório GitHub **já inicializado**, com a branch escolhida
   existente e as notas em UTF-8. O gateway não inicializa repositórios vazios.
   Configure o sincronismo do vault com esse repositório fora deste gateway.
2. Crie um personal access token **fine-grained**, restrito a esse repositório,
   com permissão **Contents: Read and write** e Metadata de leitura. Não é
   necessário conceder Workflows: caminhos administrativos estão bloqueados.
   Expiração, aprovação de organização e regras de proteção de branch também
   podem impedir gravações; o gateway não contorna essas regras.
3. Crie um projeto Apps Script independente na conta que será proprietária.
   Execute `npm run build` localmente, copie `dist/Code.gs` para um arquivo
   `Code.gs` no editor e habilite a visualização do manifesto para copiar
   `dist/appsscript.json`. Use o bundle completo, não os três fontes separados.
4. Em **Configurações do projeto → Propriedades do script**, preencha a tabela
   abaixo. As propriedades são compartilhadas pelo projeto, não pelo usuário
   anônimo. Nunca use UserProperties ou parâmetros de URL para guardar o token.
5. Habilite a **Google Calendar API** no projeto Google Cloud associado quando
   necessário (obrigatório em projeto Cloud padrão configurado manualmente).
   As chamadas são REST: não é preciso adicionar o serviço avançado `Calendar`
   ao editor. O manifesto concede `calendar.events`, `script.external_request`
   e `script.storage`; não usa CalendarApp, Drive ou acesso geral ao calendário.
6. Execute manualmente `autorizarGateway` como proprietário e conceda os escopos.
   A conta precisa ter acesso de escrita ao `CALENDAR_ID` escolhido.
7. **Implantar → Nova implantação → App da Web**: executar como **Eu /
   proprietário**, acesso **Qualquer pessoa / Anyone** (inclusive anônimo).
   `USER_DEPLOYING` e `ANYONE_ANONYMOUS` estão no manifesto. Políticas de domínio
   podem proibir esse acesso; nesse caso este modo de operação fica indisponível.
8. Use a URL **HTTPS `/exec`** da implantação. Atualizações exigem nova versão
   da implantação. `/dev` é para editores e não é a URL de produção.

Nenhum desses passos é executado pelos scripts locais. Este código não faz deploy,
login, habilitação de API ou acesso a contas durante build/lint/testes.

### Propriedades do script

| Nome | Obrigatória / padrão | Uso |
| --- | --- | --- |
| `GATEWAY_TOKEN` | Sim, 32 a 1024 caracteres | Segredo compartilhado, aleatório, enviado apenas no corpo POST. |
| `GITHUB_TOKEN` | Sim | PAT fine-grained do repositório, nunca devolvido ao cliente. |
| `GITHUB_OWNER` | Sim | Dono/organização fixo; não vem dos argumentos do modelo. |
| `GITHUB_REPO` | Sim | Repositório fixo. |
| `GITHUB_BRANCH` | `main` | Branch fixa para leitura, índice, histórico e gravação. |
| `VAULT_PREFIX` | Vazio | Allowlist opcional de pasta, por exemplo `Notas` ou `vault/Notas`, sem `/` final. |
| `CALENDAR_ID` | `primary` | Calendário do proprietário ou ID explicitamente configurado. |
| `TIME_ZONE` | Sim | Fuso IANA válido, por exemplo `America/Sao_Paulo`; validado na configuração. |

`TIME_ZONE` documenta o fuso do ambiente; não converte datas nem supre o argumento
`timeZone`, obrigatório em `calendar_create`. Alinhe também `timeZone` do manifesto
ao seu ambiente. IDs do ledger usam o prefixo reservado `gw-ledger:` em Script
Properties; não altere essas entradas manualmente durante operação normal.

Uma forma local de gerar 32 bytes aleatórios, sem salvar arquivo:

```sh
node -e "console.log(require('node:crypto').randomBytes(32).toString('hex'))"
```

Esse comando deve ser executado pelo operador em terminal privado. Não registre
tokens em issues, exemplos, logs, mensagens de commit ou URLs.

## Protocolo fixo

Envie `Content-Type: application/json` (opcionalmente `; charset=utf-8`) e JSON
UTF-8. Os nomes dos campos são sensíveis a maiúsculas/minúsculas:

```json
{
  "version": 1,
  "token": "<segredo-compartilhado-configurado>",
  "id": "pedido-20261020-001",
  "tool": "vault_append",
  "args": { "path": "Notas/Inbox.md", "text": "- [ ] Revisar [[Projeto]]", "heading": "Tarefas" }
}
```

```json
{ "version": 1, "id": "pedido-20261020-001", "ok": true, "result": { "path": "Notas/Inbox.md", "commit": "<sha>", "reconciled": false } }
```

```json
{ "version": 1, "id": "pedido-20261020-001", "ok": false, "error": { "code": "SHA_CONFLICT", "message": "Conflito persistente após três leituras e tentativas.", "retryable": true } }
```

O `id` deve ter de 1 a 80 caracteres de `[A-Za-z0-9_-]`. Gere-o no executor,
persistindo o envelope antes do envio. Um retry de mutação precisa reutilizar
**o mesmo id, ferramenta e argumentos**, mesmo após timeout. Não deixe o modelo
inventar um novo id para repetir a mesma operação. Uma nova intenção requer novo id.
O segredo nunca faz parte do hash persistido, então é possível rotacioná-lo sem
invalidar o ledger. O hash SHA-256 cobre `version`, `id`, `tool` e `args`, com
ordenação canônica de chaves; a ordem dos campos JSON não muda a identidade.

Antes da autenticação/validação de id, erros retornam `id: null`. Campos extras,
chaves JSON duplicadas (inclusive escapadas), Unicode inválido e profundidade
maior que oito são recusados. Não se extrai o token de headers ou query string.
A comparação do token percorre sempre os 64 dígitos de dois hashes SHA-256, sem
saída antecipada por diferença de byte; JavaScript gerenciado não oferece uma
primitiva nativa com garantia formal de tempo constante em nível de CPU.

### Transporte Apps Script

O cliente precisa seguir o redirecionamento HTTPS de ContentService para a URL
temporária em `script.googleusercontent.com`. Não reenviar o POST com token a
destinos arbitrários: siga o redirecionamento de resposta como GET conforme o
cliente HTTP/plataforma. Não use `--post302`/`--post303` indiscriminadamente.
Os erros da aplicação estão no JSON, normalmente com HTTP 200; ContentService não
permite definir livremente códigos HTTP. Erros da plataforma, autorização, cota
ou término forçado podem produzir HTML ou nenhuma resposta: trate como resultado
incerto e preserve o id da mutação. O cliente deve validar `version`, `id` e `ok`.

`GET /exec` retorna somente `{"version":1,"status":"ok"}`. Não consulta properties,
não valida token em query e não testa GitHub/Calendar: é saúde do código HTTP,
não diagnóstico de permissões. Tokens em query nunca são aceitos para POST.

## Ferramentas

Importe ou espelhe `tool-schema.json.functionDeclarations` no executor Gemini.
São sete declarações com `name`, `description` e `parameters`. O próprio servidor
valida os tipos e campos usando esse schema, além dos limites abaixo. O executor
acrescenta `version`, `token` e `id` ao envelope; esses campos não são argumentos
das functions. Nenhuma alteração no firmware é necessária para testar este módulo.

| Ferramenta | Argumentos | Resultado / comportamento |
| --- | --- | --- |
| `vault_search` | `query`, `limit?` | `items`, `truncated`, `limits`, `scope`; literal case-insensitive, sem sintaxe de qualifiers. |
| `vault_read` | `path` | `path`, SHA do blob e conteúdo literal; falha se a nota não couber. |
| `vault_create` | `path`, `content` | Cria apenas se ausente; `path`, commit SHA e `reconciled`. |
| `vault_append` | `path`, `text`, `heading?` | Acrescenta ao fim ou ao final da seção ATX existente e única, antes da próxima de nível igual/menor. |
| `vault_replace` | `path`, `oldText`, `newText` | Substituição literal de exatamente uma ocorrência, incluindo detecção de ocorrências sobrepostas. |
| `calendar_list` | `timeMin`, `timeMax` | Até 50 eventos, `truncated`, `limit`; projeção com título resumido, início/fim e status, sem descrição. |
| `calendar_create` | `title`, `start`, `end`, `timeZone`, `description?`, `reminderMinutes?` | `eventId`, status e `reconciled`. Usa id remoto determinístico e propriedades privadas com id/hash. |

`web_search` **não é implementada nem declarada**: nenhum provedor gratuito foi
explicitamente configurado. Uma chamada direta recebe `TOOL_DISABLED`. Não existe
fallback de pesquisa por conhecimento do modelo, scraping ou provedor pago implícito.

### Vault: segurança e preservação

- Caminhos são **relativos à raiz do repositório**, inclusive quando há prefixo:
  com `VAULT_PREFIX=Notas`, envie `Notas/Inbox.md`, não somente `Inbox.md`.
- Apenas extensão `.md` minúscula, até 240 bytes UTF-8 e 16 componentes. Recusa
  caminhos absolutos, `.`/`..`, componentes vazios, barras invertidas, `%`, `:`,
  controles, `?` e `#`. Componentes `.git`, `.obsidian`, `.config` são bloqueados
  sem distinguir caixa. Não há URL ou repositório fornecido pelo modelo.
- O adaptador resolve cada componente da árvore Git e só aceita blobs regulares
  (`100644`/`100755`). Não segue symlinks/submódulos nem atalhos que escapem do prefixo.
  Não usa download URLs do conteúdo remoto nem segue redirects com credenciais.
- Conteúdo é lido via Git blobs/árvores, validado como UTF-8 e escrito em Base64
  via Contents API. YAML, wikilinks, espaços e fim de linha existentes são
  preservados byte a byte fora do trecho alterado. Não há serialização Markdown.
- Append usa CRLF se presente na nota, senão LF, para os separadores que insere.
  O texto fornecido não é normalizado. A seção opcional usa título ATX literal
  (`## Tarefas` → `Tarefas`); ignora frontmatter inicial e fences de crases/til.
  Headings Setext/HTML não são suportados. Seção ausente ou duplicada gera erro;
  não cria uma seção silenciosamente. Sem seção, acrescenta ao final da nota.
- `vault_create` recebe o conteúdo literal; não inventa templates nem frontmatter.
  Cabe ao chamador fornecer a nota de acordo com as convenções do seu vault.

### Busca: escopo e orçamento explícitos

O índice usa árvore Git recursiva, limita a **2.000 notas** e cerca de **80.000
bytes de caminhos serializados**, com cache de **120 segundos**. Os títulos são
**nomes de arquivos sem `.md`**, não headings H1 ou títulos extraídos do YAML.
O cache é invalidado por gravação confirmada deste gateway; edições externas
podem demorar até o vencimento/evicção para aparecer no índice.

Primeiro pesquisa todos os nomes indexados, depois lê no máximo **seis arquivos**
em ordem lexicográfica e **128 KiB totais** para busca literal no conteúdo. Pode
devolver de 1 a 20 resultados (padrão 10); não fornece trechos de notas como se
fossem resultado de uma busca global. `limits` informa notas indexadas, arquivos
lidos, bytes lidos e tetos. Notas grandes são puladas antes de buscar o blob.
`truncated` sinaliza índice remoto/local incompleto, arquivos não examinados,
arquivos pulados ou limite de resultados alcançado antes de examinar todo o conteúdo.

Não usa `/search/code`. Assim, funciona na branch configurada sem depender de
indexação da busca GitHub. Se esse endpoint for adicionado no futuro, exigirá
autenticação, queries escapadas, `repo:` montado exclusivamente da configuração,
branch padrão do repositório e `Accept: application/vnd.github.text-match+json`
para trechos. Não concatene qualifiers fornecidos pelo modelo.

### Calendar: datas e timezone

`start`, `end`, `timeMin` e `timeMax` são RFC3339 absolutos com segundos e `Z`
ou offset explícito; fração opcional com até três dígitos. Anos 1900–9999, data
real, sem leap second ou offset desconhecido `-00:00`. O fim precisa ser posterior
ao início, com duração/intervalo máximo de 366 dias. Não interpreta “amanhã”,
datas locais sem offset ou eventos all-day. `timeZone` exige nome IANA válido.
O instante é o definido pelo offset: o servidor não corrige um offset
incompatível com o fuso informado, nem resolve ambiguidades de horário de verão.

`reminderMinutes` aceita inteiro de **0 a 40320** e configura um popup; ausente,
usa os lembretes padrão do calendário. Não cria convidados ou notificações de
convite (`sendUpdates=none`). Listagem expande recorrências e não segue páginas;
informa `truncated` se houver mais eventos ou se precisar resumir títulos/resultado.

## Idempotência e recuperação

As **mutações** passam por ScriptLock (espera máxima de cinco segundos) e ledger
durável em Script Properties. Reads/search/list não criam entradas nem têm
snapshot/replay de resultado: podem observar dados atualizados usando o mesmo id.
Não reutilize o id de uma mutação para outra intenção. Um id de mutação existente
com payload ou destino diferente retorna `ID_REUSED` antes de qualquer gravação.

1. Persiste `pending` com hash do envelope e fingerprint do destino.
2. **GitHub:** captura a cabeça da branch e persiste essa fronteira antes do PUT.
   Toda atualização envia SHA do blob atual. Conflitos **409/422** fazem reler e
   reaplicar o patch, no máximo três tentativas. Nunca reenvia o conteúdo antigo
   como overwrite cego. O commit termina com `Gateway-Id: <id>` e
   `Gateway-Hash: <hash-canônico>`.
3. Após timeout ou perda da gravação final do ledger, um retry procura o trailer
   no histórico desde a fronteira durável. Se encontrar, conclui sem novo PUT.
   Reconcilia também depois da leitura e após conflitos, cobrindo um PUT que
   termina remotamente depois do timeout. Sem trailer e alcançando a fronteira,
   pode tentar a operação; sem alcançar a fronteira, bloqueia.
4. **Calendar:** usa `g` + base32hex(SHA-256(`calendar:` + id)) como event id.
   Faz GET antes de inserir e GET após 409/timeout; verifica id/hash nas
   propriedades privadas do evento antes de reconhecer sucesso. Não depende
   de título/horário para deduplicar. Um evento cancelado não é recriado;
   HTTP 410 produz `EVENT_GONE`.
5. Persiste `done` com resultado pequeno. Replays retornam esse resultado sem
   repetir a gravação. Se a gravação do resultado falhar, permanece `pending`
   e o retry reconcilia o efeito remoto.

### Fronteiras reais da garantia

- É necessário preservar **a entrada pending e sua fronteira** no GitHub e
  o histórico remoto. “Perda de ledger” recuperável é a perda da atualização
  **final para done**, inclusive após commit bem-sucedido; não a exclusão de
  todas as properties do projeto. Apagar a entrada GitHub inteira perde a
  fronteira e a vinculação durável daquele id e pode permitir duplicação.
  O Calendar ainda consegue reconciliar sem entrada local se o evento e suas
  propriedades privadas continuarem acessíveis.
- A reconciliação GitHub examina no máximo **três páginas de 100 commits**, a
  partir de uma cabeça fixada para evitar paginação instável. Se trailer e
  fronteira não estiverem nesse alcance, retorna `RECONCILIATION_REQUIRED` e
  não grava. Force-push, squash, remoção de trailers, alterações manuais de
  properties ou troca de projeto/deploy com ledger separado quebram premissas.
- Use um só projeto Apps Script por repositório/calendário e namespace de ids.
  ScriptLock não coordena projetos diferentes. Editores com permissão no
  repositório/projeto fazem parte da fronteira de confiança; um trailer forjado
  por eles não é prova criptográfica independente da origem do commit.
- O limite conservador é **400 ids de mutação por projeto**, sem expiração nem
  pruning automático. `LEDGER_FULL` bloqueia novos ids; replays existentes
  continuam possíveis. Não há armazenamento ilimitado gratuito. Para manter a
  garantia, arquive o ledger externamente e implemente migração/rejeição de ids
  antigos antes de removê-los; este módulo não fornece esse mecanismo. Não
  aumente o teto sem considerar 500 KB totais e tamanho das entradas.
- O provedor pode ter consistência eventual; a detecção de colisões de ids no
  Calendar também tem limitações documentadas. Não há transação distribuída ou
  garantia absoluta de exactly-once entre Google, GitHub e Script Properties.
  Em resultado incerto, conserve o id e o ledger e reconcilie; não gere id novo.

## Limites, cotas e falhas

| Recurso | Limite do gateway |
| --- | --- |
| Corpo POST | 64 KiB UTF-8, antes de parsear |
| `args` serializado | 48 KiB |
| Cada conteúdo/texto/descrição/oldText/newText | 16 KiB UTF-8 |
| Query/título/heading | 256 bytes UTF-8, não vazio |
| Nota fonte ou resultante de mutação | 128 KiB UTF-8 |
| Leitura integral de nota | 24 KiB de conteúdo; ainda sujeita ao envelope serializado |
| Resposta JSON completa | 32 KiB, incluindo escaping e envelope |
| Busca | 2.000 caminhos / ~80 KB; 6 arquivos / 128 KiB; até 20 resultados |
| Histórico GitHub | Até 300 commits por reconciliação |
| Calendar list | Até 50 itens e ~24 KB de itens; título resumido em 80 codepoints |
| Chamada externa | Timeout de 20 s; até 100 chamadas por execução |
| Tempo de chamadas | Para de iniciar chamadas após 4 minutos |
| JSON externo | Até 8 MiB GitHub; blobs 192 KiB; Calendar 128 KiB |
| Entrada de ledger | Até 8.000 bytes; máximo de 400 entradas |

O limite de JSON externo é checado após UrlFetchApp receber a resposta; não é
streaming nem limite de transporte. A árvore recursiva GitHub já tem seus próprios
limites de nós/tamanho e `truncated`. Busca ainda pode atingir orçamento de
chamadas/tempo em estruturas profundas. Não existe cron de indexação gratuito
oculto. Uma criação/edição pode consumir várias leituras de árvore e histórico.

As cotas oficiais variam por tipo de conta e podem mudar sem aviso. Como referência
consultada em **16/10/2026**, Apps Script documenta execução de até seis minutos,
9 KB por property e 500 KB por property store. UrlFetch, leitura/gravação de
properties, criação de eventos, GitHub rate limits e limites secundários também
se aplicam e são compartilhados com os outros usos da conta. Não há promessa de
um número de operações gratuitas por dia neste README.

Erros relevantes: `UNAUTHORIZED`, `INVALID_JSON`, `INVALID_REQUEST`, `INVALID_ARGS`,
`INVALID_PATH`, `INVALID_TIME`, `NOT_FOUND`, `ALREADY_EXISTS`, `HEADING_NOT_UNIQUE`,
`REPLACE_NOT_UNIQUE`, `RESULT_TOO_LARGE`, `FILE_TOO_LARGE`, `ID_REUSED`, `BUSY`,
`SHA_CONFLICT`, `REMOTE_UNCERTAIN`, `REQUEST_BUDGET`, `PROVIDER_AUTH_OR_QUOTA`,
`LEDGER_FULL`, `LEDGER_CORRUPT`, `RECONCILIATION_REQUIRED` e `EVENT_GONE`.
`retryable` orienta repetição do **mesmo envelope** com backoff/jitter, não retry
infinito. Permissões/cota podem exigir intervenção mesmo com `retryable: true`.
Não devolve corpos de erro de provedores, OAuth tokens, headers, stacks ou URLs
com credenciais. O código não usa Logger/console no servidor nem logs próprios
de conteúdo; a plataforma continua controlando seus próprios registros de execução.

## Referências oficiais consultadas

```text
Apps Script — Web apps / parâmetros doPost e execução como proprietário
https://developers.google.com/apps-script/guides/web
Apps Script — ContentService e redirecionamentos
https://developers.google.com/apps-script/guides/content
Apps Script — runtime V8 / limitações de módulos
https://developers.google.com/apps-script/guides/v8-runtime
Apps Script — UrlFetchApp, escopo e opções
https://developers.google.com/apps-script/reference/url-fetch/url-fetch-app
Apps Script — cotas e armazenamento de properties
https://developers.google.com/apps-script/guides/services/quotas
Calendar REST — events.insert, custom id, lembretes
https://developers.google.com/workspace/calendar/api/v3/reference/events/insert
Calendar REST — events.get / events.list
https://developers.google.com/workspace/calendar/api/v3/reference/events/get
https://developers.google.com/workspace/calendar/api/v3/reference/events/list
GitHub REST — conteúdos, SHA de atualização e conflitos
https://docs.github.com/en/rest/repos/contents
GitHub REST — árvores, blobs e histórico de commits
https://docs.github.com/en/rest/git/trees
https://docs.github.com/en/rest/git/blobs
https://docs.github.com/en/rest/commits/commits
GitHub REST — busca e text-match (endpoint não usado nesta implementação)
https://docs.github.com/en/rest/search/search
Gemini — function calling e declarações de ferramentas
https://ai.google.dev/gemini-api/docs/function-calling
```
