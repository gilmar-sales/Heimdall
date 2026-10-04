# Levantamento de regras da Rule Engine

Este documento reúne regras candidatas para qualidade de vida, modernização de C++, segurança de memória, concorrência e robustez. O status reflete o código presente no repositório: as regras lexicais estão em `core/`; `semantic/no-unused-local` está no analisador semântico e requer `--semantic`.

## Status

- **Implementada**: existe no projeto, mesmo que a cobertura seja parcial.
- **Não implementada**: proposta; ainda não existe como regra funcional.
- A implementação de uma regra semântica pode ter cobertura limitada. Consulte as observações após as tabelas.

## Qualidade de vida e consistência

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `format/no-trailing-whitespace` | Espaços e tabulações no fim da linha | Lexical | Seguro | Implementada |
| `format/require-final-newline` | Arquivo sem newline final | Lexical | Seguro | Implementada |
| `cpp/no-null` | Uso de `NULL` em vez de `nullptr` | Lexical | Geralmente seguro | Implementada |
| `cpp/no-zero-as-null` | `0` ou `0L` usado como ponteiro nulo | Sintática/semântica | Seguro (só com tipo conhecido) | Implementada (parcial) |
| `cpp/modernize-nullptr` | Conversões antigas de constante nula para ponteiro (`(T*)0`, `static_cast<T*>(0)`) | Semântica | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-auto` | Tipos explícitos substituíveis por `auto` | Sintática/semântica | Seguro (tipo repetido no inicializador) | Implementada (parcial) |
| `cpp/modernize-range-loop` | Laços por índice (`for (int i = 0; i < c.size(); ++i)`) substituíveis por range-for | Semântica | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-loop-convert` | Laços por iterador (`begin()`/`end()`) substituíveis por range-for | Semântica | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-using` | `typedef` substituível por `using` | Sintática | Possível | Implementada |
| `cpp/modernize-override` | Sobrescrita virtual sem `override` | Semântica | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-final` | Classes/métodos elegíveis a `final` | Semântica/projeto | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-constexpr` | Funções/expressões elegíveis a `constexpr` | Semântica/fluxo | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-const` | Variáveis que não são modificadas e podem ser `const` | Fluxo de dados | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/no-implicit-bool-conversion` | Conversões implícitas suspeitas em condições | Semântica | Não por padrão | Implementada (parcial) |
| `cpp/no-magic-numbers` | Literais numéricos sem contexto explicativo | Sintática | Não | Implementada |
| `cpp/modernize-const` | Motor semântico (F4, `FlowModel`: CFG e def-use por função): variável local com inicializador que nenhum uso escreve, modifica ou deixa escapar; requer `--semantic` | Só valores e objetos de classe de tipo conhecido; referências, ponteiros, parâmetros, variáveis de cabeçalho de laço, várias declarações no mesmo `;` e inicializadores constantes (que são de `modernize-constexpr`) ficam de fora. Passar a um chamador desconhecido, tomar o endereço, ligar a referência não-const, capturar em lambda, `decltype(x)`, aparecer em `{x}`/`T t(x)` ou em macro conta como escape e silencia. Objeto de classe só vale com chamadas a membros sabidamente `const` (lista curta para `std::`); `return s;` por nome fica de fora (o `const` impediria o move). Funções com `goto`, `asm`, corrotinas, nós de erro ou aninhamento acima de 128 níveis são ignoradas. Parâmetros de lambda não são ligados pela gramática. Quick fix (não aplicado por `--fix`) insere `const` antes do tipo |
| `cpp/modernize-constexpr` | Motor semântico (F4): (1) `const T x = c;` cujo inicializador é expressão constante, de tipo aritmético ou enum (troca `const` por `constexpr`; vale para globais e `static const` de classe); (2) local que nada modifica, com inicializador constante (insere `constexpr`); (3) função com ligação interna (`static`, namespace anônimo), `inline` ou membro `static`, com parâmetros e retorno de tipos literais, cujo corpo só usa o que um avaliador constante aceita; requer `--semantic` | O avaliador próprio calcula inteiros de 32 bits (estouro com sinal, divisão por zero e deslocamento inválido deixam a regra em silêncio) e `double`; `long`/`size_t` têm largura dependente de plataforma e não têm valor rastreado. Casts C, `sizeof`, strings, `.`/`->`, `new`/`delete`, `throw`, `try`, lambdas, locais `static`, `goto`, globais não constantes, chamadas desconhecidas, templates e recursão mútua tornam a função inelegível. Funções de ligação externa nunca são sugeridas: `constexpr` implica `inline` e quebraria quem as chama de outros arquivos; redeclaração da mesma assinatura também bloqueia. Quick fix (não aplicado por `--fix`) |
| `cpp/no-empty-catch` | Bloco `catch` vazio | Sintática | Não | Implementada |
| `cpp/no-todo` | Comentários `TODO`, `FIXME` ou `XXX` | Lexical | Não | Implementada |
| `cpp/no-duplicate-include` | Includes duplicados | Diretivas | Possível | Implementada |
| `cpp/no-unused-include` | Include direto cujos nomes nunca aparecem no arquivo | Semântica/projeto | Quick fix (não em lote) | Implementada |
| `cpp/prefer-forward-declaration` | Em headers, include de header do projeto usado só para ponteiros/referências a classes | Semântica/projeto | Quick fix (não em lote) | Implementada |
| `cpp/no-circular-include` | Include cujo fecho transitivo volta ao próprio arquivo (inclui auto-inclusão) | Semântica/projeto | Não | Implementada (severidade padrão: erro) |
| `cpp/sort-includes` | Includes fora da ordem configurada | Diretivas | Seguro com configuração | Implementada |
| `cpp/include-what-you-use` | Nome usado cujo header só é incluído indiretamente (os desnecessários são `cpp/no-unused-include`) | Semântica/projeto | Quick fix (não em lote) | Implementada (parcial) |

## Modernização de C++

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `cpp/modernize-nullptr` | Literal nulo legado | Lexical/sintática | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-override` | Método sobrescrito sem `override` | Semântica | Quick fix (não em lote) | Implementada (parcial) |
| `cpp/modernize-using` | `typedef` legado | Sintática | Possível | Implementada |
| `cpp/modernize-emplace` | Construção temporária em `push_back` potencialmente substituível por `emplace_back` | Semântica | Condicional | Não implementada |
| `cpp/modernize-make-unique` | Construção manual de `unique_ptr` | Semântica | Condicional | Não implementada |
| `cpp/modernize-make-shared` | Construção manual de `shared_ptr` | Semântica | Condicional | Não implementada |
| `cpp/modernize-smart-ptr` | Ownership representado por ponteiro cru | Semântica/fluxo | Condicional | Não implementada |
| `cpp/no-new-delete` | Uso direto de `new`/`delete` em código comum | Sintática/semântica | Não por padrão | Não implementada |
| `cpp/modernize-span` | Ponteiro e tamanho usados como faixa de dados | Semântica | Condicional | Não implementada |
| `cpp/modernize-string-view` | Parâmetro de leitura que copia `std::string` sem necessidade | Semântica | Condicional | Não implementada |
| `cpp/modernize-algorithms` | Laços substituíveis por algoritmos da biblioteca | Semântica | Condicional | Não implementada |
| `cpp/modernize-structured-bindings` | Acesso repetido a campos de pares/tuplas | Sintática | Condicional | Não implementada |
| `cpp/modernize-attributes` | Oportunidades para atributos como `[[nodiscard]]` | Semântica/API | Condicional | Não implementada |
| `cpp/modernize-consteval-constexpr` | Oportunidades para avaliação em tempo de compilação | Semântica | Cauteloso | Não implementada |

`new` e `delete` não devem ser proibidos sem exceções: alocadores, placement new, interoperabilidade e infraestrutura podem precisar deles. Regras para `auto`, `const`, range-for e includes também precisam evitar alterações em overload resolution, cópias e vida útil.

## Segurança de memória e ownership

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `memory/use-after-free` | Uso após liberação do objeto | Fluxo de dados | Não | Não implementada |
| `memory/double-free` | Liberação repetida do mesmo recurso | Fluxo de dados | Não | Não implementada |
| `memory/dangling-reference` | Retorno/armazenamento de referência ou ponteiro para variável local | Semântica/fluxo | Não | Não implementada |
| `memory/dangling-view` | `string_view`, `span` ou iterador que excede a vida do buffer | Semântica/fluxo | Não | Não implementada |
| `memory/invalidated-iterator` | Uso de iterador/referência invalidado por mutação do contêiner | Semântica/fluxo | Não | Não implementada |
| `memory/null-dereference` | Desreferência de ponteiro possivelmente nulo | Fluxo de dados | Não | Não implementada |
| `memory/out-of-bounds` | Índice ou faixa fora dos limites conhecidos | Semântica/fluxo | Não | Não implementada |
| `memory/unchecked-access` | Acesso não verificado potencialmente inseguro | Semântica | Condicional | Não implementada |
| `memory/array-new-delete-mismatch` | Combinação incorreta de `new[]`/`delete` | Sintática/semântica | Não | Não implementada |
| `memory/resource-leak` | Caminho de saída sem liberação do recurso | Fluxo de dados | Não | Não implementada |
| `memory/exception-unsafe-resource` | Recurso cru vulnerável a saída antecipada/exceção | Semântica/fluxo | Não | Não implementada |
| `memory/unique-ownership` | Ownership exclusivo representado por ponteiro cru ou `shared_ptr` desnecessário | Semântica/fluxo | Condicional | Não implementada |
| `memory/shared-ownership-cycle` | Ciclo provável de `shared_ptr` | Semântica/fluxo | Não | Não implementada |
| `memory/uninitialized-read` | Leitura antes da inicialização | Fluxo de dados | Não | Não implementada |
| `memory/use-after-move` | Uso suspeito de objeto após move | Semântica/fluxo | Não | Não implementada |
| `memory/unsafe-reinterpret-cast` | Cast com risco de alinhamento, aliasing ou representação | Semântica | Não | Não implementada |
| `memory/strict-aliasing` | Acesso incompatível com regras de aliasing | Semântica | Não | Não implementada |
| `memory/size-overflow` | Overflow em tamanho antes de alocação/cópia | Fluxo de dados | Não | Não implementada |
| `memory/unsafe-memcpy` | Tamanho ou tipo incompatível em `memcpy`/`memmove` | Semântica/fluxo | Não | Não implementada |
| `memory/stack-address-escape` | Endereço de variável local que escapa do escopo | Semântica/fluxo | Não | Não implementada |
| `memory/borrowed-resource-escape` | View, span ou ponteiro emprestado que excede a vida do dono | Semântica/fluxo | Não | Não implementada |

## Concorrência e sincronização

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `concurrency/data-race` | Acesso concorrente a dado mutável sem sincronização conhecida | Interprocedural/concorrência | Não | Não implementada |
| `concurrency/unguarded-shared-state` | Estado global/compartilhado mutável sem proteção | Semântica/concorrência | Não | Não implementada |
| `concurrency/lock-not-held` | Acesso a dado protegido sem o mutex esperado | Semântica/concorrência | Não | Não implementada |
| `concurrency/double-lock` | Reaquisição de mutex não recursivo | Semântica/concorrência | Não | Não implementada |
| `concurrency/lock-order-inversion` | Ordem inconsistente de aquisição, com risco de deadlock | Interprocedural/concorrência | Não | Não implementada |
| `concurrency/lock-held-across-call` | Chamada desconhecida ou bloqueante enquanto há lock | Semântica/concorrência | Não | Não implementada |
| `concurrency/callback-under-lock` | Callback sob mutex, com risco de reentrância/deadlock | Semântica/concorrência | Não | Não implementada |
| `concurrency/missing-join-or-detach` | Thread sem gerenciamento claro de vida | Semântica | Não | Não implementada |
| `concurrency/thread-lifetime` | Thread que acessa estado após destruição do dono | Interprocedural/concorrência | Não | Não implementada |
| `concurrency/capture-dangling-reference` | Lambda assíncrona captura variável local por referência | Semântica/concorrência | Não | Não implementada |
| `concurrency/async-reference-capture` | Tarefa assíncrona retém referências sem garantia de vida útil | Semântica/concorrência | Não | Não implementada |
| `concurrency/unsafe-shared-mutable` | `shared_ptr` compartilhado a objeto mutável sem sincronização | Semântica/concorrência | Não | Não implementada |
| `concurrency/volatile-is-not-atomic` | `volatile` usado como sincronização | Sintática/semântica | Não | Não implementada |
| `concurrency/atomic-ordering` | Ordem de memória suspeita ou aparentemente insuficiente | Semântica/concorrência | Não | Não implementada |
| `concurrency/condition-variable-predicate` | Espera sem predicado ou suscetível a sinalização perdida | Semântica | Não | Não implementada |
| `concurrency/stop-token-ignored` | Trabalho cancelável que não observa cancelamento | Semântica | Não | Não implementada |
| `concurrency/blocking-on-ui-or-io-thread` | Operação longa/bloqueante em thread sensível à latência | Semântica/configuração | Não | Não implementada |

Detecção confiável de data race e deadlock exige mais que análise local. A Rule Engine pode complementar ferramentas como ThreadSanitizer e Clang-Tidy, mas não deve alegar substituí-las.

## Robustez e segurança de entrada

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `security/unsafe-c-function` | APIs de cópia/formatação sem limite ou com risco conhecido | Sintática/semântica | Condicional | Não implementada |
| `security/format-string` | Formato controlável externamente ou incompatível com argumentos | Semântica/taint | Não | Não implementada |
| `security/integer-overflow` | Overflow que afeta tamanho, índice ou segurança | Fluxo de dados | Não | Não implementada |
| `security/tainted-input` | Entrada externa que chega a shell, SQL, caminho ou formato sem validação | Taint/interprocedural | Não | Não implementada |
| `security/path-traversal` | Caminho controlável que pode escapar do diretório-base | Taint/interprocedural | Não | Não implementada |
| `security/command-injection` | Entrada concatenada em comando de sistema | Taint/interprocedural | Não | Não implementada |
| `security/untrusted-deserialization` | Desserialização de entrada não confiável sem validação | Semântica/taint | Não | Não implementada |
| `security/insecure-random` | PRNG não criptográfico usado para segredo/token | Semântica/contexto | Não | Não implementada |
| `security/hardcoded-secret` | Credenciais, chaves ou tokens aparentes no código | Lexical/heurística | Não | Não implementada |
| `security/tls-verification-disabled` | Verificação de certificado explicitamente desativada | Semântica/configuração | Não | Não implementada |
| `security/unsafe-temp-file` | Arquivo temporário com criação previsível/insegura | Semântica/plataforma | Não | Não implementada |
| `security/unchecked-result` | Resultado ignorado de operação crítica | Semântica | Não | Não implementada |
| `security/assert-for-input-validation` | `assert` usado para validar entrada externa | Semântica | Não | Não implementada |

## API e correção semântica

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `api/virtual-destructor` | Classe polimórfica sem destrutor virtual apropriado | Semântica | Condicional | Implementada (parcial) |
| `api/missing-nodiscard` | Função cujo resultado não deveria ser ignorado | Semântica/API | Condicional | Não implementada |
| `api/pass-by-value` | Parâmetro caro passado por valor sem necessidade aparente | Semântica | Condicional | Não implementada |
| `api/pass-by-const-reference` | Objeto grande de leitura passado por valor | Semântica | Condicional | Não implementada |
| `api/const-correctness` | Métodos/parâmetros que poderiam ser `const` | Semântica | Condicional | Não implementada |
| `api/unsafe-downcast` | Conversão descendente sem verificação | Semântica | Não | Não implementada |
| `api/slicing` | Tipo derivado armazenado/passado por valor como base | Semântica | Não | Não implementada |
| `api/implicit-conversion` | Conversores implícitos propensos a conversão acidental | Semântica/API | Não | Não implementada |
| `api/explicit-constructor` | Construtor de um argumento sem `explicit` | Semântica | Possível | Implementada (parcial) |
| `api/virtual-call-in-constructor` | Chamada virtual em construtor/destrutor | Semântica | Não | Implementada (parcial) |
| `api/overload-hiding` | Declaração derivada que esconde overloads da base | Semântica | Possível | Implementada (parcial) |

## Regras implementadas hoje

| Código | Implementação atual | Limitações |
|---|---|---|
| `cpp/no-null` | `RuleEngine` encontra `NULL` fora de comentários, literais e diretivas e sugere `nullptr` | Não substitui toda análise semântica de contexto |
| `format/no-trailing-whitespace` | Remove espaços e tabulações no fim das linhas | Autofix preserva o conteúdo restante e trata CRLF |
| `format/require-final-newline` | Garante newline final | Preserva CRLF quando detecta esse estilo |
| `cpp/modernize-override` | Motor semântico (`SemanticModel` + `Binder`, ver [semantic-engine-architecture.md](semantic-engine-architecture.md)): método de classe que casa nome e assinatura com uma função virtual de uma base declarada no mesmo arquivo; requer `--semantic` | Só bases do próprio arquivo (as bases de headers já são conhecidas pelo `HeaderSummary` da F3, mas esta regra ainda não as usa); bases com template (`Base<T>`) ou não resolvidas deixam a regra em silêncio; assinatura comparada pela grafia dos tipos dos parâmetros (`int` e `std::int32_t` não casam); destrutores e funções-template não são analisados; o fix só é oferecido como quick fix |
| `cpp/modernize-nullptr` | Motor semântico: cast C ou `static_cast`/`reinterpret_cast` de `0`, `0L` ou `NULL` para tipo ponteiro; requer `--semantic` | Não reporta o idioma `offsetof` (`((T*)0)->m`), código inativo (`#if 0`) nem macros; o fix é só quick fix porque `nullptr` pode mudar overload e dedução de `auto` |
| `cpp/no-zero-as-null` | Motor semântico: `0`/`0L` em inicialização, atribuição e comparação com variável ou parâmetro declarado com `*`, e `return 0;` em função cujo tipo de retorno escrito é ponteiro; requer `--semantic` | Ponteiros via `typedef`, `auto`, referência ou array não contam (tipo desconhecido); argumentos de chamada ficam de fora (exigem overload resolution); acesso por objeto (`s.p = 0`) fica de fora até a F2 |
| `cpp/modernize-auto` | Motor semântico: `T* p = new T...`, `T x = static_cast<T>(...)` (e `dynamic_`/`reinterpret_`/`const_cast`) e `std::unique_ptr<T> p = std::make_unique<T>(...)` (idem `shared_ptr`); requer `--semantic` | Só quando o tipo é idêntico, token a token; ignora `const`/`volatile`/referências/arrays, vários declaradores, placement new, membros de classe e cast de constante nula; declarações de iterador exigem o tipo do contêiner e não são reportadas; no escopo de namespace, `T x = f(...)` é lido como declaração de função pela gramática e não é analisado |
| `cpp/no-implicit-bool-conversion` | Motor semântico (Typer, F2): expressão de tipo inteiro, ponto flutuante ou ponteiro, de tipo conhecido, usada onde se espera `bool`: condição de `if`/`while`/`for`/`?:` e operandos de `!`, `&&` e `\|\|`; requer `--semantic` | Tipo desconhecido (template, nome não resolvido, tipo da biblioteca como `std::unique_ptr`) deixa a regra em silêncio, assim como classes e enums; literais (`while (1)`) e `!!x` são tratados como intencionais; condições com declaração ou init-statement (`if (T* p = f())`, `if (init; c)`), `do`/`while` e casts C (`(bool)x`) não são analisados; sem autofix: a comparação certa depende da intenção |
| `cpp/modernize-range-loop` | Motor semântico (Typer, F2): `for (T i = 0; i < c.size(); ++i)` (também `i++`, `i += 1`, `std::size(c)` e limite literal igual ao tamanho de um array) sobre variável local ou parâmetro que o Typer sabe ser array ou `std::vector`/`deque`/`array`/`string`/`string_view`/`span`, cujo corpo só usa `c[i]`; requer `--semantic` | Silêncio se `i` ou `c` aparece de outra forma no corpo (`c[i + 1]`, `c.push_back`, `use(i)`), se há lambda, se `c` é membro de classe, `std::vector<bool>` ou de tipo desconhecido; o quick fix (não aplicado por `--fix`) reescreve o laço inteiro com `auto&` (`const auto&` se o contêiner é `const`) e escolhe um nome livre para o elemento |
| `cpp/modernize-loop-convert` | Motor semântico (Typer, F2): `for (auto it = c.begin(); it != c.end(); ++it)` (também `cbegin`/`cend`, `std::begin(c)`/`std::end(c)` e `it++`) sobre variável local ou parâmetro que o Typer sabe ser contêiner da biblioteca padrão (ou array, com `std::begin`), cujo corpo só usa `*it` e `it->`; requer `--semantic` | Silêncio se o iterador escapa (`use(it)`, `it + 1`, `erase(it)`), se o contêiner é usado no corpo, se há lambda, ou se o contêiner é membro, `std::vector<bool>` ou de tipo desconhecido; só `auto` (iteradores com tipo escrito, `rbegin`/`rend` e `begin() + n` não casam); o quick fix (não aplicado por `--fix`) reescreve o laço inteiro. Laços que fariam melhor com algoritmos (`std::find`, `std::accumulate`) ainda não são detectados |
| `cpp/include-what-you-use` | Motor semântico (F3, `HeaderSummary` + `ProjectIndex`): identificador do arquivo cuja declaração vem de um header que o arquivo só alcança por outro include. Para headers do projeto o mapa símbolo → header sai do `HeaderSummary` de cada header do fecho de includes (nomes declarados em escopo de namespace/global, com o namespace); para a biblioteca padrão, de uma tabela curada (`std::vector` → `<vector>`...). Uma ocorrência por header, na primeira; requer `--semantic` + compile command | Silêncio quando: algum include direto não foi encontrado (header entre aspas, ou angular fora de `#if`), o nome também é declarado no arquivo (inclusive parâmetros, locais, parâmetros de template e `class N;`), o nome vem de mais de um header, o uso é membro (`.`/`->`), o header é textual (`.inc/.def/.inl/.tpp...`) ou tem `IWYU pragma: private`, algum include direto o reexporta com `IWYU pragma: export`, ou o header vem do header primário (`foo.cpp` ↔ `foo.hpp`). Nome sem qualificação só casa com o namespace envolvente (ou um pai) ou com `using namespace`; função/variável sem qualificação fica de fora em arquivos com bases não resolvidas (podem ser membros herdados). Nomes da biblioteca padrão só contam se algum header da tabela estiver mesmo no fecho; `size_t`, `move`, `pair`, `begin` e semelhantes (vários donos) não estão na tabela. Macros ainda não entram. O quick fix insere o `#include` depois do último include incondicional, no estilo do projeto (angular se já há includes angulares de headers do projeto), e não é aplicado por `--fix` |
| `cpp/modernize-final` | Motor semântico (F3): classe polimórfica (virtual próprio ou herdado, inclusive de bases definidas em headers do projeto, via `HeaderSummary`) que nada deriva, e função `override` que nenhuma classe derivada redeclara. Só vale onde as derivadas são todas visíveis: namespace anônimo, ou qualquer classe definida em arquivo-fonte (`.cpp/.cc/.cxx/.c++/.cp`, que ninguém inclui); requer `--semantic` | Classes de header fora de namespace anônimo nunca são reportadas (qualquer outro arquivo pode derivar delas; ver [semantic-engine-architecture.md](semantic-engine-architecture.md)). Silêncio para classes abstratas (`= 0`), templates, uniões, já `final`, base de polimorfismo desconhecida (não achada, ou ambígua, nos headers), base não resolvida de outra classe com o mesmo nome (`ns::C`, `C<T>`) e header que liste a classe como base. Para métodos, basta o nome em uma derivada para considerar redeclarado (assinatura escrita de outro jeito ainda pode ser override); `final` em método só é sugerido quando a classe não é folha (folhas recebem a sugestão de classe). O quick fix (não aplicado por `--fix`) insere ` final` depois do nome da classe ou do `override` |
| `api/virtual-destructor` | Motor semântico: struct/class que declara função `virtual` ou pura e cujo destrutor não é virtual (nem herdado de base resolvida com destrutor virtual); requer `--semantic` | Silêncio para classe `final`, união, destrutor protegido/privado (idioma de base não deletável) e base não resolvida ou `Base<T>`. Quick fix (não aplicado por `--fix`) só quando o destrutor está declarado: insere `virtual` |
| `api/explicit-constructor` | Motor semântico: construtor definido na classe que pode ser chamado com um argumento (os demais com valor padrão) e não é `explicit`; requer `--semantic` | Silêncio para construtor de cópia/movimento (parâmetro que cita a própria classe por referência), `std::initializer_list`, variádico, `= default`/`= delete` e construtores definidos fora da classe. Quick fix (não aplicado por `--fix`) insere `explicit`, pois pode quebrar conversões implícitas existentes |
| `api/overload-hiding` | Motor semântico: função membro cujo nome é o de uma função virtual de base resolvida (direta ou indireta), sem `using Base::nome;` e sem declarar a mesma assinatura; requer `--semantic` | Só funções virtuais da base (como `-Woverloaded-virtual`); assinatura comparada pela grafia dos tipos dos parâmetros (`int` e `std::int32_t` não casam); templates, operadores e bases não resolvidas ficam de fora. Quick fix (não aplicado por `--fix`) insere `using Base::nome;` antes da função |
| `api/virtual-call-in-constructor` | Motor semântico: chamada não qualificada (ou `this->`) a função virtual em construtor ou destrutor definido dentro da classe; requer `--semantic` | Silêncio para classe ou função `final`, chamadas qualificadas (`A::f()`), por objeto (`o.f()`), dentro de lambda e construtores/destrutores definidos fora da classe; não segue chamadas indiretas (um não virtual que chama um virtual). Sem fix |
| `cpp/designated-init-order` | Motor semântico: `T{.b = 1, .a = 2}` com membros declarados na ordem `a`, `b` (C++20 exige a ordem da declaração); classe definida no arquivo, sem bases; requer `--semantic` | Silêncio para classe de cabeçalho/template/com base, membro desconhecido ou repetido e lista mista. Quick fix (não aplicado por `--fix`: muda a ordem de avaliação) permuta as entradas mantendo os separadores |
| `cpp/no-empty-catch` | `RuleEngine` encontra `catch` com corpo vazio (só espaço/comentários), fora de comentários, literais e diretivas | Quick fix (só no editor, não aplicado por `--fix`): insere `throw;` preservando comentários do corpo, pois a ação correta depende do contexto |
| `cpp/no-todo` | `RuleEngine` encontra marcadores `TODO`, `FIXME` ou `XXX` (maiúsculos, palavra inteira) em comentários de linha e de bloco | Sem autofix: o comentário precisa ser resolvido ou movido para um rastreador |
| `cpp/no-magic-numbers` | `RuleEngine` encontra literais numéricos fora de comentários, literais e diretivas, exceto `0` e `1` em qualquer base/escrita (`0x0`, `1u`, `0.0`, `1.0f`...) e exceto o valor que dá nome à constante (`constexpr`/`const`/`constinit` com inicializador direto, valores de `enum`) | Sem autofix: só o autor sabe o nome certo para a constante |
| `cpp/no-duplicate-include` | Compara includes literais (`<...>` e `"..."`) fora de blocos condicionais | Quick fix (só no editor, não aplicado por `--fix`) remove a linha do include duplicado: não é seguro em lote por causa de `#define`/`#undef` entre includes (X-macros); includes em ramos `#ifdef` distintos são ignorados |
| `cpp/no-unused-include` | `IncludeAnalyzer` resolve cada `#include` direto e seu fecho transitivo (inclui `#include_next`), extrai os nomes declarados em escopo de namespace/global, enumeradores e macros, e reporta o include se nenhum identificador do arquivo (fora de comentários, literais e linhas `#include`) coincide com eles | Requer `--semantic` + compile command (como `semantic/no-unused-local`). Conservadora: um nome fornecido transitivamente mantém o include direto "usado", exceto quando outro include direto já o cobre. Nunca reporta: header não resolvido ou com fecho incompleto, includes dentro de `#if` (guarda de include ignorada), header primário (`foo.cpp`/`foo.hpp`), `// IWYU pragma: keep/export`, arquivos só com includes, `.inc/.def/.inl/.tpp/.ipp/.tcc`, headers de uso implícito (`<initializer_list>`, `<compare>`, `<typeinfo>`, `<new>`, `<coroutine>`, `<tuple>`...) e headers do projeto com `operator` ou especialização em escopo de namespace. Nomes após `.`/`->` não contam. Cabeçalhos de sistema usam extração estrita (só nomes declarados); do projeto, extração ampla. Quick fix remove a linha, mas não é aplicado por `--fix` (pode haver dependência transitiva) |
| `cpp/prefer-forward-declaration` | Mesma análise de includes (`IncludeAnalyzer`), só em arquivos `.h/.hh/.hpp/.hxx/.h++` e só para headers fora dos diretórios do sistema. Todos os nomes do header usados no arquivo precisam ser `class`/`struct`/`union` não-template, fora de namespace anônimo/`inline`, e cada uso precisa ser `N *`/`N &` (com `const`/`volatile`), `class N;` ou `friend class N;`. Qualquer outro uso (valor, base, `N::`, argumento de template, `sizeof`, macro, corpo de função, inicializador, enumerador) impede o aviso, assim como desreferenciar por nome (`p->`, `p.`, `p[`, `*p`, `delete p`, casts) um ponteiro/referência declarado como `N *p` | Mesmos requisitos de `--semantic` + compile command. O quick fix troca a linha do include pelas declarações (`namespace a::b { class N; }`), mas não é aplicado por `--fix`: o `.cpp` correspondente precisa incluir o header. Corpos inline que só repassam o ponteiro são aceitos |
| `cpp/no-circular-include` | `IncludeAnalyzer` compara o fecho transitivo de cada `#include` não condicional com o próprio arquivo (`std::filesystem::equivalent`) | Único aviso com severidade `error` por padrão; sem quick fix (a correção certa é reestruturar, por exemplo com forward declaration). Quando há ciclo, só ele é reportado para aquele include: os nomes do próprio arquivo "voltam" pelo ciclo e mascarariam `no-unused-include`. Requer `--semantic` + compile command |
| `cpp/modernize-using` | Reescreve `typedef` de declarador simples como `using T = ...`, com autofix | Pula ponteiros de função, definições de classe, múltiplos declaradores e atributos; requer forma tokenizável |
| `cpp/sort-includes` | `RuleEngine` agrupa includes literais em blocos de linhas adjacentes e compara cada bloco com a ordem configurada (`include-order`) | Opt-in: só roda quando `"rules"` ou `--rule` habilita o código (a ordem é convenção do projeto). Autofix seguro em lote reordena apenas dentro de um bloco; linhas em branco, comentários, outras diretivas, `#include_next` e includes por macro quebram blocos. Ordem padrão `angle` antes de `quote`, comparação sem maiúsculas/minúsculas (configurável); preserva terminadores de linha e indentação |
| `semantic/no-unused-local` | Analisador semântico detecta algumas variáveis locais não usadas | Cobertura limitada a declarações simples; requer `--semantic` e contexto de compilação |

## Priorização sugerida

1. **Curto prazo:** completar testes e metadados das regras existentes; adicionar regras locais de alta confiança, como `cpp/no-empty-catch`, includes duplicados e modernizações sintáticas seguras.
2. **Médio prazo:** melhorar o parser/análise semântica para `override`, `using`, `make_unique`, variáveis não usadas, dangling references/views e uso após move.
3. **Longo prazo:** análise de fluxo para null, limites, leaks e ownership; análise de concorrência e taint tracking; integração opcional com ferramentas especializadas.

## Requisitos para implementação

- Registrar código estável, categoria, severidade padrão, camada requerida e disponibilidade de autofix para cada regra.
- Manter regras semânticas opt-in, para não impor custo a análises que só precisam de lint lexical/sintático.
- Distinguir correção segura de sugestão; autofix em lote deve aplicar apenas correções seguras por padrão.
- Testar positivos, negativos, código incompleto, macros/diretivas e preservação de CRLF/comentários em autofixes.
- Permitir configuração, exclusões e supressões locais com validação do código da regra.
- Preferir nenhum diagnóstico a um diagnóstico sem evidência suficiente; não implementar regras semânticas como buscas textuais frágeis.

## Infraestrutura disponível

A Rule Engine agora também oferece:

- **Catálogo de metadados**: `RuleCatalog()` registra código estável, categoria,
  severidade padrão, camada requerida e disponibilidade de autofix de cada regra.
  `IsKnownRuleCode()` é a fonte única de validação dos códigos no carregador de
  configuração e na CLI, então uma regra nova só precisa de uma entrada no catálogo.
- **Overrides por código** via `RuleOptions::overrides`, com habilitação/desabilitação e severidade (`Warning`/`Error`). O último override para o mesmo código prevalece. Para regras opt-in como `cpp/sort-includes`, um override habilitado também liga a regra.
- **Ordem de includes** (`cpp/sort-includes`): a seção `"include-order"` do arquivo de configuração registra a convenção do projeto:

  ```json
  {"rules": {"cpp/sort-includes": "warning"},
   "include-order": {"groups": ["angle", "quote"], "case-insensitive": true}}
  ```

  `groups` lista os grupos `"angle"` (`<...>`) e `"quote"` (`"..."`) na ordem desejada, ambos obrigatórios; `case-insensitive` (padrão `true`) define a comparação. A configuração mais próxima prevalece. A regra é opt-in: só diagnostica quando habilitada por `"rules"` ou por `--rule cpp/sort-includes=warning`.
- **CLI**: `heimdall lint --rule cpp/no-null=off arquivo.cpp` desabilita uma regra; `--rule cpp/no-null=error` eleva sua severidade. Os valores aceitos são `off`, `warning` e `error`; códigos desconhecidos são rejeitados. A opção está disponível em `lint` e `check`.
- **Supressão por comentário**: `// heimdall-disable-line cpp/no-null` suprime o código na linha do comentário; `// heimdall-disable-next-line cpp/no-null` suprime na linha seguinte. Pode-se listar códigos separados por espaço/vírgula ou omitir a lista/usar `*` para suprimir todos os diagnósticos naquela linha.
- **Validação de autofix**: `ApplyFixes` ignora edições cuja faixa não esteja contida na faixa do diagnóstico, além de rejeitar edições fora do arquivo ou sobrepostas. Não muda a política de quais regras oferecem correções.

Exemplos:

```sh
heimdall lint --rule cpp/no-null=error src
heimdall check --rule format/no-trailing-whitespace=off src
heimdall lint --rule cpp/sort-includes=warning --fix src
```

Esses mecanismos são infraestrutura; não adicionam regras de lint além das listadas como implementadas acima. Supressões de bloco (`disable`/`enable`) e configuração via arquivo ainda não estão disponíveis.
