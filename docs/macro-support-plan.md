# Plano: suporte completo a macros, independente de onde são definidas

## 1. Situação atual (verificada no código)

| Área | Hoje | Limite |
|------|------|--------|
| `Preprocessor` (`core/src/Preprocessor.cpp`) | Avalia `#if/#ifdef/#elif`, calcula faixas ativas e `local_macros` | Só macros **object-like** (`MacroMap = nome → texto`); o `#define` com `(` é ignorado; não segue `#include` |
| Macros predefinidas | `-D` do `compile_commands.json`; para headers, `CompilerMacros` (`semantic/src/IncludeIndex.cpp:365`) roda `c++ -dM -E` num `.cpp` temporário a cada sondagem | Depende de compilador instalado; só object-like; sem cache por configuração; sem MSVC |
| Gramática (`GrammarParser`) | Marca como "invisível" o identificador cuja macro expande para decoração (`EXPORT`, `_GLIBCXX_NOEXCEPT`, `m_decoration`) | Macro que expande para código real (`TEST(A, B) { ... }`, X-macros, `DECLARE_FOO(x)`) vira nó de erro |
| Índice (`index/`, `ProjectIndex`) | Nenhum símbolo de macro; símbolos gerados por macro ficam fora | Rename e IWYU recusam ou ignoram macros (`docs/cross-file-index-plan.md`, `rule-engine-roadmap.md`) |
| `IncludeAnalyzer` | Extrai nomes de macros do fecho de includes para `no-unused-include` | Informação não chega ao highlight, hover, definição, completion |
| Highlight (`SyntaxHighlight`) | Só `#define` anterior no mesmo arquivo | Macro vinda de header não é reconhecida |
| LSP | Nada específico de macro | Sem hover/definição/referências/expansão |

Raiz comum: **não existe um modelo de macro nem um ambiente de macros por unidade de tradução (TU)**. Hoje há três fontes ad hoc (`-D`, sonda do compilador, `#define` local) que nunca se juntam.

## 2. Definição de "suporte completo"

Para qualquer identificador num arquivo aberto, o motor sabe:

1. se é macro **naquele ponto** da TU (considerando ordem de `#define/#undef`, `#include`, guardas e condicionais);
2. onde foi definida (arquivo, offset, parâmetros, corpo), inclusive em header de sistema, `-include`, `-D` e builtin do compilador;
3. em que expande (com proveniência: cada token expandido aponta para o token de origem e a invocação);
4. quais declarações e referências ela gera ou contém.

Fora de escopo: avaliar `__has_include` de arquivos inexistentes além do que o resolver acha, módulos C++ (`import`), e expansão de macros de assembly/inline asm.

## 3. Arquitetura proposta

```
              ┌────────────────────────────────────────────────────┐
 compile cmd ─►  MacroEnvironment (por TU / config_hash)            │
 builtins   ──►  persistente, compartilha estrutura entre pontos     │
 -include   ──►  MacroEnvironment::At(offset) → MacroTable snapshot  │
 #include*  ──►                                                      │
              └───────────────┬────────────────────┬───────────────┘
                              │                    │
                    MacroExpander (core)     ShardBuilder / Index
                    hide-set, #, ##, VA_OPT  símbolos Macro + usos
                              │                    │
                    GrammarParser / Binder    LSP: highlight, hover,
                    (stream expandido +       definição, referências,
                     tabela de proveniência)  completion, "expandir macro"
```

Princípios:

- `heimdall_core` continua só-STL, sem exceções/RTTI. Acesso a arquivos entra por um **resolver injetado** (`IncludeResolver`, ponteiro de função + contexto), não por `std::filesystem` dentro do `Preprocessor`. Isso também resolve overlay de buffers abertos (`docs/completion-limitations.md` §1).
- Dados em SoA/colunas: definições num vetor de `MacroDef` com corpo em arena de tokens; nada de `std::string` por token expandido.
- Resultados imutáveis e versionados (padrão já usado em `TokenSnapshot`/`IndexSnapshot`).
- Falha explícita: limite de profundidade/tamanho de expansão e `stop_token` em todo laço; estouro gera `Diagnostic`, nunca trava.

## 4. Fases

### F0 — Contrato, corpus e métricas (1 semana)

- Corpus em `test/data/macros/`: guardas de include, `#undef`/redefinição, function-like, `#`/`##`, `__VA_ARGS__`/`__VA_OPT__`, expansão recursiva e mútua (hide-set), X-macros, macros-de-decoração, `TEST()` do GoogleTest, `BENCHMARK()`, `Q_OBJECT`, macros do Windows (`min/max`, `EXPORT`), `-D` com valor, `-include`.
- Linha de base: contar nós de erro do parser em GoogleTest/Benchmark **com** os `-D` do banco (P12 de `cpp-syntax-coverage.md` pede exatamente essa medição antes de agir).
- Critério de saída: script que imprime a contagem de erros por projeto-amostra; testes que hoje falham marcados `DISABLED_` com o motivo.

### F1 — Modelo de macro e expansor (core) (2–3 semanas)

Arquivos novos: `core/include/Heimdall/MacroTable.hpp`, `MacroExpander.hpp`, `core/src/MacroTable.cpp`, `MacroExpander.cpp`.

- `MacroDef`: `name`, `kind` (object/function), `params`, `variadic`, `bodyTokens` (span na arena), `origin` (`CommandLine | Builtin | File{fileId, offset} | Include`), `definitionHash`.
- `MacroTable`: mapa persistente (estrutura compartilhada) com `Define/Undef`, consulta por `string_view` sem alocar (reusar `TransparentStringHash`). Snapshot O(1).
- `MacroExpander`: algoritmo de Prosser (hide-sets) sem recursão de pilha; `#`, `##`, `__VA_ARGS__`, `__VA_OPT__` (C++20), argumentos pré-expandidos, `defined` em `#if`. Saída: vetor de tokens + coluna `origin` (token de origem, invocação, nível de expansão).
- Estender `Preprocessor::Process` para registrar function-like (hoje descartado, `Preprocessor.cpp:754`) e avaliar `#if` com chamadas de macro.
- Compatibilidade: `MacroMap` (nome→texto) vira adaptador sobre `MacroTable`; chamadores atuais não mudam na F1.
- Testes (`MacroExpanderSpec`): casos da especificação (C++ [cpp.replace]), incluindo os exemplos do padrão (`f(2)(9)`, `t(x,y,z)`, `hash_hash`), limites e cancelamento.
- Critério: 100% dos exemplos normativos do padrão; expansão determinística; nenhuma alocação por token no caminho feliz (verificar em `PreprocessorBench`).

### F2 — Ambiente de macros por TU, com includes (2–3 semanas)

Arquivos: `core/include/Heimdall/MacroEnvironment.hpp`, `semantic/src/MacroEnvironment.cpp` (resolução de include usa `IncludeIndex`/`CompileCommand`).

- `MacroEnvironment::Build(tu, command, resolver)`: percorre a TU na ordem real: builtins → `-D/-U` → `-include/-imacros` → arquivo, e entra em cada `#include` ativo seguindo busca real (`<>`/`""`, `-I`, `-isystem`, `#include_next`).
- `At(offset)` devolve o snapshot de macros vigente naquele ponto (necessário para highlight, hover e expansão corretos mesmo com `#undef` e redefinições).
- Guardas: `#pragma once`, guarda clássica detectada, e **cache por (hash do conteúdo, impressão digital das macros que o header lê)**. O header declara no seu `MacroEffect` o que define, remove e consulta; só reavalia quando uma macro consultada mudou. Isso evita reprocessar `<vector>` para cada includer.
- Builtins do compilador: tabela embutida por (compilador, versão, `-std`, alvo) para GCC/Clang/MSVC como fallback; quando há compilador, **uma** sondagem `-dM -E` por `config_hash`, cacheada em disco e em memória (substitui a sonda por consulta de `CompilerMacros`). Macros function-like entram (hoje descartadas).
- Overlay: headers abertos no editor usam o buffer, não o disco (via `SourceOverlay`).
- Invalidação: editar um header muda o `MacroEffect`; só TUs cujo ambiente leu uma macro alterada reconstroem.
- Orçamento: ambiente de uma TU típica (`<iostream>` + projeto) em < 50 ms a frio, < 5 ms a quente; memória proporcional às macros únicas, não ao número de includes.
- Testes: ordem de `#define/#undef` entre includes, guardas, include circular, `#include_next`, header incluído duas vezes com macros diferentes (X-macro), `__has_include`, cancelamento.

### F3 — Ganho rápido: highlight e navegação por macro (1 semana, pode ir logo após a F2 parcial)

Antes mesmo da F4, usar o ambiente da F2 nos recursos LSP que hoje não enxergam macros:

- `SyntaxHighlight::Extract(tree, const MacroTable&)`: identificador presente em `At(offset)` vira `macro` (+ modificador `declaration` na definição). Corrige a limitação atual de macros de outros headers.
- Código inativo (`active_ranges`) emitido como modificador (`inactive`) para o cliente esmaecer.
- Hover: assinatura da macro, corpo, origem; `textDocument/definition` para o `#define` (inclusive em header de sistema).
- Completion: macros visíveis no ponto, com `CompletionItemKind.Constant/Function`.
- Critério: abrir `.cpp` que usa `assert`, `NULL`, `EXPECT_EQ`, `Q_OBJECT` e ver realce/hover/definição corretos sem depender do arquivo local.

### F4 — Parser consciente de expansão (3–4 semanas)

O formatter e o highlight exigem tokens **lossless**; portanto o parser trabalha sobre o fluxo original, e a expansão é uma visão paralela.

- Tabela lateral `ExpansionTable` (SoA): por invocação `{firstToken, lastToken, macroId, argRanges, expandedBegin, expandedEnd}` e coluna de proveniência para os tokens expandidos.
- Estratégia em camadas, na ordem de custo/benefício:
  1. **Decoração** (já existe): trocar a heurística `IsDecorationMacro` por "expande para sequência vazia ou só atributos/qualificadores", usando a expansão real.
  2. **Macro de expressão/instrução**: invocação em contexto de expressão ou statement expande, o fragmento é parseado, e o nó resultante é filho de um `MacroInvocation` com argumentos mapeados ao fonte.
  3. **Macro de declaração** (`TEST(A,B){...}`, `DECLARE_X(T)`, X-macros): o fluxo expandido é parseado como declarações; símbolos gerados carregam `origin = invocação`.
  4. **Falha controlada**: se a expansão não parseia, o nó vira `MacroInvocation` opaco com um `Diagnostic` informativo (não cascata de erros no resto do arquivo), preservando recuperação atual.
- Reuso incremental (`ParseReuse`): a invocação passa a depender do `definitionHash` da macro; mudar a definição invalida só os itens que a usam.
- Critério: queda mensurável (meta ≥ 90%) dos nós de erro de F0 em GoogleTest/Benchmark/Qt-like; nenhum teste de formatter muda de saída; `ParserBench` sem regressão acima de 3% em código sem macros.

### F5 — Índice e semântica (2–3 semanas)

- `ShardBuilder`: novo `SymbolKind::Macro` com definição, parâmetros, corpo resumido; várias definições por `config_hash` (registrar a lacuna em vez de adivinhar, como o plano de índice já decide).
- Usos: invocações e menções em `#if/#ifdef`/`defined(...)`, em argumentos de outras macros e no corpo de macros.
- Símbolos gerados por macro entram no índice com `provenance` (invocação) e navegam para a macro e para a invocação.
- `Binder`/`Typer`: consomem os tokens expandidos via proveniência; erros semânticos são reportados no token de **invocação** (nunca no corpo da macro, a menos que a macro seja do projeto).
- `ProjectSymbolIndex`/`workspace/symbol`: macros do projeto pesquisáveis.
- Critério: "Find references" de macro do projeto retorna todos os usos entre arquivos; símbolos gerados por `TEST()` aparecem no outline.

### F6 — Regras e refatoração (1–2 semanas)

- `refactor/MacroSafety`: trocar a recusa genérica por checagem baseada em proveniência (renomear dentro de argumento de macro quando o mapeamento token↔fonte é exato; recusar só o que nasce de `##`/`#`).
- Rename de macro (nova): todos os usos + menções em diretivas; recusa se houver `##` que forme o nome.
- `cpp/include-what-you-use` e `cpp/no-unused-include`: ler macros do `MacroEnvironment` em vez da extração própria (remove duplicação) e tratar uso de macro como uso do header.
- `cpp/modernize-nullptr` (`NULL`), `no-unused-local` e `modernize-const` deixam de tratar toda macro como "escape" quando a expansão é conhecida.
- Quick fixes: **"Expandir macro"** (code action que substitui a invocação pelo texto expandido, só quando segura) e **"Mostrar expansão"** (hover/inlay).

### F7 — Robustez, desempenho e documentação (contínuo, fechamento em 1 semana)

- `bench/`: `MacroExpanderBench`, `MacroEnvironmentBench` (TU fria/quente, 1 000 includes), `LspLatencyBench` para hover/highlight com ambiente quente. Orçamento: realce de arquivo de 5 000 linhas < 30 ms com ambiente em cache.
- Fuzz: expansões patológicas (explosão exponencial `#define A B B`, recursão, `##` inválido, argumentos desbalanceados) com limites de tamanho (`kMaxExpandedTokens`) e de profundidade.
- Smoke LSP em Python (padrão `LspMemberAccessSmoke`) para hover/definição/highlight de macro em header de sistema.
- Atualizar `README.md`, `docs/cpp-syntax-coverage.md` (P12), `docs/completion-limitations.md`, `docs/cross-file-index-plan.md` (linhas sobre macros) e `docs/rule-engine-roadmap.md`.

## 5. Ordem e dependências

```
F0 ─► F1 ─► F2 ─┬─► F3 (valor visível cedo)
                ├─► F4 ─► F5 ─► F6
                └─────────────────► F7 (bench/fuzz desde F1)
```

F3 só depende de F1+F2; F4 e F5 podem andar em paralelo depois que `ExpansionTable` estiver definido. Estimativa total ≈ 12–16 semanas de uma pessoa; F0–F3 (≈ 6–8 semanas) já resolvem a dor atual de highlight/navegação.

## 6. Riscos e decisões em aberto

| Tema | Risco | Proposta |
|------|-------|----------|
| Compilador ausente | Sem builtins, `#if __cplusplus >= ...` seleciona nada (já ocorre) | Tabelas embutidas por compilador/versão como fallback; sondagem opcional cacheada |
| Variantes de header | Um header com várias configs (`config_hash`) | Uma entrada por config; resposta exibe a lacuna em vez de escolher |
| Custo de entrar em todos os includes | `<windows.h>`, Qt, Boost expandem para milhares de macros | `MacroEffect` memoizado por (conteúdo, macros lidas); limite de arquivos por TU configurável |
| Formatter e tokens expandidos | Misturar fluxos quebraria idempotência | Formatter só vê tokens originais; expansão sempre em tabela lateral |
| Macros que geram código inválido isolado | Cascata de erros | `MacroInvocation` opaco + um único diagnóstico |
| MSVC | `/Zc:preprocessor` muda semântica de `__VA_ARGS__` e `##` | Modo do expansor selecionado pelo comando de compilação; testes dedicados na F1 |
| Política "core sem dependências" | Resolver de includes precisa de I/O | Resolver injetado; `CorePolicySpec` continua valendo |

## 7. Decisões tomadas

1. **Compilador permitido.** `-dM -E` é a fonte preferida de builtins, rodado uma vez por `config_hash` e cacheado (memória e disco). As tabelas internas por compilador/versão continuam como fallback para quando ele não existe ou falha (MSVC incluído).
2. **Limites padrão**, alinhados ao que o padrão recomenda e ao que os compiladores praticam. Todos configuráveis em `.heimdall.json` (`macros.*`); estourar um limite gera `Diagnostic` e interrompe só aquele ramo.

   | Limite | Padrão | Referência |
   |--------|--------|------------|
   | Profundidade de `#include` | 200 | GCC `-fmax-include-depth` e Clang usam 200; o padrão (Anexo B) recomenda ao menos 256 aninhamentos, então 200 é o que os compiladores reais aceitam |
   | Macros definidas simultaneamente | 65 536 | mínimo recomendado pelo Anexo B; GCC/Clang não impõem teto, mas headers como Windows/Qt/Boost ficam abaixo disso |
   | Parâmetros / argumentos por macro | 256 | mínimo recomendado pelo Anexo B |
   | Aninhamento de condicionais | 256 | mínimo recomendado pelo Anexo B |
   | Tokens por expansão (`kMaxExpandedTokens`) | 1 000 000 | não há norma; proteção contra explosão exponencial (`#define A B B`); calibrar com o fuzz da F7 |
   | Profundidade de expansão aninhada | 256 | proteção própria; a recursão é iterativa, o limite protege o tempo |
   | Arquivos visitados por TU | sem teto fixo; só o de profundidade e `stop_token` | incluir é barato com o cache de `MacroEffect` |

   Valores do padrão e dos compiladores citados de memória: confirmar contra a documentação do GCC (`-fmax-include-depth`) e o rascunho do padrão antes de fixar as constantes na F2.
3. **`workspace/symbol` inclui macros**, também as de headers de sistema. Para não poluir a busca, o resultado marca a origem (`projeto` ou `sistema`) em `containerName` e ordena as do projeto primeiro; o escopo de headers de sistema só é indexado depois que o fecho de includes dos arquivos abertos foi calculado (sem varrer `/usr/include` inteiro).
