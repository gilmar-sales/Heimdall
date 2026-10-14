# Suporte a SFINAE: estado da implementação

Esta entrega implementa um **subconjunto local e conservador**, não a totalidade do
[plano](../reports/Plano%20de%20suporte%20a%20SFINAE.md) nem conformidade integral de C++.
O lexer continua lossless: SFINAE não introduz tokens ou keywords, e `>>` permanece
um token físico, inclusive quando o parser o interpreta como fechamento de templates.

## Capacidades implementadas

- `typename` em parâmetros dependentes sem nome preserva o type-specifier completo.
- `decltype` em specifiers e trailing returns preserva um nó de operando não avaliado.
- Membros dependentes guardam receptor canônico e nome, inclusive nomes ausentes no Binder.
- Alias templates são declarados no escopo externo, mantendo seus parâmetros no escopo próprio.
- Substituição estrutural verifica ponteiros para referências, referências para `void`,
  arrays de `void`/referências, tamanho zero e reference collapsing.
- Chamadas locais simples deduzem parâmetros de tipo e combinam argumentos explícitos
  com defaults necessários, substituídos usando os argumentos anteriores.
- Assinaturas são verificadas em ordem textual, distinguindo retorno prefixado de
  trailing return; uma falha comprovada interrompe a tentativa.
- Alias templates de parâmetros de tipo sem packs substituem também argumentos não
  utilizados pelo target. Não há tratamento especial por nome para `enable_if` ou `void_t`.
- Lookup de membros de classes locais completas distingue ausência de membro, categoria
  incorreta e dados insuficientes. Efeitos de classes template auxiliares são avaliados
  com uma fronteira de instanciação diferente da assinatura imediata.
- Especializações parciais de classes com parâmetros de tipo: matching por padrões,
  bindings próprios da parcial e ordenação bidirecional entre parciais. Parciais
  ambíguas invalidam a instanciação em vez de eleger um vencedor arbitrário. O idiom
  de detecção com alias variádico de `void` funciona sobre esse mecanismo, sem
  tratamento especial por nome.
- Partial ordering de funções sobre padrões de tipo (ponteiro, referência, const):
  o padrão mais especializado vence em empates de conversão; padrões incomparáveis
  ficam indeterminados.
- Constraints associadas: literais booleanos, conjunção/disarjunção com curto-circuito
  (o operando que decide impede a instanciação do outro lado), type-requirements com
  receptor substituído, concept-ids com satisfação do corpo do conceito e subsunção
  por identidade de átomos entre candidatos viáveis. Falha de instanciação exigida
  dentro de `requires` é erro duro, como no GCC; constraint atômica válida de tipo
  não-booleano também. `requires` em classes é preservado pelo parser e avaliado na
  instanciação: restrição falsa descarta o uso no contexto imediato.
- `noexcept` simples é ignorado sem afetar a seleção; especificação de exceção
  parenthesizada deixa a chamada indeterminada. `void` como parâmetro é aceito;
  `void` substituído em parâmetro ou array como retorno descartam o candidato.
- Expression SFINAE cobre um subconjunto de `decltype`: parâmetros, literais,
  parênteses, dereference/address-of, operações aritméticas/integrais builtin,
  vírgula builtin, cast para `void` e value initialization builtin sem argumentos.
  `decltype(value)` e `decltype((value))` preservam regras distintas de referência.
- Seleção compara ranks por argumento, sem somar scores; fallback por ellipsis,
  promoção aritmética e preferência por função não-template em empates simples são suportados.
- `TypeModel::TemplateCall` expõe estado, declaração selecionada, retorno canônico e
  causa estruturada em um snapshot imutável. Consultas não reexecutam a substituição.

## Diagnósticos e integração

`cpp/template-substitution` é uma regra semântica, com severidade padrão `error` e
sem autofix. Publica erros de instanciação obrigatória comprovados, ausência final
de overload viável e ambiguidades dentro do subconjunto fechado. Rejeitar um candidato
e selecionar um fallback **não** gera um erro de substituição.

O dispatcher comum e o agregador legado usam os mesmos resultados. CLI e LSP respeitam
configuração, severidade e suppressions pelo mecanismo existente. Uma edição reconstrói
o resultado no novo snapshot; resultados de snapshots antigos permanecem válidos.

`Dependent`, `Incomplete`, falha de dedução, falha de substituição e erro duro são
estados distintos. Budgets e operações não modeladas nunca autorizam eliminar um
candidato ou publicar “nenhum overload”. As categorias reservadas para constraints
não representam implementação de satisfação/subsunção nesta entrega.

## Limites explícitos

Continuam pendentes partes substanciais de M1–M8:

- Representação integral de template heads/type-ids e cursor angular compartilhado,
  incluindo instanciação semântica confiável de argumentos aninhados com `>>`.
- Construção completa da TU com headers/macros/forced includes, target/ABI e provenance.
- Two-phase lookup/ADL completo, hidden friends e lookup dependente em bases.
- Especializações parciais de variáveis, parciais com parâmetros não-tipo/packs e
  partial ordering sobre template-ids aninhados além do subconjunto coberto.
- Packs, argumentos não tipo/template-template, structural NTTP e constexpr completo.
- Tipos de função, ponteiros para membros, qualificação `volatile` e conversões completas.
- Expression SFINAE envolvendo operadores de classes, chamadas dependentes e biblioteca.
- `explicit(bool)` (sem nó dedicado na árvore), lambdas/CWG 2672 além da recusa
  conservadora, e avaliação de `noexcept(expr)` além de `noexcept` simples.
- Scheduler completo para dedução de retorno pelo corpo, defaults de argumentos de funções
  (chamadas que omitem argumentos opcionais ficam indeterminadas), static_asserts,
  corpos obrigatórios e efeitos de bases/membros.
- Fechamentos `>>` sem espaço em listas de argumentos aninhadas (forma com espaços
  funciona); o lexer preserva os tokens físicos em ambos os casos.
- Certificação de redeclarações/IFNDR, caches de substituição com dependências negativas
  e refactorings certificados por uma cobertura de instanciações fechada.

Chamadas cujo ambiente contém includes, macros ativos, imports por `using`, um oracle
externo de tipos, ADL não modelado ou operações além da cobertura ficam indeterminadas.
Empates que exigem partial ordering ou ranking refinado de referências também ficam
indeterminados, não são certificados como ambiguidade. Completar um helper com bases,
métodos ou static_asserts não modelados impede uma conclusão definitiva.

Essas recusas conservadoras são intencionais. Aceitar a sintaxe de um idiom da STL ou
guardar um estado no modelo **não** comprova suporte semântico integral a esse idiom.

## Validação reproduzível

```bash
cmake --build build --parallel
./build/test/Tests_run --gtest_filter='SfinaeSpec.*'
ctest --test-dir build --output-on-failure
./build/bench/SemanticBench --benchmark_filter='BM_Sfinae.*'
```

`SfinaeCompilerDifferential` compila as mesmas fixtures raw dos testes (36, incluindo
parciais, constraints, subsunção e erros duros verificados contra o GCC) contra o compilador
selecionado e Clang, quando instalado, em C++20/23/26 disponíveis. Assertions de tipos
verificam a seleção, e fixtures de erros duros devem falhar. Não executa binários gerados
e não compara mensagens literais. IFNDR e exemplos sem ambiente fechado não entram nessa
trilha como requisitos de rejeição. O harness não é backend de produção.

`LspSfinaeSmoke` verifica diagnóstico, fallback silencioso, didChange e configuração.
Há ainda testes estruturais, limites, truncamentos, metamorfismos de trivia/renomeação,
invalidação de snapshots e preservação dos tokens físicos de shifts/closers.

Benchmarks medem substituição com vários candidatos e leitura de resultados publicados.
Não existe medição de speedup nem certificação de latência baseada nesses benchmarks.

## Notas da segunda tranche (parciais, constraints, `noexcept`)
- O Binder não marca mais templates apenas por conter `requires`; constraints são
  avaliadas na substituição. O rename mantém a recusa conservadora para qualquer
  template com `RequiresClause`: satisfação não certifica todas as instanciações.
- `SfinaeCompilerDifferential` cobre 36 fixtures (108 combinações GCC em C++20/23/26),
  incluindo erros duros confirmados contra o GCC (falha aninhada em `requires`,
  constraint não-booleana, parciais ambíguas).
- Limites que permanecem: packs/NTTP sem modelo de valores, `explicit(bool)` sem nó
  na árvore, lambdas/CWG 2672 além da recusa conservadora, `noexcept(expr)` além de
  `noexcept` simples, `>>` sem espaço em argumentos aninhados, ADL/headers e
  especializações parciais de variáveis.
