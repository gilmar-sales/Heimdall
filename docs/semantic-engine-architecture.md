# Arquitetura do motor semântico

Este documento define as boas práticas e a arquitetura do motor semântico do Heimdall, necessário para concluir as regras marcadas como "Não implementada" em [rule-engine-roadmap.md](rule-engine-roadmap.md). Segue os pilares de [architecture-review-prompt.md](../architecture-review-prompt.md): memória, strings, concorrência, incrementalidade e layout de dados.

## Princípios

1. **Imutabilidade.** Snapshot de texto, `ParseTree` e `SemanticModel` são imutáveis depois de prontos. Não há mutex sobre eles.
2. **Zero-copy.** Nomes e tokens apontam para o buffer original; nunca copiam para `std::string`.
3. **Índices, não ponteiros.** Entidades são `u32` em vetores contíguos.
4. **Sob demanda.** Cada camada só é calculada quando um consumidor a pede.
5. **Honestidade semântica.** Sem compilador completo, o motor declara o que não sabe (`Unknown`) e as regras se calam.
6. **Tolerância a falhas.** Código incompleto nunca derruba a análise; apenas gera `Unknown` localmente.

## 1. Gerenciamento de memória

- Todo o `SemanticModel` de uma versão do documento vive em uma `Arena` (bump allocator) e é liberado em O(1) com o `ParseTree`.
- Proibido `std::unique_ptr`/`std::shared_ptr` por símbolo, escopo ou tipo.
- `shared_ptr<const T>` é permitido apenas em fronteiras: o próprio modelo (entre thread de I/O e workers) e o `HeaderSummary` (cache global).
- Referência entre entidades: `SymbolId`, `ScopeId`, `TypeId`, `NameId`, todos `std::uint32_t`. Sentinela de ausência: `~0u`.

## 2. Strings e interning

- O buffer do arquivo é imutável e vive mais que o modelo (`SyntaxTree::HoldSource`).
- Nomes são `std::string_view` sobre esse buffer, internados em um `InternPool` que devolve `NameId`.
- Comparação semântica de nomes (mesmo símbolo, mesma assinatura, método sobrescrito) é comparação de inteiros.
- Resumos de header usam o próprio pool, para poderem ser compartilhados entre documentos sem cópia.

## 3. Layout de dados (SoA)

```cpp
struct SymbolTable {              // Structure of Arrays
    std::vector<NameId>  name;
    std::vector<ScopeId> scope;
    std::vector<u32>     decl_node;   // índice em GrammarNode
    std::vector<TypeId>  type;
    std::vector<u32>     flags;       // const, virtual, override, final, constexpr, explicit, static...
};

struct ScopeTable {
    std::vector<ScopeId>  parent;
    std::vector<u8>       kind;       // translation unit, namespace, class, function, block
    std::vector<SymbolId> owner;      // namespace/class symbol that owns the scope
    std::vector<u32>      node;       // defining grammar node
};
// Nomes por escopo: tabela hash (escopo, nome) -> cadeia de símbolos
// (SymbolTable::next_same_name), em vez de faixas contíguas por escopo.

struct RefTable {
    std::vector<u32>      token;      // índice do token que referencia
    std::vector<SymbolId> target;     // ~0u = não resolvido
};
```

- Regras que filtram por uma propriedade (por exemplo `override`) varrem só o vetor dessa propriedade.
- `TypeTable` usa hash-consing: `Builtin(k)`, `Named(sym)`, `Pointer(T)`, `Ref(T)`, `Const(T)`, `Unknown`. Igualdade de tipos é igualdade de `TypeId`.
- Não misturar campos quentes e frios no mesmo struct.

## 4. Contrato de confiança (`Known` / `Unknown`)

O motor não implementa overload resolution, instanciação de templates nem expansão completa de macros. Por isso:

| Situação | Resultado |
|---|---|
| Nome não resolvido, tipo dependente de template, macro de função envolvida, nó `Error` | `Unknown` |
| `Unknown` em uma subexpressão | propaga para a expressão inteira |
| Regra com entrada `Unknown` | não emite diagnóstico |
| Autofix | só com todas as entradas `Known` e sem macros no trecho |

Teste de regressão obrigatório para cada regra: um caso `Known` (dispara) e um caso `Unknown` (silêncio, sem autofix).

## 5. Camadas e consultas sob demanda

```
Snapshot ─► ParseTree ─► Binder ─► Typer ─► Facts (hierarquia, CFG/def-use)
                                     ▲
                       HeaderSummary (cache global)
```

| Camada | Produz | Custo pago por quem |
|---|---|---|
| Binder | escopos, símbolos, referências | regras F1 em diante |
| Typer | `TypeTable`, tipo de expressões | regras F2 em diante |
| Hierarquia | bases e métodos virtuais por classe | `override`, `final` |
| CFG / def-use | grafo intraprocedural | `modernize-const`, `constexpr` |
| HeaderSummary | símbolos exportados por header | `include-what-you-use`, bases externas |

- Cada camada é uma consulta memoizada por versão do documento.
- O formatter e as regras lexicais/sintáticas nunca acionam o motor semântico.
- `--semantic` / `enableSemantic` continua sendo o gate. Uma regra declara a camada de que precisa e o motor não calcula além dela.

## 6. Incremental e tolerância a falhas

- O Binder trabalha por `TopLevelItem`. O sub-modelo de cada item é chaveado por hash do conteúdo, estendendo o mecanismo `ParseReuse` existente.
- Edição no fim do arquivo reaproveita todos os itens anteriores; só os itens editados são religados e a resolução entre itens é refeita.
- Nós `Error` e `ErrorExpression` produzem `Unknown` e nunca interrompem a análise. As regras seguem no restante do arquivo.
- Nenhuma rotina do motor pode assumir árvore bem formada; todo acesso por índice é validado ou delimitado por `subtree_end`.

## 7. Concorrência e LSP

- A thread de I/O só recebe as mudanças e cria o snapshot (texto + versão). Nenhuma análise roda nela.
- Workers recebem `shared_ptr<const SemanticModel>` e um `std::stop_token`. Edição nova cancela o trabalho da versão antiga.
- Sem locks sobre o modelo, que é imutável. Estado compartilhado mutável é só o cache de `HeaderSummary`.
- `HeaderSummary` é chaveado por hash do conteúdo e flags de compilação, construído uma vez e em paralelo, e reutilizado por todos os documentos.
- No `lint` em lote o paralelismo continua sendo por arquivo (`std::jthread` + índice atômico).
- Meta: respostas do LSP abaixo de 50 ms (p95), medida em bench.

## 8. Camada de projeto

`HeaderSummary` reaproveita `IncludeIndex`/`ScopeIndex` e contém:

- símbolos exportados e seus tipos;
- classes com bases e métodos virtuais;
- includes diretos.

Habilita `include-what-you-use` (mapa símbolo → header) e `override`/`final` com bases definidas em headers.

## 9. Interface das regras

```cpp
enum class Confidence : std::uint8_t { Unknown, Likely, Certain };

struct SemanticRule {
    RuleId id;
    SemanticLayer needs;   // Binder | Typer | Hierarchy | Flow | Project
    void Run(const SemanticModel&, DiagnosticSink&) const;
};
```

- A regra recebe só `const SemanticModel&`; não altera nada e não faz I/O.
- Autofix só é gerado com `Certain`, como `TextEdit` (mesma infraestrutura de `RuleEngine::ApplyFixes`).
- Regras antigas baseadas em heurística de tokens (`AnalyzeUnusedLocals`) migram para consumir `RefTable`, sem re-lexar.

## 10. Roadmap por fase

| Fase | Entrega | Regras desbloqueadas |
|---|---|---|
| F1 Binder | `InternPool`, `SymbolTable`, `ScopeTable`, `RefTable` | `modernize-override`, `modernize-nullptr`, `no-zero-as-null` (ponteiros declarados no TU), `modernize-auto` (`new`, `make_*`, cast) |
| F2 Typer | `TypeTable`, tipo de literais, referências, membros e retorno de função | `no-implicit-bool-conversion`, `modernize-range-loop`, `modernize-loop-convert` |
| F3 Projeto | `HeaderSummary`, mapa símbolo → header | `include-what-you-use`, `modernize-final` (hierarquia completa) |
| F4 Fluxo | CFG e def-use intraprocedural | `modernize-const`, `modernize-constexpr` |

Observações:

- `modernize-final` sem visão de projeto só vale para classes em namespace anônimo.
- O CFG da F4 deve ser desenhado pensando nas futuras regras `memory/*` e `concurrency/*`, sem implementá-las agora.

## 11. Decisão: motor próprio vs. libclang

Adotado: **motor próprio com política `Unknown`**.

- Mantém latência de milissegundos e baixo consumo de memória, e evita dependência pesada.
- Quase nenhuma regra da tabela exige semântica completa.
- Limitação aceita: casos que dependem de overload resolution ou templates ficam `Unknown`.
- libclang como camada opcional no `lint` em lote fica como alternativa futura, fora do escopo atual.

## 12. Qualidade e validação

- **Testes:** cada regra tem testes de regressão `Known` e `Unknown`; cada camada tem testes de unidade próprios; edição incremental tem teste de reuso.
- **Benchmarks** em `bench/` (Google Benchmark), um por camada: tempo de `Bind` por KLOC, bytes por símbolo, taxa de reuso incremental e latência LSP p95.
- **Política de dependências:** o motor vive em `semantic/`; `core/` continua só com a biblioteca padrão (`CorePolicySpec`).
- **Critério de pronto de uma regra:** documentada no roadmap, testada nos dois cenários de confiança, sem regressão nos benchmarks da camada que ela usa.

## 13. Estado da implementação

**F1 (Binder): implementada, com a regra `cpp/modernize-override`.**

| Peça | Onde |
|---|---|
| `InternPool`, `SymbolTable`, `ScopeTable`, `BaseTable`, `RefTable`, `SemanticModel` | `semantic/include/Heimdall/SemanticModel.hpp`, `semantic/src/SemanticModel.cpp` |
| `Binder::Bind(const ParseTree&)` | `semantic/src/Binder.cpp` |
| `SemanticRules::AnalyzeOverride` | `semantic/src/SemanticRules.cpp` |
| Adaptador de arena para `std::pmr` (`Arena::Resource()`) | `core/include/Heimdall/Arena.hpp` |
| Benchmarks (`BM_Bind`, `BM_BindHierarchy`, `BM_ModernizeOverride`) | `bench/src/SemanticBench.cpp` (alvo `SemanticBench`) |

Decisões e limites desta fase:

- O modelo é construído por uma passada sobre `ParseTree::Nodes()`, que **não** está em pré-ordem: o pai de um nó pode ter índice maior. O Binder cria escopos sob demanda subindo a cadeia de pais, nunca assume ordem.
- Nomes de funções, construtores, destrutores e operadores vêm de uma leitura dos tokens do cabeçalho da declaração, porque a gramática não os nomeia de forma confiável (por exemplo, `virtual ~Base();`).
- `RefTable` só registra `IdentifierExpression`. Acesso por objeto (`obj.m`, `p->m`) fica não resolvido até o Typer (F2); nomes de template e de namespace anônimo também ficam não resolvidos. Em escopos de função e bloco só enxerga declarações anteriores ao uso.
- Bases com argumentos de template (`Base<T>`) ficam não resolvidas de propósito.
- Assinatura de função = hash dos tokens dos tipos dos parâmetros (sem nomes nem valores padrão) mais qualificadores `const`, `volatile`, `&` e `&&`. Comparação textual: pode perder overrides (falso negativo), mas a regra continua em silêncio nesses casos.
- Hierarquias são percorridas com marcação de visitados (ciclos e diamantes terminam, sem limite de profundidade).
- Ainda não há reuso incremental por `TopLevelItem` (seção 6) nem `HeaderSummary` (seção 8): o modelo é refeito a cada versão do documento, como o `ParseTree`.

Próximos passos da F1: `modernize-nullptr`, `no-zero-as-null` e `modernize-auto`.
