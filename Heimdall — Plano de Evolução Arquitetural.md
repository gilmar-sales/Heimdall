# Heimdall — Plano de Evolução Arquitetural

## 1. Objetivo

Este documento define uma proposta de evolução arquitetural para o Heimdall visando preparar a engine para crescimento de longo prazo sem comprometer suas características atuais de desempenho e simplicidade.

Os principais objetivos são:

- aumentar a escalabilidade da análise para projetos de grande porte;
- permitir análise incremental;
- reduzir recomputações durante uso via LSP;
- melhorar a eficiência de memória;
- preservar locality e eficiência de cache;
- permitir evolução das estruturas internas sem quebrar consumidores;
- facilitar a implementação de novas features;
- estabelecer uma arquitetura consistente para análise sintática e semântica;
- permitir execução concorrente segura;
- introduzir suporte futuro a plugins;
- manter regras nativas no caminho de maior desempenho;
- evitar dependências desnecessárias no núcleo da engine.

A evolução deve ser incremental. Não é objetivo substituir imediatamente a arquitetura atual nem introduzir abstrações sem necessidade concreta.

---

# 2. Princípios arquiteturais

A evolução do Heimdall deve seguir alguns princípios fundamentais.

## 2.1. Dados antes de abstrações

O Heimdall deve continuar favorecendo estruturas de dados explícitas e eficientes em vez de hierarquias extensas de objetos.

Sempre que possível, estruturas utilizadas em hot paths devem favorecer:

- armazenamento contíguo;
- acesso previsível;
- poucas alocações;
- IDs compactos;
- arenas;
- views;
- spans;
- índices;
- estruturas SoA quando justificadas;
- boa utilização de cache.

Abstrações públicas não devem determinar a representação física interna dos dados.

---

## 2.2. Separação entre representação interna e API pública

Estruturas internas como:

```cpp
SyntaxNode
Symbol
Type
Scope
Reference
```

não devem necessariamente fazer parte de APIs públicas.

Consumidores externos devem preferencialmente utilizar identificadores opacos:

```cpp
DocumentId
NodeId
SymbolId
TypeId
ScopeId
StringId
```

Isso permite que estruturas internas sejam reorganizadas sem quebrar compatibilidade.

Por exemplo, uma estrutura interna inicialmente baseada em:

```cpp
std::vector<Symbol>
```

pode posteriormente ser alterada para uma representação SoA:

```cpp
struct Symbols {
    std::vector<StringId> names;
    std::vector<TypeId> types;
    std::vector<ScopeId> scopes;
};
```

sem alterar a identidade pública de:

```cpp
SymbolId
```

---

## 2.3. Não sacrificar regras nativas pela estabilidade da Plugin API

Regras compiladas junto ao Heimdall são controladas pelo próprio projeto e podem utilizar APIs internas mais eficientes.

Plugins externos possuem requisitos diferentes:

- compatibilidade;
- isolamento;
- estabilidade;
- validação;
- segurança;
- independência da representação interna.

Portanto, regras nativas e plugins não precisam utilizar a mesma interface física.

Devem, entretanto, compartilhar:

- modelo de regras;
- configuração;
- severidade;
- diagnósticos;
- suppressions;
- identificação;
- scheduling;
- métricas.

---

# 3. Arquitetura alvo

A arquitetura conceitual de longo prazo é:

```text
                     Workspace
                         │
                  AnalysisSnapshot
                         │
        ┌────────────────┼────────────────┐
        │                │                │
        ▼                ▼                ▼
     Syntax           Semantic          Project
    Database          Database           Index
        │                │                │
        └────────────────┼────────────────┘
                         │
                 Analysis Context
                         │
             ┌───────────┴───────────┐
             │                       │
             ▼                       ▼
      Built-in Engine            Plugin Host
             │                       │
        Internal API             Public API
             │                       │
             │                  External Rules
             │                       │
             └───────────┬───────────┘
                         │
                  Diagnostic Sink
                         │
                ┌────────┴────────┐
                ▼                 ▼
               CLI               LSP
```

Essa arquitetura não precisa corresponder diretamente à estrutura física de diretórios.

Ela representa principalmente as fronteiras de responsabilidade.

---

# 4. Evolução dos módulos

A arquitetura atual pode evoluir conceitualmente para:

```text
foundation
    ↓
syntax
    ↓
semantic
    ↓
analysis
    ↓
features
    ↓
application
```

## 4.1. Foundation

Responsável por infraestrutura de baixo nível.

Exemplos:

```text
Arena
MappedBuffer
LineTable
StringTable
IDs
containers especializados
utilities
```

Deve permanecer extremamente leve.

Idealmente:

```text
STL
+
código próprio
```

Sem dependências desnecessárias.

---

## 4.2. Syntax

Responsável exclusivamente pela representação sintática.

```text
Lexer
Preprocessor
GrammarParser
ParseTree
SyntaxNode
Token
SourceRange
```

Fluxo:

```text
Source
  ↓
Lexer
  ↓
Preprocessor
  ↓
Parser
  ↓
SyntaxTree
```

Essa camada não deve depender da análise semântica.

---

## 4.3. Semantic

Responsável por atribuir significado à estrutura sintática.

Exemplos:

```text
Binder
Typer
ScopeGraph
SymbolIndex
TypeDatabase
ReferenceIndex
```

Fluxo conceitual:

```text
SyntaxTree
    │
    ▼
 Binder
    │
    ▼
Symbols / Scopes
    │
    ▼
 Typer
    │
    ▼
Types / References
```

A semântica deve ser estruturada de forma que seus dados possam ser invalidados e reconstruídos independentemente sempre que possível.

---

## 4.4. Analysis

Responsável pelas análises realizadas sobre syntax e semantic.

Exemplos:

```text
RuleEngine
SemanticRules
FlowAnalysis
ConstantAnalysis
Modernization
API checks
Documentation checks
```

Essa camada não deve possuir o estado fundamental do projeto.

Ela deve consumir dados fornecidos pelo contexto de análise.

---

## 4.5. Features

Features destinadas diretamente a consumidores da engine:

```text
Completion
Navigation
Hover
References
Rename
Formatter
Code Actions
Signature Help
```

Nem todas as features precisam depender da camada semântica.

Por exemplo:

```text
Formatter
    ↓
Syntax
```

enquanto:

```text
Completion
    ↓
Syntax + Semantic + ProjectIndex
```

---

# 5. Workspace

Uma das principais evoluções propostas é tornar o `Workspace` responsável pelo estado global necessário para análise.

Conceitualmente:

```text
Workspace
│
├── FileDatabase
├── CompilationDatabase
├── IncludeGraph
├── ProjectIndex
├── SyntaxDatabase
├── SemanticDatabase
└── SnapshotManager
```

O `Workspace` conhece:

- documentos abertos;
- arquivos do projeto;
- versões dos documentos;
- compile commands;
- relações entre arquivos;
- caches;
- índices;
- dependências.

Features não devem modificar arbitrariamente o Workspace.

---

# 6. AnalysisSnapshot

`AnalysisSnapshot` representa uma visão consistente do estado da análise.

Exemplo:

```text
Workspace
    │
    ├── Snapshot #40
    ├── Snapshot #41
    └── Snapshot #42
```

Uma requisição LSP pode adquirir:

```cpp
auto snapshot = workspace.snapshot();
```

e trabalhar exclusivamente sobre essa visão.

Exemplo:

```cpp
completion.complete(
    snapshot,
    documentId,
    position
);
```

Isso evita que uma alteração concorrente do documento modifique estruturas utilizadas por uma análise em andamento.

---

## 6.1. Identidade dentro do snapshot

Os dados podem ser representados por IDs compactos:

```cpp
using DocumentId = uint32_t;
using NodeId = uint32_t;
using SymbolId = uint32_t;
using TypeId = uint32_t;
using ScopeId = uint32_t;
using StringId = uint32_t;
```

Esses identificadores podem ser válidos dentro do contexto de determinado snapshot.

Exemplo:

```text
Snapshot #42

DocumentId = 7
NodeId     = 1821
SymbolId   = 93
TypeId     = 17
```

A engine continua livre para alterar sua representação física.

---

# 7. Incrementalidade

Incrementalidade deve possuir prioridade sobre paralelismo intra-documento.

Para uma alteração pequena:

```text
didChange(foo.cpp)
```

o objetivo não deve ser executar novamente:

```text
lex
parse
bind
type
index
rules
```

sobre todo o projeto.

O objetivo deve evoluir para:

```text
change
  ↓
identify affected data
  ↓
invalidate
  ↓
recompute minimum necessary state
```

Conceitualmente:

```text
foo.cpp changed
      │
      ▼
 Syntax invalidated
      │
      ▼
 Semantic invalidated
      │
      ▼
 dependency graph
      │
      ├── foo.cpp
      ├── foo.hpp
      └── dependents
```

---

# 8. Dependency Graph

Para suportar incrementalidade de projeto, dependências devem ser representadas explicitamente.

Exemplo:

```text
main.cpp
   │
   ├── player.hpp
   │      │
   │      └── entity.hpp
   │
   └── world.hpp
```

Mudanças em:

```text
entity.hpp
```

podem invalidar consumidores relevantes.

Entretanto, invalidação não deve automaticamente significar reconstrução completa.

Sempre que possível, utilizar informações resumidas sobre interfaces exportadas.

---

# 9. Header Summary

Headers podem possuir uma representação resumida contendo informações semanticamente relevantes para dependentes.

Exemplo:

```text
HeaderSummary
│
├── exported declarations
├── exported types
├── macros relevantes
├── namespaces
└── semantic fingerprint
```

Após alteração:

```text
header.hpp
```

o Heimdall pode comparar:

```text
old summary
    vs
new summary
```

Se alterações internas não modificarem a interface semanticamente visível, dependentes podem não precisar ser invalidados.

Isso pode reduzir significativamente recomputações em projetos grandes.

---

# 10. Estratégia de concorrência

A estratégia preferencial deve continuar sendo paralelismo em unidades grandes.

Exemplo:

```text
worker 0 → foo.cpp
worker 1 → bar.cpp
worker 2 → baz.cpp
worker 3 → qux.cpp
```

em vez de introduzir sincronização fina dentro de Lexer, Binder ou Typer.

Princípio:

```text
workspace = concurrent
analysis of individual unit = mostly local
```

Isso reduz:

- locks;
- atomics;
- false sharing;
- complexidade;
- contenção;
- comportamento imprevisível.

---

## 10.1. Snapshots e concorrência

Snapshots imutáveis permitem:

```text
                     Snapshot #42
                          │
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
      Completion        Hover          Diagnostics
       thread 1         thread 2        thread 3
```

Enquanto isso:

```text
didChange
   ↓
Workspace
   ↓
Snapshot #43
```

Requisições antigas podem terminar utilizando `Snapshot #42`.

Novas requisições utilizam `Snapshot #43`.

Isso reduz significativamente a necessidade de locks durante leitura.

---

# 11. Eficiência de memória

A evolução arquitetural deve evitar duplicação desnecessária de representações.

Evitar:

```text
Internal AST
    ↓ copy
Public AST
    ↓ copy
Plugin AST
```

Preferir:

```text
Internal Data
     │
     ├── Native views
     │
     └── Public handles
```

---

## 11.1. Handles

Plugins e APIs públicas devem trabalhar principalmente com handles compactos:

```cpp
NodeId
SymbolId
TypeId
StringId
DocumentId
```

Isso permite que múltiplas APIs referenciem os mesmos dados sem duplicação.

---

## 11.2. Strings

Evitar armazenamento repetido de strings.

Preferir interning:

```text
StringTable

0 → "std"
1 → "vector"
2 → "Player"
3 → "update"
```

Estruturas armazenam:

```cpp
StringId
```

em vez de:

```cpp
std::string
```

sempre que adequado.

---

## 11.3. Estruturas orientadas a dados

Hot paths devem ser medidos antes de alterações estruturais.

Quando justificado, estruturas AoS podem evoluir para SoA.

De:

```text
Symbol
├ name
├ type
└ scope

Symbol
├ name
├ type
└ scope
```

para:

```text
names  → N N N N N
types  → T T T T T
scopes → S S S S S
```

A API baseada em IDs permite realizar essa alteração sem afetar consumidores.

---

# 12. API interna

Regras nativas e features internas devem utilizar uma API otimizada.

Exemplo conceitual:

```cpp
class AnalysisContext {
public:
    auto syntax(DocumentId document) const;
    auto symbols(DocumentId document) const;

    const Type& typeOf(SymbolId symbol) const;

    auto references(SymbolId symbol) const;

    ScopeId scopeAt(
        DocumentId document,
        Position position) const;
};
```

A API interna pode retornar:

```cpp
std::span<const Symbol>
const Type&
SyntaxNodeRef
```

e outras representações eficientes.

Ela não possui obrigação de estabilidade entre versões do Heimdall.

---

# 13. Regras nativas

Regras nativas devem permanecer no caminho de maior desempenho.

Elas podem utilizar:

```text
AnalysisContext
    │
    ├── direct views
    ├── spans
    ├── references
    └── internal IDs
```

Não devem ser obrigadas a atravessar a Plugin API.

Isso permite otimizações específicas sem comprometer a estabilidade externa.

---

# 14. Rule Engine

O `RuleEngine` deve ser responsável por coordenar tanto regras nativas quanto plugins.

Conceitualmente:

```text
                     RuleEngine
                         │
                   RuleScheduler
                         │
             ┌───────────┴───────────┐
             ▼                       ▼
       Native Rules              Plugin Rules
             │                       │
       Internal API               PluginHost
             │                       │
             └───────────┬───────────┘
                         ▼
                  DiagnosticSink
```

Os dois tipos de regra compartilham:

```text
RuleId
RuleMetadata
Severity
Configuration
Suppression
Diagnostics
Timing
Profiling
```

---

# 15. Rule Scheduling

Regras não devem necessariamente percorrer individualmente toda a árvore.

Cada regra pode declarar quais elementos deseja observar.

Exemplo:

```cpp
RuleInterest NoRawNew::interest()
{
    return {
        NodeKind::NewExpression
    };
}
```

Outra regra:

```cpp
return {
    NodeKind::FunctionDeclaration,
    NodeKind::MethodDeclaration
};
```

O scheduler constrói os consumidores relevantes:

```text
FunctionDeclaration
    ├── rule A
    ├── rule B
    └── plugin C

NewExpression
    ├── rule D
    └── plugin E
```

A árvore pode então ser percorrida de maneira coordenada.

```text
                     SyntaxTree
                         │
                    traversal
                         │
         ┌───────────────┼───────────────┐
         ▼               ▼               ▼
 FunctionDeclaration  NewExpression    CallExpr
         │               │               │
       R1 R2           R3 P1           R4 P2
```

Isso reduz traversals redundantes e melhora locality.

---

# 16. Plugin Architecture

Plugins devem acessar o Heimdall através de uma API pública controlada.

```text
                 AnalysisSnapshot
                        │
                 Internal API
                        │
                   PluginHost
                        │
                  Plugin API v1
                        │
           ┌────────────┼────────────┐
           ▼            ▼            ▼
        Plugin A     Plugin B     Plugin C
```

Plugins não devem receber diretamente:

```cpp
ParseTree&
SemanticModel&
Symbol&
Type&
```

Devem utilizar:

```cpp
NodeId
SymbolId
TypeId
DocumentId
```

---

# 17. Plugin API

A API deve fornecer queries sobre os dados internos.

Exemplos:

```cpp
NodeKind nodeKind(NodeId);

SourceRange nodeRange(NodeId);

SymbolId resolveSymbol(
    DocumentId,
    Position);

TypeId symbolType(SymbolId);

StringId symbolName(SymbolId);
```

Entretanto, APIs excessivamente granulares podem causar overhead.

Por isso também devem existir operações em lote.

Exemplo:

```cpp
NodeSpan children(NodeId);

NodeSpan descendants(NodeId);

SymbolSpan symbolsInScope(
    DocumentId,
    Position);

ReferenceSpan references(SymbolId);
```

Objetivo:

```text
evitar 100.000 crossings de API
```

quando uma única query pode executar a operação internamente.

---

# 18. Estratégia de plugins

A implementação pode evoluir em etapas.

## Etapa inicial

Validar a API utilizando plugins compilados junto ao projeto.

Objetivo:

```text
validar modelo
antes de estabilizar ABI
```

## Etapa intermediária

Introduzir uma C ABI estável, caso necessário.

Exemplo conceitual:

```text
plugin.so
   │
   ▼
C ABI
   │
PluginHost
```

## Etapa futura

Avaliar execução através de WebAssembly.

```text
Heimdall
   │
PluginHost
   │
WASM Runtime
   │
   ├── plugin-a.wasm
   ├── plugin-b.wasm
   └── plugin-c.wasm
```

WASM pode oferecer:

- isolamento;
- portabilidade;
- controle de memória;
- controle de capabilities;
- estabilidade independente da ABI C++;
- prevenção de crashes diretos no processo principal.

A adoção de WASM deve ser baseada em benchmarks e necessidades reais.

## 18.1. Sequência recomendada para ABI e WASM

A decisão inicial é manter a Plugin API experimental, sem estabilizar uma ABI
prematuramente. C ABI fornece interoperabilidade binária; WASM fornece uma
fronteira de execução mais controlável. São objetivos diferentes.

1. **Validar o contrato de queries com plugins reais compilados na build.**
   Exercitar regras sintáticas e semânticas, handles, lifetime dos snapshots,
   configuração, suppressions, diagnósticos e operações em lote. Ajustar a API
   enquanto não houver compromisso de compatibilidade binária.
2. **Medir antes de escolher o mecanismo externo.**
   Comparar execução nativa e experimental, número de queries/crossings, latências
   P95/P99 e memória. Incluir o custo de inicialização, atualizações incrementais
   e cancelamento, além do tempo de execução da regra. Registrar hardware,
   corpus e orçamento de desempenho para permitir comparações reproduzíveis.
3. **Introduzir C ABI para plugins confiáveis, se houver necessidade de
   carregamento independente.**
   Só avançar após validar o contrato e os benchmarks. Usar tabela de funções
   versionada, tamanhos explícitos de estruturas, tipos de largura fixa, handles,
   ranges, strings UTF-8 com comprimento e códigos de erro. Documentar ownership,
   lifetime e convenção de chamada. Não expor STL, classes virtuais, allocators
   ou representações internas; nenhuma exception pode atravessar a fronteira.
4. **Introduzir WASM quando houver necessidade concreta de executar código não
   confiável.**
   Se plugins de terceiros sem revisão fizerem parte do produto, priorizar WASM
   antes de permitir seu carregamento nativo. Escolher o runtime por benchmarks
   e requisitos de isolamento. Definir limites de memória e execução,
   capabilities explícitas, validação de handles/resultados e cancelamento
   apoiado pelo runtime. Não conceder filesystem, rede ou processos implicitamente.
5. **Manter os adaptadores sobre o mesmo modelo de análise e regras.**
   C ABI e WASM, caso ambos sejam necessários, compartilham configuração,
   scheduling, severidade, suppressions, diagnósticos e métricas. Regras nativas
   continuam usando a API interna diretamente. O runtime WASM e o carregador
   externo ficam fora do core STL-only, em targets separados.

## 18.2. Consequências das escolhas

| Escolha | Benefícios | Custos e riscos |
|---|---|---|
| Plugins compilados na build | Simplicidade, desempenho nativo e liberdade para alterar a API | Exigem recompilação; não oferecem isolamento |
| C ABI (`.dll`/`.so`) | Carregamento independente, interoperabilidade entre linguagens e baixo overhead potencial | Binários por plataforma/arquitetura; compromisso de compatibilidade; código nativo pode bloquear, comprometer memória ou derrubar o servidor |
| WASM | Artefato mais portátil, isolamento de memória e controle de capabilities/recursos | Runtime, memória e inicialização adicionais; queries cruzam a fronteira de execução; desempenho precisa ser medido |
| C ABI + WASM | Caminhos distintos para plugins confiáveis e terceiros | Dois mecanismos para manter, testar e documentar; adotar apenas com necessidade demonstrada |

`extern "C"` sozinho não garante uma ABI estável. Capturar exceptions de plugins
nativos também não os transforma em um sandbox. Execução nativa em processo
separado é uma alternativa de isolamento, mas acrescenta IPC e gerenciamento de
processos e deve ser avaliada se necessária.

WASM não elimina todos os riscos: imports do host também precisam respeitar
limites e não bloquear indefinidamente. A API deve favorecer queries em lote e
copiar somente os resultados necessários para a memória do plugin, sem criar
uma segunda AST completa.

---

# 19. Features

Features devem consumir `AnalysisSnapshot` em vez de controlar diretamente o ciclo de parsing e análise.

Exemplo:

```cpp
completion.complete(
    snapshot,
    document,
    position);
```

Navigation:

```cpp
navigation.definition(
    snapshot,
    document,
    position);
```

References:

```cpp
references.find(
    snapshot,
    symbol);
```

Isso permite reutilização consistente entre:

```text
CLI
LSP
tests
future integrations
```

---

# 20. LSP

O LSP deve permanecer uma camada de adaptação.

Responsabilidades:

```text
JSON-RPC
document lifecycle
request cancellation
LSP types
client capabilities
publishing diagnostics
```

Não deve possuir lógica semântica exclusiva quando ela puder residir na engine.

Fluxo desejado:

```text
LSP request
    │
    ▼
Workspace
    │
    ▼
AnalysisSnapshot
    │
    ▼
Feature
    │
    ▼
Result
    │
    ▼
LSP conversion
```

Assim a mesma feature pode futuramente ser consumida por outros frontends.

---

# 21. CLI

A CLI continua sendo otimizada para processamento batch.

```text
files
  │
  ├── worker 0
  ├── worker 1
  ├── worker 2
  └── worker N
```

O objetivo é reutilizar os mesmos componentes fundamentais do LSP sem obrigar a CLI a carregar infraestrutura desnecessária.

---

# 22. Métricas

Mudanças arquiteturais relacionadas a desempenho devem ser acompanhadas por benchmarks.

Métricas mínimas:

```text
parse time
bind time
type analysis time
rule execution time
peak RSS
allocations
cache reuse
incremental update latency
completion latency
diagnostic latency
project indexing time
```

Para LSP, observar principalmente percentis:

```text
P50
P95
P99
```

em vez de apenas média.

---

# 23. Memory Budget

Subsistemas importantes devem possuir métricas próprias.

Exemplo:

```text
Workspace

Source buffers          X MB
Syntax trees            X MB
Symbols                 X MB
Types                   X MB
References              X MB
String table            X MB
Project index           X MB
Snapshots retained      X MB
────────────────────────────
Total                   X MB
```

Isso permite identificar regressões antes que se tornem problemas arquiteturais.

---

# 24. Snapshot Lifetime

Snapshots antigos não podem permanecer vivos indefinidamente.

Exemplo:

```text
Snapshot #40 ── request A
Snapshot #41 ── request B
Snapshot #42 ── current
```

Quando:

```text
request A finished
```

e nenhum consumidor utilizar `#40`, seus dados exclusivos podem ser liberados.

Estruturas imutáveis podem compartilhar armazenamento quando vantajoso:

```text
Snapshot #41 ─────┐
                  ├── shared syntax data
Snapshot #42 ─────┘
```

O objetivo é obter consistência sem duplicar o workspace inteiro a cada alteração.

---

# 25. Compatibilidade e versionamento

A arquitetura deve diferenciar claramente:

```text
Internal API
Public API
Plugin API
```

### Internal API

Pode mudar livremente junto ao código do Heimdall.

### Public API

Deve possuir política explícita de compatibilidade.

### Plugin API

Deve ser versionada.

Exemplo:

```text
Heimdall Plugin API v1
```

Plugins declaram:

```text
required_api = 1
```

O `PluginHost` valida compatibilidade antes de carregar o plugin.

---

# 26. O que evitar

A evolução deve evitar alguns padrões.

## Não criar uma segunda AST pública

Evitar:

```text
ParseTree
   ↓ conversion
PublicAST
```

a menos que exista justificativa concreta.

Preferir views e handles.

## Não expor estruturas internas como ABI

Evitar:

```cpp
extern "C" Symbol* heimdall_get_symbol(...);
```

Preferir:

```cpp
SymbolId
```

## Não transformar tudo em interfaces virtuais

A arquitetura não deve evoluir para:

```text
IFoo
IBar
IBaz
IWhatever
```

sem necessidade.

Polimorfismo deve ser introduzido quando existir uma fronteira real.

## Não paralelizar tudo

Concorrência fina pode destruir locality e introduzir overhead superior ao ganho obtido.

## Não introduzir incrementalidade sem medição

Caches possuem custo.

Um cache que custa mais memória e invalidação do que a computação original não deve existir.

---

# 27. Roadmap de evolução

A evolução deve acontecer em fases independentes.

## Fase 1 — Fronteiras arquiteturais

Formalizar conceitualmente:

```text
foundation
syntax
semantic
analysis
features
application
```

Sem necessariamente criar imediatamente novos targets CMake.

Objetivo:

reduzir dependências incorretas antes de mover arquivos.

---

## Fase 2 — AnalysisContext

Introduzir uma API interna centralizada para consultas de análise.

```text
AnalysisContext
```

Migrar regras gradualmente para ela.

Objetivo:

evitar que regras dependam diretamente de detalhes internos de Binder, Typer e índices.

---

## Fase 3 — Workspace

Centralizar estado de projeto.

Introduzir:

```text
DocumentId
FileDatabase
document versions
CompilationDatabase
ProjectIndex
IncludeGraph
```

Objetivo:

criar uma fonte consistente de estado.

---

## Fase 4 — AnalysisSnapshot

Criar snapshots imutáveis para operações de leitura.

Migrar:

```text
Completion
Navigation
Diagnostics
```

para trabalhar sobre snapshots.

Objetivo:

simplificar concorrência e preparar incrementalidade.

---

## Fase 5 — Incrementalidade

Adicionar invalidação granular.

Inicialmente:

```text
document-level invalidation
```

Depois, somente se benchmarks justificarem:

```text
syntax-level
semantic-level
dependency-level
```

Não começar pela granularidade máxima.

---

## Fase 6 — Rule Scheduler

Permitir que regras declarem interesses.

Reduzir traversals redundantes.

Adicionar métricas por regra:

```text
execution count
total time
average time
P95
diagnostics emitted
```

---

## Fase 7 — Plugin API experimental

Definir handles:

```text
DocumentId
NodeId
SymbolId
TypeId
StringId
```

Criar `PluginContext`.

Inicialmente executar plugins experimentais dentro da própria build.

Objetivo:

validar a API sem assumir compromisso prematuro de ABI.

---

## Fase 8 — Plugin ABI

Depois que a API estiver estabilizada, escolher mecanismo externo.

Possibilidades:

```text
C ABI
WASM
C ABI + WASM
```

A decisão deve ser tomada com benchmarks.

---

## Fase 9 — Otimização estrutural

Somente depois de profiling:

```text
AoS → SoA
specialized arenas
compact IDs
string interning
memory pools
compressed indexes
shared immutable storage
```

O uso de handles permitirá essas mudanças sem alterar APIs externas.

---

# 28. Critério para novas abstrações

Toda nova abstração deve responder pelo menos uma destas perguntas:

```text
Reduz acoplamento real?

Permite incrementalidade?

Reduz memória?

Reduz CPU?

Melhora locality?

Permite concorrência segura?

Permite uma nova feature?

Estabiliza uma fronteira pública?
```

Se a resposta for negativa para todas elas, provavelmente a abstração não deve ser adicionada.

---

# 29. Prioridade arquitetural

A prioridade recomendada é:

```text
1. preservar performance atual
        ↓
2. organizar fronteiras
        ↓
3. AnalysisContext
        ↓
4. Workspace
        ↓
5. AnalysisSnapshot
        ↓
6. incrementalidade
        ↓
7. scheduling eficiente
        ↓
8. Plugin API
        ↓
9. ABI/runtime externo
        ↓
10. otimizações avançadas orientadas por profiling
```

Plugins não devem dirigir prematuramente a arquitetura.

A arquitetura deve primeiro possuir uma fronteira consistente de análise. A Plugin API então se torna uma adaptação dessa fronteira.

---

# 30. Resultado esperado

Ao final dessa evolução, o Heimdall deve permitir:

```text
                        Heimdall
                           │
                       Workspace
                           │
                   AnalysisSnapshot
                           │
           ┌───────────────┼───────────────┐
           │               │               │
         Syntax         Semantic        Project
           │               │               │
           └───────────────┼───────────────┘
                           │
                    AnalysisContext
                           │
           ┌───────────────┼───────────────┐
           │               │               │
        Analysis        Features       PluginHost
           │               │               │
      Native Rules     Completion       Plugin API
                       Navigation            │
                       Formatter         External
                       Rename             Plugins
                       Hover
           │               │               │
           └───────────────┼───────────────┘
                           │
                    Diagnostic / Result
                           │
                     ┌─────┴─────┐
                     ▼           ▼
                    CLI         LSP
```

O objetivo não é maximizar abstração.

O objetivo é manter o Heimdall:

- rápido;
- previsível;
- eficiente em memória;
- incremental;
- concorrente onde houver benefício;
- extensível;
- compatível com novas features;
- independente de detalhes de frontend;
- capaz de evoluir suas estruturas internas sem comprometer APIs externas.

A principal diretriz é preservar a liberdade de otimização interna.

`Workspace` fornece o estado.

`AnalysisSnapshot` fornece consistência.

`AnalysisContext` fornece acesso eficiente aos dados.

IDs fornecem identidade sem expor representação.

Regras nativas utilizam o caminho interno de alto desempenho.

Plugins utilizam uma fronteira pública estável.

O `RuleEngine` coordena ambos.

CLI e LSP permanecem consumidores da engine, e não proprietários da lógica de análise.

Essa estrutura permite que o Heimdall cresça em complexidade sem exigir que sua arquitetura cresça na mesma proporção.
