/* Núcleo síncrono UMD: Node para testes, global GatewayCore no Apps Script V8. */
var GatewayCore = (function () {
  'use strict';
  const LIMITS = Object.freeze({ body: 65536, args: 49152, text: 16384,
    result: 32768, file: 131072, read: 24576, index: 2000,
    contentFiles: 6, contentBytes: 131072, historyPages: 3, ledger: 400 });

  class GatewayError extends Error {
    constructor(code, message, retryable) {
      super(message);
      this.code = code;
      this.retryable = Boolean(retryable);
    }
  }
  function fail(code, message, retryable) { throw new GatewayError(code, message, retryable); }
  function bytes(s) {
    // Também rejeita surrogates soltos, que não têm representação UTF-8 canônica.
    try { return encodeURIComponent(s).replace(/%[A-F\d]{2}/gi, 'x').length; }
    catch (_) { fail('INVALID_JSON', 'Unicode inválido.'); }
  }
  function object(x) { return x !== null && typeof x === 'object' && !Array.isArray(x); }
  function validId(id) {
    // Não usa $: no JavaScript ele também pode casar ANTES de uma quebra final.
    return typeof id === 'string' && id.length >= 1 && id.length <= 80 && !/[^A-Za-z0-9_-]/.test(id);
  }
  function canonical(x) {
    if (Array.isArray(x)) return '[' + x.map(canonical).join(',') + ']';
    if (object(x)) return '{' + Object.keys(x).sort().map(k => JSON.stringify(k) + ':' + canonical(x[k])).join(',') + '}';
    return JSON.stringify(x);
  }
  function text(s, max, nonempty) {
    if (typeof s !== 'string' || bytes(s) > max || (nonempty && !s.trim())) {
      fail('INVALID_ARGS', 'Texto ausente, inválido ou acima do limite.');
    }
  }
  function exactKeys(x, allowed, required) {
    if (!object(x) || Object.keys(x).some(k => !allowed.includes(k)) ||
        required.some(k => !Object.prototype.hasOwnProperty.call(x, k))) {
      fail('INVALID_ARGS', 'Campos ausentes ou desconhecidos.');
    }
  }
  function parseStrict(raw) {
    // Valida a gramática e chaves duplicadas antes de JSON.parse (inclusive em args).
    let i = 0;
    const bad = () => fail('INVALID_JSON', 'JSON inválido, profundo ou com chaves duplicadas.');
    const ws = () => { while (/[\x20\t\r\n]/.test(raw[i] || '\0')) i++; };
    function string() {
      if (raw[i++] !== '"') bad();
      const begin = i - 1;
      while (i < raw.length) {
        const c = raw[i++];
        if (c === '"') {
          try { const s = JSON.parse(raw.slice(begin, i)); bytes(s); return s; }
          catch (_) { bad(); }
        }
        if (c === '\\') i++;
      }
      bad();
    }
    function value(depth) {
      if (depth > 8) bad();
      ws();
      const c = raw[i];
      if (c === '"') { string(); return; }
      if (c === '{' || c === '[') {
        i++;
        const end = c === '{' ? '}' : ']';
        const keys = new Set();
        ws();
        if (raw[i] === end) { i++; return; }
        while (i < raw.length) {
          if (c === '{') {
            ws();
            const key = string();
            if (keys.has(key) || ['__proto__', 'prototype', 'constructor'].includes(key)) bad();
            keys.add(key);
            ws();
            if (raw[i++] !== ':') bad();
          }
          value(depth + 1);
          ws();
          if (raw[i] === end) { i++; return; }
          if (raw[i++] !== ',') bad();
        }
        bad();
      }
      const m = /^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/.exec(raw.slice(i));
      if (!m) bad();
      i += m[0].length;
    }
    value(0); ws();
    if (i !== raw.length) bad();
    try { return JSON.parse(raw); } catch (_) { bad(); }
  }
  function safePath(path, prefix) {
    text(path, 240, true);
    const parts = path.split('/');
    if (/[\\%:\x00-\x1f\x7f?#]/.test(path) || path.startsWith('/') ||
        !path.endsWith('.md') || parts.length > 16 || parts.some(p => !p || p === '.' || p === '..' ||
          ['.git', '.obsidian', '.config'].includes(p.toLowerCase())) ||
        (prefix && !path.startsWith(prefix + '/'))) {
      fail('INVALID_PATH', 'Caminho Markdown fora da área permitida.');
    }
    return path;
  }
  function rfc3339(s) {
    text(s, 40, true);
    const m = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(?:\.\d{1,3})?(Z|[+-]\d{2}:\d{2})$/.exec(s);
    if (!m || m[0] !== s) fail('INVALID_TIME', 'Use RFC3339 absoluto com Z ou offset explícito.');
    const [, y, mo, d, h, mi, sec, zone] = m;
    const leap = +y % 4 === 0 && (+y % 100 !== 0 || +y % 400 === 0);
    const days = [31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
    if (+y < 1900 || +mo < 1 || +mo > 12 || +d < 1 || +d > days[+mo - 1] ||
        +h > 23 || +mi > 59 || +sec > 59 || zone === '-00:00' ||
        (zone !== 'Z' && (+zone.slice(1, 3) > 23 || +zone.slice(4) > 59))) {
      fail('INVALID_TIME', 'Data ou offset inválido.');
    }
    const n = Date.parse(s);
    if (!Number.isFinite(n)) fail('INVALID_TIME', 'Data inválida.');
    return n;
  }
  function interval(start, end) {
    const a = rfc3339(start), b = rfc3339(end);
    if (b <= a || b - a > 366 * 86400000) fail('INVALID_TIME', 'Intervalo deve ser positivo e de até 366 dias.');
  }
  function timeZone(zone) {
    text(zone, 100, true);
    if (!/^[A-Za-z]/.test(zone) || /[^A-Za-z0-9_+\/-]/.test(zone)) {
      fail('INVALID_TIME', 'Use nome de fuso IANA, não offset isolado.');
    }
    try { new Intl.DateTimeFormat('en', { timeZone: zone }).format(0); }
    catch (_) { fail('INVALID_TIME', 'Fuso IANA inválido.'); }
  }
  function append(content, addition, heading) {
    const nl = content.includes('\r\n') ? '\r\n' : '\n';
    let at = content.length;
    if (heading !== undefined) {
      // Ignora YAML inicial e blocos fenced; não interpreta Setext/HTML como seção ATX.
      const lines = content.match(/[^\n]*\n|[^\n]+$/g) || [];
      let offset = 0, fence = null, yaml = false, target = null, count = 0;
      for (let n = 0; n < lines.length; n++) {
        const line = lines[n].replace(/\r?\n$/, '');
        if (n === 0 && line === '---') yaml = true;
        else if (yaml && /^(---|\.\.\.)$/.test(line)) yaml = false;
        else if (!yaml) {
          const f = /^ {0,3}(`{3,}|~{3,})(.*)$/.exec(line);
          if (f) {
            if (!fence) fence = { char: f[1][0], size: f[1].length };
            else if (f[1][0] === fence.char && f[1].length >= fence.size && !f[2].trim()) fence = null;
          } else if (!fence) {
            const h = /^ {0,3}(#{1,6})[ \t]+(.+?)\s*$/.exec(line);
            if (h) {
              const title = h[2].replace(/[ \t]+#+[ \t]*$/, '');
              if (target !== null && h[1].length <= target && at === content.length) at = offset;
              if (title === heading) { target = h[1].length; count++; }
            }
          }
        }
        offset += lines[n].length;
      }
      if (count !== 1) fail('HEADING_NOT_UNIQUE', 'A seção deve existir exatamente uma vez.');
    }
    const left = content.slice(0, at), right = content.slice(at);
    return left + (left && !left.endsWith('\n') ? nl : '') + addition +
      (!addition.endsWith('\n') ? nl : '') + right;
  }
  function patch(tool, content, args) {
    if (tool === 'vault_create') {
      if (content !== null) fail('ALREADY_EXISTS', 'A nota já existe.');
      return args.content;
    }
    if (content === null) fail('NOT_FOUND', 'Nota não encontrada.');
    if (tool === 'vault_append') return append(content, args.text, args.heading);
    const at = content.indexOf(args.oldText);
    // Inclui ocorrências sobrepostas ("aa" em "aaa" também é ambíguo).
    if (at < 0 || content.indexOf(args.oldText, at + 1) >= 0) {
      fail('REPLACE_NOT_UNIQUE', 'oldText deve ocorrer exatamente uma vez.');
    }
    return content.slice(0, at) + args.newText + content.slice(at + args.oldText.length);
  }
  function base32hex(hex) {
    const alphabet = '0123456789abcdefghijklmnopqrstuv';
    let acc = 0, bits = 0, out = '';
    for (let i = 0; i < hex.length; i += 2) {
      acc = (acc << 8) | parseInt(hex.slice(i, i + 2), 16); bits += 8;
      while (bits >= 5) { bits -= 5; out += alphabet[(acc >>> bits) & 31]; }
      acc &= (1 << bits) - 1;
    }
    if (bits) out += alphabet[(acc << (5 - bits)) & 31];
    return out;
  }
  function create(deps) {
    const { config: c, schema, hash, ledger, lock, github, calendar, webSearch } = deps;
    const definitions = schema.functionDeclarations;
    function configure() {
      if (typeof c.GATEWAY_TOKEN !== 'string' || c.GATEWAY_TOKEN.length < 32 || c.GATEWAY_TOKEN.length > 1024) {
        fail('CONFIG_ERROR', 'Configuração do gateway inválida.');
      }
      if (!c.GITHUB_TOKEN || !c.GITHUB_OWNER || /[^A-Za-z0-9_.-]/.test(c.GITHUB_OWNER) ||
          !c.GITHUB_REPO || /[^A-Za-z0-9_.-]/.test(c.GITHUB_REPO) || !c.TIME_ZONE) {
        fail('CONFIG_ERROR', 'Configuração do gateway inválida.');
      }
      timeZone(c.TIME_ZONE);
      if (c.VAULT_PREFIX) safePath(c.VAULT_PREFIX + '/probe.md', '');
    }
    function authenticate(token) {
      // Compara sempre todos os 64 dígitos SHA-256; não sai no primeiro byte distinto.
      const candidate = typeof token === 'string' && token.length <= 1024 ? token : '';
      const a = hash(candidate), b = hash(c.GATEWAY_TOKEN);
      let difference = 0;
      for (let i = 0; i < 64; i++) difference |= a.charCodeAt(i) ^ b.charCodeAt(i);
      if (difference !== 0) fail('UNAUTHORIZED', 'Autenticação inválida.');
    }
    function validate(req) {
      exactKeys(req, ['version', 'token', 'id', 'tool', 'args'], ['version', 'token', 'id', 'tool', 'args']);
      if (req.version !== 1 || !validId(req.id)) {
        fail('INVALID_REQUEST', 'Versão ou id inválido.');
      }
      const def = definitions.find(d => d.name === req.tool);
      if (!def) fail(req.tool === 'web_search' ? 'TOOL_DISABLED' : 'UNKNOWN_TOOL', 'Ferramenta indisponível.');
      const p = def.parameters;
      exactKeys(req.args, Object.keys(p.properties), p.required);
      if (bytes(JSON.stringify(req.args)) > LIMITS.args) fail('INVALID_ARGS', 'Argumentos acima do limite.');
      for (const k of Object.keys(req.args)) {
        if (p.properties[k].type === 'string') text(req.args[k], LIMITS.text, false);
        else if (!Number.isSafeInteger(req.args[k])) fail('INVALID_ARGS', 'Esperado número inteiro.');
      }
      const a = req.args;
      if (a.path !== undefined) safePath(a.path, c.VAULT_PREFIX || '');
      for (const k of ['query', 'title', 'heading']) if (a[k] !== undefined) text(a[k], 256, true);
      for (const k of ['text', 'oldText']) if (a[k] !== undefined) text(a[k], LIMITS.text, true);
      if (a.heading !== undefined && /[\r\n]/.test(a.heading)) fail('INVALID_ARGS', 'Título de seção inválido.');
      if (a.limit !== undefined && (a.limit < 1 || a.limit > 20)) fail('INVALID_ARGS', 'limit deve estar entre 1 e 20.');
      if (a.reminderMinutes !== undefined && (a.reminderMinutes < 0 || a.reminderMinutes > 40320)) {
        fail('INVALID_ARGS', 'Lembrete fora do intervalo 0–40320 minutos.');
      }
      if (req.tool === 'calendar_list') interval(a.timeMin, a.timeMax);
      if (req.tool === 'calendar_create') { interval(a.start, a.end); timeZone(a.timeZone); }
    }
    function identity(req) {
      return hash(canonical({ version: 1, id: req.id, tool: req.tool, args: req.args }));
    }
    function target(req) {
      return hash(canonical(req.tool.startsWith('vault_') ?
        { owner: c.GITHUB_OWNER, repo: c.GITHUB_REPO, branch: c.GITHUB_BRANCH || 'main', prefix: c.VAULT_PREFIX || '' } :
        { calendar: c.CALENDAR_ID || 'primary' }));
    }
    function githubMutation(req, state, save) {
      const a = req.args;
      const trailer = '\n\nGateway-Id: ' + req.id + '\nGateway-Hash: ' + state.hash;
      if (state.base) {
        const recovered = github.reconcile(state.base, req.id, state.hash, LIMITS.historyPages);
        if (recovered) return { path: a.path, commit: recovered, reconciled: true };
      } else {
        state.base = github.head();
        save(state); // A fronteira deve estar durável ANTES da chamada que pode gravar.
      }
      for (let attempt = 0; attempt < 3; attempt++) {
        const current = github.read(a.path);
        // Uma requisição anterior pode ter terminado remotamente após seu timeout.
        // Reconcilia DEPOIS da leitura e também após cada conflito, antes de reaplicar.
        const late = github.reconcile(state.base, req.id, state.hash, LIMITS.historyPages);
        if (late) return { path: a.path, commit: late, reconciled: true };
        const next = patch(req.tool, current === null ? null : current.content, a);
        if (bytes(next) > LIMITS.file) fail('FILE_TOO_LARGE', 'Nota acima de 128 KiB.');
        if (current && !current.sha) fail('REMOTE_INVALID', 'GitHub não forneceu SHA.');
        try {
          const commit = github.write(a.path, next, current ? current.sha : null,
            'Gateway: ' + req.tool + trailer);
          return { path: a.path, commit, reconciled: false };
        } catch (error) {
          if (!(error instanceof GatewayError) || error.code !== 'SHA_CONFLICT') throw error;
          // 409/422: relê e reaplica a operação, nunca envia novamente conteúdo antigo.
        }
      }
      fail('SHA_CONFLICT', 'Conflito persistente após três leituras e tentativas.', true);
    }
    function calendarMutation(req, state) {
      const eventId = 'g' + base32hex(hash('calendar:' + req.id));
      function recover(event) {
        const p = event.extendedProperties && event.extendedProperties.private;
        if (!p || p.gatewayHash !== state.hash || p.gatewayId !== req.id) {
          fail('ID_REUSED', 'Id já associado a outro payload.');
        }
        // Mesmo evento removido/cancelado não deve ser recriado pelo retry.
        return { eventId, status: event.status || 'confirmed', reconciled: true };
      }
      const existing = calendar.get(eventId);
      if (existing) return recover(existing);
      const a = req.args;
      const event = { id: eventId, summary: a.title,
        start: { dateTime: a.start, timeZone: a.timeZone },
        end: { dateTime: a.end, timeZone: a.timeZone },
        extendedProperties: { private: { gatewayId: req.id, gatewayHash: state.hash } } };
      if (a.description !== undefined) event.description = a.description;
      if (a.reminderMinutes !== undefined) event.reminders = {
        useDefault: false, overrides: [{ method: 'popup', minutes: a.reminderMinutes }] };
      try {
        const result = calendar.insert(event);
        return { eventId, status: result.status || 'confirmed', reconciled: false };
      } catch (e) {
        if (e instanceof GatewayError && ['EVENT_CONFLICT', 'REMOTE_UNCERTAIN'].includes(e.code)) {
          const found = calendar.get(eventId);
          if (found) return recover(found);
        }
        throw e;
      }
    }
    function mutate(req) {
      return lock.run(() => {
        const key = hash('ledger:' + req.id), digest = identity(req), destination = target(req);
        let state = ledger.get(key);
        if (state && (state.hash !== digest || state.target !== destination)) {
          fail('ID_REUSED', 'Id já associado a outro payload ou destino.');
        }
        if (state && state.status === 'done') return state.result;
        if (!state) {
          if (ledger.count() >= LIMITS.ledger) fail('LEDGER_FULL', 'Ledger cheio; requer manutenção explícita.');
          state = { hash: digest, target: destination, status: 'pending' };
          ledger.set(key, state);
        }
        const save = s => ledger.set(key, s);
        const result = req.tool === 'calendar_create' ? calendarMutation(req, state) : githubMutation(req, state, save);
        save({ hash: digest, target: destination, status: 'done', result });
        return result;
      });
    }
    function search(a) {
      const index = github.index(), q = a.query.toLowerCase(), limit = a.limit || 10;
      const items = [], seen = new Set();
      let filesRead = 0, contentBytes = 0, examined = 0, skipped = false;
      const allowed = index.paths.filter(p => {
        try { safePath(p, c.VAULT_PREFIX || ''); return true; } catch (_) { return false; }
      }).sort();
      for (const path of allowed) {
        const title = path.split('/').pop().slice(0, -3);
        if (title.toLowerCase().includes(q) && items.length < limit) {
          items.push({ path, title, match: 'title' }); seen.add(path);
        }
      }
      for (const path of allowed) {
        if (items.length >= limit || filesRead >= LIMITS.contentFiles || contentBytes >= LIMITS.contentBytes) break;
        examined++;
        if (seen.has(path)) continue;
        // O adaptador verifica size antes de decodificar o conteúdo.
        const note = github.read(path, LIMITS.contentBytes - contentBytes);
        filesRead++;
        if (!note) continue;
        if (note.tooLarge) { skipped = true; continue; }
        contentBytes += bytes(note.content);
        if (note.content.toLowerCase().includes(q)) items.push({ path, title: path.split('/').pop().slice(0, -3), match: 'content' });
      }
      return { items, truncated: Boolean(index.truncated || examined < allowed.length || skipped),
        limits: { indexedFiles: allowed.length, maxIndexedFiles: LIMITS.index,
          contentFilesRead: filesRead, maxContentFiles: LIMITS.contentFiles,
          contentBytesRead: contentBytes, maxContentBytes: LIMITS.contentBytes, resultLimit: limit,
          cacheSeconds: 120 }, scope: 'filename-and-budgeted-content' };
    }
    function execute(req) {
      if (req.tool === 'web_search') {
        if (!webSearch) fail('TOOL_DISABLED', 'Pesquisa web não configurada.');
        return webSearch.search(req.args.query);
      }
      if (['vault_create', 'vault_append', 'vault_replace', 'calendar_create'].includes(req.tool)) return mutate(req);
      if (req.tool === 'vault_search') return search(req.args);
      if (req.tool === 'vault_read') {
        const note = github.read(req.args.path);
        if (!note) fail('NOT_FOUND', 'Nota não encontrada.');
        if (bytes(note.content) > LIMITS.read) fail('RESULT_TOO_LARGE', 'Nota acima do limite de leitura de 24 KiB.');
        return { path: req.args.path, sha: note.sha, content: note.content };
      }
      return calendar.list(req.args.timeMin, req.args.timeMax, 50);
    }
    function handle(raw, mime) {
      let id = null;
      try {
        if (typeof raw !== 'string' || bytes(raw) > LIMITS.body) fail('BODY_TOO_LARGE', 'Corpo deve ter até 64 KiB.');
        if (/[\r\n]/.test(mime || '') || !/^application\/json(?:\s*;\s*charset=utf-8)?$/i.test(mime || '')) {
          fail('CONTENT_TYPE', 'Envie application/json UTF-8.');
        }
        configure();
        const req = parseStrict(raw);
        authenticate(object(req) ? req.token : undefined);
        if (validId(req.id)) id = req.id;
        validate(req);
        const response = { version: 1, id, ok: true, result: execute(req) };
        if (bytes(JSON.stringify(response)) > LIMITS.result) fail('RESULT_TOO_LARGE', 'Resposta acima de 32 KiB.');
        return response;
      } catch (error) {
        // Nunca propaga mensagens de provedores, URLs, stack traces ou credenciais.
        const known = error instanceof GatewayError;
        return { version: 1, id, ok: false, error: {
          code: known ? error.code : 'INTERNAL_ERROR',
          message: known ? error.message : 'Falha interna ou de persistência.',
          retryable: known ? error.retryable : true } };
      }
    }
    return { handle };
  }
  return { create, GatewayError, LIMITS, bytes, canonical, parseStrict, safePath,
    rfc3339, interval, append, patch, base32hex };
})();
if (typeof module !== 'undefined' && module.exports) module.exports = GatewayCore;
