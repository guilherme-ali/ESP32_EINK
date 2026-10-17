'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { fixture } = require('./fixture.cjs');

const config = { GEMINI_SEARCH_API_KEY: 'search-key-test-abcdefghijk',
  GEMINI_SEARCH_MODEL: 'gemini-2.5-flash', SEARCH_FREE_CONFIRMED: 'true' };
function grounded() {
  return { candidates: [{ finishReason: 'STOP', content: { parts: [{ text: 'Resposta fundamentada.' }] },
    groundingMetadata: { webSearchQueries: ['consulta'], groundingChunks: [
      { web: { uri: 'https://example.org/fonte', title: 'Fonte' } }],
    groundingSupports: [{ segment: { startIndex: 0, endIndex: 21, text: 'Resposta fundamentada.' }, groundingChunkIndices: [0] }] } }] };
}
function ready(data = grounded(), status = 200, overrides = {}) {
  const f = fixture({}, { ...config, ...overrides });
  f.faults.fetch = (_url, _options, response) => response(status, data);
  return f;
}

test('pesquisa exige confirmação, chave e candidato explícito, sem chamadas quando desabilitada', () => {
  for (const overrides of [{ SEARCH_FREE_CONFIRMED: '' }, { GEMINI_SEARCH_API_KEY: '' },
    { GEMINI_SEARCH_MODEL: 'gemini-2.5-pro' }]) {
    const f = ready(grounded(), 200, overrides);
    assert.equal(f.post('web_search', { query: 'consulta' }).error.code, 'TOOL_DISABLED');
    assert.equal(f.calls.length, 0);
  }
});
test('requisição fixa google_search, chave apenas no header e resposta com fontes verificáveis', () => {
  const f = ready();
  const result = f.post('web_search', { query: 'consulta atual' });
  assert.equal(result.ok, true);
  assert.equal(result.result.grounded, true);
  assert.equal(result.result.sources[0].url, 'https://example.org/fonte');
  assert.equal(result.result.groundingMetadata.groundingSupports[0].sourceIds[0], 's1');
  const request = f.calls[0];
  assert.equal(request.url, 'https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent');
  assert.equal(request.options.headers['x-goog-api-key'], config.GEMINI_SEARCH_API_KEY);
  assert.equal(request.options.followRedirects, false);
  const body = JSON.parse(request.options.payload);
  assert.deepEqual(body.tools, [{ google_search: {} }]);
  assert.deepEqual(JSON.parse(body.contents[0].parts[0].text), { query: 'consulta atual' });
  assert.equal(JSON.stringify(result).includes(config.GEMINI_SEARCH_API_KEY), false);
});
test('não apresenta conhecimento sem grounding como pesquisa web', () => {
  for (const alter of [d => delete d.candidates[0].groundingMetadata,
    d => d.candidates[0].groundingMetadata.webSearchQueries = [],
    d => d.candidates[0].groundingMetadata.groundingSupports = [],
    d => d.candidates[0].groundingMetadata.groundingChunks[0].web.uri = 'javascript:alert(1)']) {
    const data = grounded(); alter(data);
    assert.equal(ready(data).post('web_search', { query: 'consulta' }).error.code, 'SEARCH_NOT_GROUNDED');
  }
});
test('cota e autenticação não disparam fallback nem ecoam corpo do provedor', () => {
  for (const [status, code] of [[429, 'SEARCH_QUOTA_EXCEEDED'], [403, 'SEARCH_AUTH_ERROR'],
    [404, 'SEARCH_MODEL_UNAVAILABLE'], [503, 'SEARCH_UNAVAILABLE']]) {
    const f = ready({ error: { message: config.GEMINI_SEARCH_API_KEY } }, status);
    const response = f.post('web_search', { query: 'consulta' });
    assert.equal(response.error.code, code);
    assert.equal(f.calls.length, 1);
    assert.equal(JSON.stringify(response).includes(config.GEMINI_SEARCH_API_KEY), false);
  }
});
test('recusa resposta truncada e segredo refletido', () => {
  const truncated = grounded(); truncated.candidates[0].finishReason = 'MAX_TOKENS';
  assert.equal(ready(truncated).post('web_search', { query: 'consulta' }).error.code, 'SEARCH_INCOMPLETE');
  const secret = grounded(); secret.candidates[0].content.parts[0].text = config.GEMINI_SEARCH_API_KEY;
  assert.equal(ready(secret).post('web_search', { query: 'consulta' }).error.code, 'SEARCH_RESPONSE_INVALID');
});
test('transporte incerto retorna erro e não repete chamada de busca', () => {
  const f = ready(); f.faults.fetch = () => { throw new Error(config.GEMINI_SEARCH_API_KEY); };
  const response = f.post('web_search', { query: 'consulta' });
  assert.equal(response.error.code, 'SEARCH_TRANSPORT_ERROR');
  assert.equal(f.calls.length, 1);
  assert.equal(JSON.stringify(response).includes(config.GEMINI_SEARCH_API_KEY), false);
});
