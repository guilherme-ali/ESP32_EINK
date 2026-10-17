/* Adaptadores de produção. Nenhuma chamada de rede acontece ao carregar o arquivo. */
var GatewayAppsScript = (function () {
  'use strict';
  function create(env, core, schema, searchModule) {
    const ErrorType = core.GatewayError;
    const fail = (code, message, retryable) => { throw new ErrorType(code, message, retryable); };
    const properties = env.PropertiesService.getScriptProperties();
    const config = {};
    for (const key of ['GATEWAY_TOKEN', 'GITHUB_TOKEN', 'GITHUB_OWNER', 'GITHUB_REPO',
      'GITHUB_BRANCH', 'VAULT_PREFIX', 'CALENDAR_ID', 'TIME_ZONE', 'GEMINI_SEARCH_API_KEY',
      'GEMINI_SEARCH_MODEL', 'SEARCH_FREE_CONFIRMED']) config[key] = properties.getProperty(key) || '';
    config.GITHUB_BRANCH = config.GITHUB_BRANCH || 'main';
    config.CALENDAR_ID = config.CALENDAR_ID || 'primary';
    const hash = input => env.Utilities.computeDigest(env.Utilities.DigestAlgorithm.SHA_256,
      input, env.Utilities.Charset.UTF_8).map(b => ((b + 256) % 256).toString(16).padStart(2, '0')).join('');
    const encode = encodeURIComponent;
    const ghRoot = 'https://api.github.com/repos/' + encode(config.GITHUB_OWNER) + '/' + encode(config.GITHUB_REPO);
    const calRoot = 'https://www.googleapis.com/calendar/v3/calendars/' + encode(config.CALENDAR_ID) + '/events';
    let requests = 0;
    const started = Date.now();
    function request(provider, path, method, payload, allowed, cap) {
      if (Date.now() - started > 240000) fail('REQUEST_BUDGET', 'Orçamento de quatro minutos esgotado; repita com o mesmo id.', true);
      if (++requests > 100) fail('REQUEST_BUDGET', 'Orçamento de 100 chamadas externas esgotado.', true);
      const isGit = provider === 'github';
      const options = { method: method || 'get', muteHttpExceptions: true,
        followRedirects: false, validateHttpsCertificates: true, timeoutSeconds: 20,
        headers: { Authorization: 'Bearer ' + (isGit ? config.GITHUB_TOKEN : env.ScriptApp.getOAuthToken()) } };
      if (isGit) {
        options.headers.Accept = 'application/vnd.github+json';
        options.headers['X-GitHub-Api-Version'] = '2022-11-28';
      }
      if (payload !== undefined) { options.contentType = 'application/json'; options.payload = JSON.stringify(payload); }
      let response;
      try { response = env.UrlFetchApp.fetch((isGit ? ghRoot : calRoot) + path, options); }
      catch (_) { fail('REMOTE_UNCERTAIN', 'Falha de transporte; repita com o mesmo id.', true); }
      const status = response.getResponseCode();
      if (status === 429 || status >= 500) fail('REMOTE_UNCERTAIN', 'Provedor indisponível ou com cota excedida; repita com o mesmo id.', true);
      if (status === 401 || status === 403) fail('PROVIDER_AUTH_OR_QUOTA', 'Permissão ou cota do provedor indisponível.', true);
      if (!(allowed || [200]).includes(status)) {
        fail('PROVIDER_ERROR', 'Provedor recusou a operação. Confira configuração e permissões.');
      }
      if ([404, 409, 410, 422].includes(status)) return { status, data: null };
      const raw = response.getContentText('UTF-8');
      if (core.bytes(raw) > (cap || 8 * 1024 * 1024)) fail('REMOTE_TOO_LARGE', 'Resposta externa acima do orçamento.');
      let data;
      try { data = JSON.parse(raw); } catch (_) { fail('REMOTE_INVALID', 'JSON externo inválido.'); }
      return { status, data };
    }
    const gh = (path, method, payload, allowed, cap) => request('github', path, method, payload, allowed, cap);
    const cal = (path, method, payload, allowed) => request('calendar', path, method, payload, allowed, 128 * 1024);
    const trees = new Map();
    function snapshot() { return gh('/commits/' + encode(config.GITHUB_BRANCH)).data; }
    function tree(sha) {
      if (!trees.has(sha)) {
        const value = gh('/git/trees/' + encode(sha)).data;
        if (value.truncated || !Array.isArray(value.tree)) fail('TREE_INCOMPLETE', 'Diretório GitHub incompleto.');
        trees.set(sha, value.tree);
      }
      return trees.get(sha);
    }
    function locate(path) {
      core.safePath(path, config.VAULT_PREFIX);
      let sha = snapshot().commit.tree.sha;
      const parts = path.split('/');
      for (let i = 0; i < parts.length; i++) {
        const entry = tree(sha).find(e => e.path === parts[i]);
        if (!entry) return null;
        if (i < parts.length - 1) {
          if (entry.type !== 'tree' || entry.mode !== '040000') fail('INVALID_PATH', 'Diretório não regular.');
          sha = entry.sha;
        } else {
          if (entry.type !== 'blob' || !['100644', '100755'].includes(entry.mode)) {
            fail('INVALID_PATH', 'Symlinks e submódulos não são notas regulares.');
          }
          return entry;
        }
      }
      return null;
    }
    const cache = env.CacheService.getScriptCache();
    const indexKey = 'gw-index:' + hash(ghRoot + ':' + config.GITHUB_BRANCH + ':' + config.VAULT_PREFIX);
    const github = {
      head() { return snapshot().sha; },
      read(path, remaining) {
        const entry = locate(path);
        if (!entry) return null;
        const max = remaining === undefined ? core.LIMITS.file : Math.min(remaining, core.LIMITS.file);
        if (!Number.isSafeInteger(entry.size) || entry.size > max) {
          if (remaining !== undefined) return { tooLarge: true };
          fail('FILE_TOO_LARGE', 'Nota acima de 128 KiB.');
        }
        const blob = gh('/git/blobs/' + encode(entry.sha), undefined, undefined, [200], 192 * 1024).data;
        if (blob.encoding !== 'base64') fail('REMOTE_INVALID', 'Codificação do arquivo inválida.');
        const decoded = env.Utilities.base64Decode(blob.content.replace(/\s/g, ''));
        const content = env.Utilities.newBlob(decoded).getDataAsString('UTF-8');
        // Rejeita UTF-8 inválido em vez de regravar substituições silenciosas de bytes.
        if (env.Utilities.base64Encode(content, env.Utilities.Charset.UTF_8) !== env.Utilities.base64Encode(decoded)) {
          fail('REMOTE_INVALID', 'Nota não é UTF-8 válido.');
        }
        if (core.bytes(content) > max) fail('REMOTE_TOO_LARGE', 'Arquivo excedeu o orçamento.');
        return { sha: entry.sha, content };
      },
      write(path, content, sha, message) {
        core.safePath(path, config.VAULT_PREFIX);
        const payload = { message, content: env.Utilities.base64Encode(content, env.Utilities.Charset.UTF_8),
          branch: config.GITHUB_BRANCH };
        if (sha !== null) payload.sha = sha;
        const r = gh('/contents/' + path.split('/').map(encode).join('/'), 'put', payload, [200, 201, 409, 422]);
        if ([409, 422].includes(r.status)) fail('SHA_CONFLICT', 'Conflito de versão GitHub.', true);
        if (!r.data.commit || !r.data.commit.sha) fail('REMOTE_UNCERTAIN', 'Commit sem confirmação; repita com o mesmo id.', true);
        try { cache.remove(indexKey); } catch (_) { /* Cache é apenas otimização. */ }
        return r.data.commit.sha;
      },
      reconcile(base, id, digest, pages) {
        // Captura a cabeça uma vez para manter páginas consistentes durante a recuperação.
        const tip = snapshot().sha;
        for (let page = 1; page <= pages; page++) {
          const commits = gh('/commits?sha=' + encode(tip) + '&per_page=100&page=' + page).data;
          if (!Array.isArray(commits)) fail('REMOTE_INVALID', 'Histórico inválido.');
          for (const commit of commits) {
            if (commit.sha === base) return null;
            const message = commit.commit.message;
            const found = /\n\nGateway-Id: ([A-Za-z0-9_-]{1,80})\nGateway-Hash: ([a-f0-9]{64})\s*$/.exec(message);
            if (found && found[1] === id) {
              if (found[2] !== digest) fail('ID_REUSED', 'Id no histórico associado a outro payload.');
              return commit.sha;
            }
          }
          if (commits.length < 100) break;
        }
        fail('RECONCILIATION_REQUIRED', 'Fronteira ausente nos 300 commits; requer inspeção manual.', false);
      },
      index() {
        let stored;
        try { stored = cache.get(indexKey); } catch (_) { /* Evicção/indisponibilidade não afeta correção. */ }
        if (stored) {
          try {
            const parsed = JSON.parse(stored);
            if (Array.isArray(parsed.paths) && parsed.paths.length <= core.LIMITS.index) return parsed;
          } catch (_) { /* Reconstrói cache inválido. */ }
        }
        const root = snapshot().commit.tree.sha;
        const data = gh('/git/trees/' + encode(root) + '?recursive=1').data;
        if (!Array.isArray(data.tree)) fail('REMOTE_INVALID', 'Índice GitHub inválido.');
        let truncated = Boolean(data.truncated), size = 0;
        const paths = [];
        for (const e of data.tree) {
          if (e.type !== 'blob' || !['100644', '100755'].includes(e.mode)) continue;
          try { core.safePath(e.path, config.VAULT_PREFIX); } catch (_) { continue; }
          const added = core.bytes(JSON.stringify(e.path)) + 1;
          if (paths.length >= core.LIMITS.index || size + added > 80000) { truncated = true; continue; }
          paths.push(e.path); size += added;
        }
        const result = { paths, truncated };
        try { cache.put(indexKey, JSON.stringify(result), 120); } catch (_) { /* Não é ledger. */ }
        return result;
      }
    };
    const calendar = {
      get(id) {
        const r = cal('/' + encode(id), undefined, undefined, [200, 404, 410]);
        if (r.status === 410) fail('EVENT_GONE', 'Evento removido; não será recriado por retry.');
        return r.data;
      },
      insert(event) {
        const r = cal('?sendUpdates=none', 'post', event, [200, 201, 409]);
        if (r.status === 409) fail('EVENT_CONFLICT', 'Id de evento existente.', true);
        return r.data;
      },
      list(timeMin, timeMax, max) {
        const fields = 'items(id,status,summary,start(date,dateTime,timeZone),end(date,dateTime,timeZone)),nextPageToken';
        const r = cal('?timeMin=' + encode(timeMin) + '&timeMax=' + encode(timeMax) +
          '&singleEvents=true&orderBy=startTime&maxResults=' + max + '&fields=' + encode(fields));
        const items = [];
        let truncated = Boolean(r.data.nextPageToken), size = 0;
        for (const e of r.data.items || []) {
          const title = Array.from(e.summary || '').slice(0, 80).join('');
          if (title !== (e.summary || '')) truncated = true;
          const item = { eventId: e.id, title, start: e.start, end: e.end, status: e.status };
          const added = core.bytes(JSON.stringify(item)) + 1;
          if (items.length >= max || size + added > 24000) { truncated = true; break; }
          items.push(item); size += added;
        }
        return { items, truncated, limit: max };
      }
    };
    const prefix = 'gw-ledger:';
    const ledger = {
      get(key) {
        const raw = properties.getProperty(prefix + key);
        if (raw === null) return null;
        let state;
        try { state = JSON.parse(raw); } catch (_) { fail('LEDGER_CORRUPT', 'Ledger inválido; requer manutenção.'); }
        if (!state || !/^[a-f0-9]{64}$/.test(state.hash || '') ||
            !/^[a-f0-9]{64}$/.test(state.target || '') || !['pending', 'done'].includes(state.status) ||
            (state.status === 'done' && !state.result)) fail('LEDGER_CORRUPT', 'Ledger inválido; requer manutenção.');
        return state;
      },
      set(key, value) {
        const raw = JSON.stringify(value);
        if (core.bytes(raw) > 8000) fail('LEDGER_FULL', 'Entrada de ledger acima do limite.');
        properties.setProperty(prefix + key, raw);
        if (properties.getProperty(prefix + key) !== raw) fail('LEDGER_UNCERTAIN', 'Persistência do ledger não confirmada.', true);
      },
      count() { return Object.keys(properties.getProperties()).filter(k => k.startsWith(prefix)).length; }
    };
    const lock = {
      run(fn) {
        const mutex = env.LockService.getScriptLock();
        if (!mutex.tryLock(5000)) fail('BUSY', 'Gateway ocupado; repita com o mesmo id.', true);
        try { return fn(); }
        finally { mutex.releaseLock(); }
      }
    };
    const webSearch = searchModule ? searchModule.create(env, core, config) : null;
    return core.create({ config, schema, hash, github, calendar, ledger, lock, webSearch });
  }
  return { create };
})();
if (typeof module !== 'undefined' && module.exports) module.exports = GatewayAppsScript;
