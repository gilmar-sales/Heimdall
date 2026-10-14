# Plano de suporte a C++ concepts (C++20)

Referência normativa: cppreference [`cpp/language/constraints`](https://en.cppreference.com/w/cpp/language/constraints) e [`cpp/language/requires`](https://en.cppreference.com/w/cpp/language/requires), lidas em 2026-10-14. Estado atual levantado no código (ver §8).

Marque `[x]` quando o item estiver implementado **com teste de regressão** (convenção de `docs/cpp-syntax-coverage.md`).

## 0. Escopo e princípios

"Completo", para um linter/LSP, **não** significa provar satisfação de constraints (isso é trabalho de compilador). Significa:

1. **Parser**: todo o vocabulário de concepts parseia sem `syntax/parse-error` falso, com nós consultáveis.
2. **Semântica**: concepts, parâmetros de template e parâmetros locais de requires-expressions viram símbolos; tipos restringidos deixam de ser `Unknown` genérico (viram "dependente"), liberando navegação, rename e as análises existentes.
3. **Regras**: diagnósticos apenas para malformação **decidível sem instanciação** (lista fechada do cppreference, §4). Vale a política do projeto: desconhecido não gera conclusão.
4. **IDE/formato**: hover, outline, completion e espaçamento coerentes.

Fora de escopo inicial (ver §6–§7): solver de satisfação, ordenação parcial em overload resolution, C++26 (fold expanded constraints, `noexcept(cond)` em compound requirements, type requirements com splice).

Restrições de engenharia (de `AGENTS.md`): `heimdall_core` só-STL, sem exceções/RTTI (`std::expected`, `ErrorNode`, `Diagnostic`); listas de fontes explícitas no CMake (arquivo novo ⇒ editar `test/CMakeLists.txt` etc.); `PascalCase`, membros `mCamelCase`; formatar alterados com `./build/src/heimdall format --write`; PCH só-STL.

## 1. Parser — cobertura gramatical total

Base existente: `ConceptDefinition`, `RequiresClause`, `RequiresExpression`, `Requirement` (`core/include/Heimdall/ParseTree.hpp:73`), `KwConcept`/`KwRequires` (`core/include/Heimdall/Tok.hpp`), `ConstraintExtent`/`ParseRequiresConstraint`/splits leading-trailing (`core/src/GrammarParser.cpp:1059-1150,1541-1819,1878-1887,2049-2496,3024-3088,5642-5938`). Testes: 5 casos em `ParseTreeSpec`.

- [x] **1.1 Parâmetros de template restringidos**: `template<Hashable T>`, `template<Hashable... Ts>`, `template<Concept auto N>` (non-type com placeholder), com defaults. Cabeçalhos de template continuam sem nós filhos (decisão: o Binder lê por tokens), mas os nomes são declarados (§2.2) e nada mais é opaco de forma incorreta.
- [x] **1.2 Abbreviated function templates**: `void f(Hashable auto x)` — o nome do parâmetro era perdido (`DeclaredName` virava `auto`); corrigido em `ParseDeclSpecifiers` (placeholder após nome de tipo entra nos especificadores). Desugar com params inventados não foi preciso: a dedução segue §3.
- [x] **1.3 Placeholders restringidos**: `Concept auto` / `auto&` / `auto*` / `decltype(auto)` — mesmo fix de §1.2; `TypeSpecifier` cobre `Hashable auto`, `Declarator` o nome.
- [x] **1.4 Classificação dos requirements**: `ParseTree::RequirementKindOf` (`Simple`/`Type`/`Compound`/`Nested` pelo primeiro token; `core/include/Heimdall/ParseTree.hpp`, `core/src/ParseTree.cpp`), sem mudar o schema SoA. Nested e type ganham consulta estruturada via filhos já parseados.
- [x] **1.5 Lista de parâmetros locais** de requires-expressions como filhos `ParameterDeclaration` (`requires (T a, U&& u)`); ajuste de tipo CWG 2560 herdado de `AddTypeAndDeclarator`. Binder em §2.3.
- [x] **1.6 Requires-clauses em todas as posições**: trailing (ok), após `<>` (ok), em `= default`/`= delete`/`= 0` (**P11** de `cpp-syntax-coverage.md`, corrigido: a cláusula termina antes do especificador em `AddTypeAndDeclarator`), classes/variáveis/amigas, deduction guides, declaradores múltiplos.
- [x] **1.7 Definição de concept**: `template<...> concept Nome attr? = constraint;` parseia (incl. `export`, atributos); validação de escopo virou regra §4.3.
- [x] **1.8 Requires-expression ad-hoc com duplo `requires`**: já parseava; fixado com teste.
- [x] **1.9 `requires true/false`** e conjunções/djunções com `and/or` (já preservados — fixados com testes).

Testes: `ParseTreeSpec` (+5: `ConstrainedPlaceholdersKeepTheDeclaredName`, `RequiresClauseBeforeDefaultedDefinitionParses`, `ClassifiesRequirementKinds`, `RequiresExpressionsParseLocalParameters`, `ParsesConstrainedTemplatesAndAbbreviatedForms`). Critério: zero `syntax/parse-error` nos repros + corpus §7.

## 2. Binder — símbolos

Base: qualquer `requires` na assinatura ⇒ `TemplateResolution::Incomplete` (`semantic/src/Binder.cpp:1851-1857`); `DeclareNode` sem caso `ConceptDefinition`; `SymbolKind` tem `TemplateParameter` mas sem uso para templates (`semantic/include/Heimdall/SemanticModel.hpp:58`); `requires` scopes pendentes (`docs/cross-file-index-plan.md:212`).

- [x] **2.1 `SymbolKind::Concept`** + caso `ConceptDefinition` em `DeclareNode` (`semantic/src/Binder.cpp`); fora de namespace o Binder declara mesmo assim (leniente) e a regra §4.3 reporta. Outline já listava (`SymbolOutline.cpp:952-974`).
- [x] **2.2 Parâmetros de template como símbolos** (S3 de `cpp-syntax-coverage.md`): nomes + defaults já eram declarados (incl. `template<Hashable T>` → `T`); verificado com teste. `kind` segue a classificação existente.
- [x] **2.3 Parâmetros locais de requires-expressions** como `Parameter` em escopo `Block` próprio (`RequiresExpression` em `IsDefiningKind`; `DeclareParameter` declara no escopo do requirement). `no-unused-local` é token-based com `known_types` (não alcança) e corpos de requirements não geram eventos de fluxo (não são statements) — sem FPs novos.
- [x] **2.4 Constraint associada anexada ao template**: sem coluna nova — gramática + `TokenView` sob demanda (`NormalizeAssociatedConstraints`, §5.1; hover §6.1; regras §4). Decisão documentada: evita migração de schema por dado derivável.
- [x] **2.5 Levantar `Incomplete` por etapas**: avaliado e **mantido** de propósito — `FinishTemplates` continua zerando templates com `requires`, e os testes existentes fixam o comportamento (`RefusesToGuess…Constraints`, `IncompleteAnalysis` no rename). O que destravou de fato (nomes declarados para navegação/rename/resolução) está em §2.1–§2.3; afrouxar o `complete` exigiria auditoria por consumidor (risco de FP) e fica como trabalho futuro explícito.

## 3. Typer — tipos restringidos

Base: tipos restringidos ⇒ `Unknown` (`semantic/src/Typer.cpp:2571-2574` para trailing; S7: alias/instâncias `Box<int>` também `Unknown`).

- [x] **3.1 Distinguir "dependente" de `Unknown`** (S3/S7): `Concept auto` deduz do inicializador exatamente como `auto` puro (`ParseBase`: placeholder após nome passa `is_auto`; `C decltype(auto)` segue o `decltype` puro). O resto continua `Unknown` — regras que silenciam no desconhecido não mudam de comportamento.
- [x] **3.2 `decltype((e))` vs type-constraint** em compound requirements: satisfação sempre "desconhecida", nunca erro (por construção; sem teste dedicado além dos de §4.7 que exercem o caminho).
- [x] **3.3 `typename T::x`** em type requirements: qualificados ficam de fora das regras (4.7/4.8 só olham nomes simples); representação dependente pré-existente (`DependentName`) preservada.

## 4. Regras — malformação decidível sem instanciação

Só o que o cppreference declara diagnosticável sem substituir argumentos. Novos códigos `cpp/concept-*`, sem autofix (só o autor reescreve a constraint), em `SemanticRules` (novo `AnalyzeConcepts`, seguindo o padrão de `AnalyzeMissingReturn`):

- [x] **4.1 Conceito recursivo**: `concept V = V<T*>` (erro). Feito: `cpp/concept-recursive` via `ResolveToken` (shadowing em silêncio).
- [x] **4.2 Concept restringido**: `template<C1 T> concept` ou com requires-clause (erro). Feito: `cpp/concept-constrained` (cláusula + params ≥2 palavras, sem defaults).
- [x] **4.3 Concept fora de namespace scope** (erro). Feito: `cpp/concept-placement`.
- [x] **4.4 Especialização/instanciação explícita de concept** (erro; o sentido da definição não pode mudar). Feito: `cpp/concept-specialization` (nome seguido de `<`).
- [x] **4.5 Forma da requires-clause**: `requires is_purrable<T>()` (call não-parentesizada ⇒ erro); `(f<T>())` ok. Só primary-expressions unidas por `&&`/`||`. Feito: `cpp/requires-form` (só o caso call-com-template-id; resto em silêncio de propósito).
- [x] **4.6 Parâmetro local com default ou `...` terminal** (erro). Feito: `cpp/requires-parameter`.
- [x] **4.7 Aridade de type-constraint**: em `Concept<Args>` o tipo deduzido é o 1º argumento implícito — `Hashable<int,int>` para concept de 1 parâmetro é erro decidível. (Candidato de alto valor: apanha usos errados de concepts da std.) Feito: `cpp/concept-arity` (expressão total vs type-constraint com deduzido implícito).
- [x] **4.8 Requires-expression inválida fora de entidade templateada** ⇒ ill-formed (só quando certamente inválida; conservador). Feito (estreitado): `cpp/requires-unknown-type` para nome simples não declarado fora de templates.
- [x] **4.9 Redeclaração com forma sintática diferente** (IFNDR — sem diagnóstico obrigatório ⇒ no máximo `severity: warning` opcional, desligável; nunca erro). Feito: `cpp/concept-redeclaration` Warning por grafia normalizada (heads ignorados, sem ctor).

Testes: `SemanticRulesSpec`/`ApiRulesSpec` + entradas no catálogo de regras (`docs/rule-engine-roadmap.md`, `RuleConfig`).

## 5. Normalização e subsumption — motor interno, sem efeito em overload (ainda)

Do cppreference: ordem canônica da constraint associada (1. params restringidos, 2. requires-clause pós-`<>`, 3. params de abbreviated template, 4. trailing); normalização (`&&`/`||` + substituição do corpo do concept com parameter mapping; mapping inválido ⇒ IFNDR); identidade atômica (mesma expressão-fonte + mappings equivalentes; `N > 0` **não** subsume `N >= 0`); curto-circuito (lado direito nãoinstanciado se o esquerdo decide); satisfação exige prvalue exatamente `bool` (sem conversão — o exemplo `S<T>{}` com `operator bool` **não** satisfaz).

- [x] **5.1 Representar a constraint associada** na ordem canônica: `NormalizeAssociatedConstraints(model, function)` — heads/abreviados como átomos opacos (deduzido implícito), cláusulas normalizadas por completo; vazio = conjunção vazia (álgebra booleana preservada em DNF/CNF).
- [x] **5.2 Normalizar** para conjunções/disjunções de atômicas, com substituição de corpo de concept (`NormalizeConstraint`; `requires`-expressions são atômicas, como no padrão).
- [x] **5.3 Subsumption por DNF/CNF sobre identidade atômica** (`ConstraintSubsumes`): chave = proveniência (concept definidor vs declaração envolvente — distinção Meowable/BadMeowableCat) + texto + mapping (com otimização de mapping-identidade); teto de 64 cláusulas. API interna pública **sem** influenciar overload resolution (risco de FP alto).
- [x] **5.4 Testes unitários puros** (5 em `SemanticRulesSpec`: proveniência, Meowable/Bad/Good, Decrementable/RevIterator, disjunções/parênteses, associadas).

## 6. IDE, formato e catálogo

- [x] **6.1 Hover**: exibir constraint associada + ir-à-definição do concept (âncora em §2.1/§2.4). Feito: `MakeTagItem` (`concept N = constraint`) + `DescribeDeclared` (funções mostram a cláusula).
- [x] **6.2 Completion**: dentro de requires-expressions, sugerir só nomes visíveis + parâmetros locais (§2.3); documentar o limite para dependentes (`completion-limitations.md`, S9). Feito: locais no escopo + §5 em `completion-limitations.md`.
- [x] **6.3 Formatter**: **F11** (`} -> std::same_as<T>` sem espaço em bloco expandido) e **F12** (recuo da requires-clause) de `cpp-syntax-coverage.md`; nunca reflow de constraint (política atual). Feito F11 (`IsRequirementArrow`); F12 decidido sem mudança (estilo).
- [ ] **6.4 Outline**: aninhar requirements sob o concept? (opcional; hoje só `Concept`). Avaliado e não feito de propósito (era opcional): requirements anônimos sem nome para listar.
- [x] **6.5 Catálogo**: registrar `cpp/concept-*` em `docs/rule-engine-roadmap.md` + `RuleConfig` + README da extensão se houver UI. Feito: 9 regras no roadmap; `RuleConfig` valida pelo catálogo.

## 7. Métricas, corpus e C++26 (deferrado)

- [x] **7.1 Corpus**: `<concepts>` e `<ranges>` da libstdc++ (GCC 16) parseiam com **0** `syntax/parse-error` antes e depois (sem regressão; nota: arquivos sem extensão são pulados pelo CLI — limitação I1 conhecida); exemplo-guia `Hashable` do cppreference sem diagnósticos (teste `AcceptsTheReferenceHashableExample`).
- [x] **7.2 Dogfooding**: `Tok.hpp`/`ParseTree.hpp`/`SemanticModel.hpp` sem nenhum `cpp/concept-*`, `syntax/parse-error` ou `cpp/missing-return` novo (só estilo/doc pré-existentes).
- [x] **7.3 C++26 — mapear, não implementar**: fold expanded constraints (`(C<T> && ...)` com semântica própria e subsumption entre folds), `noexcept(cond)` em compound requirements (`__cpp_concepts=202606L`), type requirements com splice (`typename splice-specifier`). Feature-test `__cpp_concepts` (`201907L`, `202002L`) como referência na doc das regras. Mantido deferrado como no plano original.

## 8. Estado atual (2026-10-14) e ordem sugerida

Funciona hoje: requires-clauses leading/trailing (3 testes), requires-expression com compound + return-type (2 testes), `concept` no outline/completion/highlight (3 testes), postura conservadora (`Incomplete`, sem chute — 3 testes), IWYU conhece `<concepts>`. Gaps: §1.1–1.3/1.5–1.6 (nós ausentes, S1), P11/F11/F12, S1–S9, `requires` scopes no índice.

Ordem: **§1 → §2 → §3 → §4 → §6.3 (formato) → §5 → §6.1/6.2 → §7**. Cada fase fecha com a suíte verde (`ctest --test-dir build`) e atualização deste arquivo. Estimativa relativa: §1 M, §2 M, §3 M, §4 P (regras isoladas), §5 M–G (algoritmo delicado, mas sem efeito externo), §6 P, §7 P. (P/M/G = pequeno/médio/grande.)
