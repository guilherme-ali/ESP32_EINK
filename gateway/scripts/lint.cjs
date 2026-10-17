'use strict';
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
let count = 0;
function visit(dir) {
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    if (['dist', 'node_modules', '.git'].includes(entry.name)) continue;
    const file = path.join(dir, entry.name);
    if (entry.isDirectory()) { visit(file); continue; }
    if (/\.(js|cjs)$/.test(file)) {
      const result = spawnSync(process.execPath, ['--check', file], { stdio: 'inherit' });
      if (result.status !== 0) process.exit(result.status || 1);
      count++;
    }
    if (file.endsWith('.json')) JSON.parse(fs.readFileSync(file, 'utf8'));
    if (/\.(js|cjs|json|md)$/.test(file) && /[\t ]+$/m.test(fs.readFileSync(file, 'utf8'))) {
      throw new Error('Espaço no fim de linha: ' + file);
    }
  }
}
visit(root);
console.log('Lint sintático e JSON concluído: ' + count + ' arquivos JavaScript.');
