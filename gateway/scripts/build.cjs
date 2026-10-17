'use strict';
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');

function bundle() {
  const schema = JSON.parse(fs.readFileSync(path.join(root, 'tool-schema.json'), 'utf8'));
  const chunks = ['/* Gerado por npm run build. Sem properties, tokens ou dependências Node. */',
    'var GatewaySchema = ' + JSON.stringify(schema) + ';'];
  for (const file of ['core.js', 'web-search.js', 'apps-script.js', 'entry.js']) {
    // Remove apenas a exportação Node; nenhuma substituição de valores de ambiente.
    chunks.push(fs.readFileSync(path.join(root, 'src', file), 'utf8')
      .replace(/^if \(typeof module !== 'undefined'.*module\.exports.*;\r?$/gm, ''));
  }
  const result = chunks.join('\n');
  if (/\brequire\s*\(|\bprocess\.|module\.exports/.test(result)) throw new Error('Artefato contém referência Node.');
  new vm.Script(result, { filename: 'Code.gs' });
  return result;
}
if (require.main === module) {
  if (!fs.existsSync(root)) throw new Error('Diretório gateway ausente.');
  fs.mkdirSync(path.join(root, 'dist'), { recursive: true });
  fs.writeFileSync(path.join(root, 'dist', 'Code.gs'), bundle(), 'utf8');
  fs.copyFileSync(path.join(root, 'appsscript.json'), path.join(root, 'dist', 'appsscript.json'));
  console.log('Gerados gateway/dist/Code.gs e appsscript.json, ignorados pelo Git.');
}
module.exports = { bundle };
