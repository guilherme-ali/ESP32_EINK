#pragma once
namespace Assistant {
// Semantic copy of gateway/tool-schema.json. assistant_schema_test.py checks it.
static const char kToolSchema[] = R"schema({
  "functionDeclarations": [
    {
      "name": "vault_search",
      "description": "Busca literal em títulos de arquivos Markdown e conteúdo sob orçamento. Consulte truncated e limits; não é busca completa do vault.",
      "parameters": {
        "type": "object",
        "properties": {
          "query": { "type": "string", "description": "Texto literal não vazio, até 256 bytes UTF-8." },
          "limit": { "type": "integer", "description": "Máximo de resultados: 1 a 20; padrão 10." }
        },
        "required": ["query"]
      }
    },
    {
      "name": "vault_read",
      "description": "Lê um arquivo .md relativo à raiz do repositório, dentro de VAULT_PREFIX quando configurado.",
      "parameters": {
        "type": "object",
        "properties": { "path": { "type": "string", "description": "Caminho relativo .md, sem traversal, até 240 bytes UTF-8." } },
        "required": ["path"]
      }
    },
    {
      "name": "vault_create",
      "description": "Cria uma nota .md somente se ainda não existir. Preserve o YAML e os wikilinks no conteúdo enviado.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": { "type": "string" },
          "content": { "type": "string", "description": "Conteúdo literal, até 16 KiB UTF-8." }
        },
        "required": ["path", "content"]
      }
    },
    {
      "name": "vault_append",
      "description": "Acrescenta texto literal ao final da nota ou da seção ATX indicada. Reutilize o mesmo id nas retentativas.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": { "type": "string" },
          "text": { "type": "string", "description": "Texto não vazio, até 16 KiB UTF-8." },
          "heading": { "type": "string", "description": "Título literal da seção existente, sem #, único, até 256 bytes." }
        },
        "required": ["path", "text"]
      }
    },
    {
      "name": "vault_replace",
      "description": "Substitui exatamente uma ocorrência literal de oldText; falha se ausente ou ambígua. Não reformata a nota.",
      "parameters": {
        "type": "object",
        "properties": {
          "path": { "type": "string" },
          "oldText": { "type": "string", "description": "Texto não vazio, até 16 KiB UTF-8." },
          "newText": { "type": "string", "description": "Substituição literal, até 16 KiB UTF-8." }
        },
        "required": ["path", "oldText", "newText"]
      }
    },
    {
      "name": "calendar_list",
      "description": "Lista até 50 eventos no intervalo absoluto RFC3339 com Z ou offset; informa truncated.",
      "parameters": {
        "type": "object",
        "properties": {
          "timeMin": { "type": "string", "description": "RFC3339 com timezone explícito." },
          "timeMax": { "type": "string", "description": "RFC3339 posterior a timeMin; intervalo máximo 366 dias." }
        },
        "required": ["timeMin", "timeMax"]
      }
    },
    {
      "name": "calendar_create",
      "description": "Cria evento com datas absolutas RFC3339 e fuso IANA; nunca interpreta datas relativas. Mesmo id evita duplicação.",
      "parameters": {
        "type": "object",
        "properties": {
          "title": { "type": "string", "description": "Título não vazio, até 256 bytes UTF-8." },
          "start": { "type": "string", "description": "RFC3339 com Z ou offset explícito." },
          "end": { "type": "string", "description": "RFC3339 posterior a start; duração máxima 366 dias." },
          "timeZone": { "type": "string", "description": "Fuso IANA, por exemplo America/Sao_Paulo. Não corrige offsets fornecidos." },
          "description": { "type": "string", "description": "Até 16 KiB UTF-8." },
          "reminderMinutes": { "type": "integer", "description": "Opcional: lembrete popup de 0 a 40320 minutos; ausente usa padrão do calendário." }
        },
        "required": ["title", "start", "end", "timeZone"]
      }
    },
    {
      "name": "web_search",
      "description": "Pesquisa real no Google via Gemini, somente quando o provedor gratuito estiver explicitamente configurado. Retorna resposta fundamentada, fontes e metadados; sem grounding retorna erro, nunca conhecimento como pesquisa.",
      "parameters": {
        "type": "object",
        "properties": {
          "query": { "type": "string", "description": "Consulta literal não vazia, até 256 bytes UTF-8. O servidor fixa modelo, ferramentas e limites." }
        },
        "required": ["query"]
      }
    }
  ]
})schema";
} // namespace Assistant
