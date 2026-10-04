# Cobertura de sintaxe C++: levantamento e checklist

Levantamento feito em 2026-10-04 com o `heimdall.exe` de `build/src/`, rodando `parse`, `format` e `lint --semantic` sobre ~250 trechos de C++ e sobre os headers do GoogleTest e do Google Benchmark (`build/_deps`), mais leitura do Binder, do Typer e do Formatter.

Marque `[x]` quando o item estiver corrigido **com teste de regressão** (`ParseTreeSpec`, `FormatterSpec`, `SemanticModelSpec`...).

Todos os repros usam `heimdall parse --std c++23` (ou `format --pointer-alignment left --reference-alignment left`). Um repro "falha" quando `parse` reporta `syntax/parse-error` em código válido; no `lint` esse erro aparece com severidade *error*.

## 1. Parser: construções que falham

Estas são as prioridades, porque geram erro falso em código válido. Ponto de partida de vários itens: `core/src/GrammarParser.cpp` (tratamento de `if` perto da linha 2810, que não pula `constexpr`/`consteval`).

- [ ] **P1. `if constexpr` / `if consteval` / `if !consteval` / `else if constexpr`**
  - Repro: `void f() { if constexpr (a) { g(); } }`
  - Sintoma: `expected ';' before end of compound statement`, mais um segundo erro no `return` seguinte.
- [ ] **P2. `::` global em expressão ou statement dentro de função**
  - Repro: `int f() { return ::g(1); }`, `void f() { ::std::stringstream ss; }`, `static_cast<char>(::tolower(c))`, `int x = ::y;`
  - Em escopo de namespace (`::std::string s;`) já passa.
- [ ] **P3. Inicializador de membro com chaves**
  - Repro: `struct S { S() : a{1} {} int a; };`
- [ ] **P4. Fold expressions**
  - Repro: `(xs + ...)`, `(... && xs)`, `(0 + ... + xs)`, `(f(xs), ...)` dentro de um corpo de função.
  - Sintoma: `expected expression`.
- [ ] **P5. `.template`, `->template` e `T::template` em expressão**
  - Repro: `void f() { t.template g<int>(); }`, `Foo<int>::template bar<2>();`, `auto x = T::template get<0>();`
  - Em contexto de tipo (`typename T::template rebind<int>::other`) já passa.
- [ ] **P6. Operadores de conversão**
  - Repro: `struct S { operator bool() const; };`, `explicit operator int() const;`
  - A versão template (`template <typename T> operator T() const;`) passa.
- [ ] **P7. `explicit(cond)`**
  - Repro: `struct S { explicit(true) S(long); };`
- [ ] **P8. `[[likely]]` / `[[unlikely]]` após `if` e `else`**
  - Repro: `void f() { if (x) [[likely]] g(); }`, `if (x) { } else [[unlikely]] { }`
- [ ] **P9. Módulos e `inline namespace` no topo do arquivo**
  - Repro: `inline namespace C { int x; }`, `export void f();`, `export namespace n { void f(); }`, `export { void f(); }`
  - `inline namespace` aninhado em outro namespace passa.
- [ ] **P10. Atributos de declaração e extensões de compilador**
  - Repro: `alignas(16) int x;`, `int x __attribute__((unused));`, `void g() __attribute__((noreturn));`, `__declspec(dllexport) void f();`
  - `struct alignas(16) S {};` e `[[nodiscard("r")]] int f();` passam.
- [ ] **P11. `requires` final em construtor `= default`**
  - Repro: `template <typename T> struct S { S() requires std::is_default_constructible_v<T> = default; };`
- [ ] **P12. Macros de decoração desconhecidas**
  - Repro: `EXPORT void f(int);`, `GTEST_API_ AssertionResult AssertionSuccess();`, `GTEST_DISABLE_MSC_WARNINGS_POP_()` (sem `;`).
  - 108 das 269 linhas com erro em GoogleTest/Benchmark mencionam macros assim. **Não reavaliado com `-D` do `compile_commands.json`**: confirmar primeiro quanto some quando os defines do banco de compilação são usados, e só então decidir se há trabalho aqui.

## 2. Formatter: bugs de espaçamento

Verificados com alinhamento `left` para ponteiro e referência.

- [ ] **F1. Espaço antes do `>` que fecha argumentos de template, quando o último termina em `*`, `&` ou `)`**
  - `std::vector<int*>` vira `std::vector<int * >`
  - `static_cast<std::vector<int>&>(x)` vira `... & >(x)`
  - `std::array<int, (3 > 2 ? 1 : 2)>` vira `... (3 > 2 ? 1 : 2) >`
- [ ] **F2. Fold expressions**: `(xs + ...)` vira `(xs +...)`; `(... && xs)` vira `(...&& xs)`.
- [ ] **F3. `if constexpr (a)` perde o espaço**: sai `if constexpr(a)`.
- [ ] **F4. Lambda com cabeça de template**: `auto l = []<typename T>(T x)` vira `auto l =[] <typename T>(T x)`.
- [ ] **F5. Structured bindings**: `auto [a, b]` vira `auto[a, b]`; `for (auto& [k, v] : m)` vira `for (auto &[k, v]: m)`.
- [ ] **F6. Retorno final com `decltype`**: `auto g() -> decltype(x)` vira `auto g()->decltype(x)` (com `-> int` funciona).
- [ ] **F7. Atributo após `struct`**: `struct [[deprecated("x")]] S` vira `struct[[deprecated("x")]] S`.
- [ ] **F8. Designated initializer**: `{.a = 1, .b = 2}` vira `{.a = 1,.b = 2}`.
- [ ] **F9. Lista de inicialização sem `=`**: `int arr[]{1, 2, 3};` é explodida em bloco Allman de 4 linhas.
- [ ] **F10. Pack com referência e operador**: `Ts&&... args` vira `Ts &&... args`; `A& operator<<(int)` vira `A & operator<<(int)` (mesmo com alinhamento `left`).
- [ ] **F11. Requires-expression**: `{ a + a } -> std::same_as<T>;` vira `}->std::same_as<T>;` em bloco expandido.
- [ ] **F12. A decidir (pode ser estilo)**: recuo da cláusula `requires` (hoje sem recuo) e expansão de funções de uma linha em Allman.

## 3. Camada semântica

Nos 32 cenários com templates testados não houve falso positivo; os problemas são falsos negativos e falta de modelagem.

- [ ] **S1. Nós de gramática ausentes**: parâmetro de template, fold expression, structured binding, inicializador entre chaves. Parâmetros de lambda também não viram nós.
- [ ] **S2. Structured bindings não viram símbolos**: `auto [a, b] = p;` não é rastreado por `modernize-const` nem por `no-unused-local`.
- [ ] **S3. Parâmetros de template não são símbolos** (`T`, `N` ficam `Unknown`). Distinguir "dependente de template" de `Unknown` genérico no Typer, para liberar casos como `T x = a;` (hoje nunca recebe `const`).
- [ ] **S4. Bases com argumentos de template** (`struct D : B<int>`) não são resolvidas (`semantic/src/Binder.cpp`, `ResolvePath` por volta da linha 1465). Efeito: `modernize-override` e `api/overload-hiding` ficam em silêncio.
- [ ] **S5. Membros definidos fora da classe** (`template <typename T> T Box<T>::get()`) não são associados à classe `Box`.
- [ ] **S6. Especializações totais e parciais** não são ligadas à classe primária (só existe a flag `Template`). Verificar o que o Binder cria para `template <> struct S<int>` ao lado do primário.
- [ ] **S7. Typer**: função template, alias template e instâncias de classe template (`Box<int>`) são `Unknown`; faltam `T::value_type` e `decltype`.
- [ ] **S8. Avaliador constante**: qualquer função template é inelegível para `modernize-constexpr`; variable templates e `if constexpr` não são modelados.
- [ ] **S9. Navegação e completion** (ver `completion-limitations.md`): tipos dependentes, iteradores, lambdas, structured bindings, range-for, `using namespace` (3a) e alias como qualificador (3b).
- [ ] **S10. Mensagem do `no-implicit-bool-conversion`** imprime `'<unknown>*'` para `T* q`; deveria dizer algo como `T*` ou omitir o tipo.

## 4. Infraestrutura

- [ ] **I1. Teste de corpus**: rodar o parser sobre GoogleTest, Google Benchmark e simdjson (já baixados pelo `FetchContent`) com um teto de erros. Linha de base atual: GoogleTest 14 de 23 arquivos com erro (197 erros), Google Benchmark 13 de 42 (72 erros), sem `compile_commands.json`.
- [ ] **I2. Descoberta de arquivos ignora headers da libstdc++ sem extensão** (`skipping unsupported file: .../algorithm`).

## Ordem sugerida

1. P1 a P11 (parser), cada um com repro em `ParseTreeSpec`.
2. F1 a F11 (formatter), com repros em `FormatterSpec`.
3. S1 a S6 (gramática e Binder).
4. S3 e S7 (Typer).
5. I1 assim que P1 a P5 estiverem feitos, para ter a métrica de progresso.
