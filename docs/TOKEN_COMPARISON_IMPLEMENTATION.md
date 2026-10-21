# Implementação do comparador e enriquecimento semântico

## Componentes entregues

1. `bench/TokenComparison.py`: relatório auditável versão 2 com hashes, comandos,
   versão anunciada pelos servidores, legendas, diagnósticos da versão aberta,
   tokens completos e métricas independentes de cobertura, tipo e modificador.
   Uma publicação de diagnóstico de outro documento ou versão não libera a coleta.
   O clangd não precisa devolver completion completa: seu truncamento de sugestões
   não indica AST incompleta. Para ele, os diagnósticos versionados e a resposta de
   tokens completos são a barreira. O Heimdall também aguarda completion completa
   para sincronizar os headers.
2. `semantic/SemanticHighlight`: unidade testável que usa símbolos, referências,
   escopos e tipos. Substitui as varreduras textuais do handler LSP por identidade
   semântica e preserva os spans lexicais, macros e regiões inativas. Classifica
   locais, parâmetros, sombras, structured bindings, lambdas e enums qualificados.
   O parser também distingue inicializadores diretos com argumentos de valor,
   como `Item item(1, nullptr)`, de listas de parâmetros. A distinção exige um
   prefixo de tipo conhecido, preserva declarações ambíguas e não transforma
   chamadas em condições em declarações de variáveis.
3. `SemanticHighlightIndex`: metadados próprios e qualificados dos headers, com
   detecção de conflitos e ciclos de aliases. O índice é criado junto ao
   `IncludeIndex`, reutilizando o parse de cada header. Usa a configuração de
   compilação e os overlays existentes; não introduz outro cache. As mesmas chaves
   e invalidações de headers/defines protegem ambos os índices. O enriquecimento
   resolve também aliases locais para tipos externos e membros diretos/por ponteiro
   com identidade conhecida, usando a grafia do tipo confirmada pelo índice quando
   o typer ainda não consegue representá-lo.
4. `lsp/Server.cpp`: handler reduzido a preparar contexto, obter snapshot dos
   headers, analisar e codificar. Ao publicar um índice de headers, o servidor pede
   `workspace/semanticTokens/refresh` somente se o cliente anunciou suporte.
   O snapshot de headers é adquirido antes do parse para atualizar o type oracle.
5. `HighlightModifier`: flags de definição, constância, static e escopo, com máscara
   de 32 bits. `declaration` e `inactive` mantêm os índices originais da legenda;
   novos tipos são acrescentados ao final, preservando os índices existentes.
6. Corpus determinístico sem STL, testes unitários, smoke LSP, diferencial clangd
   opcional no CTest e benchmarks `LexicalHighlight`, `SemanticHighlight` e
   `HeaderHighlightMetadata` em
   `SemanticBench`. Os testes comuns não dependem do clangd.

## Contrato de tipos e modificadores

- O realce lexical do core permanece disponível sem banco de compilação, binder ou
  dependência externa. A nova unidade pertence à biblioteca semântica.
- Declarações locais classificadas semanticamente usam `class` para records;
  o fallback sintático ainda pode usar `struct`. Aliases preservam `type`, mesmo
  quando o clangd escolhe realçar o tipo subjacente como `class`.
- Métodos e campos explicitamente estáticos usam `function` e `variable`; os de
  instância usam `method` e `property`.
- Parâmetros de tipo usam `typeParameter`, conceitos usam `concept` e parâmetros
  de valor usam `parameter`. O suporte depende dos símbolos que o binder produz.
- `readonly` deriva do tipo do objeto, incluindo referências, e não apenas de um
  pointee const. Tipos desconhecidos permanecem conservadores.
- A legenda mantém `operator` para delimitadores que o clangd chama `bracket`.
  Keywords, strings e comentários adicionais continuam sendo cobertura intencional.
- Extensões como `defaultLibrary`, `deduced`, `userDefined` e
  `usedAsMutableReference` não são fabricadas a partir de heurísticas. Portanto,
  igualdade integral com a taxonomia do clangd não é o objetivo do perfil atual.
- Conflitos no índice de headers não escolhem arbitrariamente uma declaração.
  Dados incompletos conservam o fallback sintático; não tornam todo identificador
  desconhecido uma variável.

## Reprodução

```powershell
cmake --build build --parallel
python test/TokenComparisonSpec.py
python test/TokenParitySmoke.py --heimdall build/lsp/heimdall-lsp.exe --clangd clangd
ctest --test-dir build --output-on-failure --parallel 6
build/bench/SemanticBench.exe --benchmark_filter='^(LexicalHighlight|SemanticHighlight|HeaderHighlightMetadata)$' --benchmark_min_time=0.1s --benchmark_repetitions=3
python bench/TokenComparison.py core/src/SyntaxHighlight.cpp --heimdall build/lsp/heimdall-lsp.exe --compile-commands build/token-comparison-input/compile_commands.json --timeout 90 --output build/token-comparison-implemented.json
```

O banco de comparação local remove somente as três flags de módulos GCC listadas
no planejamento original. O build principal mantém compilador e opções originais.
As diferenças entre bibliotecas padrão MSVC e MinGW continuam sendo uma limitação
da comparação neste ambiente.

## Resultado final medido em 20/10/2026

No mesmo `core/src/SyntaxHighlight.cpp`, com clangd 22.1.0:

| Medida | Baseline | Implementado |
| --- | ---: | ---: |
| Tokens Heimdall | 1.570 | 1.995 |
| Tokens clangd | 1.470 | 1.470 |
| Iguais em intervalo, tipo e modificadores | 236 | 843 |
| Intervalos comuns | 995 | 1.375 |
| Cobertura dos intervalos do clangd | 67,7% | 93,5% |
| Mesmo tipo entre intervalos comuns | 83,5% | 93,6% |
| Divergências de tipo | 164 | 88 |
| Divergências apenas de modificadores | 595 | 444 |
| Apenas no clangd | 475 | 95 |

Os relatórios `build/token-comparison-implemented.json` e
`build/token-comparison-implemented-repeat.json` têm tokens e diferenças idênticos.
Ambos foram coletados com barreiras de prontidão e os mesmos executáveis finais.
O baseline é histórico: o arquivo passou a conter dois nomes adicionais na tabela
de categorias do próprio highlighter, sem alterar o total de tokens de referência.

Os 95 ausentes são 88 operadores, três referências a `std::nullopt`, duas a
`std::string_view::npos` e os enumeradores `Tok::Hash`/`Tok::DigraphHash` gerados por
X-macro. A cobertura desses operadores e a resolução dos sete símbolos externos
continuam sendo limitações do perfil atual. Não há ausência de `highlighter` nas
duas funções `Extract`: a correção de inicialização direta reconhece suas declarações
e referências. Também desapareceram os pares `parameter → property` e
`method → enumMember` do baseline.

Permanecem 620 extras, dos quais 451 pertencem às categorias lexicais de keywords,
strings, números e comentários. Os demais não devem ser automaticamente chamados de
bugs nem removidos para aproximar contagens: incluem diferenças de taxonomia,
cobertura e interpretação dos headers. Os JSONs preservam cada ocorrência para
triagem. Igualdade completa exige ainda decisões e suporte fora do perfil definido
acima; o comparador não oculta essas diferenças.

## Validação final

- Build completo com Ninja/GCC e opções do build existente: concluído.
- CTest: **2.000 de 2.001 testes passaram**. A única falha é
  `FormatterSpec.TokListMacroStaysHumanReadable`: ele lê `Tok.hpp` em modo binário e
  espera `\\` no fim de cada linha, mas o checkout usa CRLF e o último caractere é
  `\r`. O arquivo contém 328 linhas CRLF e é idêntico ao HEAD após normalização de
  finais de linha. Nem esse arquivo nem a implementação do formatter foram alterados.
- 23 testes da nova unidade semântica, duas regressões do parser e os testes de
  codificação/legenda passaram. O teste existente de parâmetros foi ajustado para
  exigir `declaration` apenas na declaração, mantendo a comparação dos outros bits.
- 16 testes Python do comparador passaram; o smoke e o diferencial opcional real
  com clangd passaram em sete categorias e dois casos de constância.
- Lint dos novos fontes/headers/testes/benchmark: nenhum diagnóstico. No header do
  core permanecem apenas os avisos de nomenclatura das duas constantes legadas
  `kHighlightDeclaration`/`kHighlightInactive`. As novas flags são valores de enum
  com PascalCase. Nenhuma regra foi desabilitada.
- Todos os C++ alterados foram formatados pelo executável final do Heimdall; a
  última passagem não produziu alterações. `git diff --check` passou.

Medições Release locais, três repetições, mínimo de 0,1 s por benchmark; medianas de
tempo real no corpus sintético de cem funções:

| Operação | Mediana |
| --- | ---: |
| Realce lexical | 0,195 ms |
| Enriquecimento semântico, incluindo typing | 0,658 ms |
| Metadados semânticos de header já vinculado | 0,557 ms |

Parse e binding ficam fora dessas três regiões cronometradas. O benchmark existente
`CoreBench/BM_Parse`, em seu próprio corpus, mediu 0,197 ms. Os resultados completos
estão em `build/token-highlight-benchmark.json` e
`build/token-highlight-parser-benchmark.json`. São custos observados, sem alegação
de ganho sobre uma implementação anterior ou de latência equivalente no LSP completo.
