# Regras de formatação do Freyr

Este documento registra as 31 opções do `C:/dev/Freyr/.clang-format`, seus valores de
referência e o estado de suporte no formatter do Heimdall. O formatter do Heimdall é uma
implementação própria: uma opção documentada aqui não implica compatibilidade integral com
`clang-format`.

## Configuração por projeto

As opções suportadas pelo formatter podem ser definidas no objeto `format` do
`.heimdall.json`. A configuração mais próxima do arquivo formatado substitui somente as
chaves que declara; as demais continuam herdando os valores do diretório pai ou os defaults.
O schema em `schemas/heimdall.schema.json` valida os tipos e valores aceitos.

```json
{
  "format": {
    "indent-width": 4,
    "use-tabs": false,
    "column-limit": 100,
    "align-trailing-comments": true,
    "pointer-alignment": "left",
    "reference-alignment": "left",
    "blank-line-between-methods": true,
    "max-parameters-per-line": 3
  }
}
```

Para desativar uma regra booleana suportada, use `false`. Para valores de estilo, use a
opção correspondente documentada abaixo. `column-limit: 0` desativa a quebra por limite de
colunas; `max-parameters-per-line: 0` desativa a quebra de parâmetros.

## Opções do `.clang-format` do Freyr

| Opção | Freyr | Função | Heimdall |
|---|---|---|---|
| `BasedOnStyle` | `Microsoft` | Usa o estilo Microsoft como base para opções não especificadas. | Não é um modo do formatter. As opções Heimdall têm defaults próprios. |
| `AlignAfterOpenBracket` | `Align` | Alinha a continuação de chamadas e listas após delimitadores de abertura. | Não exposto; alinhamento compatível com clang-format não está implementado. |
| `AlignConsecutiveMacros` | `true` | Alinha definições consecutivas de macros. | Suportado para diretivas `#define` de uma linha; chave: `format.align-consecutive-macros` (boolean, default `false`). |
| `AlignConsecutiveAssignments` | `true` | Alinha operadores de atribuição em linhas consecutivas. | Suportado para `=` no nível externo de instruções de uma linha; chave: `format.align-consecutive-assignments` (boolean, default `false`). |
| `AlignConsecutiveDeclarations` | `true` | Alinha declarações consecutivas. | Não implementado. |
| `AlignEscapedNewlines` | `Right` | Alinha as barras invertidas de macros multilinha. | Diretivas são preservadas; alinhamento não implementado. |
| `AlignOperands` | `true` | Alinha operandos em expressões quebradas em várias linhas. | Não implementado. |
| `AlignTrailingComments` | `true` | Alinha comentários `//` consecutivos na mesma coluna. | Suportado. Chave: `format.align-trailing-comments` (boolean). |
| `AllowAllArgumentsOnNextLine` | `false` | Controla se todos os argumentos podem ficar juntos na linha seguinte. | Não há equivalente direto; o Heimdall quebra parâmetros de declarações por limite. |
| `ColumnLimit` | `100` | Define o limite de largura de linha. | Suportado com quebra conservadora. Chave: `format.column-limit` (inteiro; `0` desativa). |
| `AllowShortFunctionsOnASingleLine` | `InlineOnly` | Permite funções curtas inline em uma linha. | Não configurável por projeto; o formatter preserva ou remodela blocos segundo suas regras próprias. |
| `AllowShortIfStatementsOnASingleLine` | `Never` | Impede `if` curto em uma linha. | Há estilos internos para blocos de controle, mas ainda não há chave de projeto equivalente. |
| `AlwaysBreakTemplateDeclarations` | `Yes` | Coloca declarações de template em linha separada. | Não implementado com equivalência ao clang-format. |
| `BreakBeforeTernaryOperators` | `true` | Coloca `?` e `:` de ternários no início das linhas de continuação. | Não implementado. |
| `BreakConstructorInitializers` | `AfterColon` | Quebra a lista de inicializadores após `:`. | Não implementado com essa política configurável. |
| `ConstructorInitializerIndentWidth` | `4` | Define a indentação da lista de inicializadores. | Não implementado como opção independente. |
| `Cpp11BracedListStyle` | `false` | Seleciona espaçamento e quebra para listas com chaves. | A quebra ainda é própria do formatter; o espaçamento antes das chaves é configurável separadamente por `format.space-before-cpp11-braced-list`. |
| `ExperimentalAutoDetectBinPacking` | `true` | Detecta automaticamente o empacotamento de parâmetros/argumentos. | Não implementado. |
| `IndentCaseLabels` | `true` | Indenta rótulos `case` dentro de `switch`. | O tratamento é definido pelo formatter; não há chave de projeto. |
| `IndentPPDirectives` | `BeforeHash` | Controla a indentação de diretivas do pré-processador. | Diretivas são preservadas; não há opção de indentação. |
| `IndentWidth` | `4` | Define espaços por nível de indentação. | Suportado. Chave: `format.indent-width` (inteiro). |
| `Language` | `Cpp` | Seleciona regras de linguagem C++. | Heimdall formata C++; não é uma opção configurável. |
| `NamespaceIndentation` | `All` | Indenta o conteúdo de namespaces. | Aplicado pelo modelo de indentação; não há chave de projeto independente. |
| `PointerAlignment` | `Left` | Em declaradores, associa `*` ao tipo (`int* value`). | Suportado. Chave: `format.pointer-alignment` (`left`/`right`). Heimdall oferece também `reference-alignment`, além das opções do Freyr. |
| `ReflowComments` | `true` | Requebra parágrafos de comentários para respeitar largura. | Não implementado; comentários multilinha são preservados. |
| `SpaceAfterCStyleCast` | `true` | Insere espaço após casts C-style. | Suportado para casts com tipo fundamental; chave: `format.space-after-c-style-cast` (boolean). |
| `SpaceAfterLogicalNot` | `false` | Controla espaço entre `!` e o operando. | Suportado; chave: `format.space-after-logical-not` (boolean). |
| `SpaceBeforeCpp11BracedList` | `true` | Controla espaço antes de inicializadores com chaves. | Suportado; chave: `format.space-before-cpp11-braced-list` (boolean). |
| `SpaceBeforeParens` | `ControlStatements` | Usa espaço antes de parênteses de controle (`if (...)`), não em chamadas. | Não configurável por projeto. |
| `UseTab` | `Never` | Seleciona tabs ou espaços para indentação. | Suportado. Chave: `format.use-tabs` (boolean). |
| `PenaltyIndentedWhitespace` | `1` | Peso usado pelo algoritmo de decisão de quebras de linha. | Não há algoritmo de custo equivalente configurável. |

## Outras opções Heimdall já configuráveis

Estas opções não correspondem diretamente a uma das 31 entradas do Freyr, mas podem ser
ajustadas por projeto:

| Chave em `.heimdall.json` | Tipo / valores | Default |
|---|---|---|
| `format.max-empty-lines` | inteiro não negativo | `1` |
| `format.sort-includes` | boolean | `false` |
| `format.blank-line-after-control-block` | boolean | `true` |
| `format.blank-line-after-type-definition` | boolean | `true` |
| `format.space-before-inheritance-colon` | boolean | `true` |
| `format.reference-alignment` | `left` / `right` | `left` |
| `format.blank-line-between-methods` | boolean | `true` |
| `format.max-parameters-per-line` | inteiro não negativo; `0` desativa | `3` |

As opções descritas como não implementadas não são aceitas no objeto `format`; o loader
rejeita chaves desconhecidas em vez de ignorá-las silenciosamente. A tabela é também o
inventário das diferenças atuais entre o conjunto de regras do Freyr e o formatter próprio
do Heimdall.
