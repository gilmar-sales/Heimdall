# Refatorações C++: avaliação e plano de implementação

Data: 2026-10-06. Escopo: refatorações de código C++ oferecidas pelo Heimdall e seu LSP. Este documento é um plano, não uma implementação das funcionalidades.

## 1. Diagnóstico

O projeto tem infraestrutura reaproveitável, mas ainda não tem um subsistema de refatoração semântica de projeto. O gargalo é provar quais entidades e usos serão afetados e preservar seu significado, não gerar substituições de texto.

Evidências verificadas no código:

| Área | O que existe | Limite para refatoração |
|---|---|---|
| `analysis/include/Heimdall/Workspace.hpp` | Snapshots, versões, dependências, caches sincronizados e compile database | Sem API de plano de mudanças com pré-condições e validação de múltiplos arquivos |
| `semantic/include/Heimdall/SemanticModel.hpp` | Símbolos, escopos, referências, assinaturas e bases em tabelas SoA | IDs locais ao modelo; referências sem resolução usam `kNone`; falta identidade canônica entre unidades |
| `semantic/include/Heimdall/ProjectIndex.hpp` | Exports/classes por nome, summaries de headers e consultas de hierarquia | Não é um índice completo de ocorrências, chamadas e redeclarações do workspace |
| `core/include/Heimdall/Navigation.hpp` | Definition/implementation, herança e using-directives | Lookup explicitamente aproximado; busca de definições usa nome, escopo e quantidade de parâmetros, insuficiente para distinguir overloads |
| `semantic/include/Heimdall/TypeModel.hpp` | Tipos básicos, classes, aliases, ponteiros, referências e arrays | `Unknown` reúne casos dependentes, não resolvidos e não modelados; não prova resolução completa de chamadas |
| `semantic/include/Heimdall/FlowModel.hpp` | CFG intraprocedural, eventos por variável e escapes | Eventos em ordem de token, não de avaliação; funções incompletas; insuficiente para extração/inlining geral |
| `lsp/Server.cpp`, `CodeActions` | Quick fixes e fix-all de diagnósticos | Edições do documento atual por `changes`, sem versões nas edições; sem rename/prepareRename/references na inicialização e despacho examinados |
| `core/include/Heimdall/RuleEngine.hpp` | Um `TextEdit` por diagnóstico e flag `fix_is_safe` | A flag classifica uma correção; não constitui evidência de preservação semântica de uma refatoração |
| `lsp/Document.hpp` | Conversão de bytes para posições UTF-16 por snapshot | Reutilizar; não substituir por colunas de bytes da `LineTable` |
| `semantic/src/IncludeIndex.cpp` | Leitura/indexação de headers em disco | Buffers de headers não salvos precisam entrar no mesmo snapshot/overlay da análise |

Os documentos `cpp-syntax-coverage.md` e `completion-limitations.md` registram outras lacunas. Seus checklists devem ser reproduzidos contra o código atual antes de virar backlog definitivo: documentação pode estar defasada. A navegação já tem suporte a using-directives, por exemplo; isso não comprova paridade do Binder ou completion.

### Limite da avaliação

Leitura estática dos componentes e execução do binário de testes existente: 45 testes de `WorkspaceSpec`, `NavigationSpec` e `DocumentSpec` passaram. O filtro também mencionava suites sem correspondência nesse binário; portanto não houve validação das suites semânticas por essa execução. Não foi realizado rebuild nem validação integral de CTest. Há uma alteração prévia em `analysis/include/Heimdall/AnalysisEngine.hpp`, preservada.

## 2. Catálogo essencial e ordem de entrega

Cada funcionalidade deve começar com um subconjunto comprovável. Suporte a C++ geral exige expandir a modelagem e a matriz de configurações; não deve ser anunciado a partir de casos locais.

| Prioridade | Refatoração | Primeira entrega | Requisitos para ampliar |
|---|---|---|---|
| P0 | Renomear variável local/parâmetro | Símbolo resolvido, todos os usos cobertos e sem captura/colisão | Lambda captures, structured bindings e parâmetros de lambda modelados |
| P1 | Renomear funções, tipos, namespaces e membros | Projeto indexado; entidades e redeclarações canônicas | Overloads, construtores/destrutores, overrides, aliases, templates, ADL e variantes de compilação |
| P1 | Extrair variável | Uma expressão com tipo/categoria conhecidos; mesma execução e lifetime | Pureza, efeitos, ordem de avaliação, short-circuit e temporários |
| P1 | Inline de variável | Uma inicialização e um uso elegível; sem alteração de efeitos ou lifetime | Múltiplos usos somente com prova de segurança |
| P1 | Extrair função/método | Sequência contígua de statements, uma entrada/saída, tipos conhecidos | Live-in/live-out, aliasing, referências, escapes, RAII, acesso e contexto de `this` |
| P2 | Alterar assinatura | Inserir/remover/reordenar parâmetros em função interna simples | Chamadas e redeclarações completas, efeitos dos argumentos, defaults, overloads, ponteiros de função e hierarquias virtuais |
| P2 | Mover definição entre header/source | Função não template em destino conhecido | ODR, linkage, inline/constexpr, dependências, comentários, defaults e atributos |
| P2 | Organizar/adicionar/remover includes | Dependências explícitas e resolução completa no perfil suportado | Macros, includes condicionais, headers sem autonomia e efeitos de ordem |
| P2 | Encapsular campo | Campo privado simples com usos internos conhecidos | Leituras/escritas, identidade/endereço, referências, friend, aggregate initialization e API pública |
| P3 | Inline de função | Corpo simples e chamada direta, argumentos sem efeitos | Substituição de parâmetros, returns, avaliação única, acesso, RAII e temporários |
| P3 | Mover símbolo/arquivo; extrair interface/base | Plano com todos os usos e dependências conhecidos | Includes, namespaces, módulos, build system, herança, ABI e consumidores externos |

Gerar declaração/definição e construtor pode aproveitar o mesmo mecanismo de edições como assistência de código. Conversões para const/constexpr/override e outras modernizações já têm parte da infraestrutura de regras; manter sua classificação separada das refatorações e revisar suas garantias antes de reaproveitá-las.

## 3. Contrato de segurança

Uma operação disponível deve carregar evidências verificáveis, além do texto proposto:

1. **Entrada consistente:** revisão do snapshot, versões dos buffers, hashes de arquivos fechados, fingerprint dos comandos/configurações e índice utilizado.
2. **Cobertura declarada:** unidades analisadas, variantes consideradas, arquivos excluídos, limites atingidos e ocorrências não resolvidas potencialmente relevantes. Ausência de referência no índice não prova ausência no projeto.
3. **Identidade:** uma entidade canônica para declaração, definição e usos; relações explícitas de overload e override. IDs de modelos não podem ser comparados entre snapshots.
4. **Pré-condições semânticas:** colisões/captura, tipos e categorias de valor, efeitos, acesso, lifetime e fluxo, conforme a operação.
5. **Origem editável:** posição escrita no arquivo e contexto de pré-processamento. Bloquear usos gerados por macros sem mapeamento confiável; distinguir argumentos de macro, expansão, token-pasting e stringification.
6. **Edições válidas:** ranges nos limites de tokens, sem sobreposição, inserções no mesmo offset com ordem definida, conteúdo esperado e arquivos de destino permitidos. Preservar comentários, CRLF/LF, encoding suportado e código não selecionado.
7. **Validação posterior:** aplicar em overlay temporário, reparse/rebind dos afetados e conferir invariantes específicas. Compilar pode detectar erros, mas não prova equivalência de comportamento.
8. **Aplicação controlada:** preview e uma operação de undo quando suportados; revalidar antes de entregar/aplicar. Recusar plano obsoleto. Não presumir atomicidade de edição multi-arquivo no cliente.

Estados sugeridos: `Available`, `Blocked(reason)` e `NeedsExplicitReview(reason)`. Incerteza semântica que impede a prova deve bloquear a operação, não ser resolvida apenas com um aviso. Revisão explícita é adequada para impactos de API/ABI e escopo externo, com plano completo dentro do escopo conhecido.

Não prometer segurança para consumidores externos não indexados, código gerado ou configurações não analisadas. Tornar esse escopo visível e evitar classificar mudanças de API pública como automaticamente seguras.

## 4. Arquitetura proposta

Criar um módulo `heimdall_refactor`, dependente de `heimdall_analysis`/`heimdall_semantic`, com adaptadores finos no CLI/LSP. Manter o core STL-only, sem RTTI/exceções, e simdjson PRIVATE. Listar fontes explicitamente no CMake.

Componentes propostos, ainda inexistentes:

- `SourceProvider`: overlay de buffers abertos e arquivos fechados, compartilhado por parser, include index e validador. Uma requisição lê uma visão consistente.
- `SymbolIdentity` e `OccurrenceIndex`: entidade canônica, localizações, usos, redeclarações, chamadas e overrides por configuração. Manter IDs densos locais e uma camada de identidade de projeto; hashes precisam de verificação de igualdade.
- `SemanticCoverage`: distinguir resolved/unresolved/ambiguous/dependent/unsupported e registrar por que a análise é incompleta.
- `RefactoringContext`: snapshot, seleção, configurações e cancelamento.
- `RefactoringPlan`: edições por arquivo, criação/movimentação quando aplicável, pré-condições, justificativas, impacto e validação requerida. Não forçar múltiplas edições dentro do `Diagnostic::fix` atual.
- `EditBuilder`: ranges de origem, deduplicação, conflitos e preservação de trivia. Geração localizada; não executar formatter no arquivo inteiro.
- `RefactoringValidator`: análise do overlay modificado e comparação das invariantes.
- `RefactoringService`: descobrir ações elegíveis, construir e validar planos; algoritmos específicos separados para rename/extract/inline/signature/move.

No LSP, implementar `textDocument/references`, `prepareRename` e `rename`; ações `refactor.extract`, `refactor.inline` e `refactor.rewrite`; resolução adiada de ações para trabalho pesado. Negociar capacidades e usar `documentChanges` com versões em documentos abertos. Para arquivos fechados, hash é uma pré-condição interna: não há proteção equivalente à versão do buffer apenas por serializar uma edição. Revalidar, restringir aplicação quando necessário e documentar o intervalo de corrida restante.

Não transformar `Server.cpp` no lugar onde vivem as provas semânticas. A mesma operação deve poder ser testada e usada pelo CLI sem JSON-RPC.

## 5. Fases de implementação e critérios de conclusão

### Fase 0 — Baseline e escopo

Reproduzir lacunas sintáticas/semânticas relevantes, medir referências resolvidas em fixtures/corpus e registrar uma matriz de recursos/configurações. Executar build e CTest com toolchain suportada. Definir política de macros, templates, APIs externas e arquivos gerados.

**Concluída quando:** cada limite conhecido tem fixture e motivo de bloqueio esperado; a baseline identifica toolchain, commit e comandos de compilação.

### Fase 1 — Planos de edição e rename local

Implementar plano, conflitos, pré-condições, overlay e validação. Adicionar busca de referências locais baseada em identidade; verificar colisões em cada uso, incluindo nomes que passariam a ser capturados. Reusar posições UTF-16 do LSP. Oferecer rename local e de parâmetro apenas onde declaração e todos os usos estão modelados.

**Concluída quando:** rename altera somente a entidade pretendida, preserva bindings dos demais nomes e bloqueia macros/ambiguidade/versões obsoletas. Cobrir concorrência e unicode no protocolo.

### Fase 2 — Índice semântico de projeto e rename global

Unificar redeclarações e definições; indexar arquivos fontes, headers e buffers não salvos. Chavear resultados também pelo contexto de compilação: um header pode representar entidades distintas em duas TUs. Resolver chamadas/overloads corretamente ou bloquear os casos não cobertos. Indexar dependências reversas e invalidar consumidores.

Introduzir suporte a famílias de construtores/destrutores e métodos virtuais em etapas. Quando um nome novo alterar overload sets ou lookup, validar os usos afetados, inclusive os que não recebem edição.

**Concluída quando:** rename entre header/source é completo nas configurações declaradas; índice truncado/incompleto impede o resultado seguro; mudanças em headers não salvos são observadas.

### Fase 3 — Extrair e inline de variável

Ampliar tipos com categoria de valor e informações suficientes de efeitos/lifetime. Calcular ponto de inserção sem elevar expressão para fora de condição, loop, short-circuit ou argumento com interação de efeitos. Preservar dedução e lifetime; `auto` não é sempre equivalente a uma expressão ou referência original.

**Concluída quando:** corpus inclui temporários, referências, funções com efeitos, volatile/atomic e operadores sobrecarregados; casos não provados são bloqueados.

### Fase 4 — Extrair função/método

Acrescentar predecessores, dominância, liveness, entradas/saídas da seleção, efeitos e informação conservadora de aliases ao fluxo existente. Definir parâmetros por tipo e modo de passagem. Começar com nenhum salto para fora da seleção e no máximo uma saída de dados simples. Verificar escopo, `this`, acesso privado, capturas e destruição de objetos.

**Concluída quando:** entradas/saídas e bindings após a extração coincidem; o algoritmo bloqueia mudanças de lifetime, fluxo não modelado e escapes sem prova. Retornos múltiplos e protocolos de resultado ficam para expansão posterior.

### Fase 5 — Assinaturas, definições e includes

Usar índice de chamadas para alterar assinatura, inclusive declaração/definição/defaults. Remover argumento com efeitos pode mudar comportamento; reordenar parâmetros pode mudar avaliação ou overload selecionado. Bloquear esses casos na primeira entrega.

Mover definição exige evitar defaults duplicados, preservar linkage/ODR e qualificação. Não mover template/inline/constexpr para source sem estratégia de visibilidade comprovada. Organizar includes exige reconhecer dependências de macros e ordem; remoção baseada apenas em símbolos conhecidos não basta.

**Concluída quando:** testes multi-TU comprovam consistência; validação compila TUs afetadas; efeitos de argumentos, includes condicionais e linkage têm casos negativos.

### Fase 6 — Transformações estruturais e C++ avançado

Encapsular campo, inline de função, mover símbolos/arquivos e extrair interfaces. Expandir templates, especializações, conceitos, ADL, módulos e macros com rastreabilidade. Tratar integração com build system como adaptador explícito, não como edição heurística de qualquer CMake.

**Concluída por recurso**, conforme matriz de suporte; não há um marco único que garanta equivalência para toda transformação em todo C++.

Dependências: fases 0 → 1 → 2; fase 3 pode avançar após 1 dentro do arquivo; 4 depende de 3 e fluxo ampliado; 5 depende de 2; 6 depende dos fundamentos específicos de cada operação. Estimar prazo somente após baseline e protótipo de identidade entre TUs.

## 6. Estratégia de verificação

- Fixtures antes/depois com invariantes semânticas específicas: entidade resolvida, tipo, overload selecionado e fluxo relevante; comparação textual sozinha é insuficiente.
- Casos negativos: shadowing/capture, overloads de mesma aridade, using/ADL, overrides, function pointers, macros, templates, parse errors e índice parcial.
- Projetos multi-arquivo: header compartilhado, buffers sujos, arquivos fechados modificados externamente, múltiplos comandos por TU e variantes `-D`/`-std`.
- Protocolo: seleção UTF-16, emoji/acentos, URIs Windows, versões obsoletas, cancelamento, partial failure do cliente, criação/movimentação de arquivo e undo.
- Build de fixtures transformadas em compiladores suportados e testes de comportamento pertinentes; manter baseline para não atribuir erros preexistentes à refatoração.
- Testes diferenciais incrementais versus análise completa; planos determinísticos; edição fora da seleção só quando declarada no plano.
- Benchmarks de índice, busca de referências e geração de plano com p50/p95, memória e taxa de bloqueio. Definir budgets depois de medir corpus representativo; evitar metas de latência sem baseline.

Adicionar suites específicas (`RefactoringPlanSpec`, `RenameSpec`, `ExtractVariableSpec`, `ExtractFunctionSpec`, `ChangeSignatureSpec`) e smokes LSP de refatoração conforme cada fase, com fontes explícitas em `test/CMakeLists.txt`.

## 7. Primeiro backlog executável

1. Registrar baseline atual e reproduzir overloads de mesma aridade, símbolos de headers e usos em macros.
2. Criar target/módulo de refatoração e contrato `RefactoringPlan` com versões, hashes e razões de bloqueio.
3. Implementar `SourceProvider` e fazer include index/validador consumirem buffers não salvos.
4. Acrescentar classificação de resolução e cobertura por arquivo; não exigir semântica completa de C++ para o primeiro caso local.
5. Implementar referências locais, preflight de colisões e rename de variável/parâmetro.
6. Integrar prepareRename/rename ao LSP, validar stale plans e testar o fluxo real na extensão.
7. Construir identidade/ocorrências entre TUs antes de anunciar rename de projeto.

A primeira entrega útil é rename local com bloqueios corretos e planos versionados. Ela valida o mecanismo que as demais operações reutilizarão; a maior parcela de esforço posterior estará no índice de projeto e nas provas semânticas.

## 8. Estado da implementação

Estado do checkout de desenvolvimento, 2026-10-06. As fases abaixo **não estão todas concluídas**.

| Fase | Estado implementado | Trabalho restante |
|---|---|---|
| 0 | Build da baseline e 929 testes passaram com GCC; testes LSP precisaram de acesso às pastas temporárias fora do sandbox | Corpus/matriz de configurações, métricas de cobertura e reprodução integral do checklist sintático |
| 1 | Target `heimdall_refactor`, planos fixados, preview, conflitos/cancelamento, rename LSP versionado, comparação de bindings, includes verificados pelo pré-processador GCC/Clang e diagnóstico no prepareRename | Proveniência geral de macros, MSVC e ampliação da cobertura de nomes |
| 2 | Overlay de includes; `ProjectSymbolIndex` separado da navegação, identidade entre declarações/definições de funções livres escalares e símbolos externos simples, ocorrências classificadas, cache por snapshot e importação explícita de TUs/headers fechados preservando buffers sujos | Tipos canônicos gerais, resolução de overloads/ADL/overrides, proveniência de macros, análise de headers por TU/configuração e rename de projeto |
| 3 | Extrair expressão constante integral/booleana de um return para variável; inline de local integral com literal de mesmo tipo e usos diretamente em returns | Expressões gerais, ponto flutuante, efeitos, categorias de valor, temporários e lifetime |
| 4 | Extrair retorno de literal integral de função livre para helper interno `constexpr`/`noexcept` | Seleções de statements, parâmetros, live-in/live-out, aliases, RAII, métodos e saídas múltiplas |
| 5 | Não implementada | Alteração de assinatura, movimentação header/source e refatoração de includes com as provas do plano |
| 6 | Não implementada | Encapsulamento, inline de funções, mover símbolos/arquivos, interfaces e C++ avançado |

Extract/inline requerem documentos sem diretivas nem macros predefinidas. Rename local aceita includes com `compile_commands.json` e GCC/Clang: compara tokens escritos com o pré-processamento antes/depois da edição e bloqueia expansões, remoções condicionais e headers incluídos com alterações não salvas. Não há proveniência geral de macros. Novos nomes são ASCII não reservados e precisam não ocorrer como identificadores no documento. O cliente precisa anunciar `workspaceEdit.documentChanges`; o servidor não faz fallback para edições sem versão.

Os algoritmos de extract/inline validam sintaxe, bindings e tipos das declarações preservadas. Suas garantias também dependem dos subconjuntos estruturais admitidos; reparse/rebind sozinho não prova equivalência de C++ arbitrário. A extração de função implementada move somente um literal, sem variáveis de entrada/saída ou objetos de classe.

As ações anunciam os kinds `refactor.extract.variable`, `refactor.extract.function` e `refactor.inline.variable`, com filtros específicos e por kind pai. Expressões de ponto flutuante (inclusive dentro de comparações booleanas) e literais definidos pelo usuário são bloqueados. Extração de variável também bloqueia funções com `goto` ou labels de `case`: introduzir uma inicialização pode tornar inválidos saltos que cruzem sua declaração. Clientes sem `documentChanges` continuam recebendo referências, mas não recebem planos de refatoração sem versão.

Verificação adicionada: testes unitários de planos/rename/extract/inline/overlay, testes LSP com UTF-16, seleção de ações por kind específico/pai, negociação de edições versionadas, rename com o comando real de compilação do CLI, atualização e navegação de header não salvo e compilação das transformações com `static_assert` dos resultados. Build concluído e CTest final: **953/953 passaram** com GCC no Windows. `npm run compile` da extensão também passou. A verificação de macros baseada no compilador não é oferecida para drivers MSVC; o teste correspondente é omitido nessa configuração. A biblioteca retorna planos/previews; aplicação e undo são responsabilidade do cliente. Não foram adicionados comandos CLI de refatoração ou um aplicador transacional de arquivos fechados.

Validação adicional de caminhos/headers: `RenameSpec.PreservesIncludeSearchOrderAndRejectsDirtyTransitiveHeaders` passou, cobrindo prioridade do diretório do fonte sobre `-iquote`, `-I` relativo ao diretório do comando, headers transitivos limpos/sujos e restauração do buffer. Os oito testes direcionados de rename/LSP passaram, inclusive as oito edições em `src/cli.cpp`. Na nova execução integral, **953/954 passaram**; `RuleSchedulerSpec.DispatchesOnlyInterestedNodesAndProfilesEachRule` falhou por métricas de tempo iguais a zero, inclusive na reexecução isolada. Nenhum código de profiling foi alterado nesta validação.

Próximo requisito crítico: completar a fase 2 antes de expor rename de projeto ou alterações de assinatura. O índice aproximado de navegação não é usado como prova de identidade.

Ampliação do índice semântico, 2026-10-08: ver [contrato, APIs e limitações do índice de símbolos](project-symbol-index.md). O índice registra gaps de cobertura e candidatos ambíguos em vez de tratar lookup por nome como prova. O importador de fontes é transacional; o cache acompanha revisão, buffers, opções e versões do snapshot. Essa infraestrutura ainda não habilita refatorações globais no protocolo.

Validação desta ampliação: build concluído e **971/971 testes CTest passaram**, incluindo 17 testes novos de `ProjectSymbolIndexSpec`. A falha de timing registrada na execução anterior de `RuleSchedulerSpec` não ocorreu nesta execução; seu código não foi modificado.
