/* Funções públicas reconhecidas pelo runtime Apps Script. */
function gatewayJson_(value) {
  return ContentService.createTextOutput(JSON.stringify(value)).setMimeType(ContentService.MimeType.JSON);
}
function doGet() {
  // Não lê properties nem parâmetros (inclusive token em query string).
  return gatewayJson_({ version: 1, status: 'ok' });
}
function doPost(e) {
  try {
    const app = GatewayAppsScript.create({ PropertiesService, Utilities, UrlFetchApp,
      ScriptApp, CacheService, LockService }, GatewayCore, GatewaySchema, GatewayWebSearch);
    const post = e && e.postData;
    if (e && (e.contentLength > GatewayCore.LIMITS.body || (post && post.length > GatewayCore.LIMITS.body))) {
      return gatewayJson_({ version: 1, id: null, ok: false, error: {
        code: 'BODY_TOO_LARGE', message: 'Corpo deve ter até 64 KiB.', retryable: false } });
    }
    return gatewayJson_(app.handle(post && post.contents, post && post.type));
  } catch (_) {
    return gatewayJson_({ version: 1, id: null, ok: false, error: {
      code: 'INTERNAL_ERROR', message: 'Falha interna ou de persistência.', retryable: true } });
  }
}
function autorizarGateway() {
  // Execute manualmente no editor, como proprietário, para conceder os escopos do manifesto.
  PropertiesService.getScriptProperties().getProperty('TIME_ZONE');
  ScriptApp.getOAuthToken();
}
