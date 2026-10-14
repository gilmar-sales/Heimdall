# Heimdall precisa provar substituições, não reconhecer idioms

**Suportar SFINAE no Heimdall exige um pipeline de dedução, substituição e seleção com fronteiras explícitas; reconhecer `enable_if` não basta.** A grafia correta é **SFINAE**, de *Substitution Failure Is Not An Error*, e não “SIFNAE”. O projeto já possui infraestrutura lexical, sintática e semântica reutilizável, mas a inspeção das notas não demonstra conformidade end-to-end. A prioridade é representar os locais de substituição sem perda de origem e distinguir falha imediata, erro de instanciação e informação insuficiente. O plano abaixo preserva o núcleo STL-only, a organização SoA e os contratos conservadores existentes. Propõe entregas incrementais verificáveis para C++20/23/26, com política explícita de defect reports e sem prometer um compilador C++ completo. Este documento é **plano, não implementação**: nenhuma alteração de código, execução de testes ou medição de desempenho foi realizada nesta síntese.

## A regra protege contextos imediatos, não qualquer erro em templates

### O escopo correto separa dedução, substituição e seleção

SFINAE permite que uma tentativa de dedução/substituição falhe sem transformar essa falha em erro independente do programa. Em funções template, a especialização correspondente deixa de participar do conjunto de candidatos; em matching de especializações parciais, a parcial pode ser descartada. A chamada ou instanciação ainda pode falhar por ausência de alternativa ou ambiguidade. A proteção aplica-se à formação inválida de tipos e expressões no **contexto imediato**, não a todos os erros encontrados durante a análise de templates ([cppreference](https://cppreference.com/cpp/language/sfinae), [draft](https://eel.is/c++draft/temp.deduct.general#8)).

O draft corrente enumera como locais de substituição dedutiva o tipo da função **excluindo o noexcept-specifier**, o explicit-specifier, as declarações dos parâmetros de template e a lista de argumentos de uma especialização parcial. A substituição abrange tipos e expressões nesses locais e verifica também acesso. A regra geral não deve virar uma whitelist de nomes de traits ou de meia dúzia de tipos inválidos ([temp.deduct.general](https://eel.is/c++draft/temp.deduct.general#7)).

Uma operação que instancie outra especialização, deduza retorno a partir de corpo ou gere uma função implicitamente definida pode produzir **erro duro**. Nenhuma instrução de corpo pertence ao contexto imediato dos locais citados. Assim, `typename T::X` diretamente na assinatura não é equivalente a `typename A<T>::X` quando completar `A<T>` exige uma declaração inválida dentro da classe ([temp.deduct.general](https://eel.is/c++draft/temp.deduct.general#8), [retorno auto](https://eel.is/c++draft/dcl.spec.auto.general#15)).

O objetivo funcional abrange type SFINAE, expression SFINAE, alias templates, matching/ordenação de parciais de classes e variáveis, dedução de funções e seleção de overloads. Constraints, conversões, ADL, constexpr e instanciação por necessidade são dependências ou mecanismos associados, **não sinônimos de SFINAE**. CTAD, deduction guides, operadores reescritos, módulos e novas construções C++26 devem ter capacidade declarada separadamente; lacunas nessas integrações impedem conclusões que dependam delas.

### A ordem lexical determina quais efeitos podem acontecer

A substituição deve seguir a ordem lexical e parar na primeira condição que faça a dedução falhar. Um retorno prefixado precede os parâmetros; um trailing return sucede-os. Defaults de parâmetros de template devem ser substituídos somente quando necessários e usando argumentos anteriores. Avaliar antecipadamente todas as regiões para “completar” o modelo pode alcançar um erro duro que a regra manda evitar ([CWG 1227](https://cplusplus.github.io/CWG/issues/1227.html), [temp.deduct.general](https://eel.is/c++draft/temp.deduct.general#7)).

Essa ordem não é a ordem de avaliação de operadores em execução. Também não elimina a sequência normativa entre argumentos explícitos, ajustes P/A, dedução, preenchimento de defaults, constraints e substituição final. **Ordenar todos os nós uma única vez e executar tudo avidamente é incorreto**: cada fase visita apenas os locais exigidos, em sua ordem textual aplicável ([temp.deduct.general](https://eel.is/c++draft/temp.deduct.general#2)).

Lookup em duas fases é outra dimensão: nomes não dependentes conservam o contexto da definição, enquanto referências dependentes usam as regras de especialização e instanciação. Contexto não deduzido, como operando de `decltype` ou nested-name-specifier, significa que aquele trecho não fornece o argumento por dedução; não significa que a substituição ou validação pode ser omitida ([temp.res.general](https://eel.is/c++draft/temp.res.general#1), [temp.deduct.type](https://eel.is/c++draft/temp.deduct.type#5)).

### Dialeto, revisão normativa e implementação do compilador são três eixos

O Heimdall representa atualmente apenas **C++20, C++23 e C++26**, conforme [CppStandard.hpp](../core/include/Heimdall/CppStandard.hpp); o padrão usado para compilar a ferramenta não comprova cobertura integral do arquivo analisado. Modos C++98/11/14/17 são referência histórica e eventual extensão futura, não promessa desta entrega. `CompileDatabase` interpreta opções e aliases dos modos disponíveis, e o default do parser é C++20 ([CompileDatabase.cpp](../semantic/src/CompileDatabase.cpp), [ParseTree.hpp](../core/include/Heimdall/ParseTree.hpp)).

| Dialeto | Evolução relevante da linguagem/biblioteca | Compromisso do plano |
|---|---|---|
| C++98 | Type SFINAE e expressões constantes em tipos; parciais de classes com DRs | Referência histórica; fora dos modos atuais |
| C++11 | Expression SFINAE, decltype, trailing return, aliases, packs, `std::enable_if` | Idioms herdados presentes nos modos atuais |
| C++14 | Variáveis template, parciais de variáveis, retorno auto | Separar retorno declarado de retorno deduzido pelo corpo |
| C++17 | `std::void_t`, if constexpr, noexcept no sistema de tipos | Não confundir disponibilidade de biblioteca e regra linguística |
| C++20 | explicit(bool), concepts/requires, lambdas em contextos não avaliados | Primeiro perfil operacional, com DRs documentados |
| C++23 | Interações com parâmetros de objeto explícitos | Cobertura específica por recurso e toolchain |
| C++26 | Draft corrente e novas formas dependentes | Núcleo SFINAE separado de reflection, splices e pack indexing |

A cronologia da tabela descreve introdução de mecanismos, não certifica compiladores ou o Heimdall ([cppreference](https://cppreference.com/cpp/language/sfinae), [tipos dependentes](https://eel.is/c++draft/temp.dep.type)). A política deve congelar documento/revisão, dialeto e DRs por fixture. As notas registraram a revisão `c7015b485cc3db8efaa9dfb9ff0809c5394a4ed1`; ela oferece uma âncora reproduzível, enquanto links de eel.is continuam móveis ([snapshot](https://github.com/Eelis/draft/blob/c7015b485cc3db8efaa9dfb9ff0809c5394a4ed1/source/templates.tex)).

| DR prioritário | Consequência concreta para implementação/teste |
|---|---|
| [CWG 295](https://cplusplus.github.io/CWG/issues/295.html) | Adicionar cv a tipo de função não é falha; ignorar os qualificadores pertinentes |
| [CWG 1227](https://cplusplus.github.io/CWG/issues/1227.html) | Ordem lexical e interrupção antes de efeitos posteriores |
| [CWG 1558](https://cplusplus.github.io/CWG/issues/1558.html) | Substituir argumentos não usados de aliases, base do `void_t` simples |
| [CWG 2054](https://cplusplus.github.io/CWG/issues/2054.html) | Lista de argumentos da parcial integra os locais protegidos |
| [CWG 2322](https://cplusplus.github.io/CWG/issues/2322.html) | Redeclarações que alteram ordem/ocorrência de instanciações podem ser IFNDR |
| [CWG 2672](https://cplusplus.github.io/CWG/issues/2672.html) | Refinar contexto imediato de lambdas, requisitos e constraints atômicas |

**Há uma divergência que não deve ser escondida:** cppreference afirma amplamente que uma lambda não integra o contexto imediato; o draft atual exclui o **corpo** e construções separadamente instanciadas. Seus exemplos distinguem falha em init-capture ou retorno declarado de falha em instrução do corpo. A implementação deve seguir o perfil normativo escolhido e registrar divergências de versões de compiladores, não codificar a simplificação ampla como regra universal ([cppreference](https://cppreference.com/cpp/language/sfinae#Explanation), [draft e exemplos](https://eel.is/c++draft/temp.deduct.general#9), [CWG 2672](https://cplusplus.github.io/CWG/issues/2672.html)).

`explicit(expr)` é local protegido: expressão inválida pode eliminar a tentativa, mas resultado válido `true` exige as regras normais de inicialização/conversão. Já `f() noexcept(expr)` tem instanciação própria, quando necessária, e pode produzir erro duro. O operador `noexcept(expr)` dentro de uma expressão protegida é distinto do specifier; consultar uma função pode ainda disparar a instanciação de sua especificação ([temp.deduct.general](https://eel.is/c++draft/temp.deduct.general#7), [except.spec](https://eel.is/c++draft/except.spec#13)).

IFNDR significa *ill-formed, no diagnostic required*. Inclui certas divergências entre redeclarações e templates sem especialização válida nas condições normativas. Aceitação por vários compiladores não prova validade, e esses casos não devem ser convertidos automaticamente em candidatos descartados ([temp.res.general](https://eel.is/c++draft/temp.res.general#6), [CWG 2322](https://cplusplus.github.io/CWG/issues/2322.html)).

## A base existente aceita estruturas, mas ainda não prova seleção normativa

### O inventário é estático e delimita o que está realmente verificado

As três notas fornecidas resultam de inspeção estática em 14/10/2026. Testes citados foram lidos, não executados; ausência de estrutura completa não prova que todo exemplo seja rejeitado pelo parser. Esta síntese revisitou diretamente a enumeração de dialetos e os contratos de resolução/cobertura. Os demais achados abaixo conservam a atribuição à inspeção registrada nas notas, sem transformá-la em resultado runtime.

| Camada | Evidência no repositório | Limite verificado e implicação |
|---|---|---|
| Lexer | [Lexer.cpp](../core/src/Lexer.cpp), [Lexer.hpp](../core/include/Heimdall/Lexer.hpp), [Tok.hpp](../core/include/Heimdall/Tok.hpp) | Pontuadores compostos, offsets e trivia já existem; não criar token SFINAE |
| Árvore | [ParseTree.hpp](../core/include/Heimdall/ParseTree.hpp) | GrammarNodeSoA e vários nós template/requires; faltam regiões individualizadas em locais importantes |
| Parser | [GrammarParser.cpp](../core/src/GrammarParser.cpp) | Cabeçalhos/defaults, alias RHS, decltype e specifiers ainda têm leitura opaca/parcial |
| Binder | [Binder.cpp](../semantic/src/Binder.cpp), [SemanticModel.hpp](../semantic/include/Heimdall/SemanticModel.hpp) | Categorias de parâmetros, packs, defaults e dependência; reinterpreta tokens de cabeçalhos |
| Tipos | [TypeModel.hpp](../semantic/include/Heimdall/TypeModel.hpp), [TemplateModel.cpp](../semantic/src/TemplateModel.cpp) | Tipos dependentes e substituição limitada; sem taxonomia completa de falhas |
| Typer | [Typer.cpp](../semantic/src/Typer.cpp) | Instanciação conservadora de parâmetros de tipo/defaults e explícitas; parciais/packs geram incompletude |
| Dedução textual | [TemplateDeduction.cpp](../core/src/TemplateDeduction.cpp) | Recipes limitadas para apresentação; não prova normativa de viabilidade |
| Chamadas/concepts | [CallArguments.cpp](../semantic/src/CallArguments.cpp), [ConceptRules.cpp](../semantic/src/ConceptRules.cpp) | Regras conservadoras existentes; não equivalem a best viable function/subsunção |
| Coordenação | [Workspace.cpp](../analysis/src/Workspace.cpp) | Snapshots, construção lazy e dependências reutilizáveis |
| Consumidores | [Pipeline.cpp](../src/Pipeline.cpp), [Server.cpp](../lsp/Server.cpp) | CLI/LSP já encaminham análise; novos resultados devem ser compartilhados |

`TemplateResolution` distingue `Resolved`, `Dependent`, `Ambiguous` e `Incomplete`; `TemplateCoverage` separa `None`, `Workspace` e `Invariant`. Isso refuta a ideia de que dependência e desconhecimento são necessariamente um único estado no código atual. Também impede interpretar bindings observados no workspace como prova para todos os usos de um template público ([TypeModel.hpp](../semantic/include/Heimdall/TypeModel.hpp), [TemplateModel.cpp](../semantic/src/TemplateModel.cpp)).

O Typer inspecionado aceita conservadoramente parâmetros de tipo sem packs, aplica defaults anteriores e seleciona explícitas por igualdade de argumentos; retorna incompletude para parciais ou alternativas não resolvidas. Os limites registrados incluem profundidade de template 32, typing 192 e busca de membros 64. São mecanismos operacionais atuais, **não limites do padrão C++**, e precisam ser recalibrados para o novo pipeline ([Typer.cpp](../semantic/src/Typer.cpp), [TemplateModel.cpp](../semantic/src/TemplateModel.cpp)).

As suites [SemanticModelSpec.cpp](../test/src/SemanticModelSpec.cpp), [SemanticTyperSpec.cpp](../test/src/SemanticTyperSpec.cpp) e [TemplateDeductionSpec.cpp](../test/src/TemplateDeductionSpec.cpp) documentam defaults, dependência, instâncias simples, recusas conservadoras e fallback de hover. São baseline de regressão, não certificado de suporte SFINAE.

### O ambiente de compilação ainda não fecha uma unidade de tradução

“TypeOracle” é uma descrição arquitetural; a inspeção encontrou o oracle local `SemanticAnalyzer`, não um compilador de templates com esse nome. Coleta local de tipos e desambiguação não carregam automaticamente declarações transitivas completas ([SemanticAnalyzer.hpp](../semantic/include/Heimdall/SemanticAnalyzer.hpp)). O `TypeNameOracle` do parser auxilia decisões de nomes, mas não substitui lookup dependente completo ([parser-name-resolution.md](../docs/parser-name-resolution.md)).

`CompileCommand` preserva defines, undefines, diretórios e argumentos; `FindOrNearest` pode emprestar uma configuração próxima para headers. Isso melhora edição, mas não prova o comando real de todas as TUs que incluem o arquivo. O mesmo header pode ter semânticas diferentes conforme macros, alvo e include order ([CompileDatabase.hpp](../semantic/include/Heimdall/CompileDatabase.hpp)).

`MacroEnvironment` acompanha macros por posição, includes, forced includes e overlays; seus limites e sua noção de `Complete()` não certificam ausência de dependências relevantes em system headers. `IncludeIndex` serve discovery/completion, e `HeaderSummary` não contém AST completo de assinaturas e corpos de templates. Logo, juntar nomes dos índices não fornece o ambiente necessário para decidir SFINAE ([MacroEnvironment.hpp](../semantic/include/Heimdall/MacroEnvironment.hpp), [IncludeIndex.hpp](../semantic/include/Heimdall/IncludeIndex.hpp), [HeaderSummary.hpp](../semantic/include/Heimdall/HeaderSummary.hpp)).

A arquitetura deve escolher explicitamente um **backend nativo por subconjunto** como caminho do plano, enriquecendo a visão de TU sob demanda. Se for desejado um backend assistido por compilador para ampliar cobertura, ele exige decisão separada de produto: processo/adaptador fora do core, disponibilidade e versão documentadas, tradução de resultados e provenance, cancelamento e isolamento. O harness diferencial não é backend de produção, e nenhum desses caminhos autoriza dependências externas em `heimdall_core`.

Modo standalone pode provar casos autocontidos com ambiente fechado; sem configuração/includes suficientes deve retornar incompletude localizada. O modo configurado exige comando, alvo e dependências pertinentes. **Encontrar uma classe não prova que ela está semanticamente completa; não encontrar um membro só prova ausência quando a busca está fechada.**

## Uma representação lossless deve alimentar substituição tipada e contextual

### O lexer preserva bytes; o parser interpreta fechamentos de templates

Não há novos keywords para `enable_if`, `void_t` ou SFINAE: são nomes de templates/aliases comuns. O lexer deve manter tokens físicos, trivia, offsets e comportamento incremental de relex. `>>` continua pontuador composto; separá-lo globalmente quebraria shifts e clientes de ranges ([Lexer.hpp](../core/include/Heimdall/Lexer.hpp), [Lexer.cpp](../core/src/Lexer.cpp)).

A proposta é um cursor contextual `(tokenIndex, subtokenOffset)`, capaz de consumir metades de `>>` somente como fechamentos quando a gramática exigir. Endpoints lógicos ou ranges de bytes ficam em tabela lateral, preservando os intervalos físicos atuais. `>>=`, `>=`, `<<` e casos de maximal munch precisam de regras e testes próprios; não devem ser divididos por analogia.

A desambiguação angular deve centralizar regras hoje distribuídas entre `FindTemplateClose`, `FindComma`, `ParseScope` e Binder. Contextos de type-id, template-argument e expressão, delimitadores locais, `typename`, `template` e evidência de nomes substituem contadores cegos. Comparações/shifts válidos e ambiguidades reais devem conservar recuperação ou incompletude, não certeza inventada ([GrammarParser.cpp](../core/src/GrammarParser.cpp), [Binder.cpp](../semantic/src/Binder.cpp)).

O aceite lexical inclui `X<Y<T>>`, `X<Y<T> >`, `A<(N >> 1)>`, `A<(N < 2)>`, defaults parentizados, argumentos com packs, `a < b > (c)` e truncamentos em cada fronteira. Verificar texto reconstruído, offsets, ranges e equivalência relex/full lex; ausência de diagnóstico isoladamente é insuficiente.

### A árvore precisa individualizar os locais de substituição

Cada template head deve representar parâmetros Type/Value/Template, parâmetros template-template, declaradores não tipo, nomes opcionais, packs, defaults e constraints. `template<>`, múltiplos cabeçalhos e especializações devem conservar identidade e ordem. O Binder passa a consumir essa estrutura, em vez de ter um segundo balanceamento angular ([ParseTree.hpp](../core/include/Heimdall/ParseTree.hpp), [Binder.cpp](../semantic/src/Binder.cpp)).

Um parser uniforme de type-id deve servir retorno prefixado/trailing, parâmetros, aliases, defaults e argumentos. A cobertura inclui nomes qualificados dependentes, `typename`, `template`, cv/ref/pointer, ponteiro para membro, arrays, funções e declaradores abstratos. `decltype(expr)` e `decltype(auto)` precisam de representação distinta e adequada; o RHS de alias não deve ser tratado como inicializador de variável.

Operandos de `decltype`, `explicit(expr)`, `noexcept(expr)`, bounds e defaults deixam de ser blocos opacos. Expressões preservam parênteses, vírgula, chamadas dependentes, operações, folds e expansões. Requires-expressions mantêm requisitos simples, de tipo, compostos e aninhados, com noexcept e restrição de retorno, sem antecipar satisfação semântica ([GrammarParser.cpp](../core/src/GrammarParser.cpp), [ParseTree.cpp](../core/src/ParseTree.cpp)).

Uma tabela lateral SoA de **locais de substituição** registra owner, papel, origem, fase aplicável, posição textual e fronteira de contexto. Sobreposições exigem hierarquia explícita entre slot externo e percurso interno. Corpo de função/lambda, instanciação separada, default de argumento de função e exception specification não recebem a mesma política de assinatura/default de template.

A ordem de inserção dos nós atuais não é garantia de ordem textual nem de pré-ordem: o parser reparenta operandos anteriores em expressões. Antes de ampliar travessias, caracterizar esses invariantes e escolher adjacência explícita ou normalização com remapeamento. Não ordenar a SoA in-place, pois isso ameaça IDs, parent, subtree_end e reuso incremental ([ParseTree.cpp](../core/src/ParseTree.cpp), [GrammarParser.cpp](../core/src/GrammarParser.cpp)).

### O IR semântico conserva dependência, categoria e prova

Estender os modelos modernos, sem criar AST orientada a objetos paralela. Colunas/side tables devem distinguir type-dependent, value-dependent, instantiation-dependent, packs não expandidos, completude e motivo de ausência de prova. Identidades de nomes/tipos dependentes devem guardar receiver, caminho qualificado e escopo da definição, não depender somente de spelling.

Argumentos de template precisam de categorias Type/Value/Template, valores constantes tipados e packs em faixas. Tipos devem representar funções, ponteiros para membro, cv completo, bounds simbólicos e referências. Expressões conservam lvalue/xvalue/prvalue e informação necessária para `decltype((expr))`, forwarding e reference collapsing. Canonicalização por identidade evita confundir aliases, overloads e tipos apenas textualmente semelhantes.

O backend constexpr inicial deve ser bounded e declarar seu subconjunto: operações tipadas, curto-circuito, casts, sizeof/alignof e modelo de alvo/ABI. Resultados incluem conhecido, dependente, incompleto, inválido e não constante conforme contexto. Usar tamanho/range dos tipos do host como ABI universal é incorreto; operando não avaliado não dispensa lookup ou formação de tipos.

### O pipeline executa apenas consultas semanticamente exigidas

Lookup produz faixas de candidatos, origem e prova de conjunto completo. Preserva ordinary lookup da definição, visibilidade por posição, current instantiation, bases dependentes, using e acesso. ADL considera entidades associadas e hidden friends, com suas regras de supressão; não busca indiscriminadamente todo o workspace ([temp.res.general](https://eel.is/c++draft/temp.res.general), [basic.lookup.argdep](https://eel.is/c++draft/basic.lookup.argdep)).

Por candidato, o fluxo combina argumentos explícitos, substituição inicial, ajustes P/A, dedução, conflitos, defaults necessários, constraints e checagens na ordem normativa, e substituição final. Modos de chamada, endereço, conversão e partial ordering compartilham infraestrutura, mas têm regras próprias. Defaults de argumentos da função não são defaults de parâmetros de template e não fornecem argumentos para partial ordering ([temp.deduct](https://eel.is/c++draft/temp.deduct), [temp.deduct.partial](https://eel.is/c++draft/temp.deduct.partial)).

`SubstituteType` e `SubstituteExpression` recebem contexto explícito: local, posição, acesso, ambientes de definição/instanciação e pilha de efeitos. Aliases são substituídos genericamente, incluindo argumentos não usados; `void_t` não pode apagar argumentos inválidos antes da verificação. Traits próprios e de biblioteca devem usar o mesmo mecanismo ([temp.alias](https://eel.is/c++draft/temp.alias#2), [CWG 1558](https://cplusplus.github.io/CWG/issues/1558.html)).

O scheduler de instanciação distingue declaração, assinatura, completude de classe, membro, corpo, return deduction, default de argumento e exception specification. Não instancia todos os corpos preventivamente, mas também não adia efeitos exigidos. A invalidade dentro de uma classe auxiliar ou corpo requerido mantém sua fronteira de erro duro, mesmo se foi alcançada durante uma tentativa especulativa.

Constraints têm normalização, satisfação e subsunção próprias. Conjunção/disjunção respeitam curto-circuito; uma constraint atômica válida exige expressão constante de tipo bool, não apenas conversibilidade contextual. Negação não transforma uma falha de substituição em verdadeiro. Identidade de átomos e parameter mappings não se reduz a comparar texto ([temp.constr.atomic](https://eel.is/c++draft/temp.constr.atomic), [temp.constr.op](https://eel.is/c++draft/temp.constr.op)).

Após viabilidade, comparar sequências de conversão por argumento e desempates aplicáveis; **não somar scores de especificidade**. Partial ordering de funções usa dedução bidirecional apropriada. Matching de parciais de classes/variáveis seleciona primário ou parcial e resolve múltiplos matches pela ordenação normativa, não pela primeira declaração ([over.match.best.general](https://eel.is/c++draft/over.match.best.general), [temp.spec.partial.match](https://eel.is/c++draft/temp.spec.partial.match), [temp.spec.partial.order](https://eel.is/c++draft/temp.spec.partial.order)).

### Unknown nunca autoriza eliminar um candidato

| Estado proposto | Significado e política |
|---|---|
| Success/Resolved | Operação válida no escopo provado; não certifica corpo inteiro |
| Dependent | Argumentos legitimamente ainda simbólicos; adiar |
| Incomplete/Unknown | Dados, configuração, operação ou budget insuficientes; não eliminar |
| DeductionFailure | Incompatibilidade/conflito/não dedução comprovados; razão distinta |
| SubstitutionFailure | Invalidade comprovada em contexto imediato aplicável; descartar tentativa |
| ConstraintUnsatisfied | Constraint não satisfeita por suas regras; razão própria |
| Ambiguous | Conjunto fechado sem vencedor único comprovado |
| HardError | Invalidade diagnosticável em operação obrigatória fora da proteção |
| PotentialIFNDR | Indício de condição sem obrigação diagnóstica; categoria separada |

Cada resultado carrega reason, origem, dependências e cobertura. Um candidato incompleto impede “nenhum overload corresponde” se ainda puder servir; impede vencedor definitivo se ainda puder vencê-lo. Ausência de header, ciclo, timeout ou pack não modelado não são falhas de substituição. Incompletude distante não impede um diagnóstico independente já provado.

Transações por tentativa separam notas de rejeição de erros de efeitos obrigatórios. Descartar um candidato elimina seus diagnósticos especulativos, não apaga erro duro exigido só porque ele perdeu. Publicação exige prova, versão atual e ausência de cancelamento; notas de candidatos e pilha de instanciação devem ter ordenação determinística. Internings imutáveis válidos podem sobreviver, mas scratch descartado não pode deixar views/IDs pendurados.

## A cobertura precisa provar seleção e fronteiras, não apenas parsing

### A matriz combina casos positivos, negativos e informação ausente

Na matriz, **P** significa seleção válida; **N**, candidato rejeitado suavemente com fallback válido ou diagnóstico final se não há alternativa; **H**, erro duro quando a operação é exigida; **I**, IFNDR sem obrigação de rejeição; **U**, incompletude sem diagnóstico especulativo. Toda família verifica estrutura, estado, motivo, candidato/tipo selecionado, efeitos visitados e diagnósticos. As expectativas derivam das regras citadas, não de resultados já executados ([temp.deduct.general](https://eel.is/c++draft/temp.deduct.general), [temp.res.general](https://eel.is/c++draft/temp.res.general)).

| Família | Cobertura P/N obrigatória | Fronteiras H/I/U |
|---|---|---|
| Membro dependente | `typename T::type` presente/ausente em classe completa | U com forward declaration/header ausente |
| Categoria/acesso | Nome type/value/template correto; acesso privado imediato | H no corpo requerido; U sem contexto de acesso |
| Formação de tipos | Pointer/ref/member-pointer, arrays e tipos de função válidos | N para pointer-to-reference, void&, array inválido, member-pointer de não classe |
| Ajustes de tipos | Reference collapsing e cv em função conforme DR | Não rejeitar qualificadores cobertos por CWG 295 |
| Expressões | `decltype(t+t)`, chamada/member call, vírgula, `decltype((x))` | U se ADL/conversão não modelada; H por efeito obrigatório |
| Aliases/idioms | `enable_if`, `enable_if_t`, `void_t`, aliases próprios/encadeados | Argumento não usado inválido; H dentro de classe auxiliar |
| Ordem lexical | Retorno prefixado e default anterior interrompem cedo | H em variante trailing; I em redeclarações com ordem divergente |
| Defaults | Defaults de template necessários e anteriores visíveis | Default de função não deduz T; H quando default inválido é usado |
| Dedução | Explícitos+deduzidos, cv/ref, decay, forwarding, non-deduced contexts | DeductionFailure por conflitos; U por operação não suportada |
| NTTP/packs | Valores tipados, packs vazio/não vazio, múltiplas expansões | N por narrowing/comprimentos incompatíveis; U por structural NTTP não modelado |
| Corpo/retorno | Assinatura válida sem instanciar corpo desnecessário | H em retorno auto/decltype(auto) deduzido de corpo inválido |
| Lambda | Init-capture/retorno declarado em perfil CWG 2672 | H em corpo; expectativa versionada conforme DR/toolchain |
| Explicit/noexcept | explicit válido/ inválido, operador noexcept protegido | H no specifier quando requerido; U se momento não modelado |
| Parciais de classe | Primário fallback, parcial válida/inválida, ordering | Ambiguidade de parciais; requisitos da declaração não são SFINAE |
| Parciais de variável | Matching/ordering equivalente, seleção positiva | H no inicializador exigido; não confundir com lista de argumentos |
| Lookup/ADL | Nomes fixos na definição, hidden friends, bases e namespaces associados | U com conjunto aberto; I em divergências cross-TU normativas |
| Ranking | Conversões estruturadas, desempates e partial ordering de funções | Ambiguidade por conversões cruzadas; U com candidato desconhecido |
| Constraints | True/false, requisitos, normalização e subsunção | H por expressão válida de tipo inadequado/efeitos externos; U se não modelado |
| Constexpr | Curto-circuito, alvo/ABI, ramo descartado de if constexpr | U por budget; H no ramo requerido; erro sintático não é apagado |
| IFNDR | Redeclarações, validade de templates e pontos de instanciação | Não exigir consenso de rejeição dos compiladores |
| Ambiente | -D/-U, includes, forced includes, overlays, comando por TU | U para nearest não comprovado, dependência ausente e provenance insuficiente |
| Incremental | Cold/warm/update equivalentes | Include criado, busca negativa invalidada, cancel/retry seguro |

As regressões discriminadoras iniciais devem comparar `typename T::X` direto com erro dentro de `A<T>`, retorno prefixado com trailing return e retorno auto pelo corpo com trailing `decltype`. Acrescentar alias com argumento não usado inválido, duas parciais ambíguas, constraint que evita efeito à direita e lambda capture versus corpo. Esses pares impedem uma implementação que aparente funcionar apenas reconhecendo `enable_if` ([exemplos normativos](https://eel.is/c++draft/temp.deduct.general), [CWG 1558](https://cplusplus.github.io/CWG/issues/1558.html)).

### O diferencial é um auxílio, não uma votação de conformidade

Fixtures devem ser autocontidas, inicialmente com traits próprios e depois STL real. Um manifesto registra categoria P/N/H/I/U, dialeto, DR/revisão, alvo, macros/includes, seleção esperada e milestone. P/N usam tags de retorno ou `static_assert` sobre `decltype` para comprovar o escolhido; N sem fallback exige diagnóstico final. Não basta que o programa compile.

A validação futura deve usar GCC e Clang nas combinações instaladas, com versão, target, biblioteca e comando exato registrados; MSVC é integração adicional com flags de conformidade apropriadas à versão. Compilar apenas para análise sintática, sem executar fixtures. Comparar status, categoria e assertions, não texto literal de mensagens. Divergências passam por revisão normativa/DR; IFNDR fica em trilha observacional.

Execução do harness deve ocorrer em diretório isolado, preferencialmente `/tmp/opencode`, com limites e argumentos controlados. Flags de compile DB não podem virar comandos shell arbitrários. Dependência de compiladores externos fica no harness/adaptador, jamais no core.

Testes determinísticos verificam ordering de candidatos/notas e equivalência de execução com cache habilitado/desabilitado. Metamórficos seguros incluem mudanças de trivia, renomeação consistente de parâmetros, aliases equivalentes e `>>` versus `> >` em contexto válido. Transformar retorno prefixado em trailing **não é** metamorfismo preservador: deve ser teste de sensibilidade à ordem. Reordenar declarações só é equivalente quando a visibilidade e a norma garantem isso.

As suites existentes de lexer/parser, Binder/Typer, chamadas, dispatch, workspace, macros, compile DB e includes recebem regressões mínimas. Formatter, truncation, completion, navigation e highlights preservam contratos. A suite `LeavesTemplatesAndShiftOperatorsAsTyped` merece proteção específica: a nova interpretação angular não pode reescrever shifts como closers ([FormatterSpec.cpp](../test/src/FormatterSpec.cpp), [ParserTruncationSpec.cpp](../test/src/ParserTruncationSpec.cpp), [test/CMakeLists.txt](../test/CMakeLists.txt)).

## Fases condicionadas por provas controlam custo, cache e publicação no LSP

### Cada entrega depende de uma capacidade verificável anterior

| Fase | Dependências e entrega | Critério de aceitação |
|---|---|---|
| M0 — contratos/baseline | Inventário, perfil normativo, fixtures, estados e cobertura | Distinguir existente/proposto; nenhuma expansão de falso diagnóstico |
| M1 — estrutura lossless | M0; cursor angular, template heads, type-id e expressões | Ranges/pais/texto corretos; full/incremental equivalentes; Binder usa mesma estrutura |
| M2 — ambiente e IR | M0/M1; TU contextual, headers sob demanda, provenance, argumentos/tipos | Dependent/U/invalid separados; ambientes distintos não colidem |
| M3 — lookup | M2; conjuntos de candidatos, two-phase lookup, bases/ADL inicial | Definição/instanciação corretas; conjunto aberto permanece U |
| M4 — substituição inicial | M1–M3; dedução básica, aliases, defaults, immediate contexts e transações | Fallback correto, parada lexical e zero leakage nos pares prioritários |
| M5 — seleção e parciais | M4; conversões, best viable, ordering, matching e constraints inicial | Escolhido/ambiguidade/no-match provados; nenhum score heurístico |
| M6 — efeitos e ampliação | M4/M5; scheduler, auto, lambdas, noexcept/defaults, constexpr, packs/NTTP | H preservado; budgets retornam U; perfil de DR demonstrado |
| M7 — integração/caches | Inicia em M2, conclui após M6; queries comuns, invalidação e LSP | Cold/warm/update iguais; cancelamento, versões e CLI/LSP coerentes |
| M8 — cobertura publicada | M7; STL real, casos restantes e integrações adicionais | Matriz por dialeto/recurso/backend; lacunas explicitadas |

O scheduler mínimo e as fronteiras de efeitos são necessários já em M4; M6 amplia sua cobertura, não introduz tardiamente a distinção de erro duro. Constraints capazes de bloquear efeitos entram antes de se anunciar seleção dos casos que dependem delas. Cada fase pode entregar um subconjunto fechado, sem emitir conclusões fora dele.

Gate comum proposto: **100% das fixtures do subconjunto declarado classificadas corretamente e zero falsos diagnósticos/autofixes no corpus U curado**. Isso é meta futura, não dado medido nem garantia universal. A aceitação inclui regressões pertinentes e políticas CorePolicy/ArchitecturePolicy preservadas. Não há estimativa fundamentada de semanas: ambiente, lookup e efeitos são frentes de motor semântico, não simples regras locais.

Na implementação futura, localizar primeiro o `CMakeCache.txt` ativo e preservar compilador/opções; em caso de ambiguidade, pedir o diretório. Novos arquivos entram nas listas explícitas de CMake. C++ novo/materialmente alterado deve ser formatado com o executável Heimdall selecionado, sem formatter concorrente ou alterações em massa ([AGENTS.md](../AGENTS.md)).

### Cache precisa invalidar inclusive ausência de informação

Chaves por consulta incluem identidade/revisão da declaração, argumentos canônicos, fase/local/acesso, contexto de lookup/ADL, ponto de instanciação relevante e fingerprint do ambiente. Hash exige comparação estrutural; IDs locais não atravessam snapshots/modelos sem tradução. Cachear causa estruturada, não mensagem formatada para um call site específico.

Dependências incluem buscas negativas, headers não encontrados e diretórios pertinentes. Mudança em base, friend, alias, default, constraint, corpo necessário, overlay, macros, compile DB, alvo ou toolchain invalida consultas afetadas. Criar um header antes ausente também invalida falhas antigas. Resolver includes/macros pode mudar sintaxe: preservar syntax na invalidação semântica exige verificar o fingerprint correspondente ([Workspace.cpp](../analysis/src/Workspace.cpp)).

Modelos publicados permanecem imutáveis; caches podem ser por snapshot/worker ou coordenados nas fronteiras. Eviction não quebra lifetimes presos por snapshots. Estado InProgress detecta ciclos; profundidade, instâncias, packs, candidatos, comparações e passos constexpr recebem budgets separados. Limite ou cancelamento não vira SubstitutionFailure, e consulta cancelada não deixa resultado publicável memorizado.

CLI e LSP devem consultar os mesmos resultados por AnalysisSnapshot/AnalysisFeatures. Hover pode mostrar dependente/indeterminado; signature help pode listar candidatos sem declarar vencedor. Rename/autofix continua exigindo prova adequada, origem editável e invariância: observações Workspace não equivalem a Invariant ([TypeModel.hpp](../semantic/include/Heimdall/TypeModel.hpp), [Pipeline.cpp](../src/Pipeline.cpp), [Server.cpp](../lsp/Server.cpp)).

Smokes futuros cobrem didChange, cancelamento, edição não salva de header, atualização de compile DB, restauração de dependência e paridade CLI/LSP. Nenhuma versão antiga publica após resultado novo. Regras existentes e o analyzer legado precisam consumir ou respeitar a mesma cobertura para não reintroduzir erro especulativo em candidatos descartados.

### Desempenho deve ser medido sem alterar a semântica

Baseline usa Lexer/Parser, Bind/Type/rules, Workspace, memória de documento, macros e latência LSP. Novos benchmarks podem separar lookup, dedução, substituição, effects, constexpr e ranking; arquivos adicionais exigem CMake explícito. O core continua STL-only, sem exceções/RTTI, sem simdjson público ou dependência de backend externo ([AGENTS.md](../AGENTS.md), [SemanticBench.cpp](../bench/src/SemanticBench.cpp), [bench/CMakeLists.txt](../bench/CMakeLists.txt)).

Casos parametrizados incluem 1/8/32/128 candidatos, falha lexical cedo/tarde, profundidade de aliases/bases, múltiplas parciais, packs e consultas repetidas/distintas. Medir cold/warm/update, latências p50/p95/p99, memória usada/reservada, pico RSS, instâncias visitadas, efeitos evitados, cache hit/miss e incompletude por motivo. Separar probes de compilador de custo semântico e controlar build, corpus, host e opções.

Budgets numéricos dependem de baseline. Como proposta inicial negociável, investigar regressão de mediana acima de 10% no corpus não-template; não reprovar por amostra única. Metas LSP só recebem números após definir operação, corpus e hardware. Nenhum speedup, consumo ou cumprimento de latência foi demonstrado nesta pesquisa.

| Risco dominante | Controle e condição de bloqueio |
|---|---|
| Unknown convertido em falha | Estados/provas explícitos; bloquear rejeição com conjunto aberto |
| Instanciação antecipada | Scheduler e trace de efeitos; provar parada lexical |
| Draft vivo/DR divergente | Snapshot normativo e fixtures por versão/dialeto |
| Headers/macros incompletos | Ambiente por TU, provenance e motivos U |
| Explosão combinatória | Budgets, scratch reutilizável, métricas e cancelamento |
| Cache stale/IDs inválidos | Dependências negativas, fingerprints e ownership por snapshot |
| Regressão sintática/formatter | Ranges lossless, closers/shifts, truncation e full/incremental |
| Escopo virar compilador completo | Capabilities publicadas; backend/recursos avançados como decisões próprias |

### Referências e rastreabilidade da pesquisa

O ponto de entrada solicitado é [cppreference — SFINAE](https://cppreference.com/cpp/language/sfinae). A autoridade para o perfil atual vem de [temp.deduct.general](https://eel.is/c++draft/temp.deduct.general), [temp.deduct.type](https://eel.is/c++draft/temp.deduct.type), [temp.alias](https://eel.is/c++draft/temp.alias), [temp.res.general](https://eel.is/c++draft/temp.res.general), [partial matching](https://eel.is/c++draft/temp.spec.partial.match), [partial ordering](https://eel.is/c++draft/temp.spec.partial.order), [constraints](https://eel.is/c++draft/temp.constr.atomic) e [best viable function](https://eel.is/c++draft/over.match.best.general). Os DRs prioritários estão vinculados individualmente na tabela normativa.

A rastreabilidade local completa está nas notas [léxico/sintaxe](../research_notes/Plano%20de%20suporte%20a%20SFINAE/lexico_sintatico.md), [regras normativas](../research_notes/Plano%20de%20suporte%20a%20SFINAE/regras.md) e [semântica/testes](../research_notes/Plano%20de%20suporte%20a%20SFINAE/semantica_testes.md). Documentação arquitetural histórica orienta princípios, mas capacidades atuais foram atribuídas ao código inspecionado, não inferidas de descrições possivelmente desatualizadas. Não houve pesquisa anterior a incorporar nem comparação integral das edições ISO publicadas.

## Conclusão

A unidade de entrega correta é uma **consulta semântica com prova e cobertura**, não uma lista de idioms reconhecidos. Isso permite melhorar precisão progressivamente sem transformar lacunas do motor em erros do usuário. A decisão sobre ambiente/backends deve preceder qualquer promessa de cobertura ampla da STL: sem declarações e configuração suficientes, aumentar regras de substituição apenas aumenta o risco de falsa certeza.

O plano torna a fronteira entre suportado e desconhecido parte verificável do produto. Quando uma nova fase reduz U com prova, CLI, LSP e refactorings ganham confiança juntos; quando não pode provar, a resposta conservadora continua correta. Essa é a condição para anunciar suporte SFINAE delimitado e reproduzível, em vez de “conformidade completa” baseada em padrões `enable_if`.
