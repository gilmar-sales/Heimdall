# Parse integrado com resolução de nomes

A gramática de C++ não é livre de contexto: `a * b;` é uma declaração se `a` é um tipo e um produto se `a` é uma variável; `(a) x` é um cast se `a` é um tipo e um erro se não é. Compiladores resolvem isso consultando uma tabela de símbolos **durante** o parse. O Heimdall faz o mesmo, em escala reduzida, sem abandonar a separação `ParseTree → Binder → Typer`.

## O que o parser sabe

| Fonte | Quando | O que registra |
|---|---|---|
| O próprio arquivo | enquanto o parser avança | tipos (`class`/`struct`/`union`/`enum`, `using X =`, `typedef`), variáveis, funções, enumeradores |
| `TypeNameOracle` (`ParserOptions::type_names`) | injetado por quem chama | tipos declarados em nível de namespace pelos headers incluídos |

- Nomes são comparados pelo último componente (`string` para `std::string`): é o que basta para decidir tipo ou valor.
- **Escopos de bloco.** Cada `{ }` e cada `for`/`if`/`while` abre um escopo; parâmetros entram no escopo do corpo. Um valor local esconde um tipo de mesmo nome (`T * x;` depois de `int T;` é um produto).
- **Nível de arquivo.** Uma tabela plana de nomes, registrada à medida que cada declaração é produzida, com um hash independente de ordem.

## O que muda na árvore

- `a * b;` com `a` variável conhecida vira `ExpressionStatement`, não `DeclarationStatement`.
- `(T) operando`, com `T` certamente um tipo (builtin, ou nome conhecido como tipo), vira `CastExpression` com dois filhos: `TypeSpecifier` e o operando. O Typer dá ao nó o tipo do cast.
- **Nome desconhecido mantém a leitura de antes** (declaração; parêntese seguido de operando). Informação ausente nunca piora o parse: o oráculo só refina.

## Reuso incremental

Um item de topo deixou de depender só dos próprios tokens: apagar um `struct Foo` muda como os itens seguintes são lidos. Por isso cada `TopLevelItem` guarda o `names_hash` do estado em que começou, e só é copiado quando o estado atual tem o mesmo hash. O hash começa com `TypeNameOracle::Fingerprint()`, então trocar o conjunto de headers também invalida o reuso. Os itens copiados registram seus nomes de novo (`RegisterNodes`), para que o estado siga igual ao de um parse completo.

## No LSP

- `IncludeIndex::TypeNames()` constrói o oráculo junto com o índice de headers.
- O servidor guarda o último oráculo por documento (`m_type_names`) e o entrega ao `CachedParse`. **O parse nunca espera o índice**: sem oráculo ainda, vale só o que o arquivo declara.
- Quando o índice chega e o conjunto de nomes muda, o cache de parse do documento é invalidado e a próxima requisição relê o arquivo com os nomes novos.

## Limites conhecidos

- Só nomes de nível de namespace vêm dos headers; membros de classe herdados de headers não são vistos.
- `using namespace` e escopos de namespace não filtram a consulta: um tipo de qualquer header incluído conta, a menos que o arquivo declare um valor com o mesmo nome.
- Template com `<` ambíguo (`a < b > (c)`) ainda segue a heurística.
- Cast de função (`T(x)`) e a decisão `T(x);` declaração vs. chamada não usam a tabela.
