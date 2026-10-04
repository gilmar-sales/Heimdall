# Revisão de Arquitetura — Heimdall (LSP / Linter / Formatter C++)

> Foco: gargalos de performance, consumo de memória, concorrência e design da AST.
> Pilares: (1) memória da AST, (2) strings/tokens, (3) concorrência/LSP <50ms,
> (4) parsing incremental e tolerância a falhas, (5) Data-Oriented Design.
> Alvo: C++23.

## Veredito resumido

O núcleo já evita os piores anti-padrões: sem `new` por nó, tokens zero-copy,
snapshot imutável no LSP, re-lex/re-parse parcial. Os gargalos residuais estão
nas bordas: cópias de `std::string` no semântico/completion/diagnóstico,
`Children()` alocante, contenção em `m_mu`, I/O síncrono no caminho interativo,
e ausência de Red-Green Tree / interning.

## 1. Gerenciamento de memória da AST — pointer chasing

### Estado atual: bom no núcleo, ruim nas bordas

Núcleo é flat, não há árvore de ponteiros:

- `core/include/Heimdall/Lexer.hpp:27` — `Token{kind:u8, tok:u8, offset:u32, length:u32}` = 12 bytes, com `static_assert`. Contíguo em `vector<Token>`.
- `core/include/Heimdall/SyntaxTree.hpp:25`, `core/include/Heimdall/ParseTree.hpp:78` — `GreenNode/GrammarNode{kind:u8, first_token:u32, token_count:u32, parent:u32, subtree_end:u32}`. Índice, não `Node*`. `IsDescendant` é teste de intervalo O(1).
- `core/include/Heimdall/Arena.hpp:15` + `core/src/Arena.cpp:8` — bump allocator sobre `pmr::monotonic_buffer_resource`, `Reset()` O(1). **Mas** usado só em `semantic/` (`semantic/src/SemanticModel.cpp:62`, `semantic/src/Typer.cpp:173`, `semantic/src/Flow.cpp:27`). `ParseTree/SyntaxTree` usam `vector` puro, não `Arena`.

### Trechos críticos

- `core/src/ParseTree.cpp:78` / `core/src/SyntaxTree.cpp:169` — `Children() -> vector<size_t>` aloca por chamada + varredura linear `O(subtree)`. Chamado em hot paths: `core/src/Navigation.cpp:1026,1052`, `core/src/Completion.cpp:2062`.
- `core/include/Heimdall/ParseTree.hpp:231` — `vector<bool> m_decoration` (bitset especializado, lento, não contíguo por byte).
- `lsp/Server.hpp:206` — `ParseSlot{once_flag, shared_ptr<ParseTree>, atomic<bool>}` + `unordered_map<string,ParseCacheEntry>` guarda `text`, `base`, `slot` por documento: 2–3x o texto vivo em `shared_ptr<string>` + árvore + tokens simultaneamente.

### Impacto estimado

Traversal do núcleo é cache-friendly (~12–16B/nó). `Children()` em loop de
completion/navigation transforma O(filhos) em O(filhos + malloc + scan).
Em arquivo 10k tokens / 2k nós, centenas de chamadas = dezenas de µs viram ms +
pressão no allocator.

### Solução arquitetural (C++23)

1. Manter `vector<Node>` (já é arena implícita). Não migrar para `unique_ptr`.
2. Eliminar `Children()` alocante: retornar `std::ranges::subrange` / par `begin/end` sobre índice de primeiro-filho, ou pré-computar `first_child/next_sibling` como `u32` no próprio nó (ainda 16–20B, cabe em 1–2 linhas de cache).
3. Trocar `vector<bool>` por `vector<uint8_t>` / `vector<char>` ou bitset PMR.
4. Unificar `Arena` para tudo: `ParseTree` alocar `tokens/nodes/diagnostics` via `pmr::vector` sobre `Arena::Resource()`. Vida = versão do documento, `Reset()` no `didClose`/evicção.

```cpp
// Antes
std::vector<std::size_t> ParseTree::Children(std::size_t n) const; // aloca

// Depois (C++23): sem alocação, O(filhos)
struct ChildRange {
  const GrammarNode* nodes;
  std::uint32_t begin, end; // índices densos de filhos
  const GrammarNode* operator[](std::size_t i) const noexcept { return &nodes[begin + i]; }
};
ChildRange children(std::size_t n) const noexcept;
```

## 2. Representação de strings e tokens

### Lexer/parser: exemplar. Camadas superiores: vazam cópias

Zero-copy no núcleo:

- `core/src/Lexer.cpp:433,435` — só `substr` como `string_view` para `LookupTok`/`MayBeKeyword`; nunca materializa `std::string`. `Tok` + `SingleCharTok` + `MayBeKeyword` evitam `memcmp`.
- `core/include/Heimdall/Preprocessor.hpp:60` — `TransparentStringHash` permite lookup por `string_view` sem chave temporária.
- `core/src/GrammarParser.cpp:260` — `Text(sig)` retorna `view` no buffer original.

### Vazamentos

- `core/include/Heimdall/RuleEngine.hpp:66` — `Diagnostic{code:string, message:string, fix{replacement:string}, fix_title:string}`. Cada lint = 3–4 alocações. `ApplyFixes:1293` faz `string result(source)` cópia integral + `Format:Format()` idem.
- `core/src/Completion.cpp:404,668,811,859,1276,1289` — `unordered_map<string,CompletionItem>`, `vector<vector<string>> BuildScopePaths`, `ScopeNameElements -> vector<string>`, `SliceRange -> string`, `CompactWs(string)` por valor. `InsertItem:2029,2285` faz `string(name)` por candidato.
- `core/src/Preprocessor.cpp:451` — `PreprocessorResult{active_source:string, ...}`; `Process()` com `build_active_source=true` copia + `ExpandObjectMacros` por linha.
- `core/src/Formatter.cpp:1362,1454,2983,3050,3613` — `string(inner)`, `normalize_comment(string(...))`, `lines.push_back({pos,string(...)})`.
- `lsp/Server.cpp:556` — `"reference to '"+name+"' is ambiguous..."` concatena por diagnóstico no publish.

### Impacto estimado

Em completion com 2k escopos, `BuildScopePaths` = O(nós × profundidade) `string`s.
É o maior custo fora do parse. Diagnósticos com `string` impedem `memcpy` da
árvore entre versões.

### Solução (C++23)

1. `Diagnostic` com `std::string_view` / `offset+length` + `code:RuleId` (já existe `RuleId:22`) em vez de `code:string`. Mensagem via `constexpr string_view` de catálogo + argumentos formatados só na borda LSP (`std::format_to` direto no buffer JSON).
2. String interning para identificadores: `StringPool: vector<char> + unordered_map<string_view,u32>` por `ParseTree`; `CompletionItem{label_id:u32}`; comparação `==` vira `==` de inteiros O(1). Manter `string_view` para texto original (lifetime = `HoldSource`).
3. `Completion`: trocar `vector<string> path` por `vector<u32>` (IDs interned) + `path_key` com hash incremental, sem concatenar `string key`.
4. `Formatter/Preprocessor`: nunca `string(view)` no interior; operar em `string_view` + `out.append(view)`.

```cpp
// Pool por documento/índice global (C++23)
struct StringPool {
  std::string buffer; // backing estável após reserve
  std::unordered_map<std::string_view, std::uint32_t,
    TransparentStringHash, std::equal_to<>> index;
  std::uint32_t intern(std::string_view s);
  std::string_view resolve(std::uint32_t id) const noexcept;
};
```

## 3. Concorrência e modelo do LSP — meta <50ms

### Modelo atual já é I/O não-bloqueante, mas com contenção e caudas longas

Acertos:

- `lsp/Server.cpp:217` — `Dispatch` copia `body` para `string` e submete ao pool; I/O volta a ler. `RequestContext{pinned DocumentSnapshot}` congela versão (`lsp/Server.hpp:48`).
- `lsp/ThreadPool.hpp:22` — pool 3–8 threads (`Server.cpp:31`), duas prioridades interativo/background.
- `lsp/Server.hpp:167` — `shared_mutex m_docs_mu` só para `m_documents`; leitores concorrentes.
- `lsp/Server.cpp:1927` — `DiagWorkerMain` com debounce 50ms (`1977`), coalescência por URI, `stop_source` para cancelar versão obsoleta, `IsCurrentVersion` antes de publicar.
- `lsp/Server.cpp:1576` — `CachedParse` com `call_once` por slot + `stop_token` cooperativo entre itens top-level.

### Falhas

- `lsp/Server.hpp:173` — `mutex m_mu` único guarda `parse_cache + include_cache + profiles + global_indices + macro_cache`. Todo `completion/hover/goto/diag` serializa ali (`HeaderScopes:1421`, `CachedParse:1585`, `ParserOptionsFor:1558`).
- `lsp/Server.cpp:1381` — `AwaitHeaderScopes` faz `sleep_for(25ms)` em loop até 10s **na thread do pool**. Hover/goto sequestram worker interativo.
- `lsp/Server.cpp:2367,2395` — `GotoDocument` faz `ReadWholeFile` + `ParseTree::Parse` síncrono de cada candidato `foo.cpp` na thread do request (`2375,2403`). Pior caso: N arquivos × parse integral = >500ms, sem paralelismo, sem cache.
- `lsp/ThreadPool.hpp:50,97` — fila única com `mutex + condition_variable + std::function<void()>` por tarefa = alocação + lock por request; sem work-stealing, sem afinidade.
- `lsp/Server.cpp:261` — `simdjson::dom::parser` re-parseia `body` no worker (duplo parse: I/O + worker). `dom` materializa DOM; SAX/`ondemand` seria suficiente.

### Impacto estimado

p50 provavelmente <50ms (cache quente, arquivo pequeno). p95/p99 estoura:
primeiro hover com índice frio, goto com 5 candidatos, burst de keystrokes com
`m_mu` disputado.

### Solução (C++23)

1. Shard `m_mu`: `docs_mu` (já), `parse_mu`, `index_mu`, `macro_mu` separados; `macro_cache` como `atomic<shared_ptr>` imutável (só cresce por `CompileCommand*`).
2. Eliminar polling: `AwaitHeaderScopes` → `std::future/shared_future<Index>` + `isIncomplete:true` imediato; cliente re-pede. Nunca `sleep` em worker interativo.
3. `GotoDocument`: índice persistente de símbolos (path→`vector<Def>` com `u32` offsets) construído no `IndexWorker`; request só faz lookup O(1), sem `Parse` síncrono. Se precisar parsear, submeter como subtarefa com mesmo `stop_token` e limite `kMaxBytes:2083` já existente.
4. Pool: `std::move_only_function` + fila MPMC lock-free (ou ao menos `mutex` por prioridade + `pmr` para tarefas), `hardware_concurrency` com cap configurável.
5. `simdjson::ondemand` no `Dispatch` + `GetString` via `string_view` direto no `body` copiado (já é `string` estável).

## 4. Parsing incremental e tolerância a falhas

### Melhor que re-parse ingênuo, longe de incremental verdadeiro

Acertos:

- `core/include/Heimdall/Lexer.hpp:74` — `Relex(tokens, edit)` com janela + `lookbehind 32` (`Lexer.cpp:444`), `binary search` do ponto de resume, shift de `offset` da cauda. `Server.cpp:738` chama por `didChange` incremental; `LineIndex::Update:41` só re-escaneia linhas do edit.
- `core/include/Heimdall/ParseTree.hpp:104,126` — `ParseReuse{previous*, offset, old/new_length}` + `TopLevelItem{first_token, token_end, sig_count, node_begin/end, diag_begin/end, reusable}`. `GrammarParser.cpp:3142,3197` copia itens fora do edit.
- `core/src/GrammarParser.cpp:2343,2391,3493` — `Error/ErrorExpression` + `unclosed/unmatched delimiter` (`SyntaxTree.cpp:78,114`) permitem AST parcial; `stop_token` entre itens + `Cancelled()` descartável.
- `lsp/Server.cpp:135` — `EditHull{Compose}` funde múltiplos `contentChanges` de um `didChange` em um edit.

### Lacunas

- `core/src/Lexer.cpp:531` — `Relex` ainda faz `for(t=tail_start; t<size) offset+=delta` O(N_tokens_cauda) por tecla. Em 100k tokens, cada caractere no topo = 100k shifts.
- `core/src/GrammarParser.cpp:3146` — `reusable` exige `diag_begin==diag_end` e `kind != Error/ErrorExpression`. Código digitado está **sempre** com erro → reuso ~0 no caso interativo mais comum. `FindSemicolon/FindComma/SkipGroup` varrem longe em recovery.
- `core/src/GrammarParser.cpp:43` — `IsDecorationMacro` faz `Lexer(value).Lex()` **por macro distinta por parse** (com memo `unordered_map<string_view,bool>` local, descartado a cada parse). Repetido a cada keystroke.
- `lsp/Server.cpp:500` — `PublishDiagnostics` faz `lines.Build(*text)` O(N) integral apesar de ter `LineIndex` incremental no snapshot. `Formatter::FormatEdits:1822` formata buffer inteiro e diffa.
- Sem Green/Red Tree: `subtree_end` permite `IsDescendant` rápido, mas não há IDs estáveis de nós; LSP não consegue `reuse + patch`, só `copy items`.

### Impacto estimado

Digitação contínua = `Relex O(cauda) + Preprocess O(N) + Grammar O(N_item_afetado)`
por versão, mais `Build O(N)` no publish. Debounce de 50ms mascara, não resolve.

### Solução (C++23)

1. Tokens com `u32` absolutos já permitem deslocamento lazy: guardar `edit_delta` + `edit_offset` e materializar offset sob demanda, ou armazenar tokens da cauda em `pmr::deque` de blocos para splice O(blocos), não O(tokens).
2. Red-Green light: tornar `GrammarNode` verdadeiramente imutável + `RedNode{green_idx, parent, abs_offset}` sob demanda (só para posição/hover). Reuso por `(green_hash, sig_count)` em vez de `diag==0`. Erro local não invalida irmãos.
3. `IsDecorationMacro` → `constexpr` table ou cache global `shared_ptr<const unordered_map<string,bool>>` por `MacroMap*` (já existe `m_macro_cache` no server, estender).
4. `PublishDiagnostics` reutilizar `snapshot.lines` (já incremental) em vez de `Build`; `Formatter` operar por `TopLevelItem` range.

## 5. Data-Oriented Design — AoS→SoA, cache L1/L2

### Layout atual é AoS compacto, não SoA; bom o suficiente para parse, ruim para queries

- `Token` 12B, `GrammarNode` ~16B (`ParseTree.hpp:78`): array de structs cabe ~4–5 nós por linha de 64B. Iteração sequencial (`ParseScope`, `FindSemicolon`) é pré-buscável. Não é pointer chasing.
- `Tok:Tok.hpp:50` como `u8` + `TokLiteral:197` (`consteval LookupTok`) transforma `Is(i,"template")` em comparação de 1 byte — vetoriza bem.
- `GrammarParser.cpp:185` — `m_sig/m_sig_tok/m_match` como `pmr::vector` sobre scratch 32KB (`m_scratch_buffer:243`) = L1-residente. Bom.

### Anti-DoD

- Nós interleavam `kind/parent/range/subtree_end`; queries como “todos `IdentifierExpression` em range” varrem tudo, sem partição por kind.
- `core/src/Completion.cpp:927` — `vector<vector<string>> paths(nodes.size())` = N vectors + N×profundidade strings; `unordered_map<string,CompletionItem>` com hash de string por lookup.
- `core/src/RuleEngine.cpp:86` — `FindSuppressions` + `AnalyzeImpl:698` re-varrem tokens por regra; sem bitset de máscara por token.
- `core/src/Formatter.cpp:3756` linhas — múltiplos passes com `string` temporários, não operação vetorial sobre spans.

### Solução (C++23)

1. SoA seletivo, não total: manter `vector<Node>` mas splitar hot columns: `vector<Tok> sig_tok`, `vector<u32> first_token`, `vector<u8> kind`. Filtros (`kind==X`) viram loop sobre `u8` contíguo, auto-vetorizável.
2. Substituir `unordered_map<string,_>` em completion por `flat_hash<u32,_>` (IDs interned) + `vector<ScopeEntry>` ordenado; prefix match via `starts_with` sobre `string_view` do pool, sem alocar.
3. Pré-computar por `ParseTree`: `vector<u32> ident_tokens`, `vector<u32> directives`, máscara `is_trivia`. Regras/linters iteram slices densos, não todos os tokens.
4. `std::mdspan` / `std::views::chunk` para passes do formatter sobre spans, sem `string` intermediário; `std::execution::par_unseq` apenas no CLI batch (`src/Pipeline.cpp:345 RunParallel` já usa `jthread` + `atomic index` — correto; estender para linter por arquivo, nunca por token dentro de um arquivo no LSP).

## Roadmap priorizado (impacto/esforço)

| # | Ação | Impacto | Esforço |
|---|------|---------|---------|
| 1 | `Children()` sem alocação + `vector<bool>`→`vector<u8>` | Elimina mallocs no hot path | Dias |
| 2 | `Diagnostic` sem `string` + interning de identificadores | Reduz RAM, comparação O(1) | Semanas |
| 3 | Shard `m_mu` + remover `sleep` de `AwaitHeaderScopes` + índice de símbolos para `Goto` | Garante p95 <50ms | Semanas |
| 4 | Reuso de `LineIndex` no publish + deltas lazy no `Relex` | Tira O(N) por tecla | Dias |
| 5 | SoA de `kind/tok` + slices densos para regras | Prepara queries demand-driven estilo salsa | Semanas |
