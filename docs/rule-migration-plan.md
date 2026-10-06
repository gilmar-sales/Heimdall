# Heimdall — Plano de migração das regras nativas

## 1. Objetivo e escopo

Migrar gradualmente as regras existentes para a arquitetura de análise
compartilhada, preservando comportamento, performance e a política do core.

Este documento complementa o [registro de evolução arquitetural](architectural-evolution.md).
É um **plano de trabalho**, não uma declaração de que as etapas abaixo já foram
implementadas. A migração não depende de estabilizar C ABI nem de adotar WASM.

Objetivos:

- evitar análises caras provocadas apenas por regras desabilitadas;
- reutilizar sintaxe, símbolos, tipos, fluxo e contexto de projeto;
- reduzir traversals redundantes quando houver benefício medido;
- centralizar configuração, identidade, suppressions, diagnósticos e métricas;
- manter regras nativas no caminho interno de maior desempenho;
- manter equivalência entre CLI, LSP e APIs anteriores durante a transição.

Não é objetivo converter todas as regras para callbacks por nó, criar uma segunda
AST, introduzir uma interface virtual por regra ou fazer regras nativas passarem
pela Plugin API.

## 2. Ponto de partida

| Componente | Estado atual | Lacuna para a migração |
|---|---|---|
| `AnalysisContext` | Fornece a sintaxe pinada e acesso aos caches semânticos/de tipos | Adaptar consumidores nativos gradualmente; não contém hoje um cache de fluxo |
| `AnalysisFeatures::Diagnostics` | Entrada compartilhada; seleciona regras semânticas antes de bind/type/flow | Seleção de profiles/regras de includes ainda nos frontends |
| `SemanticRules::Analyze` | Agrega checks especializados | O agregador com tipos constrói fluxo; filtragem individual precisa anteceder trabalho caro |
| `RuleScheduler` | Suporta `on_document`, interesses por `GrammarKind`, política e profiling | Não possui scheduling por símbolos nem requisitos declarativos de modelos |
| `AnalysisEngine` | Combina diagnósticos legados com regras registradas | Ainda não transfere a responsabilidade por uma regra built-in entre os dois caminhos |
| `RuleEngine` / `RuleCatalog` | Definem opções, identidade, metadados e política das regras shipped | Precisam continuar sendo a referência durante a transição |

As regras legadas ainda executam suas passagens especializadas. Ter um scheduler
funcional não significa que elas já foram migradas.

Locais principais:

- `core/src/RuleEngine.cpp`: regras sobre fonte, tokens e diretivas;
- `semantic/include/Heimdall/SemanticRules.hpp`: entry points das regras semânticas;
- `semantic/src/SemanticRules.cpp` e demais arquivos de checks: implementação;
- `analysis/src/AnalysisFeatures.cpp`: composição compartilhada dos diagnósticos;
- `analysis/src/SemanticRuleDispatch.cpp`: seleção antecipada e adaptadores nativos;
- `analysis/include/Heimdall/AnalysisEngine.hpp`: coordenação;
- `analysis/include/Heimdall/RuleScheduler.hpp`: regras registradas e profiling.

## 3. Princípios e fronteiras

1. **Separar migração de acesso de mudança de algoritmo.** Primeiro adaptar a
   entrada/contexto; depois alterar traversal ou scheduling, em mudanças menores.
2. **Escolher o domínio de dados correto.** Uma regra textual não precisa de AST;
   uma regra de hierarquia não deve simular um índice de símbolos através de
   callbacks genéricos sobre todos os nós.
3. **Preservar dependências descendentes.** Core e semantic não devem incluir
   headers de analysis. Adaptadores que recebem `AnalysisContext` ficam em
   `analysis`; algoritmos existentes podem continuar recebendo views/modelos nos
   módulos inferiores. Não mover código apenas para contornar essa fronteira.
4. **Usar SoA nos hot paths.** Não introduzir `ParseTree::Nodes()` em loops ou
   consultas concorrentes. Preferir colunas, spans e índices existentes.
5. **Configuração e requisitos são decisões anteriores à execução.** Suppressions
   por posição continuam sendo aplicadas aos diagnósticos; não presumir que um
   comentário de suppression permite omitir arbitrariamente um modelo inteiro.
6. **Manter o processamento unit-local.** Dados compartilhados são imutáveis;
   buffers temporários e estado de execução pertencem à análise, não a singletons
   mutáveis de regras.

## 4. Estratégia por família

| Família | Exemplos | Estratégia inicial |
|---|---|---|
| Fonte/trivia | trailing whitespace, final newline, TODO | Preservar passes sobre fonte/tokens; compartilhar varreduras somente quando medido |
| Tokens/diretivas | NULL macro, typedef, duplicate/sorted includes | Preservar reconhecimento especializado; reutilizar tokens/diretivas existentes |
| Sintaxe local | Checks com candidatos bem delimitados por expressões/declaradores | Avaliar interesses por `GrammarKind`, sem redescobrir os mesmos candidatos em cada callback |
| Símbolos/hierarquias | modernize-override, virtual-destructor, explicit-constructor, overload-hiding | Acesso direto a tabelas/modelos; compartilhar seleção de classes/funções se útil |
| Tipos | implicit-bool-conversion e checks que exigem tipos conhecidos | Solicitar typing somente quando uma regra habilitada exigir; preservar comportamento para tipos desconhecidos |
| Fluxo/constantes | modernize-const, modernize-constexpr | Reutilizar fluxo/análise de constantes da unidade; evitar reconstrução por regra |
| Documentação | require-comment, doxygen-style | Preservar opt-in e `DocScope`; compartilhar candidatos/documentação quando vantajoso |
| Projeto/includes | unused include, IWYU, modernize-final | Reutilizar profiles, summaries e índices compatíveis com a revisão/configuração analisada |

Os exemplos não substituem a auditoria de requisitos de cada implementação. Uma
regra pode consumir mais de um domínio e nem todos os candidatos são representados
por um único `GrammarKind`.

Scheduling por símbolos ou eventos semânticos é uma **possível extensão futura**,
não uma interface já existente. Só introduzi-lo quando candidatos reais e
benchmarks justificarem a abstração.

## 5. Etapas de execução

### Etapa A — Inventário e baseline

- [ ] Inventariar cada `RuleId`/código, defaults, opt-in, requisitos e entry points.
- [ ] Identificar quais traversals e modelos são compartilháveis.
- [ ] Medir custo individual e custo end-to-end antes das mudanças.
- [ ] Registrar corpus, hardware, compilador, flags, configuração e revisões.
- [ ] Definir orçamento de regressão aceitável antes de avaliar resultados.

Usar arquivos pequenos/grandes, headers, código incompleto, macros, casos sem
diagnósticos e casos com muitos diagnósticos. Medir cold e warm separadamente.
Instrumentação de timing deve ser opt-in para não contaminar o caminho normal.

**Saída:** inventário por regra e baseline reproduzível. Métricas de callbacks do
scheduler não substituem profiling das regras legadas ou latência de requisições.

### Etapa B — Requisitos e seleção antecipada

- [ ] Definir metadados internos mínimos de requisitos por regra, sem estabilizar
      uma ABI pública para esses metadados.
- [ ] Resolver opções do arquivo, defaults, opt-in e overrides antes de executar.
- [ ] Calcular a união dos requisitos das regras efetivamente selecionadas.
- [ ] Construir modelos compartilhados uma vez, apenas quando necessários.
- [ ] Preservar os limites do modo de análise solicitado pela CLI/LSP.
- [ ] Garantir que dados de projeto ausentes mantenham o comportamento atual,
      sem conclusões semânticas que a engine não consegue provar.

Fluxo implementado para as 19 regras semânticas/documentais; ainda não universal
para todas as famílias/core/profiles de includes:

```text
configuração + modo de análise
              ↓
      seleção de regras
              ↓
    união dos requisitos
              ↓
syntax → binding → typing → fluxo, conforme necessário
              ↓
    execução e diagnósticos
              ↓
       política comum
```

Os requisitos representam dependências reais: uma regra de fluxo pode exigir
typing e binding, mas uma regra que só usa símbolos não deve forçar typing.
A sintaxe ainda pode ser necessária por outra feature ou pelos diagnósticos de
parser, mesmo com todas as regras sintáticas desabilitadas.

**Prioridade inicial:** reduzir trabalho caro executado apenas para regras
desabilitadas. O filtro final de diagnósticos não realiza essa otimização.

### Etapa C — Adaptação ao contexto, sem reescrever algoritmos

- [ ] Criar adaptadores na camada analysis para os entry points existentes.
- [ ] Consumir modelos/views do `AnalysisContext`, sem cópias de árvores ou tabelas.
- [ ] Preservar as funções tree/model-based anteriores para batch e compatibilidade.
- [ ] Remover construções duplicadas de Binder/Typer nos consumidores adaptados.
- [ ] Decidir por profiling se um cache de fluxo deve integrar a análise da
      unidade/snapshot; não adicioná-lo indiscriminadamente a todos os documentos.

**Saída:** o mesmo algoritmo sobre dados fornecidos pela fronteira compartilhada,
com testes equivalentes. Esta etapa pode melhorar acoplamento e reutilização sem
alterar o mecanismo de traversal.

### Etapa D — Piloto de scheduling

- [ ] Escolher uma ou duas regras com interesses sintáticos estreitos e custo medido.
- [ ] Definir candidatos e quais partes do traversal passam a ser compartilhadas.
- [ ] Preservar `RuleId`, código, default severity, opções e informações de fixes.
- [ ] Assegurar que a regra execute por **um único caminho** na análise normal.
- [ ] Comparar resultados e custo com o caminho anterior.

O `AnalysisEngine` atual agrega built-ins e regras registradas. Registrar uma
versão migrada sem retirar sua execução do agregador legado pode duplicar
diagnósticos. É necessário definir explicitamente o proprietário da execução
(legado ou migrado) por regra durante a transição.

Não usar um override global de configuração para desabilitar só o caminho antigo:
o mesmo código/política pode desabilitar o caminho novo também. A escolha de
dispatch é interna; a configuração do usuário permanece única.

Execução dupla é permitida somente em testes ou em um modo de comparação
controlado, nunca como comportamento normal da CLI/LSP.

### Etapa E — Expansão por grupos

- [ ] Migrar pequenos grupos com domínio e requisitos semelhantes.
- [ ] Compartilhar candidatos de classes/funções antes de criar callbacks por símbolo.
- [ ] Compartilhar análises de fluxo/projeto sem reconstruí-las por regra.
- [ ] Medir memória, latência e overhead de dispatch a cada grupo.
- [ ] Manter passes especializados quando continuarem mais eficientes.
- [ ] Retirar o caminho legado de uma regra somente após os critérios de aceitação.

Regras de fluxo/hierarquia/projeto não devem ser o primeiro piloto de scheduling:
possuem mais dependências e maior risco de mudança de comportamento.

### Etapa F — Consolidação

- [ ] Atualizar catálogo, documentação e listas explícitas de fontes CMake quando
      necessário, sem manter uma segunda fonte de verdade de metadados.
- [ ] Remover código duplicado criado pela transição, não as APIs anteriores sem
      uma decisão explícita de compatibilidade.
- [ ] Confirmar integração de configuração/política e ausência de execução dupla.
- [ ] Registrar quais regras usam scheduling e quais mantêm passes especializados.
- [ ] Atualizar o status da evolução arquitetural com evidências de validação.

## 6. Critérios de aceitação por regra ou grupo

### Equivalência funcional

- [ ] Mesmo conjunto de diagnósticos, mensagens, severidades, ranges, linhas/colunas
      e ordenação observável, incluindo empates de offset.
- [ ] Mesmos defaults, opt-in, overrides e comportamento de regras desabilitadas.
- [ ] Mesmas suppressions atualmente suportadas pelo engine, incluindo
      `heimdall-disable-line` e `heimdall-disable-next-line`.
- [ ] Mesmos edits, títulos e classificação safe/unsafe dos fixes.
- [ ] Mesmos resultados de batch fix, sem edits duplicados ou sobrepostos adicionais.
- [ ] Mesma prudência diante de nomes/tipos/bases não resolvidos e headers ausentes.
- [ ] Funcionamento em código incompleto e nos dialetos/macros suportados.

Comparar resultados completos, não apenas a contagem de diagnósticos. Corrigir
um bug semântico identificado deve ser uma mudança explícita com testes próprios,
não um efeito silencioso da migração.

### Lifetime, concorrência e cancelamento

- [ ] Revisões antigas continuam analisáveis enquanto uma nova é publicada.
- [ ] Nenhuma view/ID é usado após o lifetime do modelo/snapshot correspondente.
- [ ] Nenhuma chamada lazy AoS é introduzida em leitura concorrente.
- [ ] Dados derivados de projeto não são tratados como atuais após invalidação.
- [ ] Cancelamento não publica resultados parciais nem mantém buffers sem necessidade.
- [ ] Estado de execução não é compartilhado de forma mutável entre requisições.

O suporte cooperativo deve alcançar os passes legados migrados, não apenas os
callbacks do scheduler. Não declarar cancelamento completo porque um stop token
é verificado antes e depois de um passe longo.

### Desempenho e arquitetura

- [ ] Modelos desnecessários para regras selecionadas não são construídos por elas.
- [ ] Modelos/candidatos reutilizáveis não são reconstruídos por regra.
- [ ] Benchmarks respeitam o orçamento acordado para CPU, memória e P95/P99.
- [ ] Benefício de locality/traversal não é anulado por callbacks ou locks por nó.
- [ ] Core continua STL-only, sem exceptions/RTTI, e sem dependência de analysis.
- [ ] A mudança demonstra ganho medido ou redução concreta de acoplamento, sem
      regressão inaceitável. Caso contrário, manter a implementação especializada.

## 7. Validação e métricas

Ferramentas existentes:

```text
cmake --build build
ctest --test-dir build --output-on-failure
build/test/Tests_run --gtest_filter='RuleEngineSpec.*:RuleSchedulerSpec.*:WorkspaceSpec.*:ArchitecturePolicy.*'
build/bench/CoreBench --benchmark_filter='BM_RuleEngine.*'
build/bench/SemanticBench
build/bench/WorkspaceBench
build/bench/LspLatencyBench --lines 1000,5000,20000 --iterations 50 --warmup 5 --budget-ms 50 --enforce
```

No Windows, acrescentar `.exe` aos executáveis. Incluir também as suites da regra
migrada: os filtros acima não substituem a suite completa nem os smoke tests.

Registrar por grupo:

- tempo individual das regras e tempo total da análise;
- traversals/candidatos, callbacks e queries executados;
- contagens e tempo de parse, bind, type e fluxo quando instrumentados;
- cache reuse, diagnósticos emitidos antes/depois da política;
- latência de atualização/diagnósticos/completion em P50/P95/P99;
- estimativas de storage, solicitações de alocação às arenas e RSS/peak quando medidos.

`MemoryBudget` não é RSS e contadores de arenas não são contadores de alocação heap.
P95 de callbacks do scheduler não é P95 de requisições LSP. Comparações devem
separar overhead de profiling, criação dos modelos e execução das regras.

## 8. Registro de progresso

Para cada regra/grupo, registrar:

| Campo | Informação esperada |
|---|---|
| Identidade | `RuleId` e código existente |
| Requisitos | Fonte/tokens, syntax, binding, types, flow e project |
| Caminho anterior | Entry point, algoritmo e dados consumidos |
| Caminho novo | Adaptador, domínio de scheduling ou passe especializado mantido |
| Dispatch | Proprietário único da execução durante a transição |
| Equivalência | Testes e comparação detalhada dos diagnósticos/fixes |
| Medições | Baseline e novo resultado com ambiente/corpus/configuração |
| Lifetime/cancelamento | Evidências e limitações conhecidas |
| Situação | Inventariada, adaptada, piloto, validada ou consolidada |

Nenhuma regra é considerada migrada apenas por receber uma assinatura nova ou
por existir um callback equivalente em um teste. A conclusão exige integração
no caminho real de análise, equivalência funcional e validação do custo.

## 9. Entrega: seleção antecipada e adaptação semântica

Implementadas as partes de B/C relativas aos 23 entry points de `SemanticRules`.
Os checklists das etapas continuam descrevendo a migração completa de todas as
famílias; não significam que profiles, scheduler e cancelamento estejam concluídos.

| Requisitos | Regras adaptadas (códigos existentes) |
|---|---|
| Binding | `cpp/modernize-override`, `cpp/modernize-nullptr`, `cpp/no-zero-as-null`, `cpp/modernize-auto`, `cpp/modernize-string-view`, `cpp/modernize-consteval-constexpr`, `api/virtual-destructor`, `api/explicit-constructor`, `api/overload-hiding`, `api/virtual-call-in-constructor`, `cpp/designated-init-order`, `cpp/no-integer-to-pointer` |
| Binding + typing | `cpp/no-implicit-bool-conversion`, `cpp/modernize-range-loop`, `cpp/modernize-loop-convert`, `cpp/modernize-span`, `cpp/modernize-attributes` |
| Binding + typing + fluxo | `cpp/modernize-const`, `cpp/modernize-constexpr` |
| Binding + contexto de projeto | `cpp/modernize-final`, `cpp/include-what-you-use` |
| Binding + DocScope (opt-in) | `doc/require-comment`, `doc/doxygen-style` |

- **Caminho anterior:** agregadores `SemanticRules::Analyze` e
  `AnalyzeDocumentation`, seguidos por `ApplyPolicy`.
- **Caminho novo / dispatch:** `AnalysisFeatures::Diagnostics` chama somente o
  adaptador interno selecionado, usando modelos do `AnalysisContext`. Os algoritmos
  e passes especializados não mudaram; não usam callbacks por nó nem Plugin API.
- **Metadados:** domínios de callbacks tipados determinam requisitos, sem nova ABI;
  `RuleCatalog` continua proprietário de códigos, severidades e identidade.
- **Modelos:** união antecipada; bind/type lazy existentes e uma construção de
  fluxo por execução, somente quando exigida. Não foi adicionado cache de fluxo.
- **Equivalência:** `SemanticDispatchSpec` compara todos os campos dos diagnósticos,
  fixes safe/unsafe e batch fixes, com defaults, seleção individual, severidade,
  suppressions, macros e código incompleto. Verifica também cada domínio pelos
  contadores de bind/type, opt-in, precedência do último override e modo syntax-only.
- **Validação:** build completa; 854 testes passando, incluindo smoke tests CLI/LSP.
  Benchmark A/B cold/warm e latência LSP registrados em
  [architectural-evolution.md](architectural-evolution.md).
- **Limites:** não altera descoberta/freshness de headers, `IncludeAnalyzer`,
  queries de plugins, profiling individual nem cancelamento cooperativo dos passes
  antigos. APIs anteriores continuam executando todos os checks por compatibilidade.

Situação: **adaptada e validada para seleção de modelos**. Scheduling por interesse
e consolidação completa permanecem pendentes; não se declara migração total.
