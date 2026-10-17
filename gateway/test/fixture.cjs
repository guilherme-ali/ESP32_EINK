'use strict';
// Simula os serviços GAS e o protocolo HTTP, não o núcleo: SHA, ledger e reconciliação
// dos testes passam pelos mesmos adaptadores usados em produção.
const crypto = require('node:crypto');
const core = require('../src/core.js');
const apps = require('../src/apps-script.js');
const webSearch = require('../src/web-search.js');
const schema = require('../tool-schema.json');
const clone = x => JSON.parse(JSON.stringify(x));

function fixture(initial = {}, overrides = {}) {
  const properties = new Map(Object.entries({ GATEWAY_TOKEN: 'segredo-exclusivo-de-teste-0123456789abcdef',
    GITHUB_TOKEN: 'github-segredo-nao-ecoar', GITHUB_OWNER: 'dono', GITHUB_REPO: 'vault',
    GITHUB_BRANCH: 'main', CALENDAR_ID: 'primary', TIME_ZONE: 'America/Sao_Paulo', ...overrides }));
  const blobs = new Map(), files = new Map(), snapshots = new Map(), commits = [], cache = new Map(), events = new Map();
  const calls = [];
  let sequence = 0, held = false;
  const faults = {};
  function setFile(path, content, mode = '100644') {
    const sha = crypto.createHash('sha1').update(content).digest('hex');
    blobs.set(sha, content);
    files.set(path, { sha, mode, content });
  }
  function commit(message = 'Edição externa') {
    const sha = 'c' + sequence++;
    snapshots.set(sha, new Map([...files].map(([k, v]) => [k, { ...v }])));
    commits.unshift({ sha, commit: { message, tree: { sha: 't:' + sha + ':' } } });
    return sha;
  }
  for (const [path, content] of Object.entries(initial)) setFile(path, content);
  commit('Inicial');
  function response(status, data) {
    return { getResponseCode: () => status, getContentText: () => JSON.stringify(data) };
  }
  function treeEntries(treeSha, recursive) {
    const [, version, ...rest] = treeSha.split(':');
    const dir = rest.join(':');
    const snap = snapshots.get(version);
    const result = new Map();
    for (const [path, f] of snap) {
      if (dir && !path.startsWith(dir + '/')) continue;
      const relative = dir ? path.slice(dir.length + 1) : path;
      const segments = relative.split('/');
      if (recursive || segments.length === 1) {
        result.set(relative, { path: relative, sha: f.sha, mode: f.mode, type: 'blob', size: Buffer.byteLength(f.content) });
      } else {
        const sub = segments[0];
        result.set(sub, { path: sub, sha: 't:' + version + ':' + (dir ? dir + '/' : '') + sub, mode: '040000', type: 'tree' });
      }
    }
    return [...result.values()];
  }
  function fetch(url, options) {
    calls.push({ url, options: clone(options) });
    if (faults.fetch) return faults.fetch(url, options, response);
    if (faults.onFetch) faults.onFetch(url, options);
    const u = new URL(url), p = decodeURIComponent(u.pathname), method = options.method;
    if (u.hostname === 'api.github.com') {
      if (!p.startsWith('/repos/dono/vault/')) throw new Error('Destino GitHub inesperado');
      const route = p.slice('/repos/dono/vault'.length);
      if (route.startsWith('/commits/')) {
        const value = commits.find(c => c.sha === route.slice(9)) || commits[0];
        return response(200, value);
      }
      if (route === '/commits') {
        const at = commits.findIndex(c => c.sha === u.searchParams.get('sha'));
        const page = Number(u.searchParams.get('page'));
        return response(200, commits.slice(at + (page - 1) * 100, at + page * 100));
      }
      if (route.startsWith('/git/trees/')) return response(200, {
        tree: treeEntries(route.slice(11), u.searchParams.has('recursive')), truncated: Boolean(faults.treeTruncated) });
      if (route.startsWith('/git/blobs/')) return response(200, {
        encoding: 'base64', content: Buffer.from(blobs.get(route.slice(11)), 'utf8').toString('base64') });
      if (route.startsWith('/contents/') && method === 'put') {
        if (faults.beforePut) faults.beforePut();
        const payload = JSON.parse(options.payload), path = route.slice(10), old = files.get(path);
        if ((old && old.sha !== payload.sha) || (!old && payload.sha)) return response(faults.conflictStatus || 409, {});
        setFile(path, Buffer.from(payload.content, 'base64').toString('utf8'));
        const sha = commit(payload.message);
        if (faults.githubTimeout) { faults.githubTimeout = false; throw new Error('Timeout após commit com segredo'); }
        return response(old ? 200 : 201, { commit: { sha } });
      }
    }
    if (u.hostname === 'www.googleapis.com') {
      const root = '/calendar/v3/calendars/primary/events';
      if (p.startsWith(root + '/')) {
        const id = p.slice(root.length + 1);
        if (faults.calendarGone) return response(410, {});
        return response(events.has(id) ? 200 : 404, events.get(id) || {});
      }
      if (p === root && method === 'post') {
        const event = JSON.parse(options.payload);
        if (faults.beforeInsert) faults.beforeInsert(event);
        if (events.has(event.id)) return response(409, {});
        events.set(event.id, { ...event, status: 'confirmed' });
        if (faults.calendarTimeout) { faults.calendarTimeout = false; throw new Error('Timeout após evento'); }
        return response(200, events.get(event.id));
      }
      if (p === root) return response(200, { items: [...events.values()], nextPageToken: faults.nextPageToken });
    }
    throw new Error('Rota não simulada: ' + url);
  }
  const env = {
    PropertiesService: { getScriptProperties: () => ({
      getProperty: key => properties.has(key) ? properties.get(key) : null,
      getProperties: () => Object.fromEntries(properties),
      setProperty(key, value) {
        const state = JSON.parse(value);
        if (faults.ledgerBase && state.base) throw new Error('Persistência antes de mutação indisponível');
        if (faults.ledgerDone && state.status === 'done') { faults.ledgerDone = false; throw new Error('Falha depois do commit'); }
        properties.set(key, value);
      }
    }) },
    CacheService: { getScriptCache: () => ({ get: key => cache.get(key),
      put: (key, value) => cache.set(key, value), remove: key => cache.delete(key) }) },
    LockService: { getScriptLock: () => ({
      tryLock() { if (held || faults.busy) return false; held = true; return true; },
      releaseLock() { held = false; }
    }) },
    ScriptApp: { getOAuthToken: () => 'oauth-nao-ecoar' },
    UrlFetchApp: { fetch },
    Utilities: {
      DigestAlgorithm: { SHA_256: 'sha256' }, Charset: { UTF_8: 'utf8' },
      computeDigest: (algorithm, value) => [...crypto.createHash(algorithm).update(value).digest()],
      base64Encode: value => Buffer.from(value).toString('base64'),
      base64Decode: value => [...Buffer.from(value, 'base64')],
      newBlob: value => ({ getDataAsString: () => Buffer.from(value).toString('utf8') })
    }
  };
  function envelope(tool, args, id = 'pedido-1') {
    return { version: 1, token: properties.get('GATEWAY_TOKEN'), id, tool, args };
  }
  function raw(body, mime = 'application/json') { return apps.create(env, core, schema, webSearch).handle(body, mime); }
  return { env, properties, files, events, commits, calls, faults, cache, setFile, commit, envelope, raw,
    post: (tool, args, id) => raw(JSON.stringify(envelope(tool, args, id))),
    locked: () => held };
}
module.exports = { fixture };
