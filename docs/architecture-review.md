# Revisão de arquitetura: performance, memória, concorrência e AST

Escopo: `core/` (Lexer, Preprocessor, ParseTree/GrammarParser, Formatter, RuleEngine,
Completion), `lsp/` (Server) e `semantic/`. Alvo: respostas do LSP abaixo de 50 ms
e baixo consumo de RAM em C++23.

Base de código lida no commit `b46242e`. Os números de impacto são **estimativas
por análise estática**; nenhuma foi medida. Antes de implementar cada item, meça com
os benchmarks existentes em `bench/`. Eles já cobrem lexer, parser, formatter, arena
e rule engine, mas o corpus é pequeno (`bench/corpus/*`, menos de 100 linhas cada).
Falta um corpus realista com `<vector>`, `<ranges>` e arquivos de 10 mil linhas.

## 0. Resumo executivo

Este código **já aplicou** boa parte do receituário moderno:

| Prática | Estado |
|---|---|
| Nós como array plano com índices `u32` (sem ponteiros) | Feito: `GrammarNode` tem 20 B, `Token` tem 12 B |
| Zero-copy: tokens guardam `offset/length` sobre um buffer imutável | Feito (`Lexer.hpp`, `string_view` no parser) |
| Snapshot imutável por versão (`shared_ptr<const string>`) | Feito (`Server.hpp:42`) |
| Parse compartilhado por versão, com `call_once` | Feito (`Server.cpp:809`) |
| Debounce, coalescência e descarte de versões obsoletas | Feito (`Server.cpp:990`) |
| Índice de linhas com busca binária | Feito (`LineIndex`) |
| `mmap` para o CLI | Feito (`MappedBuffer`) |

As lacunas reais, em ordem de impacto:

1. **O I/O thread executa completion, hover, formatting e codeAction, inclusive o
   parse completo.** Isso viola diretamente o requisito de 50 ms (§3.1).
2. **Não existe parsing incremental.** Toda tecla causa lex, preprocess e parse do
   arquivo inteiro, e o cache é invalidado em cada `didChange` (§4.1).
3. **`Arena` existe, tem benchmark, mas não é usada em lugar nenhum da produção.** O
   parser usa `std::vector` e `std::string` no heap, e os resultados de
   completion/hover usam `vector<string>` por nó (§1).
4. **O parser compara texto de token** (`Is(i, "(")`) em vez de IDs de token. Isso
   custa muito em ciclos e em memória (`m_sig_text` guarda 16 B por token) (§2, §5).
5. **Um único worker de diagnósticos** serializa os arquivos. `ChangeDocument` copia
   e re-indexa o buffer inteiro por mudança, em O(N) na thread de I/O (§3.2, §3.3).
6. **`SyntaxTree` é código morto em produção.** É usada só em testes e benchmarks,
   e duplica `ParseTree` (§6).

## 1. Gerenciamento de memória da AST

### O que está bom

`GrammarNode` (`ParseTree.hpp:76`) é POD de 20 bytes, guardado em
`std::vector<GrammarNode>` contíguo. Pai e filhos são **índices**, não ponteiros, e
`subtree_end` dá um layout em pré-ordem. `IsDescendant` é O(1). Não há `unique_ptr`
por nó, então o "pointer chasing" clássico **não existe** na AST. O parser usa
`std::pmr::monotonic_buffer_resource` com 32 KiB na pilha para os vetores
temporários (`GrammarParser.cpp:103`).

### Problemas

**1.1. `Arena` não está conectada ao pipeline.** `grep` confirma que ela só aparece
em `Arena.*` e `bench/ArenaBench.cpp`. A AST usa `std::vector` com crescimento
geométrico: `m_nodes.push_back` realoca e copia, e `m_tokens` e `m_nodes` ficam com
capacidade ociosa durante todo o ciclo de vida da árvore (que vive no cache por URI).

- **Impacto:** moderado. Os vetores já são contíguos e o crescimento é amortizado.
  O ganho real é de pico de RAM (até 2x transitório na realocação) e de
  `shrink_to_fit` implícito, mais fragmentação do heap no cliente de longa duração.
- **Recomendação:** `reserve` no mínimo, já que o número de nós é previsível
  (≈ `tokens/3`). Isso é barato e vale antes de qualquer arena.

**1.2. Os resultados de consulta alocam strings e vetores por nó.**
`Completion.cpp:700-735` (`BuildScopePaths`) cria `vector<vector<string>>` do
tamanho de `Nodes()`. `IndexScopes` (`:1440+`) constrói uma `std::string` chave por
caminho (`path_key`). `IncludeIndex` guarda `std::unordered_set<std::string>`.
Com headers da libstdc++ isso resulta em dezenas de milhares de strings pequenas e
alocações individuais, e é onde se concentra a memória do servidor.

- **Impacto:** alto em RAM e em tempo de construção do índice (feito em background,
  então o custo é de memória, não de latência direta).
- **Recomendação:** o índice deve guardar `StringId` (§2) e um `std::pmr::vector`
  sobre uma arena por índice. O descarte pela LRU (`kMaxGlobalIndices = 16`) vira
  O(1), pois basta liberar a arena.

### Solução em C++23

```cpp
// Arena por versão de árvore: nodes, tokens e diagnósticos.
struct TreeStorage {
    std::pmr::monotonic_buffer_resource arena{ 256 * 1024 };
    std::pmr::vector<Token>        tokens{ &arena };
    std::pmr::vector<GrammarNode>  nodes{ &arena };
    std::pmr::vector<GrammarDiagnostic> diags{ &arena };
};
// ParseTree passa a ser { shared_ptr<const std::string> src; TreeStorage st; }
// e é destruída em O(1) quando a última referência (worker de diagnósticos,
// handler de hover, cache) libera o snapshot.
```

Destaque: o `monotonic_buffer_resource` de `Arena.cpp` precisa ser **não
thread-safe e por árvore**, o que já é o contrato documentado em `Arena.hpp`.

## 2. Strings e tokens

### O que está bom

Tokens têm `{kind, offset, length}` sobre o buffer fonte. O `Formatter` e o
`RuleEngine` trabalham com `string_view`. O preprocessor usa lookup heterogêneo
(`TransparentStringHash`), sem `std::string` temporário por identificador. Isso é
exatamente o desenho recomendado.

### Problemas

**2.1. Não há interning.** Todo identificador é comparado por `string_view ==`, e
os mapas usam `std::string` como chave: `MacroMap`
(`Preprocessor.hpp:78`), `unordered_set<string>` em `SemanticAnalyzer.hpp:38`
(`CollectTypeNames`), `ScopeIndex` em `Completion`. Cada ocorrência de `std::` cai
em hash de string e comparação byte a byte.

**2.2. O parser compara palavras-chave e pontuação como texto.** `Is(i, "(")`
(`GrammarParser.cpp:136`) é chamado centenas de vezes por declaração, com o literal
passando por `string_view` e comparando tamanho e bytes. O `TokenKind` só distingue
`Punctuation` de `Identifier`, sem distinguir `(` de `{` nem `class` de um nome.
`GrammarParser.cpp:41-45` ainda monta `m_sig_text`, um vetor de `string_view` de 16 B
por token significativo, só para suportar isso.

**2.3. `GrammarDiagnostic` e `PreprocessorDiagnostic` carregam `std::string`.** São
poucos, então o custo é baixo, mas `"expected ';' before '" + std::string(Text(...))`
(`:1273`, `:1795`, `:2070`) aloca em caminho de erro, que é o caso comum durante
digitação. Prefira um enum de mensagem mais um `{offset, length}` e formate só ao
serializar.

### Solução

```cpp
enum class Tok : std::uint16_t {          // gerado por X-macro
    LParen, RParen, LBrace, RBrace, Semi, Comma, Arrow, ColonColon, Lt, Gt, ...
    KwClass, KwStruct, KwNamespace, KwTemplate, KwConst, ...   // ~90 keywords
    Ident, Number, String, ...
};
struct Token { Tok kind; std::uint32_t offset; std::uint32_t length; }; // 12 B (hoje também)

class StringPool {                          // 1 por servidor ou por arena de índice
    std::pmr::unordered_map<std::string_view, StringId> m_map;
    std::pmr::vector<std::string_view>      m_views;   // views apontam para a própria arena
public:
    StringId Intern(std::string_view s);
    std::string_view View(StringId id) const noexcept { return m_views[id.v]; }
};
```

- Classificar keyword e pontuação **no lexer** (um `switch` no primeiro char mais um
  `perfect hash` para identificadores curtos, via `consteval` em C++23). Depois disso
  `Is(i, Tok::LParen)` é uma comparação de inteiros, e `m_sig_text` pode ser
  removido.
- **Impacto estimado:** 15 a 30% do tempo de parse e −16 B/token de memória
  transitória. O ganho exato depende do perfil: meça com `ParserBench` primeiro.
- Internar só identificadores que entram em índices persistentes (escopos,
  macros, símbolos). Tokens da árvore continuam como `offset/length`.

## 3. Concorrência e modelo do LSP

### O que está bom

- Há `std::jthread` com `stop_token` para indexação e diagnósticos, e `Send` com
  mutex (`JsonRpc.cpp:34`).
- `DiagWorkerMain` faz coalescência por URI e debounce de 50 ms, e `PublishDiagnostics`
  verifica `IsCurrentVersion` entre as fases (`Server.cpp:184-191`).
- `HeaderScopes` nunca bloqueia: devolve o índice antigo e `isIncomplete=true`
  enquanto o worker reconstrói. É o desenho correto.

### Problemas

**3.1. [Crítico] Handlers de requisição rodam na thread de leitura.**
`Server.cpp:88-103` chama `FormatDocument`, `CodeActions`, `CompleteDocument` e
`HoverDocument` **diretamente no loop de `ReadMessage`**. Cada um chama `CachedParse`
(`:436`, `:481`, `:911`) e, se o worker de diagnósticos ainda não terminou, a própria
thread de I/O faz o parse completo. Enquanto isso:

- nenhum `didChange` posterior é lido, então o `$/cancelRequest` só é visto
  **depois** da resposta (`:104-117` só marca o ID num set, e `WasCancelled` só é
  consultado dentro do handler que já está bloqueando a leitura);
- o cancelamento é, na prática, inoperante para hover e completion.

**Impacto:** alto, é o maior risco para os 50 ms. Num arquivo de 5 mil linhas com
headers, um parse frio na thread de I/O congela o editor.

**Solução:** thread de I/O só decodifica e despacha. Requisições vão para um pool
(`std::jthread` com fila) e carregam um `std::stop_source` por ID de requisição:

```cpp
struct Pending { std::stop_source stop; };
std::unordered_map<RequestId, Pending> m_inflight;   // protegido; só acessado no I/O thread

// I/O thread:
if (method == "$/cancelRequest") { if (auto it = m_inflight.find(id); it != end) it->second.stop.request_stop(); }
else { auto st = m_inflight[id].stop.get_token();
       m_pool.submit([=]{ Handle(req, st); }); }
```

Leitura de mensagens e escrita de respostas ficam desacopladas, e a escrita já é
serializada por `Send`.

**3.2. Um único worker de diagnósticos.** Com vários arquivos abertos e um
`didOpen` em massa na abertura do workspace, os diagnósticos viram fila. Use um
pool de N workers, mantendo a coalescência por URI (no máximo uma tarefa viva por
documento).

**3.3. `ChangeDocument` é O(N) por evento, na thread de I/O.**
`Server.cpp:364` copia o texto inteiro (`std::string current(*base_text)`) e
`ApplyContentChange` chama `index.Build(current)` **após cada mudança do lote**
(`:337`). O comentário "F3" diz que isso foi corrigido, mas o código ainda
reconstrói o índice de linhas por mudança. Num arquivo de 1 MB com vários
`contentChanges` (colar, multi-cursor) isso são vários MB copiados e varridos
em série, bloqueando a leitura.

- **Solução curta:** reconstruir o `LineIndex` uma vez no fim do lote, atualizando
  os offsets por deslocamento onde necessário.
- **Solução estrutural:** buffer de texto como *piece table* ou *rope*
  (ex.: `immer::flex_vector` ou rope próprio). Cada edição vira O(log N) e o
  snapshot imutável sai de graça por compartilhamento estrutural. Para este
  projeto, uma piece table com *flatten* sob demanda para o lexer é suficiente.

**3.4. Contenção em `m_mu`.** Um único mutex protege documentos, caches de include,
caches de parse, macros, índice global e set de cancelamento. Todos os handlers o
pegam várias vezes (`:359`, `:377`, `:420`, `:433`, `:465`, `:478`...), e as
seções críticas incluem chamadas a `CompileDatabase::Find` e `PathFromUri`. Hoje a
contenção é baixa porque só há duas threads. Ao mover handlers para um pool (3.1),
ela sobe.

- **Solução:** separar por responsabilidade. `m_documents` e `m_parse_cache` mudam
  para um mapa por documento (`shared_ptr<DocState>` com `std::atomic<shared_ptr>`
  para o snapshot, C++20/23). Leitores fazem `load()` sem lock, e só o I/O thread
  escreve. O compile database e o cache de macros são imutáveis após `initialize`
  e dispensam lock.
- Estruturas totalmente *lock-free* (filas MPMC) **não** são necessárias: o volume é
  de dezenas de eventos por segundo. O ganho está em eliminar lock no caminho de
  leitura, não em algoritmos lock-free.

**3.5. Sem paralelismo dentro de um arquivo.** Lex, preprocess e parse são
sequenciais. Isso é aceitável, pois o paralelismo útil é entre arquivos (o CLI em
`Pipeline.cpp:220` já usa `std::jthread` com contador atômico). Só considere
paralelizar o lexer por chunks se o perfil mostrar o lexer dominante em arquivos
gigantes, o que é improvável.

## 4. Parsing incremental e tolerância a falhas

### 4.1. Sem parsing incremental

`CachedParse` é chaveado por `(uri, version, ponteiro do texto)`, e `ChangeDocument`
apaga a entrada (`m_parse_cache.erase`, `Server.cpp:384`). Toda tecla repete:
`Lexer::Lex` (O(N)), `Preprocessor::Process` (O(N), com `unordered_map` e
`std::string` por linha de macro), a construção de `m_sig`/`m_match` e o parse.
O trabalho cresce linearmente com o arquivo, em vez de com o tamanho da edição.

O debounce de 50 ms esconde isso para diagnósticos, mas **não** para completion,
hover e formatting, que precisam do parse da versão atual (§3.1).

**Estratégia recomendada, em três degraus (do mais barato ao mais caro):**

1. **Relex incremental.** A lista de tokens é de offsets absolutos. Dada uma edição
   `[a, b) → texto novo`, encontre o primeiro token afetado por busca binária,
   relexe a partir dali até ressincronizar (um token cujo fim e estado de lexer
   coincidam com o antigo), e some o delta aos offsets restantes. Para a maioria das
   teclas, são centenas de bytes. Observação: `Token` com `offset` absoluto
   obriga a um passe O(T) de deslocamento. Se isso aparecer no perfil, passe para
   *larguras relativas* (como Roslyn).
2. **Reparse por região (top-level item).** O parser já segmenta o arquivo em itens de
   topo com `ParseScope`. Guarde, por item, o hash do trecho de tokens. Ao reparsear,
   itens inalterados são copiados (ou referenciados) e só o item que contém a edição
   é reparseado. O limite natural são as chaves casadas (`m_match`), que já existem.
3. **Green tree imutável com Red facade** (Roslyn). Só vale o custo se houver demanda
   comprovada de reuso estrutural (refactorings, semantic tokens). Para o escopo
   atual (lint, format, completion), o degrau 2 entrega 90% do ganho com uma fração
   da complexidade. Nesta base, o `GrammarNode` com `first_token/token_count`
   relativos ao item já seria um "green node" quase pronto.

**Alternativa pragmática para completion:** a maioria dos pedidos acontece no corpo
de uma função. Parseie apenas o escopo envolvente do cursor (a região entre chaves
casadas) com o contexto de escopos externos vindo da última árvore boa (*stale tree*,
mesmo princípio do `HeaderScopes`).

### 4.2. Recuperação de erros

Há mecanismos reais: `m_match` casa delimitadores de forma tolerante
(`GrammarParser.cpp:60-100`, com "unclosed delimiter" e ressincronização por busca
na pilha), `SkipGroup` pula grupos quebrados, `GrammarKind::Error` e
`ErrorExpression` existem, e o parser emite "expected ';' before ..." e continua. A
suíte `ParseTreeSpec.cpp` e o fixture `parse_syntax_error.cpp` cobrem isso. É um
bom início.

Lacunas:

- **Ressincronização por `;` e `}` é implícita**, espalhada por produções. Falta um
  conjunto de *sync tokens* explícito, passado por contexto (declaração, statement,
  membro de classe, lista de parâmetros), como fazem Clang e rust-analyzer. Sem isso,
  a recuperação é difícil de raciocinar e testar.
- **Rollback por `pop_back` em `m_nodes`** (`:1155-1158`, `:1228-1233`) é um
  *backtracking* ad hoc (o próprio código comenta "Simpler: find first name"). É
  frágil: qualquer produção que também mexa em `m_diagnostics` ou `m_last_expression_node`
  durante a sondagem deixa estado sujo, porque só `m_nodes` é revertido. Uma
  API `Checkpoint{nodes, diags}` com `Rewind()` torna isso correto por construção.
- **Formatter e RuleEngine operam sobre tokens**, não sobre a AST. Isso é uma
  *vantagem* para tolerância a falhas (funcionam em arquivo quebrado), mas deve ser
  registrado como decisão: o formatter nunca depende de uma árvore válida.
  Mantenha esse contrato com um teste de propriedade ("formatar não perde
  bytes não-whitespace em entrada aleatória truncada").
- Convém um teste de *fuzz* (`libFuzzer`) para `ParseTree::Parse` e `Formatter`.
  Truncar o fixture em cada byte é um bom corpus inicial para o caso de digitação.

## 5. Data-oriented design

### Estado atual

- `Token` (12 B) e `GrammarNode` (20 B) são densos. O layout já é AoS compacto, e
  o salto para SoA só vale onde há varredura de um único campo.
- `ParseTree::Children` (`ParseTree.cpp:59`) **continua varrendo** o intervalo
  `[node+1, subtree_end)` testando `parent == node`. O commit `2b8efae` afirma
  O(degree), mas o código atual é O(tamanho da subárvore) e aloca um `vector`. Para a
  raiz, isso é O(N) por chamada. (Verificação: `Children` não é chamada fora de
  testes, então o custo real é nulo hoje; é um risco latente.)
- `Completion.cpp` tem laços `for (n = 0; n < Nodes().size(); ++n)` que filtram por
  `kind` (`:709`, `:735`, `:787`). Cada um lê 20 B por nó para olhar 1 B.

### Recomendações

1. **SoA seletivo para a AST:**

   ```cpp
   struct GrammarNodes {
       std::vector<GrammarKind>    kind;          // 1 B/nó  -> 64 nós por linha de cache
       std::vector<std::uint32_t>  first_token;
       std::vector<std::uint32_t>  token_count;
       std::vector<std::uint32_t>  parent;
       std::vector<std::uint32_t>  subtree_end;
   };
   ```

   Varreduras por tipo (`BuildScopePaths`, `BuildCallableIntervals`, regras do
   linter) passam a ler só `kind[]`: 20x menos tráfego de memória, e o compilador
   consegue vetorizar a comparação (`kind == X` gerando máscara). Para acesso a nó
   individual, o custo é de 5 linhas de cache em vez de 1, mas esse padrão é raro
   nesta base.

2. **Árvore em pré-ordem já permite filhos em O(degree):** guarde `next_sibling`
   (4 B) e use `first_child = i + 1` quando `subtree_end > i + 1`. Isso substitui o
   `Children` atual e dispensa o `vector` de retorno (devolva um range/`generator`
   do C++23).

3. **Índices secundários por tipo:** `std::vector<u32> nodes_of_kind[kCount]`
   construído em um passe. `Completion` consulta direto "todos os
   `NamespaceDefinition`" sem varrer a árvore.

4. **Tokens: separar trivia.** Os tokens de `Whitespace` e comentário ficam no mesmo
   vetor que os significativos (decisão documentada em
   `docs/linter-formatter-architecture.md` §1). O parser então reconstrói `m_sig`
   (vetor de índices) a cada parse. Alternativa: dois vetores, `sig_tokens` (denso, só o
   que o parser vê) e `trivia` (lateral, indexada por token). O parser lê o vetor
   denso diretamente, `m_sig` some, e o formatter reconstrói a ordem por merge por
   offset. O doc já aponta esse caminho de migração.

5. **Lexer:** `kCharClass` já é uma tabela de 256 entradas, bom. O próximo passo é
   SIMD só em dois pontos quentes: pular espaço em branco e achar o fim de comentário
   ou linha (`memchr`/`std::find` já são vetorizados pela libc). Não faça SIMD
   artesanal no lexer inteiro sem perfil; o ganho típico é pequeno diante da
   mudança de legibilidade.

**Impacto estimado:** SoA + índices por tipo reduzem o tempo das passadas de
Completion e das regras do linter em um fator de 3 a 10x nas varreduras por
`kind`. O ganho total do parse é menor, pois a construção domina.

## 6. Dívida arquitetural e código morto

- **`SyntaxTree` não é usada em produção** (só `core/SyntaxTree.*`, testes e
  `bench/SyntaxTreeBench.cpp`). Duplica `ParseTree`: mesmo `GreenNode`/`GrammarNode`,
  mesmo `HoldSource`, mesmo `Children`/`IsDescendant`. Remova, ou promova a fonte
  única do agrupamento de delimitadores e faça o `ParseTree` consumi-la (hoje
  `GrammarParser` reimplementa o casamento em `:60-100`).
- **`Document` struct** (`Document.hpp:19`) parece não ser mais usada depois de
  `DocumentSnapshot`; confirmar e remover.
- **Formatter devolve o documento inteiro**; o LSP gera uma única `TextEdit`
  cobrindo o arquivo (`Server.cpp:443`). Isso é correto, mas envia o arquivo todo a
  cada `formatting`. Faça um diff linha a linha (ou o formatter emitir edits) para
  reduzir o payload e preservar cursor e folds no cliente. Isso também destrava o
  `rangeFormatting`.
- **`Preprocessor::Process` varre o arquivo inteiro linha a linha** e é refeito a cada
  parse. É O(N) com baixa constante, então é candidato de segunda onda; ele se
  beneficia de cache por *bloco de diretivas* quando o relex incremental existir.

## 7. Plano priorizado

| # | Item | Esforço | Ganho | Risco |
|---|---|---|---|---|
| 1 | Mover handlers de requisição para pool + `stop_token` por ID (§3.1) | médio | **muito alto** (latência) | baixo |
| 2 | Corrigir `ApplyContentChange` (índice 1x por lote) e `reserve` de nós (§3.3, §1.1) | baixo | médio | baixo |
| 3 | Corpus de benchmark realista + medir antes/depois (§0) | baixo | habilita o resto | nenhum |
| 4 | Enum `Tok` com keywords/pontuação no lexer; remover `m_sig_text` (§2.2) | médio | alto (parse) | médio |
| 5 | Pool de N workers de diagnósticos com coalescência por URI (§3.2) | baixo | médio | baixo |
| 6 | SoA para `kind[]` e índices por tipo; `Children` O(degree) (§5) | médio | médio-alto (completion/lint) | médio |
| 7 | `Checkpoint/Rewind` no parser e sync-sets explícitos (§4.2) | médio | robustez | baixo |
| 8 | Relex incremental + reparse por item de topo (§4.1) | alto | alto (latência, escala) | alto |
| 9 | `StringPool` + índices em arena (§1.2, §2.1) | alto | alto (RAM) | médio |
| 10 | Remover `SyntaxTree`/`Document` mortos (§6) | baixo | higiene | nenhum |

A ordem pressupõe que **os itens 1 a 3 destravam o requisito de 50 ms** sem tocar na
AST. Os itens 8 e 9 são os mais caros e só se justificam depois de medir que o
parse completo (lexer + preprocess + parse) ainda estoura o orçamento com o item 1
aplicado.

## 8. O que NÃO fazer

- **Não** migrar para Red/Green Trees completas agora: o custo de reescrever
  `Completion` (2 mil linhas) e `GrammarParser` (2,1 mil linhas) é muito maior que o
  ganho enquanto o reparse por item de topo não for tentado.
- **Não** introduzir estruturas lock-free (filas MPMC, hazard pointers): o volume de
  eventos não justifica, e `std::atomic<std::shared_ptr>` resolve o caso de leitura.
- **Não** substituir `simdjson`/`std::string` de resposta por serialização zero-copy
  antes de o perfil mostrar a serialização relevante; hoje ela é pequena frente ao
  parse.
- **Cuidado com `Arena` + `shared_ptr<const ParseTree>`:** a árvore sobrevive ao
  snapshot enquanto qualquer worker a referenciar. A arena deve pertencer à árvore
  (não ao servidor), senão o descarte O(1) vira uso-após-liberação.

## 9. Como validar

1. Rodar `bench/` hoje e guardar a linha de base (o repositório já tem os alvos).
2. Adicionar um benchmark de **latência ponta a ponta**: `didOpen` de um arquivo de
   5 mil linhas, depois N `didChange` de um caractere, medindo p50/p99 do hover e do
   completion até a resposta. `test/LspProtocolSmoke.py` é o ponto de partida.
3. Para §3.1, um teste que envia `completion` seguido imediatamente de
   `$/cancelRequest` e de `didChange`, e verifica que o servidor processa o
   `didChange` **antes** de a resposta cancelada sair.
4. Contadores de alocação (`operator new` instrumentado ou `mimalloc` stats) no
   parse de `bench/corpus/*` para quantificar §1 antes e depois da arena.
