# Frigga — diagnóstico e plano de correções futuras

## Status de implementação (lotes 1 e 2, concluídos no Heimdall)

Primeiro lote (parser e preprocessor):

- **H2:** macros que encaminham `__VA_ARGS__` como statements agora são analisadas dentro da invocação, preservando statements seguintes e reportando terminadores realmente ausentes. Regressões em `MacroParseSpec.cpp`, incluindo ramo vazio como controle.
- **H3:** o preprocessor acompanha raw strings multiline com delimitadores customizados e ignora diretivas/comentários no seu conteúdo. Regressões em `PreprocessorSpec.cpp` e `RuleEngineSpec.cpp`, mantendo detecção de includes reais duplicados.
- **H4:** new-expression consome tipos fundamentais compostos como `unsigned int` e `long double`; regressão em `ParseTreeSpec.cpp` verifica statements subsequentes, array e chamada no tamanho.

Segundo lote (semântica, layout, formatter e CLI):

- **H1:** a contagem de argumentos usa vírgulas de topo (`CountSuppliedArguments`), então um cast para tipo desconhecido sem header (`(Mode)event->mod`, como `(SDL_Keymod)event->key.mod`) não divide o operando em dois argumentos. Regressão em `CallArgumentsSpec.cpp` com controle positivo de dois argumentos reais. Erro do `imgui_impl_sdl3.cpp:412` eliminado sem tocar o backend.
- **H5:** invocações de macros-statement aninhadas (`IMPL_STRUCT` do Assimp) são expandidas com a tabela viva (`ExpansionShape`) e consumidas opacamente quando resultam em nada ou em declarações completas; o caso `__VA_ARGS__` puro preserva a análise do interior. Regressões em `MacroParseSpec.cpp`. Erro do `BlenderCustomData.cpp:76` eliminado. Descoberta: macros de corpo vazio já eram removidas como decoração antes do parsing (`DecorationEnd`), o que está travado em teste.
- **H6:** `AnalyzeProjectCallArguments` considera overloads declarados em headers incluídos diretamente (aridades via `HeaderSummary`), com visibilidade por namespace envolvente, namespace global ou `using-directive` ativa. Regressões em `ProjectRulesSpec.cpp`, incluindo o formato FBX (definição binária local + declaração unária no header + `using namespace`). Os 22 erros de aridade do `FBXParser.cpp` foram eliminados.
- **H7:** `typedef struct` com tag agora gera `RecordDefinition` + declaração de alias, então o layout do hover soma os campos em vez de relatar struct vazia (1/1). Regressão em `CompletionSpec.cpp` para `SDL_DialogFileFilter` (16/8 em 64 bits).
- **H8:** divergência de expectativa resolvida contra a invariante do formatter (só indentação por profundidade de chaves, sem reflow): o formatter reindenta o array sem juntar elementos multilinha. Teste `KeepsMultilineAggregateElementsWithoutReflow` trava o comportamento atual e a idempotência; a compactação total virou proposta de funcionalidade, não correção.
- **H9:** contrato do CLI travado em `test/CheckParseCli.cmake`: `lint` sai 0 mesmo com diagnósticos; `check`/`parse` saem 1. Nenhuma mudança de código de saída.

Validação do segundo lote: testes regressivos demonstraram falhas antes das correções (H1, H5 aninhado, H6, H7). Build Release/Ninja existente reutilizado; arquivos C++ alterados formatados com Heimdall. CTest: **1.777 testes, 100% aprovados**, um teste Windows explicitamente skipped (`CompileDatabaseSpec.CommandStringKeepsWindowsPathSeparators`).

Rechecagem dos arquivos reais (`round3-recheck.json`, 303 diagnósticos): **zero erros** em `JoltPhysicsWorld.cpp`, `3DSConverter.cpp`, `BlenderCustomData.cpp`, `FBXParser.cpp`, `imgui_impl_sdl3.cpp` e `ProjectScaffold.cpp`. Nenhum arquivo do Frigga ou de dependências foi modificado.

Coleta integral de `build/` após as correções (`build-lint-round6.json`, lint sem semântica): **1.061.143 diagnósticos, 3.986 erros — 0 adicionados e 618 removidos** em relação ao baseline (`build-errors.json`: 4.604 erros). Rodada intermediária (round 5) mostrou 1 erro solitário em `TwoBodyConstraint.cpp` (cópia Sdk) que não se reproduziu no round 6 nem em execuções isoladas: flake pontual em código de terceiros, sem relação sistemática com as mudanças (618 remoções estáveis). Regressões intermediárias detectadas pelas recoletas foram corrigidas antes de concluir: `typedef enum` com alias (787 erros SDL/miniaudio, teste `ParsesTypedefRecordsWithAliases`) e macro de encaminhamento sem `;` interno (12 erros Jolt `BodyManager`, teste `ForwardingMacroRelyingOnOuterSemicolonStaysAnExpression` + análise de continuação da expansão).

Nota de higiene: `heimdall format --write` com a configuração atual converte arquivos inteiros para tabs, gerando churn em arquivos que usam espaços. Os arquivos com mudanças lógicas pequenas e exclusivas deste trabalho (`TypeLayout.cpp`, `detail/TokenView.hpp`, `SemanticRules.hpp`, `SemanticRuleDispatch.cpp`) foram revertidos e reeditados para diffs mínimos (2–14 linhas). Nos demais arquivos alterados, o diff integral permanece; usar `git diff -w` para revisar. Nada foi commitado; há trabalho paralelo não commitado de outro agente na mesma árvore (formatter, RuleConfig, ApiRules) — preservado integralmente.

Itens do Frigga auditados (somente leitura, sem alteração):

- **F1:** nenhum `delete` ou smart pointer sobre `Event` em `src/` (`EventDispatcher` usa `Event&`); o destrutor virtual seria quebra de ABI e exige o processo de versionamento do Frigga + build completo. Não implementado.
- **F2:** `new`/`delete` do `ma_engine` pareados em `Initialize`/`Shutdown` (incluindo o caminho de falha); conversão para smart pointer com deleter customizado exige validação com build do Frigga. Não implementado.
- **F3:** catch vazio justificado (fallback best-effort em terminate handler). Mantido sem mudança.

As notas de “não implementado” abaixo descrevem a etapa original de planejamento; este status prevalece para H1–H9.

## Escopo e resultado

Coleta em 13/10/2026 usando o executável existente `/home/gilmar/dev/Heimdall/build/src/heimdall`, sem rebuild, autofix ou alterações no Frigga. Analisados os diretórios `src`, `include`, `test` e `bench`; excluídos build, dependências baixadas e ferramentas. Backends ImGui presentes em `src` estão incluídos e devem ser tratados separadamente do código próprio.

Não há `.heimdall.json` no Frigga: a execução foi feita a partir dele, com as regras padrão, não com a configuração do repositório Heimdall. Avisos de estilo não são automaticamente requisitos do Frigga.

| Execução | Diagnósticos | Avisos | Erros |
|---|---:|---:|---:|
| Lint | 3386 | 3385 | 1 |
| Lint semântico | 3976 | 3974 | 2 |

Os resultados das duas execuções se sobrepõem; não somar os totais. Lint retornou código 0 apesar do erro de parsing. Isso exige investigação do contrato de saída, não prova sucesso da análise. Não foram executados compilação, testes unitários ou validação com compilador real.

Evidências completas: `/tmp/opencode/frigga-heimdall-20261013/` (`lint.json`, `lint.stderr`, `semantic.json`, `semantic.stderr`, reproduções e inventário). Os arquivos temporários devem ser preservados junto ao ticket antes da implementação.

## Reprodução da coleta

Executar a partir de `/home/gilmar/dev/Frigga`:

```bash
/home/gilmar/dev/Heimdall/build/src/heimdall lint --json --std c++26 --compile-commands /home/gilmar/dev/Frigga/build/compile_commands.json src include test bench
/home/gilmar/dev/Heimdall/build/src/heimdall lint --json --semantic --std c++26 --compile-commands /home/gilmar/dev/Frigga/build/compile_commands.json src include test bench
```

A análise semântica é a do Heimdall, não equivale à compilação. A existência do banco não garante cobertura de todos os headers, testes e templates Runtime; arquivos sem comando correspondente podem ter verificações semânticas omitidas.

## P0 — corrigir a confiabilidade do Heimdall

### H1 — contagem de argumentos com cast

- Evidência: `src/Frigga/Gui/Backends/imgui_impl_sdl3.cpp:412`; a declaração em :333 recebe um argumento e a chamada `ImGui_ImplSDL3_UpdateKeyModifiers((SDL_Keymod)event->key.mod)` também tem um, mas o diagnóstico informa dois.
- Hipótese: interpretação de cast/acesso a membro ou contexto acumulado no arquivo. Uma reprodução mínima com enum, struct e a mesma forma de chamada **não reproduziu** o erro; preservar isso como controle negativo e reduzir o arquivo real antes de escolher a correção.
- Implementação futura: examinar árvore e argumentos vinculados; corrigir apenas quando a causa estiver demonstrada. Não editar o backend para contornar o diagnóstico.
- Testes isolados em `test/src/CallArgumentsSpec.cpp`: `AcceptsSingleCStyleCastMemberArgument` (controle); `CountsCastArgumentInBackendContext` (reprodução reduzida ainda a construir); casts aninhados e `static_cast`; controle positivo com dois argumentos reais para função unária.
- Aceitação: nenhuma ocorrência indevida na reprodução e no backend, mantendo a detecção de aridade realmente incorreta.

### H2 — macro externa contendo statement

- Evidência: `src/Frigga/Physics/JoltPhysicsWorld.cpp:266–267`: `JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertFailedImpl;)` precede `JPH::Factory::sInstance = new JPH::Factory();`. O parser acusa falta de `;` antes de `JPH`.
- Hipótese: macro de header externo não expandida ou recuperação inadequada. Validar resolução da macro e seus ramos antes de concluir que o código é inválido.
- Implementação futura: revisar ambiente de macros/importação de includes e parsing de invocações de macros statement. Não inserir pontuação no Frigga apenas para satisfazer o parser.
- Testes isolados em `MacroParseSpec.cpp`/`MacroEnvironmentSpec.cpp`: macro local que expande seu argumento; macro que expande vazio; definição em header sintético resolvido por compile DB temporário; namespace e atribuição após a invocação; controle com erro sintático real após macro.
- Aceitação: ambos os ramos preservam a atribuição subsequente e não geram erro espúrio; erros reais continuam reportados.

#### Investigação complementar — reprodução isolada confirmada

- A macro foi localizada em `build/_deps/joltphysics-src/Jolt/Core/IssueReporting.h:31,35`: com asserts ativos, `#define JPH_IF_ENABLE_ASSERTS(...) __VA_ARGS__`; caso contrário, expansão vazia. `JPH_DEBUG` pode ativar `JPH_ENABLE_ASSERTS` no próprio header. Em ambos os casos, a invocação original é válida sem `;` depois do `)`.
- Reprodução sem dependência de Jolt:

```cpp
#define JPH_IF_ENABLE_ASSERTS(...) __VA_ARGS__
namespace JPH
{
    int AssertFailed;
    struct Factory { static Factory* sInstance; };
}
void Test()
{
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = 1;)
    JPH::Factory::sInstance = new JPH::Factory();
}
```

- Com compile DB sintético correspondente, o ramo `__VA_ARGS__` reproduz `expected ';' before 'JPH'`; a variante de expansão vazia não produz erro de parsing. Sem compile DB, ambas apresentam o erro, indicando diferença de disponibilidade/uso do ambiente de macros nessa execução. Portanto, a reprodução demonstra que a falha não se limita à resolução de headers externos; ainda é necessário inspecionar como o parser consome o terminador vindo da expansão ativa.
- Evidências em `/tmp/opencode/frigga-heimdall-20261013/`: `jolt-macro-enabled.cpp`, `jolt-macro-disabled.cpp`, `jolt-macro-unknown.cpp`, `jolt-compile-commands.json` e `jolt-macro-result.json`. O comando do arquivo real foi encontrado no banco e inclui o diretório Jolt; investigar macros efetivamente resolvidas, não apenas presença de `-D` na linha de comando.
- Teste regressivo prioritário: `MacroParseSpec.AcceptsVariadicStatementMacroWithInternalSemicolon`, reproduzindo o ramo ativo acima com ambiente de macros explicitamente construído. Exigir ausência de `syntax/parse-error`, preservação da atribuição dentro da macro e da atribuição após a chamada; não se limitar à contagem de diagnostics.
- Controle negativo de regressão: `AcceptsEmptyConditionalStatementMacro`, exigindo que apenas a atribuição posterior permaneça. Acrescentar macro com duas atribuições e controle positivo com atribuição realmente sem terminador para evitar recuperação permissiva demais.
- Teste de integração futuro do CLI: usar header sintético, compile DB e variações `-D`/`-U` para reproduzir os dois ramos, verificando JSON e posição dos erros. Testar também ambiente sem compile DB segundo o contrato adotado para macros desconhecidas.
- Não corrigir inserindo `;` no Frigga: isso ocultaria a falha do Heimdall e não exercitaria a expansão válida que precisa ser suportada.

### H3 — includes dentro de raw strings

- Evidência: três avisos em `src/Editor/Project/ProjectScaffold.cpp:194,195,235` correspondem a strings distintas de código gerado, não a diretivas do arquivo analisado.
- Reprodução confirmada: duas raw strings com `#include <example.hpp>` geram `cpp/no-duplicate-include` em `raw.cpp` no diretório de evidências.
- Implementação futura: a varredura de diretivas deve respeitar limites lexicais de strings e comentários; revisar `core/src/RuleEngine.cpp` e a origem das diretivas, sem adicionar heurística específica para Frigga.
- Testes isolados em `RuleEngineSpec.cpp`/`PreprocessorSpec.cpp`: `IgnoresIncludesInsideSeparateRawStrings`; delimitador customizado; include real mais literal; comentário contendo include; controle com duas diretivas reais, exigindo exatamente um aviso na segunda.
- Aceitação: zerar esses três falsos positivos sem perder duplicatas reais.

### H4 — new-expression com tipo composto (Assimp)

- Coleta complementar solicitada: `build/_deps/assimp-src/code/AssetLib/3DS/3DSConverter.cpp`, fora do escopo inicial. Confirmados três `syntax/parse-error` nas linhas 377:46, 436:39 e 711:44; 65 diagnósticos no arquivo. Não adicionar esses resultados aos totais da coleta original.
- Em :436, `pcOut->mMeshes = new unsigned int[iArray.size()];` é sintaxe válida. Reprodução mínima confirmada: `void Test(unsigned count) { auto* values = new unsigned int[count]; delete[] values; }` acusa `expected ';' before 'int'`.
- Evidências adicionais: `assimp-3ds.json` e `new-array.cpp` em `/tmp/opencode/frigga-heimdall-20261013/`.
- Implementação futura: revisar consumo de type-specifier-seq em new-expression; tratar tipos fundamentais compostos antes do declarador de array. Corrigir Heimdall, não o Assimp.
- Testes isolados em `ParseTreeSpec.cpp`: `ParsesNewArrayWithUnsignedIntType`, incluindo atribuição a membro e tamanho por chamada; variantes `signed int`, `unsigned long long`, `long double`, alocação escalar e array; controle positivo com new-expression realmente incompleta. Verificar ausência de erros, tipo completo e preservação da expressão de tamanho na árvore.
- Aceitação: reprodução mínima e os três pontos do Assimp sem erro espúrio, mantendo diagnóstico em sintaxe inválida. Prioridade P0, junto a H1–H3.

### H5 — contexto de macros e construtor no Blender

- Confirmado no CLI: `build/_deps/assimp-src/code/AssetLib/Blender/BlenderCustomData.cpp:76:5`, `expected ';' before end of compound statement`; dois diagnósticos no arquivo.
- A região contém construtor com inicializadores `Read(read), Create(create), Destroy(destroy) {}` depois de invocações de macros que geram funções usando concatenação de tokens e stringification.
- O construtor isolado com callbacks não reproduziu o erro. A causa permanece aberta: reduzir macros/contexto e inspecionar offsets da árvore antes de atribuir a falha aos inicializadores.
- Evidências: `assimp-blender-custom-data.json` e controle `constructor-init.cpp` no diretório da coleta.
- Testes planejados em `MacroParseSpec.cpp` e `ParseTreeSpec.cpp`: construtor isolado como controle negativo; reprodução reduzida com macro geradora de funções seguida de struct; concatenação `##` e stringification `#`; corpo vazio e não vazio; erro verdadeiro de statement sem ponto e vírgula como controle positivo.
- Aceitação: reprodução reduzida falha antes da correção e passa depois; o arquivo real deixa de apresentar esse erro sem alteração no Assimp.

### H6 — sobrecargas declaradas em header (FBX)

- Confirmado com `--semantic`: `build/_deps/assimp-src/code/AssetLib/FBX/FBXParser.cpp:776:24` reporta dois parâmetros exigidos para `ParseTokenAsDim(*tok[0])`.
- A sobrecarga unária é declarada em `FBXParser.h:207` e definida em `FBXParser.cpp:1312`; a binária aparece em header :198 e cpp :303. A chamada unária é válida com o header incluído.
- A execução produziu 135 diagnósticos, incluindo 22 erros de aridade na mesma família (`ParseTokenAsDim`, `ParseTokenAsFloat`, `ParseTokenAsInt`, `ParseTokenAsID`, `ParseTokenAsInt64`). Evidências em `assimp-fbx-parser.json` e `.stderr`.
- Hipótese: conjunto incompleto de sobrecargas ou visibilidade das declarações do header; investigar binding e índice antes de fixar a causa. Não adicionar segundo argumento no Assimp.
- Testes isolados em `CallArgumentsSpec.cpp`/`IncludeIndexSpec.cpp`: header sintético declarando versões unária e binária; definição binária antes da chamada e unária depois; namespace aninhado; chamadas válidas a ambas; controle com zero/três argumentos realmente inválidos. Criar compile DB temporário sem dependências reais e verificar resolução da assinatura além do diagnóstico.
- Aceitação: 22 erros espúrios removidos do arquivo, com controles inválidos preservados e reprodução mínima demonstrando a falha antes da correção.

### H7 — layout incorreto no hover de typedef struct (SDL)

- Relato do usuário: `SDL_DialogFileFilter`, definido como `typedef struct SDL_DialogFileFilter { const char *name; const char *pattern; } SDL_DialogFileFilter;`, exibe tamanho 1 e alinhamento 1. Ainda não reproduzido por chamada LSP nesta coleta.
- Para ABI convencional de ponteiros de 64 bits: tamanho 16, alinhamento 8; com ponteiros de 32 bits: tamanho 8, alinhamento 4. O resultado deve respeitar ABI e packing do alvo, não a máquina do servidor.
- Ponto de exibição confirmado: `lsp/Server.cpp` apresenta layout quando `has_layout` é true. Investigar resolução em `core/src/TypeLayout.cpp`, indexação dos dois campos e associação entre alias e record; tamanho 1 pode indicar tratamento indevido como record vazio, hipótese ainda não confirmada.
- Testes isolados planejados em `CompletionSpec.cpp`/`IncludeIndexSpec.cpp`: definição inline e em header sintético, tag e alias com mesmo nome, campos ponteiro const, ABI 32/64 bits e alias com nome diferente. Exigir dois campos e layout correto; controle com struct realmente vazia e record incompleto. Quando o layout não for confiável, omitir tamanho/alinhamento em vez de inventar 1/1.
- Aceitação: hover local e via header concordam com o layout conhecido do alvo; validação com `sizeof`/`alignof` no compilador correspondente quando disponível.

### H8 — formatação consistente de array de aggregates (SDL)

- Caso solicitado: array `const SDL_DialogFileFilter kSceneFilters[]` com um elemento quebrado em várias linhas e outro inline. O usuário aprovou a saída abaixo: brace externo Allman e elementos curtos inline com espaços internos consistentes.

Entrada:

```cpp
const SDL_DialogFileFilter kSceneFilters[] = {
    {
        "Frigga Scene", "json"
    },
    {"All files", "*"},
};
```

Saída esperada, sob configuração explícita de quatro espaços e coluna 100:

```cpp
const SDL_DialogFileFilter kSceneFilters[] =
{
    { "Frigga Scene", "json" },
    { "All files", "*" },
};
```

- Teste futuro em `test/src/FormatterSpec.cpp`: `FormatsShortAggregateArrayElementsConsistently`, usando definição sintética de `SDL_DialogFileFilter` com dois campos `const char*`, sem depender do SDL instalado. Configurar explicitamente Allman, espaços e limite de coluna para não depender da configuração raiz com tabs.
- Assert principal: comparação exata da saída do array com o esperado, preservando ordem, strings e vírgula final. Assert de idempotência: formatar a saída novamente não altera o texto.
- Controles adicionais: ambos os elementos inicialmente multiline; ambos inicialmente inline; array dentro de função; elemento que excede coluna 100 não deve ser compactado para além do limite; comentário interno deve ser preservado e impedir compactação quando necessário.
- Antes da implementação, executar a reprodução para verificar o comportamento atual. A compactação de elementos multiline é uma evolução solicitada do formatter, não uma exigência deduzida das regras de estilo do repositório. Não aplicar reflow genérico nem modificar casos não relacionados.
- Aceitação: teste principal e controles passam, sem alterações de tokens significativos, sem perda de comentários e sem regressões na suíte de formatter. Nenhum teste ou mudança do formatter foi implementado nesta etapa.

### H9 — contrato do CLI e cobertura

- Investigar por que lint retorna 0 com `syntax/parse-error`. Não mudar códigos de saída sem confrontar documentação e testes existentes.
- Planejar teste de processo isolado: arquivo válido, warning apenas, erro de parsing e erro semântico; verificar JSON parseável, stderr e código de saída conforme o contrato definido.
- Registrar separadamente arquivos descobertos, arquivos com diagnósticos e arquivos com contexto semântico. Não inferir arquivos analisados contando apenas o JSON de diagnósticos.

## P1 — avaliar riscos reais no Frigga

### F1 — destruição polimórfica

- `include/Frigga/Events/Event.hpp:58`: classe com métodos virtuais e sem destrutor virtual explícito, confirmado por inspeção.
- Decisão: auditar ownership. Se houver destruição via base, adicionar destrutor virtual; se a base for deliberadamente não owning, avaliar destrutor protegido. Considerar impacto de ABI antes de mudar API.
- Testes futuros: `EventLifetimeSpec.BaseDestructionDestroysDerivedExactlyOnce`, usando derivada mínima e contador local; assertion de trait para o contrato escolhido; manter testes de dispatch e propagação de `Handled`. Não criar um teste que execute delete polimórfico indefinido antes da correção.

### F2 — ownership em áudio e física

- `src/Frigga/Audio/MiniaudioEngine.cpp:85`: aviso de ownership em ponteiro cru; 12 avisos de `cpp/no-new-delete` no total.
- Auditar contratos de C APIs, Jolt, factory global e refcount antes de substituir `new` por smart pointers. Não tratar todo `new` como vazamento confirmado.
- Testes futuros isolados: falha de inicialização libera recurso uma vez; shutdown repetido não duplica liberação; sucesso libera no teardown; transferência de ownership não libera prematuramente. Usar backend fake/contadores, sem dispositivo de áudio, GPU ou Editor; testes reais de Jolt ficam em suíte separada.

### F3 — catch de diagnóstico best-effort

- `src/Frigga/Diagnostics/RuntimeDiagnostics.cpp:415`: catch vazio possui comentário justificando preservar relatório quando introspecção falha.
- Não remover o catch automaticamente. Avaliar política e separar montagem de mensagem do terminate handler, apenas se houver necessidade de mudança.
- Testes futuros: introspecção normal, exceção não padrão e falha simulada produzem fallback; validar helper puro com dependência fake, sem executar `_Exit` no processo de testes.

## P2 — higiene e contratos de API

Após H1–H3, revisar 155 avisos de include-what-you-use e 47 de includes não usados: validar headers públicos corretos (especialmente simdjson/Jolt), macros e sobrecargas como `std::abs` antes de aplicar sugestões. Testar com headers sintéticos no Heimdall (`IncludeAnalyzerSpec.cpp`), incluindo facade pública e símbolo homônimo; validar Frigga também sem depender acidentalmente do PCH.

Revisar 185 `api/missing-nodiscard`, 14 construtores sem explicit, 9 const-correctness e recomendações de passagem de parâmetros (25 por valor, 9 por referência). São candidatos, não mudanças de API aprovadas. Testes de compile-time para conversibilidade/nodiscard quando aplicável e testes de comportamento para métodos modificados. Não tornar fixtures GoogleTest `final` cegamente: `TEST_F` pode gerar subclasses.

Os 87 avisos de conversão para bool incluem ponteiros desconhecidos: preservar idioms de optional/smart pointer e confirmar os tipos antes de exigir comparação com nullptr.

## P3 — estilo e modernização de baixo risco

3197 avisos de magic numbers dominam a coleta. Classificar constantes de domínio, cores/UI, defaults serializados, tolerâncias e dados de teste; não criar milhares de constantes nem alterar valores persistidos. Cobrir defaults e round-trip com fixtures existentes e tolerâncias explícitas.

77 sugestões de algoritmos, 68 de emplace, 25 de auto, 12 de final, 10 de constexpr, 5 de structured bindings e demais sugestões ficam após os riscos reais. Qualquer transformação de loop deve preservar ordem, cardinalidade e efeitos colaterais: testes com entrada vazia, todos/nenhum filtrados e duplicatas. Não alegar ganho de desempenho sem benchmark.

5 avisos de newline final podem ser corrigidos mecanicamente após validar escopo; 11 TODOs exigem triagem humana, não remoção de comentários. Os dois NULL reportados estão em backends ImGui: evitar divergência de código upstream sem política explícita.

## Sequência de implementação e validação

1. Preservar baseline e versões do executável/banco; revisar cobertura e configurar regras aceitas pelo Frigga sem suprimir globalmente falhas.
2. Para cada H1–H3: adicionar reprodução mínima determinística, demonstrar falha antes, corrigir, executar spec focada e suíte aplicável; nenhuma dependência de Frigga/Jolt/SDL real nas unit tests do Heimdall.
3. Testes novos do Heimdall entram nas listas explícitas de CMake; usar o build ativo `build`, preservar compilador/opções e formatar C++ materialmente alterado com seu executável Heimdall.
4. Reexecutar os dois comandos no Frigga e comparar por `(arquivo, código, linha, coluna, mensagem)`, não somente total. Bloquear novos erros e registrar os avisos remanescentes por categoria.
5. Implementar F1–F3 apenas após validação de contratos, um módulo por mudança, com testes isolados Arrange–Act–Assert; não lançar Editor ou exigir GPU/rede em unit tests.
6. No Frigga, consultar regras locais do Editor antes de alterações; verificar concorrência de builds, construir todos os targets antes de ctest e seguir o formatter próprio do projeto. Não reutilizar cegamente a configuração de tabs do Heimdall.
7. Aplicar P2/P3 em lotes pequenos e revisáveis, sem autofix global. Mudanças em ABI/formatos exigem as verificações e migrações previstas no AGENTS.md do Frigga.

Este documento é planejamento: nenhuma correção ou teste unitário foi implementado nesta coleta.

## Ampliação de escopo para build

O usuário esclareceu que os maiores problemas estão em `build/`. A exclusão inicial limitou a coleta e não representa os diagnósticos vistos no editor em dependências. A análise de todo `/home/gilmar/dev/Frigga/build` terminou sem autofix, com JSON em `build-lint.json`, stderr em `build-lint.stderr` e extração de erros em `build-errors.json`, no diretório de evidências.

Resultado: **1.061.761 diagnósticos, 4.604 erros de parsing em 1.536 arquivos com erros**, código de saída 0. Contagem por caminho: Assimp 1.059, Jolt 1.035, GLM 440, SDL 425, simdjson 163, miniaudio 66, GoogleTest 38, ImGui 27, Freyr 4, Skirnir 1; outros caminhos em build 1.346. Esta última categoria foi obtida pela ausência de `/_deps/` no caminho: ainda não significa que todos sejam código gerado, e deve ser subdividida antes de priorizar.

Próxima etapa: agrupar `build-errors.json` por mensagem/construção, selecionar reproduções mínimas por família e verificar dialeto/tipo de arquivo e artefatos duplicados. Não tratar cada ocorrência como bug independente nem presumir que todo erro é falso positivo. A rodada semântica integral de build ainda não foi executada; casos semânticos fornecidos pelo usuário são reproduzidos individualmente, como FBX.

Separar dependências `_deps` e código gerado; priorizar regressões do parser por família sintática, não modificar bibliotecas terceiras para esconder erros do Heimdall. Totais dessa rodada devem permanecer separados da análise de código próprio. A rodada ampliada é lint semântico desativado; não cobre todos os diagnósticos semânticos do LSP.
