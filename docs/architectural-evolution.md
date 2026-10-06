# Evolução arquitetural: implementação e limites

Implementação incremental de **Heimdall — Plano de Evolução Arquitetural.md**.
O documento original continua sendo a proposta; esta página registra o que existe
no código e o que ainda depende de migração, medições ou decisões futuras.

## Fronteiras

| Camada | Localização | Dependências permitidas |
|---|---|---|
| Foundation | `core`: Arena, buffers, LineTable, IDs | STL / plataforma |
| Syntax | `core`: lexer, preprocessor, ParseTree | foundation |
| Semantic | `semantic`: Binder, Typer, summaries, índices | syntax; simdjson privado para compile DB |
| Analysis | `core` e `semantic`: regras; `analysis`: coordenação | syntax / semantic |
| Features | `core`: features locais; `analysis`: entry points de snapshot | syntax / semantic / project |
| Application | `src`, `lsp` | engine / config; protocolo e I/O |

`heimdall::analysis` é o target compartilhado de coordenação. Não altera a política
STL-only, sem exceptions e sem RTTI do core. Os testes `ArchitecturePolicy` impedem
dependências ascendentes de core/semantic para analysis. Não foi criada outra AST.

## APIs e lifetime

```cpp
heimdall::Workspace workspace;
auto source = std::make_shared<const std::string>("int value;\n");
auto opened = workspace.Open("example.cpp", source, 1);
if (!opened) return;
auto snapshot = workspace.Snapshot();
heimdall::AnalysisContext context(snapshot, *opened);
auto completion = heimdall::AnalysisFeatures::Complete(context, 9);
auto diagnostics = heimdall::AnalysisEngine().Analyze(context);
```

- `Workspace` possui documentos, versões, compile DB, grafo de includes/dependências
  e o índice de projeto. As escritas são serializadas; parse/bind/type são locais
  ao documento e executados fora do lock do workspace.
- Snapshots compartilham documentos não alterados. A publicação de uma revisão
  não altera buffers/opções/grafo de revisões antigas.
- Caches lazy são logicamente imutáveis e protegidos por locks por documento.
  `AnalysisContext::Syntax()` é uma referência direta ao parse já pinado: não
  adquire um lock por nó no hot path.
- `Semantic()` retém sua árvore; `Types()` retém o modelo e a árvore. Mesmo um
  resultado mantido após o fechamento do workspace continua válido.
- Não há histórico proprietário de snapshots. Dados antigos são liberados quando
  o último consumidor termina. Uma atualização pode reter **uma** árvore-base até
  o próximo parse; a referência-base é descartada depois da reutilização.
- IDs de documento não são reciclados durante a vida de um workspace. IDs de
  nó/símbolo/tipo/string são locais a um documento e snapshot; não são identidades
  persistentes entre edições nem entre workspaces.
- `WithSyntax` incorpora o resultado do parser assíncrono/incremental existente,
  verifica fonte/dialeto/cancelamento e evita parse e cópia de fonte adicionais.
  As opções fornecidas devem ser exatamente as utilizadas pelo parser.
- `FromSyntax` permite batch sem carregar um workspace de projeto. Para árvores
  emprestadas, o consumidor deve manter a fonte viva. `Source()` pode ser nulo;
  `Syntax().Source()` fornece a view. A CLI mantém o mapped buffer até o fim da
  análise e não copia a fonte para adaptar esse caminho.

As APIs tree-based anteriores permanecem disponíveis. A API interna C++ pode
evoluir junto à engine; não há promessa de ABI C++ binária. A Plugin API v1 é
**experimental**, compatível apenas em nível de fonte nesta etapa.

## Incrementalidade e projeto

- Uma atualização invalida o documento editado e a semântica dos dependentes
  transitivos. A sintaxe dos dependentes é preservada. Ciclos são percorridos uma
  vez através das arestas reversas.
- Alterações só de versão com texto idêntico reutilizam os dados existentes.
- O workspace calcula um edit hull e usa `ParseReuse` para itens top-level não
  afetados. Não foi introduzido outro parser incremental.
- Includes literais de arquivos **registrados** são ligados automaticamente:
  diretório local, `-iquote`, depois `-I`, respeitando quoted/angled. Alterações no
  bloco de includes, registro/fechamento de arquivos e compile DB reconstroem as
  arestas. `SetDependencies` permite fornecer uma closure externa explicitamente.
- Isso não substitui a descoberta de headers em disco, a expansão completa de
  includes via macros ou o monitoramento de arquivos ainda não registrados.
  O LSP mantém seus workers de descoberta/indexação e seus testes existentes.
- `Summary` reutiliza o modelo já bound, sem parse/bind duplicados.
  `Project()` constrói lazy um índice de `.h/.hpp/.hh/.hxx` registrados. Índices
  externos podem ser publicados com `SetProjectIndex`; mudanças de fonte/opções
  invalidam a visão externa para não servir exports antigos como atuais.
- `HeaderSummary::SemanticFingerprint` é **conservador**: inclui todos os bytes.
  Não se considera que dois headers sejam semanticamente iguais só porque
  exportam os mesmos nomes. Macros, inline bodies, layouts e `__LINE__` impedem
  essa otimização sem uma representação de interface mais completa.

## Regras e plugins

`AnalysisEngine` usa a mesma política de configuração, severity e suppressions
para built-ins, regras nativas registradas e plugins experimentais.

`RuleScheduler` compila interesses em consumidores por `GrammarKind` e faz uma
única travessia SoA para regras registradas. Regras nativas recebem o
`AnalysisContext` diretamente; plugins recebem apenas `PluginContext`, IDs,
queries e diagnósticos públicos validados. Queries de children, descendants,
símbolos e referências fazem operações em lote.

`PluginHost` verifica versão, nomes/códigos duplicados, callbacks/interesses e
shadowing de regras built-in. Registro é transacional. Ranges inválidos são
rejeitados. Exceptions de callbacks externos são contidas e removem os resultados
da regra que falhou, sem interromper outras regras. Cancelamento descarta os
diagnósticos parciais.

Plugins nesta etapa são compilados na própria build: os testes exercitam plugins
reais sobre o host e o `AnalysisEngine`. **Não** são um sandbox: código confiável
pode bloquear, alocar sem limite ou causar um crash nativo. Callbacks com estado
mutável devem garantir a própria segurança quando usados concorrentemente.
Configuração de códigos experimentais pode ser fornecida por `RuleOptions`;
o loader JSON/CLI ainda valida o catálogo estático de regras shipped.

As regras legadas conservam suas passagens especializadas sobre tokens/modelos.
Sua migração para callbacks por interesse permanece gradual; não foram forçadas
à interface pública de plugins nem reescritas indiscriminadamente.

O adaptador interno `analysis/src/SemanticRuleDispatch.cpp` seleciona as 19 regras
de `SemanticRules` antes de solicitar modelos. Os domínios dos callbacks declaram
os requisitos: binding, typing, fluxo, projeto ou documentação. Códigos e
severidades continuam vindo do catálogo; defaults/overrides e opt-in são resolvidos
pelo `RuleEngine`. Sem regras selecionadas, não há bind/type; regras só de símbolos
não solicitam typing. Fluxo é unit-local, construído uma vez por análise somente
para `modernize-const`/`modernize-constexpr`, sem novo cache permanente no snapshot.
CLI/LSP usam esse dispatch via `AnalysisFeatures::Diagnostics`; as APIs agregadas
anteriores permanecem disponíveis, mas não são executadas em paralelo no caminho
normal. O carregamento de profiles e as três regras de `IncludeAnalyzer` ainda
permanecem nos frontends.

O [plano dedicado de migração das regras](rule-migration-plan.md) define etapas,
requisitos, estratégias por família, critérios de equivalência e validação de
desempenho. A seleção antecipada das regras semânticas está implementada; a
migração de traversals para o scheduler continua condicionada a profiling.

## Métricas e validação

- `AnalysisSnapshot::Metrics`: contagens/tempo de parse, bind, type, cache hits,
  solicitações de alocação às arenas semânticas/de tipos (não alocações heap) e
  tempo de indexação de projeto.
- `Memory`: estimativa por snapshot de buffers, colunas/tokens de sintaxe, arenas
  e pools de strings de summaries, incluindo a árvore-base incremental retida.
  Não é RSS, não mede todas as alocações do
  processo, overhead dos allocators ou todos os snapshots retidos.
- `ScheduledResult::metrics`: callbacks executados, tempo total/médio/P95,
  diagnósticos emitidos/rejeitados e falha. Timing é opt-in. P95 é das execuções
  de callbacks nesta análise, não um percentil global de requisições.
- `WorkspaceBench`: pin de snapshot, cache warm, parse completo versus atualização
  incremental e scheduling nativo.
- `LspLatencyBench`: P50/P95/P99 e working set/peak do servidor real.

```text
cmake --build build
ctest --test-dir build --output-on-failure
build/bench/WorkspaceBench --benchmark_min_time=0.1s --benchmark_repetitions=3
build/bench/WorkspaceBench --benchmark_filter=SemanticDispatch --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
build/bench/LspLatencyBench --lines 1000,5000,20000 --iterations 50 --warmup 5 --budget-ms 50 --enforce
```

Medição local em Windows/GCC 16.2 Release, 05/10/2026 (não é baseline universal):

| WorkspaceBench, mediana wall time | Resultado |
|---|---:|
| Pin, 1 / 100 / 1000 documentos | ~29 ns |
| Cache semântico warm, 1000 funções | ~124 ns; 1 parse/bind/type |
| Parse completo, 1000 funções | ~916 µs |
| Update + parse, 1000 funções | ~667 µs; 999 itens reutilizados |
| Scheduling, 100 regras / 100 funções, sem diagnostics | ~60 µs |

O LSP medido na validação final com 50 amostras teve P95 de 1,97 / 7,27 / 24,15 ms para
943 / 5132 / 18254 linhas do corpus stb. Todos ficaram abaixo do orçamento de
50 ms. Esses números não demonstram, sozinhos, ausência de regressão contra uma
build anterior; os benchmarks devem ser repetidos em hardware/corpus comparáveis.

Validação da base: build completa e **848 testes passando**, incluindo 25 novos
testes de workspace, scheduler, plugins e fronteiras arquiteturais, além dos
smoke tests CLI/LSP existentes. Após a seleção antecipada: **854 testes passando**,
incluindo seis testes novos de requisitos, equivalência de diagnósticos/fixes,
modo syntax-only e configuração. Nova medição LSP (50 amostras, cinco warmups):
P95 de **2,17 / 7,39 / 24,55 ms**, todos abaixo de 50 ms.

O benchmark A/B `SemanticDispatch` usa a mesma árvore de 1000 funções e a mesma
política nos caminhos anterior e selecionado. Medianas wall time locais (ms):

| Configuração | Cold anterior → selecionado | Warm anterior → selecionado |
|---|---:|---:|
| Todas desabilitadas | 4,445 → 0,628 | 2,350 → 0,623 |
| Apenas modernize-override (binding) | 4,438 → 1,790 | 2,331 → 0,633 |
| Apenas implicit-bool (typing) | 4,097 → 2,768 | 2,357 → 1,212 |
| Apenas modernize-constexpr (fluxo) | 4,115 → 3,145 | 2,341 → 1,531 |
| Defaults shipped | 4,099 → 4,124 | 2,506 → 2,458 |

Windows/GCC 16.2.0 Release, Intel Xeon E5-2697 v3 @ 2,60 GHz, 28 CPUs lógicas;
base `50ded35` com as alterações desta entrega, medição de 06/10/2026 UTC
(05/10 local); três repetições, mínimo de 0,1 s.
Cold recria o snapshot/modelos, mas exclui parsing; warm reutiliza
os caches disponíveis. O corpus é sintético e não emite diagnósticos para esses
checks; não substitui profiling por regra em corpus real. Defaults cold variaram
~0,6%, sem ganho demonstrado nessa configuração. Os contadores confirmam zero
bind/type com tudo desabilitado e zero type no caso binding-only. Não houve medição
nova de RSS/alocações heap nem instrumentação de cancelamento nos passes legados.

## Roadmap: o que ainda não está concluído

### Próximos passos para ABI/WASM

A sequência acordada está detalhada nas seções 18.1 e 18.2 do
[plano de evolução](../Heimdall%20%E2%80%94%20Plano%20de%20Evolu%C3%A7%C3%A3o%20Arquitetural.md):

1. Validar a API experimental com plugins sintáticos e semânticos reais na build.
2. Medir execução, queries/crossings, P95/P99, memória, inicialização e cancelamento.
3. Introduzir C ABI apenas para plugins confiáveis, se carregamento independente
   for necessário, após validação do contrato e dos benchmarks.
4. Priorizar WASM antes de permitir plugins nativos de terceiros sem revisão,
   quando execução de código não confiável for um requisito concreto.
5. Compartilhar o modelo de regras entre adaptadores, preservando o caminho
   direto das regras nativas e mantendo runtimes/carregadores fora do core.

Esses passos são planejamento, não funcionalidades já implementadas. A ABI não
será estabilizada enquanto o contrato de queries ainda estiver sendo validado.

| Fase do plano | Estado desta implementação |
|---|---|
| 1 — fronteiras | formalizadas e verificadas por testes |
| 2 — AnalysisContext | implementado; entrada compartilhada CLI/LSP; migração interna gradual |
| 3 — Workspace | documentos/versões/DB/grafo/índice implementados; alguns caches/workers de headers ainda no LSP |
| 4 — snapshots | implementados; completion/hover/navigation/diagnostics adaptados |
| 5 — incrementalidade | documento/dependências/top-level reuse implementados; interface-only invalidation ainda conservadora |
| 6 — scheduler | funcional com profiling; regras legadas ainda usam passes especializados |
| 7 — Plugin API | v1 experimental compilada na build e testada; catálogo externo/config ainda não estabilizado |
| 8 — ABI/runtime externo | **não implementado**: depende de estabilização/benchmarks e escolha C ABI/WASM |
| 9 — otimização estrutural | SoA/arenas/interning existentes preservados; novos pools/compressed indexes dependem de profiling |

Portanto, esta entrega **não declara todo o plano encerrado**. Em particular, não
há carregamento de `.dll/.so`, WASM, semântica incremental intra-documento nova,
watcher completo de headers, rename/references LSP novos, instrumentação completa
de RSS/alocações ou migração total da lógica/caches de projeto do LSP. Esses itens
não devem ser marcados concluídos só pela existência das novas APIs.
