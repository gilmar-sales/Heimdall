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

**F1 (Binder): concluída.** Regras entregues: `cpp/modernize-override`, `cpp/modernize-nullptr`, `cpp/no-zero-as-null` e `cpp/modernize-auto`.

| Peça | Onde |
|---|---|
| `InternPool`, `SymbolTable`, `ScopeTable`, `BaseTable`, `RefTable`, `SemanticModel` | `semantic/include/Heimdall/SemanticModel.hpp`, `semantic/src/SemanticModel.cpp` |
| `Binder::Bind(const ParseTree&)` | `semantic/src/Binder.cpp` |
| `SemanticRules::AnalyzeOverride/AnalyzeNullptr/AnalyzeZeroAsNull/AnalyzeAuto/Analyze` | `semantic/src/SemanticRules.cpp` |
| Adaptador de arena para `std::pmr` (`Arena::Resource()`) | `core/include/Heimdall/Arena.hpp` |
| Benchmarks (`BM_Bind`, `BM_BindHierarchy`, `BM_ModernizeOverride`) | `bench/src/SemanticBench.cpp` (alvo `SemanticBench`) |

O que o modelo oferece às regras: `Significant()` (tokens de código, sem trivia, diretivas nem macros de decoração), `IsCode(token)`, `ChildrenOf(node)`, `ScopeOfNode(node)`, `ResolveToken(token)`, `Lookup`/`LookupMember` e as flags `Pointer` e `ReturnsPointer`.

Decisões e limites desta fase:

- O modelo é construído por uma passada sobre `ParseTree::Nodes()`, que **não** está em pré-ordem: o pai de um nó pode ter índice maior. O Binder cria escopos sob demanda subindo a cadeia de pais, nunca assume ordem.
- **Código inativo.** A árvore não guarda quais ramos de `#if` estão ativos. O Binder considera "código" só os tokens cobertos por algum nó que não seja diretiva; o que sobra (trivia entre itens, diretivas, ramos desligados) nunca entra em `Significant()`. Ramo inativo dentro de uma mesma declaração continua sendo visto.
- Nomes de funções, construtores, destrutores e operadores vêm de uma leitura dos tokens do cabeçalho da declaração, porque a gramática não os nomeia de forma confiável (por exemplo, `virtual ~Base();`).
- Os declaradores seguintes de `int* a = 0, b = 0;` chegam da gramática como `InitDeclarator` sem `Declarator`; o Binder os nomeia mesmo assim, mas só o primeiro tem a flag `Pointer`.
- O nó `TrailingReturnType` termina antes de um `*` final: `ReturnsPointer` lê os tokens de `->` até o corpo.
- No escopo de namespace, `T x = f(...);` é lido pela gramática como `FunctionDeclaration` (qualquer `(` antes do `;`), então esses inicializadores globais não são analisados e `x` vira um "símbolo função".
- `RefTable` só registra `IdentifierExpression`. Acesso por objeto (`obj.m`, `p->m`) fica não resolvido até o Typer (F2); nomes de template e de namespace anônimo também. Em escopos de função e bloco só enxerga declarações anteriores ao uso.
- Bases com argumentos de template (`Base<T>`) ficam não resolvidas de propósito.
- Assinatura de função = hash dos tokens dos tipos dos parâmetros (sem nomes nem valores padrão) mais qualificadores `const`, `volatile`, `&` e `&&`. Comparação textual: pode perder overrides (falso negativo), mas a regra continua em silêncio nesses casos.
- Hierarquias são percorridas com marcação de visitados (ciclos e diamantes terminam, sem limite de profundidade).
- Um conflito entre regras foi resolvido de propósito: `modernize-auto` não reporta cast de constante nula, porque depois do fix de `modernize-nullptr` não sobraria nada para o `auto` deduzir.
- Ainda não há reuso incremental por `TopLevelItem` (seção 6) nem `HeaderSummary` (seção 8): o modelo é refeito a cada versão do documento, como o `ParseTree`.

**F2 (Typer): concluída.** Regras entregues: `cpp/no-implicit-bool-conversion`, `cpp/modernize-range-loop` e `cpp/modernize-loop-convert`.

| Peça | Onde |
|---|---|
| `TypeTable` (hash-consing), `TypeModel` (tipo de cada símbolo e de cada expressão), `Typer::Type(const SemanticModel&)` | `semantic/include/Heimdall/TypeModel.hpp`, `semantic/src/Typer.cpp` |
| `SemanticRules::AnalyzeImplicitBool/AnalyzeRangeLoop/AnalyzeLoopConvert` e `Analyze(model, types)` | `semantic/src/SemanticRules.cpp` |
| Testes de unidade do Typer e das regras | `test/src/SemanticTyperSpec.cpp` |
| Benchmarks (`BM_Type`, `BM_TypeFunctions`, `BM_TypeRules`) | `bench/src/SemanticBench.cpp` |

O que o modelo oferece às regras: `TypeModel::SymbolType(symbol)` (tipo declarado de variáveis, parâmetros e campos; tipo de retorno de funções; tipo denotado por aliases), `NodeType(node)` (tipo do valor de uma expressão, nunca referência), `Types()` com `Kind`, `Strip`, `Decay`, `IsInteger`, `IsPointer`... e `Spell(type)` para mensagens. O Typer é uma consulta separada do Binder: `Analyze(model)` o calcula uma vez e o repassa às regras; regras da F1 não pagam por ele.

Decisões e limites desta fase:

- **Tipos.** `Builtin`, `Class`/`Enum` (símbolos do próprio arquivo), `External`, `Pointer`, `LRef`, `RRef`, `Const`, `Array` e `Unknown` (id 0). Um ponteiro para tipo desconhecido (`T* p`) continua sendo um ponteiro conhecido; referência e `const` de `Unknown` colapsam em `Unknown`.
- **`External`.** Tipos qualificados com `std::` ficam como nomes internados, com os argumentos de template escritos (`std::vector<int>`); só o "cabeça" (`std::vector`) importa para as regras, e `std::vector<bool>` é excluído dos laços porque o elemento é um proxy. Nomes não qualificados de bibliotecas (`vector<int>` após `using namespace std`) são `Unknown`. Exceção: `std::size_t` e `size_t` não declarado viram `SizeT`.
- **`SizeT`.** Tipo próprio (sem sinal, largura da plataforma). Conversões aritméticas que dependem da largura de `long` (`long` com `unsigned`, `long long` com `unsigned long`) e literais maiores que `int` sem sufixo resultam em `Unknown`, para o resultado não variar entre LP64 e LLP64.
- **Expressões.** Literais (inclusive `true`, `false`, `nullptr`, que a gramática lê como identificadores), identificadores, `this`, parênteses, operadores unários e binários sobre tipos primitivos e ponteiros (conversões aritméticas usuais, promoção inteira, aritmética de ponteiros), `?:`, subscript (array, ponteiro, `operator[]` de classe), acesso a membro (`.`, `->`, `::`, inclusive bases do próprio arquivo), chamadas de função (todas as sobrecargas do nome precisam ter o mesmo tipo de retorno; funções-template, construtores e destrutores são `Unknown`), `T(x)`, `static_cast<T>`/`dynamic_cast`/`reinterpret_cast`/`const_cast`, `sizeof` e `size()`/`empty()`/`length()`/`capacity()` de contêineres da biblioteca padrão. Operadores sobre classes e `External` (`a + b`, `a < b`, `!a`, `&a`) são `Unknown`: podem estar sobrecarregados.
- **`auto`.** `auto x = e;` deduz do tipo de `e` (sem referência, `const` de topo e array); `auto&` e `const auto&` mantêm a referência. `auto*`, `auto&&`, inicializador entre chaves/parênteses e `auto` sem inicializador de tipo conhecido são `Unknown`; tipo de retorno `auto` sem trailing return também (não olha o corpo).
- **Casts C e `new`.** A gramática liga o operando de `(T)x` e de `new T` ao pai errado. O Typer só confia em um nó quando o primeiro filho começa onde o próprio nó começa; caso contrário o resultado é `Unknown`, e as regras exigem que o filho cubra exatamente o trecho de tokens esperado.
- **Aliases.** `using X = T;` e `typedef T X;` são resolvidos recursivamente com guarda contra ciclos (ciclo = `Unknown`); aliases-template (`template <class T> using V = ...`) e instâncias de classes-template (`Box<int>`) são `Unknown`.
- **Profundidade.** Expressões encadeadas são tipadas com recursão limitada (192 níveis; passou disso, `Unknown`); cadeias à esquerda (`a + 1 + 1 + ...`) não consomem pilha porque os nós filhos têm índice menor.
- **Laços.** As duas regras de laço só reescrevem quando todo uso do índice/iterador e do contêiner no corpo segue o padrão esperado, e o contêiner é variável local ou parâmetro (membros e globais podem ser alterados por qualquer chamada); qualquer lambda no corpo desativa a regra. O autofix é sempre um quick fix: o corpo é reescrito por padrão de tokens.
- **Gramática.** `if (init; cond)` e declarações em condição não dão ao `cond` um nó próprio, e `do { } while (cond)` não parseia `cond`: essas condições não são verificadas. Um encadeamento de milhares de `a = a = a = ...` estoura a pilha do parser (anterior a esta fase) antes de chegar ao Typer.
- Ainda não há reuso incremental do `TypeModel` nem cache entre documentos: ele é refeito a cada versão, como o `SemanticModel`.

**F3 (Projeto): concluída.** Regras entregues: `cpp/include-what-you-use` e `cpp/modernize-final`.

| Peça | Onde |
|---|---|
| `HeaderSummary` (exportações, includes diretos, classes com bases), cache global por caminho validado por tamanho e mtime | `semantic/include/Heimdall/HeaderSummary.hpp`, `semantic/src/HeaderSummary.cpp` |
| `ProjectIndex` (mapa nome → header, classes por nome, polimorfismo e derivadas entre headers) | `semantic/include/Heimdall/ProjectIndex.hpp`, `semantic/src/ProjectIndex.cpp` |
| `IncludeProfile` ampliado (header de cada include, fecho, diretórios de sistema, `includes_known`) e `IncludeAnalyzer::DirectIncludes` | `semantic/include/Heimdall/IncludeAnalyzer.hpp` |
| `ProjectContext`, `SemanticRules::AnalyzeIncludeWhatYouUse/AnalyzeFinal` e `Analyze(model, types, context)` | `semantic/include/Heimdall/SemanticRules.hpp`, `semantic/src/IncludeWhatYouUse.cpp`, `semantic/src/ModernizeFinal.cpp` |
| Testes | `test/src/ProjectRulesSpec.cpp` |
| Benchmarks (`BM_HeaderSummary`, `BM_ModernizeFinal`, `BM_IncludeWhatYouUse`, `BM_ProjectIndex`) | `bench/src/SemanticBench.cpp` |

O que a camada oferece às regras: uma regra de projeto recebe `const SemanticModel&` mais um `ProjectContext` (arquivo, `IncludeProfile` e `CompileCommand`). Sem profile (sem compile command) ela roda sem enxergar headers: nome ou base vindo de header fica `Unknown` e a regra se cala.

Decisões e limites desta fase:

- **`HeaderSummary` vem do próprio Binder.** O header é parseado e ligado como qualquer arquivo e o resultado é copiado para um pool de texto do resumo (strings em um buffer único, ids de 32 bits em vetores paralelos). O resumo não guarda ponteiro para a árvore, então sobrevive a ela e é compartilhado (`shared_ptr<const HeaderSummary>`) entre documentos. O único estado mutável é o cache, protegido por mutex.
- **O que é exportado.** Classes (com corpo), enums, aliases, funções, variáveis e enumeradores de `enum` comum em escopo de namespace ou global, com o caminho de namespaces (`a::b`). Ficam de fora: namespace anônimo, membros de classe, locais, funções definidas fora da classe (`A::f`), `friend`, construtores/destrutores/operadores, nomes que começam com `_` e declarações antecipadas (`class N;`). Namespaces `inline` não são tratados: `lib::v1::X` só casa com a qualificação completa.
- **Só headers do projeto.** O índice cobre os headers fora dos diretórios do sistema do compilador. A biblioteca padrão usa uma tabela curada de nomes → headers (os headers do libstdc++ se dividem em arquivos internos que não servem como destino de `#include`).
- **`includes_known`.** `include-what-you-use` só roda se todo include direto foi encontrado. Includes angulares não resolvidos dentro de `#if` (cabeçalhos de plataforma) não contam. Includes condicionais têm o header resolvido, mas o fecho não é percorrido.
- **Conservadora por construção.** O nome usado precisa ser indubitável: sem declaração local de mesmo nome, vindo de um único header, com qualificação compatível com o namespace da exportação. Vários headers com o mesmo nome (sobrecargas, redeclarações) deixam a regra em silêncio.
- **`modernize-final` e a hierarquia.** Combina as bases do arquivo (`BaseTable`) com as dos headers (`HeaderSummary`): uma base de header decide se a classe é polimórfica, e um header que liste a classe como base impede o aviso. "Hierarquia completa" vale aqui para o universo fechado (arquivo-fonte ou namespace anônimo). Para classes de header, a regra precisaria dos arquivos que incluem o header, que o motor por arquivo não vê; ficam fora até existir uma visão de todo o projeto (por exemplo, um índice de bases gerado a partir do banco de compilação). Cadeias de bases com mais de 256 níveis viram `Unknown`.
- **Custo.** O `HeaderSummary` de cada header é construído uma vez (com uma revalidação por `stat` a cada uso). O `ProjectIndex` é remontado a cada análise a partir dos resumos em cache (cerca de 80 µs por header do fecho, medido em `BM_ProjectIndex`); ainda não é guardado junto com o `IncludeProfile`.
- Ainda não há reuso incremental por `TopLevelItem` (seção 6).

**F4 (Fluxo): concluída.** Regras entregues: `cpp/modernize-const` e `cpp/modernize-constexpr`.

| Peça | Onde |
|---|---|
| `FlowModel` (`FunctionTable`, `BlockTable`, `EventTable`, arestas e eventos por símbolo), `Flow::Build(const TypeModel&)` | `semantic/include/Heimdall/FlowModel.hpp`, `semantic/src/Flow.cpp` |
| `ConstantAnalysis` (avaliador de expressões constantes, elegibilidade de funções, especificadores) | `semantic/src/detail/ConstantAnalysis.hpp`, `semantic/src/ConstantAnalysis.cpp` |
| `SemanticRules::AnalyzeConst/AnalyzeConstexpr(const FlowModel&)` | `semantic/src/ModernizeConst.cpp` |
| `TokenView` compartilhado (antes privado de `SemanticRules.cpp`) | `semantic/src/detail/TokenView.hpp` |
| Testes | `test/src/SemanticFlowSpec.cpp` |
| Benchmarks (`BM_Flow`, `BM_ModernizeConst`) | `bench/src/SemanticBench.cpp` |

O que o modelo oferece às regras: uma entrada por corpo de função e por lambda; blocos básicos com sucessores (`Successors`); eventos por variável local ou parâmetro (`EventsOf(symbol)`: `Init`, `Uninit`, `Read`, `Write`, `Modify`, `Escape`); `OwnerOf`, `IsReachable`, `ExitReachable`, `HasReachableReturn` e `IsNeverModified`. O `FlowModel` é uma consulta separada: `Analyze(model, types)` o calcula uma vez para as duas regras; as regras anteriores não pagam por ele.

Decisões e limites desta fase:

- **CFG por instrução.** `if`, `for`, `while`, range-for, `do`, `switch` (com fallthrough e sem `default`), `try`/`catch` (handlers ligados ao ponto de entrada), `break`, `continue`, `return` e `throw`. Código depois de `return` ganha um bloco sem predecessores: continua analisado (uma escrita ali ainda impede `const`), mas é inalcançável. `for (;;)` só sai por `break`. A forma é pensada para as futuras regras `memory/*` e `concurrency/*` (blocos, arestas e eventos por símbolo), que não foram implementadas.
- **Função incompleta.** `goto`, `asm`, corrotinas, nós `Error` e aninhamento acima de 128 níveis marcam a função como incompleta (`complete == 0`); toda regra se cala sobre ela.
- **Ordem dos eventos.** Dentro de uma instrução os eventos vêm em ordem de token, não de avaliação (`x = x + 1` dá `Read` e depois `Write` só por posição).
- **Classificação conservadora.** O contexto de cada uso sobe pela árvore: operadores, subscript, acesso a membro, chamadas, inicializadores e `return`. Tudo que o motor não reconhece vira `Escape`. Em chamadas, a função do próprio arquivo é consultada pelos tokens dos parâmetros (valor ou `const T&` mantém o argumento; `T&`, `T&&`, `...` e templates escapam); chamadas a `std::` ou a funções não resolvidas escapam, exceto uma lista curta que recebe por valor (`printf`, `std::min`, `std::to_string`...) e só para tipos escalares.
- **Nomes que a gramática não modelou.** `S s(q);` é lido como declarador de função e `{x, y}` engole os nomes. Uma varredura de tokens por função trata todo identificador não resolvido que não seja membro (`.`, `->`, `::`) como `Escape` de todo local de mesmo nome já declarado.
- **Parâmetros de lambda** não viram nós na gramática: não têm símbolo e não são analisados. Os locais do corpo da lambda são analisados como os de uma função.
- **Avaliador constante.** Opera sobre tokens (a gramática lê casts e alguns inicializadores de forma estranha): precedência de C, literais inteiros e de ponto flutuante, `true`/`false`, enumeradores, variáveis `constexpr` ou `const` integral com inicializador constante e chamadas a funções `constexpr` ou elegíveis. Inteiros de 32 bits têm valor e sinal rastreados (estouro com sinal, divisão por zero e deslocamento fora da largura são "não constante"); tipos de largura dependente da plataforma, enumeradores e `char` têm tipo mas não valor.
- **Elegibilidade de função.** Ver a linha de `cpp/modernize-constexpr` em [rule-engine-roadmap.md](rule-engine-roadmap.md). O padrão mínimo é C++20, então laços, locais sem inicializador e múltiplas instruções são aceitos.
- **Conflito resolvido de propósito.** Um local com inicializador constante e tipo aritmético é reportado só por `modernize-constexpr`, nunca também por `modernize-const`.
- Ainda não há reuso incremental do `FlowModel` (seção 6): ele é refeito a cada versão, como o `TypeModel`. O `ProjectContext` não entra: a análise é intraprocedural e por arquivo.

Próxima fase: reuso incremental por `TopLevelItem` (seção 6) e as regras `memory/*` e `concurrency/*` sobre o `FlowModel`.
