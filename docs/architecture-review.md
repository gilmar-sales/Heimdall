# Revisão de arquitetura: memória, concorrência e latência

Escopo: `core/` (Lexer, Preprocessor, SyntaxTree, ParseTree/GrammarParser, Formatter, RuleEngine,
Completion, Navigation), `semantic/` (IncludeIndex), `lsp/` (Server, Document) e `src/` (CLI).
Método: leitura do código atual. **Nenhum número abaixo foi medido**; os impactos são estimativas
de ordem de grandeza e devem ser confirmados com os benchmarks em `bench/` (ver §7).

## 0. Resumo executivo

O núcleo já está bem mais perto de "data-oriented" do que a premissa do prompt sugere. A AST não
usa `unique_ptr` por nó, os tokens são spans `{kind, offset, length}` sobre o buffer original, e o
LSP já publica diagnósticos em worker com debounce e snapshots imutáveis. Os problemas reais estão
em outro lugar:

| # | Achado | Gravidade | Onde |
|---|--------|-----------|------|
| 1 | Todos os requests interativos (completion, hover, goto, format, codeAction) rodam **na thread de I/O**; `$/cancelRequest` não consegue ser lido enquanto isso | Crítica | `lsp/Server.cpp:51-140` |
| 2 | Data race em `CachedParse`: leitura de `slot->tree` fora do `call_once` | Crítica (correção) | `lsp/Server.cpp:1093` |
| 3 | Zero parsing incremental: cada tecla invalida o cache e refaz lex + preprocess + parse completos | Alta | `lsp/Server.cpp:569`, `core/src/ParseTree.cpp:21` |
| 4 | `ChangeDocument` copia o documento inteiro e reconstrói o `LineIndex` inteiro a cada mudança do lote | Alta | `lsp/Server.cpp:540,497` |
| 5 | `Arena` existe mas só é usada em testes/bench; AST e consumidores usam `std::vector` e `std::string` | Média | `core/include/Heimdall/Arena.hpp` |
| 6 | Consumidores (Completion/Navigation) reconstroem estruturas AoS com `std::string` por nó | Média | `core/src/Completion.cpp:775,891,2028` |
| 7 | `ParseTree::Children()` é O(tamanho da subárvore) e aloca; uso em loops vira O(n²) | Média | `core/src/ParseTree.cpp:67` |
| 8 | O parser compara texto de token (`Is(i, "(")`) em vez de um enum de token | Média | `core/src/GrammarParser.cpp:153` |
| 9 | Sem cancelamento cooperativo dentro de parse/lint/format | Média | `lsp/Server.cpp:1373` |
| 10 | Formatter sem formatação incremental; `FormatEdits` formata tudo e faz diff por linha | Baixa/Média | `core/src/Formatter.cpp:3508` |

## 1. Memória da AST

**O que já está certo.** `GrammarNode` (`ParseTree.hpp:~80`) é um registro de 20 bytes com índices
`uint32_t` (`first_token`, `token_count`, `parent`, `subtree_end`) em um `std::vector` contíguo,
em ordem de pré-ordem. Isso é a representação do Zig/Ruff: sem `Node*`, `IsDescendant` é uma
comparação de inteiros (`node < candidate < subtree_end`), e uma travessia linear é prefetch-friendly.
Não há pointer chasing na AST propriamente dita. O parser usa `pmr::monotonic_buffer_resource` em
um buffer de 32 KiB na pilha (`GrammarParser.cpp:~120`) para scratch (`m_sig`, `m_sig_text`,
`m_match`), o que é a decisão correta para dados temporários.

**Problemas.**

1. **A `Arena` pública é código morto no caminho de produção.** `grep` mostra `Arena` apenas em
   `core/src/Arena.cpp` e nos benchmarks. A árvore devolvida a quem chama usa três `std::vector`
   (tokens, nós, diagnósticos) com crescimento geométrico: cada `push_back` que realoca copia o
   vetor, e o pico de memória é ~1,5-2x o tamanho final. Em um TU com 300 mil tokens (uma TU com
   `<iostream>` já passa disso se os headers forem parseados, como o `IncludeIndex` faz), isso são
   alguns MB por parse, mas o custo dominante é a taxa de realocação, não a localidade.
2. **`GrammarDiagnostic` e `ParseDiagnostic` carregam `std::string message`.** Mensagens são
   literais ou concatenações de poucos formatos fixos (`"expected ';' before 'x'"`). Em código sendo
   digitado, o número de diagnósticos é alto e cada um aloca. Troque por `{offset, DiagCode, arg_token}`
   e formate a mensagem só ao publicar.
3. **O heap é fragmentado pelos consumidores, não pela AST.** `BuildScopePaths`
   (`Completion.cpp:891`) cria `vector<vector<string>>` com uma entrada por nó da árvore (a maioria
   vazia, mas cada `vector` vazio ainda ocupa 24 bytes: ~24 B x N nós) e `IndexScopes` usa
   `unordered_map<std::string, size_t>` (`Completion.cpp:2028`). É aqui que o "overhead do alocador"
   aparece de fato.
4. **`SyntaxTree::m_token_parents` é `vector<size_t>`** (8 B por token) onde `uint32_t` basta
   (`SyntaxTree.hpp:~75`). `LineTable::m_line_starts` também é `size_t`. Metade da memória, sem
   custo de API.

**Recomendação (C++23).** Não mexa no layout dos nós; ele já é bom. Faça três coisas:

```cpp
// Uma arena por versão de documento, dona de tokens, nós e diagnósticos.
// O snapshot do LSP passa a segurar a arena (shared_ptr) em vez de N vectors.
struct ParseArena {
    std::pmr::monotonic_buffer_resource mem{1 << 20};   // bloco inicial 1 MiB
    std::pmr::vector<Token>        tokens{&mem};
    std::pmr::vector<GrammarNode>  nodes{&mem};
    std::pmr::vector<Diag>         diags{&mem};         // Diag é POD, sem string
};
// Pré-dimensione: tokens.reserve(source.size() / 4 + 16); nodes.reserve(tokens.size() / 3);
// (razão de ~4 bytes/token e ~3 tokens/nó pode ser calibrada com ParserBench).
```

- Estimar `reserve` a partir do tamanho do arquivo elimina as realocações do `push_back`.
- Substitua `vector<vector<string>>` por um array plano `{uint32 parent_scope, uint32 name_id}` por
  nó de escopo (árvore de escopos por índices) e reconstrua o caminho qualificado só sob demanda.
- Impacto estimado: parse 15-30% mais rápido e pico de RAM por documento ~30-40% menor; consumidores
  de completion deixam de alocar proporcionalmente ao tamanho da árvore.

## 2. Strings e tokens

**O que já está certo.** `Token` tem 12 bytes e referencia o buffer por `offset/length`; `Text()` é
`string_view` sem cópia. O buffer pertence a um `shared_ptr<const std::string>` e a árvore o segura
via `HoldSource`, então trocar o texto do documento não invalida árvores em uso. A CLI usa mmap
(`MappedBuffer`). O `Preprocessor` já usa hash transparente (`TransparentStringHash`) e só constrói
`active_source` sob demanda.

**Problemas.**

1. **Não há interning nem enum de keywords/pontuação.** O lexer só distingue `Identifier` de
   `Punctuation`. O parser então decide por comparação de texto: `Is(i, "(")`, `Is(i, "template")`...
   há 82 comparações literais `== "..."` em `GrammarParser.cpp`, e `m_sig_text` guarda um
   `string_view` de 16 B por token significativo. Cada `Is()` com texto de mais de 1 caractere faz
   `memcmp`. Isso é o custo dominante do parser, depois da alocação.
2. **`MacroMap = unordered_map<string, string>`** (`Preprocessor.hpp`) copia nome e corpo de cada
   `#define` e `ExpandObjectMacros` devolve `std::string` por linha. Para headers com milhares de
   macros isso pesa; para o arquivo do usuário não.
3. **Completion/Navigation devolvem `std::string` por símbolo** (`SliceRange`, `FunctionSignature`,
   `CompactWs`), o que é aceitável para o resultado final, mas hoje também é usado em estruturas
   intermediárias.

**Recomendação.**

```cpp
enum class Tok : std::uint8_t {            // classificado UMA vez no lexer
    Ident, Number, String, Char, RawString, LineComment, BlockComment, Whitespace, Unknown,
    LParen, RParen, LBracket, RBracket, LBrace, RBrace, Semi, Comma, Colon, ColonColon, Lt, Gt, /*...*/
    KwTemplate, KwNamespace, KwClass, KwStruct, KwConst, /* ... ~90 keywords */
};
struct Token { Tok kind; std::uint32_t offset, length; };   // continua com 12 B
```

- Classifique keywords no lexer com um *perfect hash* gerado em tempo de compilação
  (`consteval` + `std::array`); o parser passa a fazer `kind == Tok::LParen`, uma comparação de um byte.
- Para identificadores, use um `StringPool` por workspace (não por arquivo): `uint32_t SymbolId`
  guardado em um campo extra ou em um vetor paralelo `symbol[token]`. Os ganhos reais estão em
  Navigation/Completion (comparar nome de declaração com nome de uso), onde hoje se compara texto.
- Impacto estimado: parser 20-40% mais rápido (menos `memcmp`, menos tráfego de cache por token);
  `m_sig_text` deixa de existir (-16 B/token).

## 3. Concorrência e modelo do LSP

**O que já está certo.** Existe snapshot imutável por versão (`DocumentSnapshot`, `Server.hpp:~40`),
worker de diagnósticos com *coalescing* e debounce de 50 ms (`Server.cpp:1373-1460`), worker de
indexação de headers com cache LRU compartilhado (`kMaxGlobalIndices = 16`), cache de parse por
versão (`CachedParse`) e a CLI paraleliza arquivos com `atomic` + `jthread` (`Pipeline.cpp:310`).
Esse desenho é razoável.

**Problemas.**

1. **A thread de I/O executa o trabalho pesado dos requests.** O laço em `Server.cpp:51-140`
   chama `CompleteDocument`, `HoverDocument`, `GotoDocument`, `FormatDocument` e `CodeActions`
   diretamente. Cada um pode disparar `CachedParse` (parse completo se o cache foi invalidado pela
   última tecla, e sempre é, ver §4), `HeaderScopes` (que, no *slow path* de `Server.cpp:~975`, faz
   `ResolveHeaders` com stat + leitura + lex dos headers na própria thread de I/O) e, no caso de
   `Goto`, parse de outros arquivos (`Server.cpp:1810,1831`). Enquanto isso, nada é lido do stdin.
   Consequência direta: **`$/cancelRequest` só é processado depois que o request que ele cancela
   terminou**. `WasCancelled()` (`Server.cpp:1107`) é verificado apenas após o parse e antes de
   responder, ou seja, só evita serializar a resposta. O orçamento de 50 ms é violado sempre que
   uma digitação cai entre `didChange` e `completion`.
2. **Data race em `CachedParse`** (`Server.cpp:1093`):

   ```cpp
   if (!slot->tree) {                       // leitura SEM sincronização
       std::call_once(slot->once, [&]{ ... slot->tree = std::move(tree); });  // escrita em outra thread
   }
   return slot->tree;
   ```

   Se a thread de diagnóstico estiver dentro do `call_once` escrevendo `slot->tree` enquanto a
   thread de I/O avalia `!slot->tree`, há data race em um `shared_ptr` (UB; na prática pode
   observar ponteiro sem o bloco de controle atualizado). A correção é remover o `if` e chamar
   sempre `std::call_once` (o caminho rápido do `call_once` já é uma carga *acquire*), ou guardar a
   árvore em `std::atomic<std::shared_ptr<const ParseTree>>` (C++20).
3. **Um único worker de diagnósticos e um mutex global `m_mu`.** `m_mu` protege documentos, parse
   cache, include cache, índices globais e macro cache ao mesmo tempo. Cada request pega `m_mu`
   várias vezes (de 3 a 5 aquisições por request, ver as dezenas de `lock_guard` em `Server.cpp`).
   Com um só worker de diagnóstico não há contenção grave hoje, mas o mutex é o gargalo assim que
   houver pool. Além disso, `HeaderScopes` mistura "decidir" com "enfileirar I/O" sob `m_mu`.
4. **Sem cancelamento cooperativo.** `ParseTree::Parse`, `RuleEngine::Analyze` e
   `CompletionEngine::*` não recebem `std::stop_token`. Um parse de um arquivo grande não pode ser
   abandonado quando a versão fica obsoleta; só é descartado depois (`IsCurrentVersion`).
5. **`m_system_threads` cria `std::jthread` que roda o compilador** (`SystemIncludes("c++")`,
   `Server.cpp:35,209`) sem limite; barato, mas deve ir para o mesmo pool.

**Recomendação.**

```cpp
// Modelo alvo: I/O thread só faz framing + roteamento + snapshot. Todo o resto vai ao pool.
struct Request { int64_t id; std::string uri; int64_t version; std::stop_source cancel; };

void LanguageServer::Run() {
    while (ReadMessage(body)) {
        auto msg = Parse(body);
        if (msg.method == "$/cancelRequest")  inflight_.at(msg.cancel_id).cancel.request_stop(); // imediato
        else if (IsNotification(msg))         ApplyToStore(msg);                  // O(edit), sob lock curto
        else                                  pool_.submit([=, tok = req.cancel.get_token()] {
                                                   Handle(msg, store_.snapshot(uri), tok);
                                              });
    }
}
```

- `DocumentStore` com `std::atomic<std::shared_ptr<const DocumentSnapshot>>` por URI: a thread de
  I/O faz *publish* (uma troca atômica); workers fazem `load()` sem mutex. Isso elimina a contenção
  de `m_mu` no caminho quente sem estruturas lock-free customizadas (que não valem a complexidade
  aqui; `atomic<shared_ptr>` basta e é correto).
- Pool de N workers (`hardware_concurrency() - 1`), com requests interativos em fila de prioridade
  sobre diagnósticos e indexação.
- Propague `std::stop_token` até o laço do `GrammarParser` (checar a cada `ParseScope` de nível
  superior, ou a cada ~4096 tokens) e até o laço de regras do `RuleEngine`.
- Impacto estimado: p99 de completion/hover deixa de incluir o tempo de parse do arquivo e I/O de
  header; cancelamentos passam a ter efeito. É a melhoria de maior impacto percebido pelo usuário.

**Status da correção (§3.1-3.5):** implementado em `lsp/Server.*`, `lsp/ThreadPool.hpp` e
`ParseTree::Parse(..., std::stop_token)`. Requests interativos rodam em um pool com snapshot
fixado na chegada; `$/cancelRequest` age na hora e responde `-32800`; `CachedParse` passa sempre
por `call_once`; `m_documents` tem `shared_mutex` próprio; um `didChange` cancela o passe de
diagnóstico em andamento; o probe de includes do compilador usa o pool em prioridade baixa.
Não coberto: `CompletionEngine`/`RuleEngine` não recebem token (cancelamento só entre etapas), e
não há TSan neste ambiente (MinGW), então a ausência de races não foi verificada por ferramenta.

## 4. Parsing incremental e tolerância a falhas

**Tolerância a falhas: já é boa.** O parser emite `ErrorExpression`/`Error` e continua
(`GrammarParser.cpp:2140-2263, 2714, 2778, 2940, 2975, 3190`). A pré-passada de delimitadores
(`GrammarParser.cpp:~55-100`) resolve pares `()[]{}` mesmo com fechamento faltando ou trocado e já
emite "unclosed delimiter". Como `FindSemicolon`/`FindComma`/`SkipGroup` pulam grupos pelo
`m_match`, um parêntese aberto não derruba o resto do arquivo. Há um benchmark específico para isso
(`BM_ParseUnclosedParen`). **Falta** documentar o conjunto de *synchronization tokens* de forma
explícita (hoje a recuperação está espalhada em ~7 pontos) e testá-lo com fuzzing (ver §7).

**Incrementalidade: não existe.** `ChangeDocument` (`Server.cpp:501-572`) faz:

1. `std::string current(*base_text)`: cópia O(N) do documento inteiro por `didChange`;
2. `index.Build(current)` **dentro do laço por mudança** (`Server.cpp:497`, chamado de
   `ApplyContentChange`), O(N) por edição. O comentário "F3" no código diz que o índice foi
   reaproveitado entre mudanças, mas ele ainda é reconstruído por inteiro a cada mudança aplicada.
   Um `didChange` com k edições custa O(k·N);
3. `m_parse_cache.erase(uri)`: descarta toda a árvore.

O próximo request refaz lex, preprocess e grammar do arquivo inteiro. Para um arquivo de 5 mil
linhas (~200 KB), a ordem de grandeza é de dezenas de milissegundos por tecla, sem contar headers.

**Recomendação, em ordem de custo/benefício:**

1. **Barato (dias): edição do texto sem O(N).** Use um *piece table* ou *rope* simples, ou mantenha
   o `std::string` com `replace` in-place sob a posse exclusiva da thread de I/O e só publique cópia
   quando um worker pedir snapshot (copy-on-read com contador de versão). Atualize `LineIndex`
   incrementalmente: só as linhas a partir de `start.line` mudam, e o deslocamento do restante é
   uma soma constante (`line_starts[i] += delta` em um laço vetorizável sobre `uint32_t`).
2. **Médio: re-lex incremental com reaproveitamento de tokens.** Como o lexer é *lossless* e sem
   estado entre linhas (exceto comentários de bloco e raw strings), dá para re-lexar só a janela
   `[linha_inicial - 1, até a primeira linha após o edit cujo estado de lexer coincida]` e
   deslocar os offsets dos tokens seguintes. Um vetor `uint32_t offset` se desloca com SIMD.
3. **Alto, mas é o ganho de verdade: reuso de subárvores no nível de declaração.** O C++ não tem
   gramática livre de contexto, então não tente um parser incremental geral (tree-sitter/LR). Faça
   o que o clangd e o rust-analyzer fazem na prática: **reparse por "item" de nível superior**.
   - Divida o TU em itens por chaves de sincronização: `}` no nível 0, `;` no nível 0 e diretivas.
   - Guarde, por item, `{byte_range, hash(texto), first_node, node_count}`.
   - Após uma edição, só os itens cujo `byte_range` intersecta o edit precisam de reparse; os
     demais copiam seus nós com offset deslocado (`first_token += Δtokens`, `parent` relativo).
   - Como os nós já são índices relativos em pré-ordem, **a representação atual já é amigável a
     isso**: um item é uma fatia contígua de `m_nodes`, que pode ser copiada com `memcpy` + um
     ajuste de índice.
   Isso é o equivalente pragmático da árvore verde/vermelha do Roslyn para este projeto: a "verde"
   é a fatia imutável de nós por item (guardada por `shared_ptr`), a "vermelha" é o offset absoluto
   somado na hora da consulta.
4. **Não faça (ainda): queries tipo `salsa`.** O ganho existe, mas o grafo de dependências de C++
   (headers, macros, templates) é grande. Um passo intermediário é suficiente: memoizar por
   `(uri, versão, hash_dos_itens)` as saídas caras e puras (`IndexScopes`, regras por item).

Impacto estimado: com (1)+(2), o custo por tecla deixa de crescer linearmente com o arquivo no
caminho de edição; com (3), reparse por tecla cai para o tamanho do item editado (tipicamente
<1 ms para uma função).

## 5. Data-Oriented Design (AoS → SoA)

**Já feito.** Tokens e nós são arrays contíguos de structs pequenas e *trivially copyable*; subárvore
por intervalo `[i, subtree_end)` permite varrer um escopo linearmente.

**Onde SoA realmente compensa (e onde não).**

| Estrutura | Hoje | Proposta | Motivo |
|-----------|------|----------|--------|
| `Token` (12 B) | AoS `{kind, offset, length}` | SoA: `kinds[]` (1 B), `offsets[]` (4 B), `lengths[]` (4 B) | A maioria dos passes (RuleEngine, suppressions, `m_sig`) só lê `kind`; 12 B → 1 B por token lido reduz o tráfego de cache 12x e permite filtrar com SIMD (`kind != Whitespace/Comment`) |
| `m_sig` + `m_sig_text` | `vector<u32>` + `vector<string_view>` (16 B/token) | Só `vector<u32>`; texto vem de `offsets/lengths` | Remove 16 B/token e uma passada de preenchimento |
| `GrammarNode` (20 B) | AoS | Manter AoS | Os consumidores quase sempre leem `kind` + `first_token` + `subtree_end` juntos; SoA só ajuda em varreduras por `kind` (ex.: `BuildCallableIntervals`), e aí um vetor auxiliar de `kind` (1 B) basta |
| `vector<vector<string>>` de escopos | N vetores | Array plano indexado por nó | Ver §1 |
| `Diagnostic` do RuleEngine | 2 `std::string` + `TextEdit` com `std::string` por diagnóstico | `{RuleId, offset, length}` + mensagem sob demanda | Alocação por diagnóstico; `trailing-whitespace` pode gerar milhares |

Prioridade: faça o SoA de **tokens** (maior ganho por menor custo, os passes de linter e o
`m_sig` são varreduras por `kind`) e deixe os nós como estão. Meça com `LexerBench`/`ParserBench`
antes de decidir, pois o ganho depende de o parser ler `offset/length` com a mesma frequência que lê
`kind`.

## 6. Outros achados

- **`ParseTree::Children()`** (`ParseTree.cpp:67`) varre toda a subárvore e filtra por `parent`,
  alocando um vetor. Para um nó com `s` descendentes e `c` filhos custa O(s). Usado em
  `Navigation.cpp:1020,1046` e `Completion.cpp:1723`; chamado em laço sobre filhos vira O(s²).
  Adicione `first_child`/`next_sibling` (2 x `uint32_t`, +8 B por nó) ou um iterador que salta por
  `subtree_end` (`i = nodes[i].subtree_end` dá o próximo irmão em O(1), sem campo novo):

  ```cpp
  // Filhos em O(#filhos), sem alocar: a ordem é pré-ordem, então o próximo irmão
  // de i começa em nodes[i].subtree_end.
  template <typename F> void ForEachChild(std::size_t n, F&& f) const {
      for (std::size_t i = n + 1; i < m_nodes[n].subtree_end; i = m_nodes[i].subtree_end)
          f(i);
  }
  ```

- **Parse duplicado.** `CompletionEngine::IndexScopes(source, options)` e `Complete(source, ...)`
  (`Completion.cpp:2018, 2800`) chamam `ParseTree::Parse` internamente. O LSP usa as sobrecargas que
  recebem `ParseTree` (bom), mas o caminho por `source` continua exposto e a CLI/testes podem
  duplicar trabalho. Marque as sobrecargas por `source` como `[[deprecated]]` fora de testes.
- **`FormatEdits`** (`Formatter.cpp:3508`) formata o arquivo inteiro, depois alinha linhas com
  hash + janela de 16 linhas. Funciona, mas formatar em `formatOnType`/`rangeFormatting` custa O(N).
  A solução estrutural é formatar por item de nível superior, usando as mesmas fronteiras do §4.
- **Documentação defasada.** `docs/linter-formatter-architecture.md` §3 diz que cada diagnóstico e
  cada completion refazem o parse; isso não é mais verdade desde `CachedParse`. Atualizar para não
  induzir quem lê o doc a refazer trabalho já feito.
- **Árvore verde/vermelha.** `SyntaxTree` usa o nome `GreenNode` mas não é uma árvore verde: tem
  `parent` e offsets absolutos de token, ou seja, não é reutilizável por deslocamento. O nome
  promete uma propriedade que o tipo não tem; renomeie ou implemente a propriedade (§4, item 3).

## 7. Roteiro priorizado e como validar

| Prioridade | Mudança | Esforço | Ganho |
|-----------|---------|---------|-------|
| P0 | Corrigir a data race de `CachedParse` (§3.2) | horas | correção |
| P0 | Mover requests interativos para pool; `$/cancelRequest` imediato (§3.1) | 3-5 dias | latência p99 |
| P1 | `DocumentStore` com `atomic<shared_ptr>`; remover `m_mu` do caminho quente (§3.3) | 2 dias | escalabilidade |
| P1 | Edição de texto/`LineIndex` sem O(N) por mudança (§4.1) | 1-2 dias | custo por tecla |
| P1 | `std::stop_token` no parser e no RuleEngine (§3.4) | 2 dias | cancelamento real |
| P2 | `Tok` enum + keywords perfeitos no lexer; remover `m_sig_text` (§2) | 3-4 dias | parser 20-40% |
| P2 | `reserve` por estimativa + arena por versão + diagnósticos POD (§1) | 2-3 dias | RAM e parse |
| P2 | Iterador de filhos O(#filhos) (§6) | 0,5 dia | remove O(n²) |
| P3 | Reparse por item de nível superior (§4.3) | 2-3 semanas | custo por tecla O(item) |
| P3 | SoA de tokens (§5) | 1 semana | cache/SIMD |

**Validação.** Os benchmarks existentes (`bench/src/*Bench.cpp`) cobrem lexer, parser, preprocessor,
formatter, completion e rule engine, mas não medem o que importa para o orçamento de 50 ms. Falta:

1. Um benchmark **ponta a ponta de latência** do LSP: `didChange` (1 caractere) seguido de
   `completion`, em arquivos de 1k, 5k e 20k linhas, reportando p50/p95/p99. É o único número que
   decide se o orçamento de 50 ms é cumprido.
2. Um benchmark de pico de memória por documento (`Arena::Used()` ou `GetProcessMemoryInfo`).
3. Fuzzing do parser com truncamento em todo offset de um arquivo válido (digitação incompleta): o
   parser nunca deve travar, nunca deve levar mais de `k x` o tempo do arquivo completo, e a
   contagem de nós `Error` deve ser limitada. Isso valida a robustez de §4 de forma mensurável.
4. ThreadSanitizer sobre `LspProtocolSmoke.py` (rodar o servidor com `-fsanitize=thread`); o
   data race do §3.2 deveria aparecer imediatamente.

## 8. O que **não** recomendo

- Reescrever a AST com ponteiros e arena: a AST atual (índices em vetor) já é superior a isso.
- Estruturas lock-free customizadas: `atomic<shared_ptr>` + um pool simples resolvem o problema
  com muito menos risco.
- Parser incremental genérico (LR/GLR): a gramática de C++ depende de contexto; o reparse por item
  entrega 90% do ganho com uma fração da complexidade.
- Migrar para queries estilo `salsa` agora: antes é preciso fechar §3 e §4.
