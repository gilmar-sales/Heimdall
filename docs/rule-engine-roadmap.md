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
| `cpp/no-zero-as-null` | `0` ou `0L` usado como ponteiro nulo | Sintática/semântica | Só com tipo conhecido | Não implementada |
| `cpp/modernize-nullptr` | Conversões e comparações antigas com ponteiro nulo | Semântica | Condicional | Não implementada |
| `cpp/modernize-auto` | Tipos explícitos substituíveis por `auto` | Sintática/semântica | Condicional | Não implementada |
| `cpp/modernize-range-loop` | Laços substituíveis por range-for | Semântica | Condicional | Não implementada |
| `cpp/modernize-loop-convert` | Laços convertíveis a algoritmos ou ranges | Semântica | Condicional | Não implementada |
| `cpp/modernize-using` | `typedef` substituível por `using` | Sintática | Possível | Não implementada |
| `cpp/modernize-override` | Sobrescrita virtual sem `override` | Semântica | Possível | Não implementada |
| `cpp/modernize-final` | Classes/métodos elegíveis a `final` | Semântica | Cauteloso | Não implementada |
| `cpp/modernize-constexpr` | Funções/expressões elegíveis a `constexpr` | Semântica | Cauteloso | Não implementada |
| `cpp/modernize-const` | Variáveis que não são modificadas e podem ser `const` | Fluxo de dados | Condicional | Não implementada |
| `cpp/no-implicit-bool-conversion` | Conversões implícitas suspeitas em condições | Semântica | Não por padrão | Não implementada |
| `cpp/no-magic-numbers` | Literais numéricos sem contexto explicativo | Sintática | Não | Não implementada |
| `cpp/no-empty-catch` | Bloco `catch` vazio | Sintática | Não | Não implementada |
| `cpp/no-todo` | Comentários `TODO`, `FIXME` ou `XXX` | Lexical | Não | Não implementada |
| `cpp/no-duplicate-include` | Includes duplicados | Diretivas | Possível | Não implementada |
| `cpp/sort-includes` | Includes fora da ordem configurada | Diretivas | Seguro com configuração | Não implementada |
| `cpp/include-what-you-use` | Includes ausentes ou desnecessários | Semântica/projeto | Fora da primeira fase | Não implementada |

## Modernização de C++

| Regra | O que detecta | Camada | Autofix | Status |
|---|---|---|---|---|
| `cpp/modernize-nullptr` | Literal nulo legado | Lexical/sintática | Condicional | Não implementada |
| `cpp/modernize-override` | Método sobrescrito sem `override` | Semântica | Possível | Não implementada |
| `cpp/modernize-using` | `typedef` legado | Sintática | Possível | Não implementada |
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
| `api/virtual-destructor` | Classe polimórfica sem destrutor virtual apropriado | Semântica | Condicional | Não implementada |
| `api/missing-nodiscard` | Função cujo resultado não deveria ser ignorado | Semântica/API | Condicional | Não implementada |
| `api/pass-by-value` | Parâmetro caro passado por valor sem necessidade aparente | Semântica | Condicional | Não implementada |
| `api/pass-by-const-reference` | Objeto grande de leitura passado por valor | Semântica | Condicional | Não implementada |
| `api/const-correctness` | Métodos/parâmetros que poderiam ser `const` | Semântica | Condicional | Não implementada |
| `api/unsafe-downcast` | Conversão descendente sem verificação | Semântica | Não | Não implementada |
| `api/slicing` | Tipo derivado armazenado/passado por valor como base | Semântica | Não | Não implementada |
| `api/implicit-conversion` | Conversores implícitos propensos a conversão acidental | Semântica/API | Não | Não implementada |
| `api/explicit-constructor` | Construtor de um argumento sem `explicit` | Semântica | Possível | Não implementada |
| `api/virtual-call-in-constructor` | Chamada virtual em construtor/destrutor | Semântica | Não | Não implementada |
| `api/overload-hiding` | Declaração derivada que esconde overloads da base | Semântica | Possível | Não implementada |

## Regras implementadas hoje

| Código | Implementação atual | Limitações |
|---|---|---|
| `cpp/no-null` | `RuleEngine` encontra `NULL` fora de comentários, literais e diretivas e sugere `nullptr` | Não substitui toda análise semântica de contexto |
| `format/no-trailing-whitespace` | Remove espaços e tabulações no fim das linhas | Autofix preserva o conteúdo restante e trata CRLF |
| `format/require-final-newline` | Garante newline final | Preserva CRLF quando detecta esse estilo |
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
