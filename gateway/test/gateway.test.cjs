'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
const core = require('../src/core.js');
const schema = require('../tool-schema.json');
const { bundle } = require('../scripts/build.cjs');
const { fixture } = require('./fixture.cjs');

const eventArgs = { title: 'Reunião', start: '2026-10-20T09:00:00-03:00',
  end: '2026-10-20T10:00:00-03:00', timeZone: 'America/Sao_Paulo', reminderMinutes: 15 };
function code(response, expected) {
  assert.equal(response.ok, false, JSON.stringify(response));
  assert.equal(response.error.code, expected, JSON.stringify(response));
}
function ok(response) { assert.equal(response.ok, true, JSON.stringify(response)); return response.result; }
function puts(f) { return f.calls.filter(c => c.options.method === 'put'); }

test('token do corpo é obrigatório, comparação usa hashes completos e não vaza credenciais', () => {
  const f = fixture({ 'n.md': 'ok' });
  for (const token of [undefined, null, '', 'a'.repeat(40), f.properties.get('GATEWAY_TOKEN') + 'x']) {
    const req = f.envelope('vault_read', { path: 'n.md' }); req.token = token;
    const result = f.raw(JSON.stringify(req));
    code(result, 'UNAUTHORIZED'); assert.equal(result.id, null);
    assert.ok(!JSON.stringify(result).includes(f.properties.get('GITHUB_TOKEN')));
  }
  assert.equal(f.calls.length, 0);
  assert.equal(ok(f.post('vault_read', { path: 'n.md' })).content, 'ok');
});

test('configuração obrigatória e prefixo inválido falham antes de rede', () => {
  for (const overrides of [{ GATEWAY_TOKEN: 'curto' }, { GITHUB_TOKEN: '' }, { TIME_ZONE: '' },
    { VAULT_PREFIX: '../fora' }, { TIME_ZONE: 'Fuso/Inexistente' }]) {
    const f = fixture({}, overrides);
    assert.equal(f.post('vault_create', { path: 'n.md', content: '' }).ok, false);
    assert.equal(f.calls.length, 0);
  }
});

test('JSON estrito: duplicatas escapadas, campos extras, profundidade, UTF-8 e tipos', () => {
  const f = fixture();
  const req = JSON.stringify(f.envelope('vault_search', { query: 'x' }));
  code(f.raw(req.replace('"version":1', '"version":1,"versi\\u006fn":1')), 'INVALID_JSON');
  code(f.raw(req + ' lixo'), 'INVALID_JSON');
  code(f.raw(req.replace('"query":"x"', '"query":"x","__proto__":{}')), 'INVALID_JSON');
  code(f.raw(req.replace('"query":"x"', '"query":"\\ud800"')), 'INVALID_JSON');
  code(f.raw(JSON.stringify({ ...f.envelope('vault_search', { query: 'x' }), extra: 1 })), 'INVALID_ARGS');
  code(f.post('vault_search', { query: 'x', limit: 1.2 }), 'INVALID_ARGS');
  code(f.post('vault_search', { query: 'x', repo: 'intruso' }), 'INVALID_ARGS');
  code(f.raw('['.repeat(10) + '0' + ']'.repeat(10)), 'INVALID_JSON');
  code(f.raw(req, 'text/plain'), 'CONTENT_TYPE');
  code(f.raw('é'.repeat(32769)), 'BODY_TOO_LARGE');
  code(f.post('vault_create', { path: 'n.md', content: 'é'.repeat(8193) }), 'INVALID_ARGS');
  assert.equal(f.calls.length, 0);
});

test('paths rejeitam traversal, área administrativa, encoding e escape da allowlist', () => {
  const f = fixture({}, { VAULT_PREFIX: 'Notas' });
  for (const path of ['../n.md', 'Notas/../n.md', '/Notas/n.md', 'Notas\\n.md', 'Notas//n.md',
    'Notas/%2e%2e/n.md', 'Notas/.GIT/n.md', 'Notas/.obsidian/n.md', 'Notas/.config/n.md',
    'Notas2/n.md', 'n.md', 'Notas/n.txt', 'Notas/n.md?ref=x', 'Notas/n\0.md']) {
    code(f.post('vault_read', { path }), 'INVALID_PATH');
  }
  assert.equal(f.calls.length, 0);
  ok(f.post('vault_create', { path: 'Notas/ação.md', content: '[[Outra nota]]' }));
  assert.ok(puts(f)[0].url.includes('a%C3%A7%C3%A3o.md'));
});

test('id não aceita newline final ou injeção de trailers; RFC3339 e MIME também são integrais', () => {
  const f = fixture();
  for (const id of ['pedido\n', 'pedido\r', 'pedido\nGateway-Hash: falso', '', 'a'.repeat(81)]) {
    const response = f.post('vault_create', { path: 'n.md', content: 'nota' }, id);
    code(response, 'INVALID_REQUEST'); assert.equal(response.id, null);
  }
  code(f.post('calendar_create', { ...eventArgs, start: eventArgs.start + '\n' }), 'INVALID_TIME');
  code(f.raw(JSON.stringify(f.envelope('vault_read', { path: 'n.md' })), 'application/json\n'), 'CONTENT_TYPE');
  assert.equal(f.calls.length, 0);
});

test('leitura não segue symlink mesmo com extensão .md', () => {
  const f = fixture(); f.setFile('atalho.md', 'fora.md', '120000'); f.commit();
  code(f.post('vault_read', { path: 'atalho.md' }), 'INVALID_PATH');
});

test('replay após resposta perdida repete resultado e nunca duplica append; payload diferente é recusado', () => {
  const f = fixture({ 'n.md': '# Lista\n' });
  const a = { path: 'n.md', text: '- [ ] item' };
  const first = f.post('vault_append', a);
  ok(first);
  assert.deepEqual(f.post('vault_append', { text: a.text, path: a.path }), first);
  code(f.post('vault_append', { ...a, text: '- outro' }), 'ID_REUSED');
  code(f.post('vault_create', { path: 'nova.md', content: 'x' }), 'ID_REUSED');
  assert.equal(puts(f).length, 1);
  assert.equal(f.files.get('n.md').content, '# Lista\n- [ ] item\n');
  assert.equal(f.locked(), false);
});

test('SHA retry 409 e 422 relê e reaplica sem apagar edição concorrente', () => {
  for (const status of [409, 422]) {
    const f = fixture({ 'n.md': 'início\n' });
    f.faults.conflictStatus = status;
    f.faults.beforePut = () => {
      f.faults.beforePut = null; f.setFile('n.md', 'início\nedição humana\n'); f.commit();
    };
    ok(f.post('vault_append', { path: 'n.md', text: 'item' }));
    assert.equal(f.files.get('n.md').content, 'início\nedição humana\nitem\n');
    const sent = puts(f).map(c => JSON.parse(c.options.payload));
    assert.equal(sent.length, 2); assert.notEqual(sent[0].sha, sent[1].sha);
  }
});

test('replace revalida unicidade após conflito, em vez de sobrescrever conteúdo concorrente', () => {
  const f = fixture({ 'n.md': 'alvo\n' });
  f.faults.beforePut = () => { f.faults.beforePut = null; f.setFile('n.md', 'alvo\nalvo\n'); f.commit(); };
  code(f.post('vault_replace', { path: 'n.md', oldText: 'alvo', newText: 'novo' }), 'REPLACE_NOT_UNIQUE');
  assert.equal(puts(f).length, 1); assert.equal(f.files.get('n.md').content, 'alvo\nalvo\n');
});

test('GitHub: falha do ledger após commit é reconciliada por trailer, sem novo PUT', () => {
  const f = fixture({ 'n.md': 'antes\n' }); f.faults.ledgerDone = true;
  const a = { path: 'n.md', text: 'novo' };
  code(f.post('vault_append', a), 'INTERNAL_ERROR');
  const ledger = [...f.properties].find(([k]) => k.startsWith('gw-ledger:'));
  assert.equal(JSON.parse(ledger[1]).status, 'pending');
  const result = ok(f.post('vault_append', a));
  assert.equal(result.reconciled, true); assert.equal(puts(f).length, 1);
  assert.match(f.commits[0].commit.message, /Gateway-Id: pedido-1\nGateway-Hash: [a-f0-9]{64}$/);
});

test('GitHub: timeout após gravação e retry encontra commit mesmo com commits humanos posteriores', () => {
  const f = fixture({ 'n.md': 'antes\n' }); f.faults.githubTimeout = true;
  const a = { path: 'n.md', text: 'novo' };
  code(f.post('vault_append', a), 'REMOTE_UNCERTAIN');
  f.setFile('outra.md', 'humano'); f.commit();
  assert.equal(ok(f.post('vault_append', a)).reconciled, true);
  assert.equal(puts(f).length, 1);
});

test('pending sem commit alcança fronteira e pode executar; fronteira ausente bloqueia', () => {
  const f = fixture({ 'n.md': 'antes\n' });
  const a = { path: 'n.md', text: 'novo' };
  f.faults.beforePut = () => { f.faults.beforePut = null; throw new Error('Transporte falhou antes do envio'); };
  code(f.post('vault_append', a), 'REMOTE_UNCERTAIN');
  ok(f.post('vault_append', a)); assert.equal(f.files.get('n.md').content, 'antes\nnovo\n');

  const g = fixture({ 'n.md': 'antes\n' }); g.faults.githubTimeout = true;
  code(g.post('vault_append', a), 'REMOTE_UNCERTAIN');
  for (let i = 0; i < 301; i++) g.commit();
  code(g.post('vault_append', a), 'RECONCILIATION_REQUIRED');
  assert.equal(puts(g).length, 1);
});

test('PUT termina após reconciliação inicial do retry: commit tardio não duplica append', () => {
  const f = fixture({ 'n.md': 'antes\n' });
  const a = { path: 'n.md', text: 'novo' };
  let delayed;
  f.faults.beforePut = () => {
    f.faults.beforePut = null;
    delayed = JSON.parse(puts(f)[0].options.payload);
    throw new Error('Timeout, PUT ainda em andamento');
  };
  code(f.post('vault_append', a), 'REMOTE_UNCERTAIN');
  f.faults.onFetch = url => {
    // Deixa a primeira reconciliação retornar a cabeça antiga; o PUT original
    // termina somente quando o retry começa a ler o arquivo para reaplicar.
    if (url.includes('/git/trees/')) {
      f.faults.onFetch = null;
      f.setFile('n.md', Buffer.from(delayed.content, 'base64').toString('utf8'));
      f.commit(delayed.message);
    }
  };
  assert.equal(ok(f.post('vault_append', a)).reconciled, true);
  assert.equal(puts(f).length, 1);
  assert.equal(f.files.get('n.md').content, 'antes\nnovo\n');
});

test('conflito persistente para após três tentativas; id pendente rejeita outros argumentos', () => {
  const f = fixture({ 'n.md': 'antes\n' });
  let count = 0;
  f.faults.beforePut = () => { f.setFile('n.md', 'externo ' + ++count); f.commit(); };
  code(f.post('vault_append', { path: 'n.md', text: 'novo' }), 'SHA_CONFLICT');
  assert.equal(puts(f).length, 3);
  code(f.post('vault_append', { path: 'n.md', text: 'outro' }), 'ID_REUSED');
  assert.equal(puts(f).length, 3);
});

test('trailer com mesmo id e hash incompatível bloqueia recuperação', () => {
  const f = fixture({ 'n.md': 'antes\n' }); f.faults.ledgerDone = true;
  const a = { path: 'n.md', text: 'novo' };
  code(f.post('vault_append', a), 'INTERNAL_ERROR');
  f.commits[0].commit.message = f.commits[0].commit.message.replace(/Gateway-Hash: .*/, 'Gateway-Hash: ' + '0'.repeat(64));
  code(f.post('vault_append', a), 'ID_REUSED');
  assert.equal(puts(f).length, 1);
});

test('falha ao persistir fronteira impede mutação remota e lock ocupado é retryable', () => {
  const f = fixture(); f.faults.ledgerBase = true;
  code(f.post('vault_create', { path: 'n.md', content: '' }), 'INTERNAL_ERROR');
  assert.equal(puts(f).length, 0); assert.equal(f.locked(), false);
  f.faults.busy = true;
  const result = f.post('vault_create', { path: 'n.md', content: '' });
  code(result, 'BUSY'); assert.equal(result.error.retryable, true);
});

test('preserva YAML, wikilinks, CRLF e seções; ignora headings em YAML e fences', () => {
  const before = '---\r\ntipo: nota\r\nlink: "[[Origem]]"\r\n---\r\n# Projeto\r\n## Tarefas\r\n- antigo\r\n### Detalhes\r\ntexto\r\n```md\r\n## Tarefas\r\n```\r\n## Outra\r\n[[Destino|alias]]\r\n';
  const f = fixture({ 'n.md': before });
  ok(f.post('vault_append', { path: 'n.md', text: '- novo', heading: 'Tarefas' }));
  assert.equal(f.files.get('n.md').content, before.replace('## Outra\r\n', '- novo\r\n## Outra\r\n'));
  ok(f.post('vault_replace', { path: 'n.md', oldText: '- antigo', newText: '- [x] antigo' }, 'replace-2'));
  assert.ok(f.files.get('n.md').content.startsWith('---\r\ntipo: nota\r\nlink: "[[Origem]]"\r\n---\r\n'));
  assert.ok(f.files.get('n.md').content.endsWith('[[Destino|alias]]\r\n'));
});

test('append heading ausente/duplicado e replace zero/múltiplas/sobrepostas não escrevem', () => {
  for (const content of ['# Outra\n', '# Alvo\n# Alvo\n']) {
    const f = fixture({ 'n.md': content });
    code(f.post('vault_append', { path: 'n.md', text: 'novo', heading: 'Alvo' }), 'HEADING_NOT_UNIQUE');
    assert.equal(puts(f).length, 0);
  }
  for (const [content, oldText] of [['abc', 'z'], ['abc abc', 'abc'], ['aaa', 'aa']]) {
    const f = fixture({ 'n.md': content });
    code(f.post('vault_replace', { path: 'n.md', oldText, newText: 'x' }), 'REPLACE_NOT_UNIQUE');
    assert.equal(puts(f).length, 0);
  }
});

test('create exige ausência e update sempre envia SHA', () => {
  const f = fixture({ 'n.md': 'existente' });
  code(f.post('vault_create', { path: 'n.md', content: 'trocar' }), 'ALREADY_EXISTS');
  ok(f.post('vault_create', { path: 'nova.md', content: '[[n]]' }, 'novo'));
  assert.equal(Object.hasOwn(JSON.parse(puts(f)[0].options.payload), 'sha'), false);
  ok(f.post('vault_append', { path: 'nova.md', text: 'adicional' }, 'append'));
  assert.equal(typeof JSON.parse(puts(f)[1].options.payload).sha, 'string');
});

test('Calendar usa id base32hex e recupera falha de ledger sem recriar evento', () => {
  const f = fixture(); f.faults.ledgerDone = true;
  code(f.post('calendar_create', eventArgs), 'INTERNAL_ERROR');
  const result = ok(f.post('calendar_create', eventArgs));
  assert.match(result.eventId, /^[0-9a-v]{53}$/); assert.equal(result.reconciled, true);
  assert.equal(f.events.size, 1);
  const event = f.events.get(result.eventId);
  assert.equal(event.start.dateTime, eventArgs.start);
  assert.equal(event.start.timeZone, 'America/Sao_Paulo');
  assert.deepEqual(event.reminders, { useDefault: false, overrides: [{ method: 'popup', minutes: 15 }] });
  assert.equal(f.calls.filter(c => c.options.method === 'post').length, 1);
  code(f.post('calendar_create', { ...eventArgs, title: 'Outra' }), 'ID_REUSED');
});

test('Calendar: timeout e 409 recuperam por GET e verificam hash do payload', () => {
  const f = fixture(); f.faults.calendarTimeout = true;
  assert.equal(ok(f.post('calendar_create', eventArgs)).reconciled, true);
  assert.equal(f.events.size, 1);

  const g = fixture();
  g.faults.beforeInsert = event => { g.events.set(event.id, { ...event, status: 'confirmed' }); };
  assert.equal(ok(g.post('calendar_create', eventArgs)).reconciled, true);
  assert.equal(g.events.size, 1);

  // Mesmo sem entrada local, o marcador remoto do Calendar impede outro payload.
  for (const key of g.properties.keys()) if (key.startsWith('gw-ledger:')) g.properties.delete(key);
  code(g.post('calendar_create', { ...eventArgs, title: 'Payload diferente' }), 'ID_REUSED');
});

test('Calendar não recria evento deletado, nem evento com marcador incompatível', () => {
  const f = fixture(); f.faults.calendarGone = true;
  code(f.post('calendar_create', eventArgs), 'EVENT_GONE');
  assert.equal(f.calls.filter(c => c.options.method === 'post').length, 0);
});

test('datas absolutas com offset, calendário real, intervalo e lembretes são validados antes da rede', () => {
  for (const change of [{ start: 'amanhã às 9h' }, { start: '2026-10-20T09:00:00' },
    { start: '2026-02-30T09:00:00Z' }, { start: '2026-10-20T25:00:00Z' },
    { end: eventArgs.start }, { timeZone: 'Fuso/Inexistente' }, { timeZone: '+03:00' },
    { reminderMinutes: -1 }, { reminderMinutes: 40321 }]) {
    const f = fixture(); assert.equal(f.post('calendar_create', { ...eventArgs, ...change }).ok, false);
    assert.equal(f.calls.length, 0);
  }
  assert.ok(Number.isFinite(core.rfc3339('2028-02-29T00:00:00Z')));
  const f = fixture();
  code(f.post('calendar_list', { timeMin: '2026-01-01T00:00:00Z', timeMax: '2028-01-01T00:00:00Z' }), 'INVALID_TIME');
});

test('busca respeita allowlist, orçamento de seis blobs, limite de resultados e cache de índice', () => {
  const files = { 'Notas/Alvo.md': 'nota', '.obsidian/Alvo.md': 'não', 'Fora/Alvo.md': 'não' };
  for (let i = 0; i < 12; i++) files['Notas/n' + i + '.md'] = 'conteúdo alvo';
  const f = fixture(files, { VAULT_PREFIX: 'Notas' });
  const result = ok(f.post('vault_search', { query: 'alvo', limit: 20 }));
  assert.equal(result.truncated, true); assert.equal(result.limits.contentFilesRead, 6);
  assert.ok(result.items.every(e => e.path.startsWith('Notas/')));
  assert.equal(result.items.length, 7);
  const treesBefore = f.calls.filter(c => c.url.endsWith('?recursive=1')).length;
  const limited = ok(f.post('vault_search', { query: 'alvo', limit: 1 }));
  assert.equal(limited.items.length, 1); assert.equal(limited.truncated, true);
  assert.equal(f.calls.filter(c => c.url.endsWith('?recursive=1')).length, treesBefore);
});

test('índice truncado, conteúdo acima do orçamento e resultados grandes não alegam completude', () => {
  const f = fixture({ 'grande.md': 'x'.repeat(131073) });
  assert.equal(ok(f.post('vault_search', { query: 'x' })).truncated, true);
  assert.equal(f.calls.filter(c => c.url.includes('/git/blobs/')).length, 0);
  const g = fixture({ 'n.md': 'x'.repeat(25000) });
  code(g.post('vault_read', { path: 'n.md' }), 'RESULT_TOO_LARGE');
  assert.ok(core.bytes(JSON.stringify(g.post('vault_search', { query: 'x' }))) <= 32768);
});

test('índice limita quantidade, filtra symlinks e propaga truncamento remoto', () => {
  const files = {};
  for (let i = 0; i < 2010; i++) files['n' + i + '.md'] = 'x';
  const f = fixture(files);
  const result = ok(f.post('vault_search', { query: 'n', limit: 1 }));
  assert.equal(result.limits.indexedFiles, 2000); assert.equal(result.truncated, true);
  const g = fixture({ 'n.md': 'x' }); g.faults.treeTruncated = true;
  // Busca por título evita uma leitura de diretório truncado, que falharia fechada.
  assert.equal(ok(g.post('vault_search', { query: 'n', limit: 1 })).truncated, true);
});

test('respostas com caracteres que expandem em JSON também respeitam 32 KiB', () => {
  const f = fixture({ 'n.md': '\0'.repeat(20000) });
  const result = f.post('vault_read', { path: 'n.md' });
  code(result, 'RESULT_TOO_LARGE');
  assert.ok(Buffer.byteLength(JSON.stringify(result)) <= 32768);
});

test('Calendar lista com projeção e truncamento em vez de ultrapassar 32 KiB', () => {
  const f = fixture();
  for (let i = 0; i < 70; i++) f.events.set('e' + i, { id: 'e' + i,
    summary: 'Título '.repeat(50), start: { dateTime: eventArgs.start }, end: { dateTime: eventArgs.end }, status: 'confirmed' });
  const response = f.post('calendar_list', { timeMin: eventArgs.start, timeMax: eventArgs.end });
  const result = ok(response);
  assert.equal(result.truncated, true); assert.ok(result.items.length <= 50);
  assert.ok(core.bytes(JSON.stringify(response)) <= 32768);
  assert.ok(f.calls[0].url.includes('fields='));
});

test('mudança de destino, corrupção e ledger cheio falham sem mutação', () => {
  const f = fixture({ 'n.md': '' }); ok(f.post('vault_append', { path: 'n.md', text: 'a' }));
  f.properties.set('GITHUB_BRANCH', 'outra');
  code(f.post('vault_append', { path: 'n.md', text: 'a' }), 'ID_REUSED');
  const key = [...f.properties.keys()].find(k => k.startsWith('gw-ledger:'));
  f.properties.set(key, '{');
  code(f.post('vault_append', { path: 'n.md', text: 'a' }), 'LEDGER_CORRUPT');
  const g = fixture();
  for (let i = 0; i < 400; i++) g.properties.set('gw-ledger:' + i, '{}');
  code(g.post('vault_create', { path: 'n.md', content: '' }), 'LEDGER_FULL');
  assert.equal(puts(g).length, 0);
});

test('erros externos não ecoam respostas, tokens nem mensagens de exceção', () => {
  const f = fixture();
  f.faults.fetch = () => { throw new Error('github-segredo-nao-ecoar oauth-nao-ecoar'); };
  const response = f.post('vault_read', { path: 'n.md' });
  code(response, 'REMOTE_UNCERTAIN');
  assert.ok(!JSON.stringify(response).includes('nao-ecoar'));
});

test('web_search é a oitava declaração Gemini e permanece desabilitada sem configuração', () => {
  assert.equal(schema.functionDeclarations.length, 8);
  assert.equal(schema.functionDeclarations[7].name, 'web_search');
  code(fixture().post('web_search', { query: 'notícias' }), 'TOOL_DISABLED');
});

test('bundle V8 executa doPost sem require e GET ignora tokens/query e properties', () => {
  const source = bundle();
  assert.doesNotMatch(source, /\brequire\s*\(|module\.exports|process\./);
  const f = fixture({ 'n.md': '[[Nota]]' });
  const context = { ...f.env,
    ContentService: { MimeType: { JSON: 'application/json' },
      createTextOutput: text => ({ text, setMimeType() { return this; } }) } };
  vm.createContext(context); vm.runInContext(source, context);
  const post = context.doPost({ postData: { type: 'application/json', contents: JSON.stringify(f.envelope('vault_read', { path: 'n.md' })) } });
  assert.equal(ok(JSON.parse(post.text)).content, '[[Nota]]');
  context.PropertiesService = { getScriptProperties() { throw new Error('GET não deve ler secrets'); } };
  assert.deepEqual(JSON.parse(context.doGet({ parameter: { token: 'qualquer' } }).text), { version: 1, status: 'ok' });
});

test('manifesto declara apenas escopos necessários, executor proprietário e acesso anônimo', () => {
  const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, '..', 'appsscript.json'), 'utf8'));
  assert.equal(manifest.webapp.executeAs, 'USER_DEPLOYING');
  assert.equal(manifest.webapp.access, 'ANYONE_ANONYMOUS');
  assert.deepEqual(manifest.oauthScopes.map(s => s.split('/').pop()).sort(), ['calendar.events', 'script.external_request', 'script.storage']);
});
