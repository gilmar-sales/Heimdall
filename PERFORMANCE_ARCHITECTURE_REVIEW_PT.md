# Revisão de memória, desempenho e arquitetura do Heimdall

Data: 10/10/2026. Código analisado: commit `5173bc5`, checkout inicialmente limpo.
Escopo: AST, tokens, parsing incremental, concorrência, memória e latência LSP.
Recomendações usam mecanismos disponíveis em C++23; o projeto permanece C++26.
Nenhuma mudança de implementação foi feita nesta revisão.

As alterações posteriores, validação e medições estão registradas em
[PERFORMANCE_IMPLEMENTATION_PT.md](PERFORMANCE_IMPLEMENTATION_PT.md).

## Parecer

O Heimdall já evita os problemas básicos descritos no pedido: a árvore gramatical
usa índices e cinco vetores SoA, tokens guardam offsets, o modelo semântico tem
interning e arena, e o LSP separa transporte, edição, parsing e requisições.
Não há evidência de um `new` por nó da AST principal nem de cópia de texto por token.

Os principais riscos restantes são trabalho global por edição, retenção simultânea
de versões, parsing compartilhado sem cancelamento, uma fila global de documentos
e cópia profunda dos metadados do workspace. A meta de 50 ms foi atendida no cenário
medido de edição + completion, mas a abertura inicial ficou acima dela. Não há base
para afirmar que toda requisição, todo workspace ou toda entrada atende esse limite.

## Medições realizadas

Build existente: `C:\dev\Heimdall\build`, Ninja, Release, GCC 16.2.0, compilador configurado
`C:/mingw64/bin/g++.exe`. Os caches em `_deps/*-subbuild` são builds auxiliares.
O build completo foi atualizado com `cmake --build build --parallel 4` e terminou
com sucesso. Os benchmarks foram executados depois do build e antes do CTest.

Validação: dos 1.116 testes CTest, 1.106 passaram inicialmente. Dez smoke tests
falharam por acesso negado aos temporários no sandbox; os dez passaram na
reexecução autorizada fora dele (`ctest --test-dir build --rerun-failed
--output-on-failure`). Portanto todos os 1.116 tiveram execução bem-sucedida,
considerando a execução inicial e a reexecução. Não houve alteração de código
entre essas execuções.

```powershell
.\build\bench\DocumentMemoryBench.exe --lines 1000,5000,20000
.\build\bench\LspLatencyBench.exe --lines 1000,5000,20000 --iterations 100 --warmup 10 --budget-ms 50
ctest --test-dir build --output-on-failure
```

O corpus usa unidades completas stb; por isso os tamanhos reais diferem dos pedidos.

| Linhas reais | Fonte KiB | Tokens | Nós | Armazenamento contado da árvore MiB | RSS adicional no processo de memória MiB |
|---:|---:|---:|---:|---:|---:|
| 1.264 | 38,0 | 7.956 | 1.706 | 0,13 | 0,4 |
| 6.805 | 209,4 | 50.658 | 13.526 | 0,86 | 0,9 |
| 18.024 | 554,8 | 131.398 | 17.189 | 2,07 | 2,0 |

| Linhas | Abertura + primeira completion ms | p50 ms | p95 ms | p99 ms | Máximo ms | Working set adicional do servidor MiB | Pico adicional MiB |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1.264 | 208,7 | 1,83 | 2,00 | 2,13 | 2,17 | 3,3 | 4,3 |
| 6.805 | 214,7 | 6,95 | 7,37 | 7,56 | 7,59 | 5,4 | 7,9 |
| 18.024 | 738,6 | 14,94 | 16,84 | 18,48 | 23,85 | 25,0 | 34,3 |

São resultados de uma execução, com dez aquecimentos e cem amostras por tamanho.
O benchmark insere `a` repetidamente em um único ponto e espera cada resposta;
também dorme 15 ms entre iterações. Não exercita acúmulo de requisições, workspace
grande, múltiplos documentos, edição de macros ou todos os métodos LSP.
Seu leitor registra respostas pelo prefixo e ID, sem validar o conteúdo de
`result` versus `error`; estes números medem chegada de resposta, não comprovam
qualidade semântica de cada completion. A abertura inclui inicialização de caches
e trabalho concorrente: não equivale ao tempo isolado do parser.

**Limitação importante do contador de memória:** `TreeBytes` em
`bench/src/DocumentMemoryBench.cpp:66` soma tokens, nós, diretivas e diagnósticos,
mas omite `Items`, máscaras, índices de identificadores e decoração. Os 2,07 MiB
não são o total do objeto, muito menos o total do servidor. `StorageBytes()` em
`core/src/ParseTree.cpp:124` é mais abrangente, porém exclui fonte/AoS e inclui
tokens compartilhados: somá-lo entre proprietários pode contar o mesmo buffer
mais de uma vez. Working set inclui páginas residentes, alocador e caches;
a diferença de 25 MiB versus 2,07 MiB não demonstra vazamento.

## 1. Layout da AST e alocação

### Estado atual: adequado

`core/include/Heimdall/ParseTree.hpp:98` define `GrammarNodeSoA`:

```cpp
std::vector<std::uint8_t> kind;
std::vector<std::uint32_t> first_token;
std::vector<std::uint32_t> token_count;
std::vector<std::uint32_t> parent;
std::vector<std::uint32_t> subtree_end;
```

`GrammarParser::Add` (`core/src/GrammarParser.cpp:326`) acrescenta registros nessas
colunas, sem alocar objetos de nó individualmente. O payload é 17 bytes por nó,
contra 20 bytes esperados para o registro AoS com alinhamento usual de 4 bytes.
Um milhão de nós exige cerca de 16,2 MiB de payload SoA, sem capacidades excedentes,
tokens, diagnósticos ou versões anteriores. Esse cálculo não é uma medição de RSS.

Uma passagem que lê apenas `kind` percorre 1 byte por nó, em vez do stride AoS de
aproximadamente 20 bytes. Isso reduz tráfego potencial; não implica ganho de 20x,
porque caches, prefetch e o restante do processamento interferem.

**Recomendação:** preservar SoA e índices. Trocar essa representação por uma arena
de objetos ligados por ponteiros não elimina pointer chasing e pode piorar a
localidade. Arena é política de alocação, não substituto de layout de dados.

### Risco: materialização AoS e leitura concorrente

`ParseTree::Nodes()` (`ParseTree.hpp:337`) chama `EnsureNodesAoS`
(`ParseTree.cpp:250`), escreve membros `mutable` e duplica os nós. Para um milhão
de nós, a compatibilidade acrescentaria aproximadamente 19,1 MiB de payload.
Duas primeiras chamadas simultâneas não são seguras.

A busca em `core`, `semantic` e `lsp` não encontrou consumidores do adaptador
`ParseTree::Nodes()`; não foi demonstrada uma corrida ativa nesses caminhos.
`SyntaxTree::Nodes()` pertence a outro tipo e não deve ser confundido com ele.

**Recomendação:** manter o adaptador fora das consultas concorrentes; tornar sua
restrição explícita e migrar consumidores restantes de compatibilidade quando
forem modificados. Se a API precisar suportar publicação concorrente, construir
antes da publicação ou prover um adaptador separado com sincronização e orçamento.

### Scratch em arena: reservar antes de crescer

`GrammarParser.cpp:270` usa `std::pmr::monotonic_buffer_resource` com buffer inicial
de 32 KiB para `m_sig`, `m_sig_tok` e `m_match`. `m_sig` cresce por `push_back`
na construção (`:137–195`) sem reserva inicial, enquanto `m_sig_tok` reserva depois.
Em um recurso monotônico, buffers abandonados pelo crescimento continuam vivos
até a destruição do recurso. Com duplicação de capacidade, a soma de capacidades
anteriores tende a mais uma capacidade final; blocos e fatores reais variam.

**Recomendação:** testar reserva de `m_sig` por estimativa de tokens significativos,
ou contagem em duas passagens, comparando bytes alocados e tempo. Reservar todos os
tokens pode desperdiçar memória em fontes com muito trivia. A arena persistente
de `SemanticModel` merece o mesmo acompanhamento de capacidades e high-water mark.
`Arena::Used()` conta solicitações, não todos os bytes/blocos do upstream.

`Arena::Reset()` libera blocos via `release()` (`core/src/Arena.cpp:17`); não há
garantia de liberação O(1), e `Arena::New` não registra destrutores de objetos.
Não usar reset como substituto de destruir objetos com recursos próprios.

## 2. Strings e tokens

`Token` (`core/include/Heimdall/Lexer.hpp:27`) tem tamanho verificado de 12 bytes:
classificação, `Tok`, offset e comprimento de 32 bits. `Text()` retorna uma view.
`ParseSnapshot` (`core/src/ParseTree.cpp:37`) retém fonte e tokens compartilhados;
já `Parse(..., lexed)` (`:33`) copia o vetor passado. O LSP usa o caminho compartilhado.

`InternPool::Intern` (`semantic/src/SemanticModel.cpp:15`) guarda views e retorna
`NameId`; `InternCopy` copia somente nomes que precisam de armazenamento próprio.
`SemanticModel` usa arena e tabelas SoA. Um `unique_ptr<Arena>` por modelo não é
um `unique_ptr` por nó. Os wrappers do workspace mantêm a árvore viva enquanto
o modelo usa suas views (`analysis/src/Workspace.cpp:13`). IDs de pools diferentes
não podem ser comparados como se tivessem identidade global.

**Conclusão:** zero-copy lexical e interning semântico já existem. Não há motivo
para adicionar um pool global de strings, com sincronização e retenção ilimitada.
O hashing inicial continua proporcional ao comprimento da string; apenas comparar
IDs já internados tem custo constante.

### Risco prioritário: cópia integral por edição

`lsp/Server.cpp:1254` copia o texto, o índice de linhas e o vetor de tokens:

```cpp
auto text = std::make_shared<std::string>(*snapshot.text);
auto lines = std::make_shared<LineIndex>(*snapshot.lines);
auto tokens = std::make_shared<std::vector<heimdall::Token>>(*snapshot.tokens);
```

Logo, editar um caractere continua tendo custo O(S + T + L). No maior corpus,
somente fonte + tokens + offsets de linhas representam aproximadamente 2,12 MiB
de payload copiado por versão, antes de deslocamentos, AST e workspace. Versões
retidas por leitores multiplicam esse custo; em rajadas isso pode dominar o pico.

`Lexer::Relex` (`core/src/Lexer.cpp:464`) reduz a região novamente tokenizada, mas
faz `erase`/`insert` e atualiza offsets de todo o sufixo. Portanto não é O(tamanho
da edição) em geral. `LineIndex::Update` também desloca o sufixo de linhas.

**Solução gradual:** primeiro medir bytes copiados e versões simultâneas. Se forem
dominantes, usar blocos imutáveis de texto e tokens, offsets locais ao bloco e
prefixos de comprimento para posição absoluta. Uma piece table/rope persistente
reduz cópia de texto, mas sozinha não resolve o vetor de tokens nem APIs que
exigem `string_view` contíguo. Introduzir um `SourceSnapshot` com acesso por faixa
e materialização contígua sob demanda exige migração explícita dessas APIs.

Offsets de 32 bits impõem limite de representação: validar tamanho antes de
converter, ou documentar/rejeitar arquivos fora do limite. Não aumentar todos os
índices para 64 bits sem necessidade, pois isso encarece as colunas quentes.

## 3. Concorrência e orçamento LSP

### O transporte já está separado

`LanguageServer` (`lsp/Server.cpp:76`) cria pool de requisições, dois workers de
parsing e um worker de documentos. `QueueDocument` (`:425`) desloca edições e
lexing para esse último. `Dispatch` (`:457`) captura versões em ordem FIFO e só
então agenda a requisição. Quando o parse não terminou, `ParsePending` e waiters
liberam o worker interativo (`:538` e `:2971`). Há workers próprios de diagnóstico
e indexação; o scan de workspace distribui arquivos entre threads (`:4029`).

Não é correto afirmar que o parser pesado normalmente executa na thread de I/O,
nem que mutexes curtos já demonstram contenção. O modelo usa locks e atomics,
não estruturas lock-free; isso por si só não é uma deficiência.

### Prioridade alta: parse compartilhado não cancelável

O caminho `CachedParse` passa `{}` como stop token em `Server.cpp:2940`:

```cpp
ParseTree::ParseSnapshot(text, options, {}, lexed, base ? &reuse : nullptr)
```

Cancelar uma requisição interrompe a espera, mas não esse parse. Apagar a entrada
do cache ao receber versão nova também não remove a tarefa capturada na fila.
Com apenas dois workers, parses antigos podem ocupar ambos e manter fontes,
tokens e árvores-base vivos. Filas de `ThreadPool` (`lsp/ThreadPool.hpp:51`) não
possuem limite. Esse risco não apareceu como estouro no teste sequencial.

**Solução C++23:** stop source por job/versão, estado de interessados e prioridade.
Cancelar quando não houver consumidores que precisem da versão; preservar pedidos
já presos a snapshots antigos. Remover jobs obsoletos ainda enfileirados, combinar
pedidos do mesmo URI/versão e limitar trabalho pendente. Não vincular o parse ao
token de um único consumidor, pois o resultado é compartilhado.

O cancelamento cooperativo precisa chegar a lexer, preprocessor e loops longos.
`ParseTree::Build` verifica entre etapas (`ParseTree.cpp:68`), mas lexing ocorre
antes disso; o preprocessor não recebe stop token. `GrammarParser` verifica em
`ParseScope` (`GrammarParser.cpp:4211`), inclusive escopos internos, mas seu
construtor e loops extensos de expressões não fazem polling. Não existe limite
temporal demonstrado para cancelar uma unidade grande.

### Prioridade alta: bloqueio entre documentos

A única fila `m_document_pool(1)` serializa edições de todos os arquivos e pins
de todas as requisições. Uma edição custosa de A atrasa uma completion de B antes
de esta chegar ao pool interativo. A separação do transporte não evita esse atraso.

**Solução:** lanes por documento, executadas sobre pool limitado, preservando
ordem didChange → pin para cada URI. Publicação do estado global precisa de barreira
curta e definição de revisão para consultas entre arquivos. Não simplesmente
aumentar o número de workers da fila atual, pois isso rompe a ordem de versões.

### Locks extensos no sistema de queries

`AnalysisSnapshot::Syntax`, `Semantic` e `Types`
(`analysis/src/Workspace.cpp:420`, `:444`, `:468`) calculam resultados com mutexes
dos caches tomados. `Project` (`:498`) constrói resumos sob lock global do cache.
Assim, a primeira consulta cara serializa outros leitores desse resultado.

**Solução:** publicar um estado de cálculo único por chave, calcular fora do lock
e usar continuação para quem espera. Revalidar a chave antes de publicar. Evitar
dois parses redundantes entre cache LSP e cache de análise; `WithSyntax` já permite
integrar o resultado LSP, portanto medir misses reais antes de unificar tudo.

## 4. Incrementalidade e recuperação

### Incrementalidade existe, mas não é compartilhamento estrutural

`ParseReuse` e `TopLevelItem` permitem reaproveitar itens. `TryReuseItem`
(`GrammarParser.cpp:4060`) verifica faixa, delimitadores e hash
de nomes. Depois copia as colunas e remapeia índices (`:4148–4180`). Uma edição
em uma função evita reexecutar a gramática de outras funções, mas ainda copia
seus nós. Names hash e replay de declarações protegem ambiguidades tipo/expressão.

Persistem passes completos: preprocessor, tokens significativos, matching de
delimitadores, atualização de `subtree_end` e auxiliares (`ParseTree.cpp:89–121`).
Logo, o custo inclui O(S + T + N) mesmo quando muitos itens são reutilizados.
Namespaces e records grandes são itens amplos: a unidade de reparse pode conter
boa parte do arquivo. Erros e delimitadores não fechados restringem reutilização.

**Solução:** começar com blocos SoA imutáveis por item, índices locais e mapas de
base de posição. Reutilizar blocos sem copiar colunas; preservar contexto de nomes,
macros e lookahead na chave. Depois avaliar fragmentação por corpos/escopos e
checkpoints do preprocessor. Alterar `#define`/`#if` pode invalidar grande sufixo;
fallback integral continua necessário.

`GreenNode` em `SyntaxTree.hpp:25` é um registro com posição absoluta de token e
índices, não uma implementação red-green persistente. Adotar red-green completo
é uma mudança grande, que só deve seguir evidência de ganho sobre blocos SoA.
Uma camada red sob demanda pode oferecer navegação sem contaminar scans quentes.

### Recuperação já tem evidências positivas, sem prova geral de latência

O matching de delimitadores (`GrammarParser.cpp:205`) recupera fechamentos
incompatíveis e emite diagnósticos; há `Error`/`ErrorExpression` e caminhos que
continuam produzindo nós para construções incompletas. `ParserTruncationSpec`
testa todos os prefixos de um exemplo, limites de erros e validade de ranges.
`IncrementalParseSpec` compara árvores reutilizadas com parses completos após
edições aleatórias e bem formadas. O limite de dois segundos desses testes é
proteção contra hangs, não validação de 50 ms.

**Recomendação:** conservar esses testes e ampliar campanhas com operadores em
cadeia, nesting profundo, strings/comentários abertos e edits de macros. Não foi
identificado nesta revisão um bug reproduzido de recuperação ou stack overflow;
o parser gramatical é recursivo, então limites de profundidade e trabalho devem
ser medidos separadamente do parser estrutural iterativo `SyntaxTree`.

## 5. Queries e escala do workspace

Já existe análise sob demanda com caches de sintaxe, binding, tipos, resumos e
índices. Isso é uma fundação de queries; não equivale automaticamente a um grafo
incremental por escopo com dependências precisas.

`WorkspaceState` (`analysis/src/Workspace.cpp:73`) copia `documents`, `paths`,
`dependencies` e `dependents`. `Workspace::Update` (`:1083`) faz essa cópia sob
`m_mutex`, invocado sob `m_docs_mu` em `Server.cpp:1276`. As listas de adjacência
têm cópia profunda: o custo cresce com D + E e bytes das chaves, não apenas com
o arquivo editado. Além disso, a atualização varre prefixo/sufixo para descobrir
o edit hull (`Workspace.cpp:1109`) mesmo quando o LSP já conhece a edição, e
`Includes` reexecuta preprocessor (`:167`).

`InvalidateDependents` (`:143`) invalida transitivamente modelos semânticos em
qualquer mudança de conteúdo. Uma alteração só no corpo de um header pode gerar
reconstruções desnecessárias, dependendo de quais propriedades seus consumidores
realmente usam. Não é seguro comparar apenas nomes: macros, inline bodies,
constexpr, templates e contexto de compilação também podem ser dependências.

**Solução:** overload de Update com edit hull validado; compartilhar mapas de
caminhos e grafo quando inalterados; usar páginas persistentes para slots de
documentos. Resumos com fingerprints por categoria de dependência permitem
invalidação mais precisa. Versionar caches por documento, opções de compilação,
macros e resumos usados. Aplicar orçamento/eviction a resultados frios sem
destruir os snapshots que leitores ainda usam.

## 6. Formatter e decisões DoD

`RangeFormatDocument` (`lsp/Server.cpp:3173`) chama `FormatEdits` para o arquivo inteiro e
só depois filtra hunks. `Formatter::FormatEdits` (`core/src/Formatter.cpp:4168`)
formata a fonte inteira. A checagem de cancelamento no loop de resposta não
interrompe esse processamento inicial. Formatar uma linha pode, portanto, pagar
o custo de todo o arquivo e gerar buffers intermediários.

**Solução:** checkpoints do estado de formatação por linha/bloco e processamento
até convergir ao estado anterior. O estado precisa incluir todas as opções e
passagens reais do formatter, não apenas profundidade de chaves. Até lá, adicionar
cancelamento e usar tokens/árvore da versão quando o caminho permitir.

Tokens AoS de 12 bytes são compactos e diversos passes leem kind + offset + length
juntos. SoA de tokens deve ser experimento, não migração automática. Passes que
leem só classificações podem usar colunas existentes (`m_sig_tok` e máscaras);
evitar duplicar offsets e comprimentos sem ganho medido. SoA facilita streaming,
mas parser com dependências e branches não vira SIMD automaticamente.

## 7. Prioridades e validação recomendada

| Ordem | Mudança | Benefício esperado, ainda não medido após implementação | Risco |
|---:|---|---|---|
| 1 | Métricas por etapa, fila e snapshot; corrigir contagem de memória | Atribuir os 738,6 ms frios e o pico de 34,3 MiB | Baixo |
| 2 | Cancelamento por versão/interessados e descarte de jobs obsoletos | Limitar CPU e retenção sob rajadas | Médio: preservar consumidores antigos |
| 3 | Evitar cópia do grafo e aceitar edit hull em Update | Remover custo proporcional ao workspace e scans redundantes | Médio: invalidação correta |
| 4 | Reserva de scratch calibrada | Reduzir capacidades antigas retidas pela arena | Baixo |
| 5 | Lanes por documento | Isolar latência de arquivos independentes | Alto: ordenação e revisões |
| 6 | Blocos persistentes de texto/tokens/SoA | Reduzir cópia por versão e compartilhamento de nós | Alto: APIs, positions e lifetime |
| 7 | Queries por dependências e formatter incremental | Reduzir reconstrução semântica e trabalho de range format | Alto: validade dos resultados |

Registrar tempos de espera e execução separadamente: edição, workspace Update,
preprocessor, grammar, cópia de nós, binding, tipos, completion e serialização.
Registrar capacidades, blocos upstream, fontes únicas, tokens únicos, versões
vivas, comprimento das filas e atraso para cancelar. Não somar shared buffers
por proprietário. Working set, private bytes e high-water mark devem ser distintos.

Repetir medições Release em processo sem build/test concorrente, com aquecimento,
múltiplas execuções e p50/p95/p99. Acrescentar arquivos maiores, rajadas sem espera,
typing em vários pontos, cancelamentos, múltiplos documentos e workspace com
milhares de arquivos. Verificar conteúdo das respostas, não só arrival time.
Comparar parsing integral e incremental no mesmo corpus; número de itens reutilizados
não mede bytes evitados. Usar os CoreBench pertinentes e profiling de alocações/cache
antes de atribuir ganhos a arena, SoA ou intern pool.

## Referências primárias para as propostas

O desenho de [threads e requisições do clangd](https://clangd.llvm.org/design/threads)
documenta scheduling por arquivo e separação de tarefas de parsing e consultas.
Ele serve como referência para lanes e continuidade; não prova a performance do
Heimdall nem exige uma thread dedicada para cada arquivo.

A [arquitetura do rust-analyzer](https://rust-analyzer.github.io/book/contributing/architecture.html)
descreve separação entre representação sintática, banco de análise e integração
com o editor. É referência para snapshots e queries; uma implementação equivalente
em C++ deve preservar dependências, invalidação e ownership explícitos.

O [rascunho oficial C++ N4944](https://isocpp.org/files/papers/N4944.pdf), seção de
`monotonic_buffer_resource`, define liberação do armazenamento obtido do upstream.
Não autoriza presumir custo O(1) independente da quantidade de blocos.
