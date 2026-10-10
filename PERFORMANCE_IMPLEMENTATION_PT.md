# Implementação da revisão de desempenho do Heimdall

Data: 10/10/2026. Base: commit `5173bc5`. Build preservado: Ninja, Release,
GCC 16.2.0 em `C:\dev\Heimdall\build`.

Este documento acompanha `PERFORMANCE_ARCHITECTURE_REVIEW_PT.md` e registra o
estado efetivamente implementado. Não constitui uma migração completa para
red-green trees nem para um banco de queries semânticas por escopo.

## Estado de cada prioridade

| Prioridade da revisão | Estado | Implementação e limites |
|---|---|---|
| 1. Instrumentação e memória | Implementada | Tempos de lexer, preprocessor, grammar e auxiliares; contadores de espera/execução das filas; bytes reservados e pico do upstream das arenas; capacidades da árvore e adaptador AoS; deduplicação de buffers compartilhados; working set, pico e private bytes do processo Windows. São estimativas de armazenamento de containers, não contabilidade completa do alocador. |
| 2. Cancelamento e retenção | Implementada | Cancelamento cooperativo em lexer, preprocessor, grammar e formatter; interesse por versão; liberação imediata dos captures de tarefas canceladas ainda na fila; limite de 256 requisições interativas. Consumidores de versões antigas preservam seus resultados. Notificações de sincronização continuam sendo aceitas em ordem. |
| 3. Publicação do workspace | Implementada | Páginas de 64 documentos, metadados do grafo e caminhos compartilhados, edit hull validado, análise de includes fora do lock global e buffers persistentes publicados sem materialização quando seguro. Mudanças no grafo ainda podem copiar seus metadados. |
| 4. Scratch e arenas | Implementada | Reserva calibrada pelo número de tokens significativos; medição do armazenamento upstream reservado e pico; proteção contra fonte excedendo offsets de 32 bits e contra recursão excessiva. |
| 5. Lanes por documento | Implementada | Três workers de documentos, FIFO por URI, progressão independente entre arquivos e barreiras para pin de operações sobre o workspace. Sem bloquear workers esperando outras lanes. |
| 6. Blocos persistentes | Parcial | Texto em peças compartilhadas de até 4 KiB e tokens em slices de blocos de até 512 tokens, com shifts por slice e compactação de fragmentos. As colunas de nós da AST ainda são vetores por árvore; o parser copia/remapeia itens reutilizados. |
| 7. Queries e formatter incremental | Parcial | Queries existentes passaram a calcular fora dos locks e compartilhar uma única construção concorrente. Cache de headers depende da presença de arquivos e do conteúdo das dependências resolvidas, com overlays materializados sob demanda. Cache limitado de formatação reutiliza resultados da mesma árvore/opções. Fingerprints semânticos por categoria e checkpoints de convergência entre edições continuam pendentes. |

## Texto, tokens e parsing

`SourceSnapshot` compartilha peças inalteradas e copia o texto inserido. A tabela
de peças ainda é copiada por edição: seu custo depende do número de peças, não é
O(1). Uma compactação amortizada limita a fragmentação em sessões longas.
`Materialize()` cria uma representação contígua uma única vez por versão e
sincroniza sua publicação.

`TokenSnapshot` relexa uma janela inicial de 256 bytes com lookbehind. A janela cresce
até encontrar um ponto de sincronização compatível com a sequência antiga.
Comentários e strings que atravessam a janela podem exigir leitura até EOF.
Prefixo e sufixo compartilham blocos; offsets do sufixo mudam por um shift do
slice. Metadados inválidos de edição usam lex integral para manter o contrato.
As APIs contíguas existentes continuam disponíveis e podem duplicar payload
enquanto uma árvore os usa. Portanto, reduzir bytes copiados por edição não
garante menor RSS em todos os cenários.

O índice de linhas lê as peças e atualiza offsets a partir do texto inserido.
Conversões UTF-16 preservam caracteres que atravessam fronteiras entre peças.
A tabela de offsets de linhas ainda é copiada; conversões podem alocar uma string
da linha consultada.

O reaproveitamento de preprocessing só ocorre para edições conservadoramente
reconhecidas como incapazes de alterar diretivas: sem mudança de linha, splice,
comentário, literal ou caracteres sensíveis. Macros e opções continuam fazendo
parte da validade do parser. Caso contrário, o preprocessing é integral.

Limites de profundidade em grammar e expressões `#if` produzem diagnóstico em vez
de esgotar a pilha. A recuperação existente e os testes de código truncado foram
preservados.

## Concorrência e caches

As notificações e os pins de requisições usam a mesma lane da URI. Rename,
prepareRename e codeAction precisam enxergar notificações anteriores de todos
os documentos; uma barreira interrompe apenas o início de tarefas posteriores
até o pin terminar. A análise pesada continua nos pools apropriados.

O cancelamento pertence à versão. Retirar uma versão cancela seu trabalho quando
não há consumidores retidos. Cancelar um único pedido não cancela o parsing
necessário para outros pedidos da mesma versão. Tarefas canceladas liberam os
captures antes de serem executadas; pequenos wrappers ainda podem permanecer
na fila até serem drenados.

O callback de cancelamento não roda enquanto o mutex de requisições está
segurado. Essa ordem evita o deadlock encontrado no teste de estresse.

Queries de sintaxe, binding, tipos, resumo, projeto e índice de símbolos calculam
fora dos locks dos caches. Construções concorrentes compartilham o resultado.
`TrimCaches()` elimina referências de caches frios sem invalidar resultados que
o chamador ainda retém. O LSP aplica um orçamento de 128 MiB para caches de
sintaxe/arenas; não se trata de um limite de RSS e não abrange todos os buffers,
índices globais ou versões retidas por usuários.

O cache de headers separa duas dependências: a presença de arquivos abertos, que
pode alterar a resolução de includes, e o conteúdo dos headers transitivos
resolvidos. Editar um arquivo não incluído mantém a chave; abrir um arquivo novo
ou editar um header usado a invalida. A igualdade de conteúdo é exata e delimitada,
sem assumir ausência de colisões de um hash.

Um contexto de diagnóstico em background sem snapshot antes alternava essa chave
com a dos pedidos interativos e ignorava headers não salvos. O caminho agora usa
o snapshot atual do workspace. Uma regressão verifica diagnóstico de referência
ambígua vindo de namespaces definidos em um header aberto sem arquivo em disco.

O cache de formatação usa identidade fraca da árvore e igualdade de todas as
opções. Retém até 16 resultados e 16 MiB de payload contado; não mantém a árvore
viva. Formatação integral e de intervalo compartilham texto e diff da mesma versão.
Uma formatação de intervalo fria ainda processa o arquivo inteiro.

## Validação

Os novos testes cobrem equivalência com lexer integral em sequências aleatórias
de edições, crescimento de janela, offsets após compactação, imutabilidade de
versões retidas, UTF-16, preprocessing, recursão, leitura concorrente do adaptador
AoS, compartilhamento de queries, eviction, FIFO, barreiras e cancelamento.

A regressão de headers não salvos falhou no executável anterior à correção do
snapshot de background e passou cinco vezes na versão corrigida. Os testes de
estresse e cancelamento passaram em três repetições após a correção do deadlock.

Suíte final: **1.152 testes CTest passaram, sem falhas**, em 78,42 s. O build
Release terminou com sucesso. A validação incluiu os smoke tests LSP, testes de
política do core, parsing incremental e tolerância a código truncado.

## Medições reproduzíveis

As medições usam o mesmo build Release, sem build ou testes concorrentes.
O corpus stb usa unidades completas: os tamanhos reais são 1.264, 6.805 e
18.024 linhas, com 38,0, 209,4 e 554,8 KiB.

```powershell
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
.\build\bench\DocumentMemoryBench.exe --lines 1000,5000,20000
.\build\bench\LspLatencyBench.exe --lines 1000,5000,20000 --iterations 100 --warmup 10 --budget-ms 50 --enforce
.\build\bench\LspLatencyBench.exe --lines 1000,5000,20000 --iterations 100 --warmup 10 --burst 8 --documents 4 --typing-delay-ms 0 --budget-ms 50 --enforce
.\build\bench\CoreBench.exe '--benchmark_filter=(Persistent|Contiguous|TokenAoS|TokenSoA)' --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
.\build\bench\WorkspaceBench.exe '--benchmark_filter=(SnapshotPin|PagedWorkspaceUpdate|IncrementalUpdate)' --benchmark_min_time=0.1s --benchmark_repetitions=3 --benchmark_report_aggregates_only=true
```

O benchmark LSP agora valida JSON, distingue `result` de `error` e verifica a
forma da resposta de completion. Isso não demonstra precisão semântica completa
de cada item. As amostras de rajada medem oito notificações seguidas mais a última
completion, alternando quatro documentos, sem pausa entre amostras; não são oito
requisições de completion simultâneas.

Working set e pico são incrementos em relação ao servidor ocioso. Private bytes
é o incremento de `PrivateUsage` no Windows, apresentado separadamente; em
plataformas sem essa medida o benchmark informa `n/a`. Abertura inclui trabalho
frio de caches e headers e não corresponde ao tempo isolado do parser.

### Latência LSP e memória do servidor

Três execuções independentes, cada uma com dez aquecimentos e cem amostras por
tamanho. A tabela mostra o intervalo entre os resultados dessas execuções,
não um intervalo de confiança estatístico.

| Linhas | Abertura ms | p95, um caractere / um documento ms | p95, oito caracteres / quatro documentos ms | Maior latência da rajada ms |
|---:|---:|---:|---:|---:|
| 1.264 | 198,9–204,7 | 1,84–1,86 | 1,82–1,89 | 2,30 |
| 6.805 | 217,7–220,6 | 5,90–6,03 | 5,83–6,02 | 6,65 |
| 18.024 | 724,5–730,3 | 18,98–19,32 | 31,39–31,76 | 33,53 |

A abertura acima corresponde ao cenário de um documento. Todos os p95 das
execuções passaram em `--enforce --budget-ms 50`. A abertura fria continua acima
de 50 ms e a medição não garante esse orçamento para todos os métodos ou entradas.
Comparada à execução histórica isolada, a digitação nos dois tamanhos menores
melhorou, enquanto o maior passou de p95 16,84 ms para aproximadamente 19 ms.

| Linhas | Working set adicional, um documento MiB | Pico adicional, um documento MiB | Private bytes adicionais, um documento MiB |
|---:|---:|---:|---:|
| 1.264 | 3,8–4,0 | 4,8–5,0 | 3,2–3,6 |
| 6.805 | 6,5–7,4 | 8,5–9,4 | 8,3–9,7 |
| 18.024 | 25,4–27,7 | 34,8–36,7 | 32,5–35,3 |

O cenário de quatro documentos grandes teve working set adicional de
47,5–51,4 MiB, pico de 62,8–63,7 MiB e private bytes de 55,1–59,4 MiB. Não houve
redução global de memória demonstrada; buffers contíguos de compatibilidade e
versões simultâneas ainda têm custo relevante.

Uma comparação A/B isolou o bug do snapshot de background com a mesma janela
inicial de 4 KiB e 40 amostras do documento grande em quatro documentos. O p95
passou de **639,12 ms para 32,59 ms**. São duas execuções isoladas, não um fator
de aceleração geral do Heimdall. Com a janela final de 256 bytes, uma execução
adicional equivalente mediu 32,89 ms.

Dados completos em `bench/results/2026-10-10/`: `LspSingle1.txt` a
`LspSingle3.txt`, `LspBurst1.txt` a `LspBurst3.txt`,
`LspBackgroundFixBefore.txt`, `LspBackgroundFixAfterControlled.txt` e
`LspBackgroundFixAfter.txt`.

### Armazenamento da árvore e etapas de parsing

| Linhas | Tokens | Nós | Containers da árvore MiB | Lexer / preprocessing / grammar / auxiliares ms |
|---:|---:|---:|---:|---|
| 1.264 | 7.956 | 1.706 | 0,18 | 0,251 / 0,224 / 0,532 / 0,060 |
| 6.805 | 50.658 | 13.526 | 1,13 | 1,099 / 0,741 / 2,751 / 0,318 |
| 18.024 | 131.398 | 17.189 | 2,67 | 3,315 / 1,930 / 5,420 / 0,794 |

Uma execução de parsing integral. O adaptador AoS não foi materializado neste
benchmark. O contador antigo omitia auxiliares e o novo parser também retém
resultados de preprocessing; os 2,07 MiB antigos não são uma medida diretamente
comparável aos 2,67 MiB atuais. Fonte, overhead do alocador, estruturas semânticas
e memória total do servidor são medidas distintas.

Dados: [DocumentMemory.txt](bench/results/2026-10-10/DocumentMemory.txt).

### Microbenchmarks de buffers

Tempos médios de parede em três repetições. Fonte sintética: 27 KiB / 540 KiB,
com aproximadamente 11 mil / 220 mil tokens. A comparação contígua inclui sua
cópia inicial; não representa um lexer integral. As versões persistentes são
criadas a partir da mesma versão inicial em cada iteração.

| Operação | 1.000 linhas | 20.000 linhas | Payload copiado por edição |
|---|---:|---:|---|
| Texto persistente | 0,375 µs | 2,224 µs | 1 byte |
| Texto contíguo | 1,190 µs | 60,611 µs | 27.000 / 540.000 bytes |
| Tokens persistentes | 2,789 µs | 9,597 µs | 16 tokens |
| Tokens contíguos | 11,211 µs | 839,267 µs | 11.000 / 220.000 tokens |
| Scan de `kind` AoS | 5,102 µs | 121,076 µs | Sem cópia no trecho medido |
| Scan de `kind` SoA | 2,708 µs | 54,247 µs | Coluna construída antes do trecho medido |

A cópia de texto contíguo grande teve CV de 14%; os demais números acima tiveram
CV de parede de até 1,5%. A janela inicial de 4 KiB foi substituída por 256 bytes
após medir seu custo excessivo em documentos pequenos. Esses resultados não
incluem materialização compatível, parsing, semântica ou transporte LSP.

Resultados completos: [Snapshots.json](bench/results/2026-10-10/Snapshots.json).

### Publicação e parsing no workspace

| Operação | 1 documento | 100 documentos | 1.000 documentos |
|---|---:|---:|---:|
| Pin de snapshot | 27,7 ns | 27,5 ns | 27,6 ns |
| Update de buffers pré-construídos, sem dependentes | 1,289 µs | 1,967 µs | 2,109 µs |

O segundo benchmark isola publicação e validação dos metadados. Não inclui criar
texto/tokens nem reconstruir o grafo. A tabela de páginas ainda cresce com o
número de páginas do workspace; não se afirma complexidade constante geral.

`IncrementalUpdate/1000`, que recebe fonte contígua e obtém sintaxe após cada
edição, mediu 0,921 ms, com 999 itens reutilizados. A medição histórica isolada
era 0,733 ms: **esse caminho não apresentou ganho**. A contagem de itens
reutilizados não demonstra menor custo total. O pin manteve o valor histórico
de aproximadamente 27,5 ns.

Resultados completos: [Workspace.json](bench/results/2026-10-10/Workspace.json).

## Mudanças arquiteturais ainda pendentes

1. Compartilhamento físico das colunas de nós entre árvores e eventual fachada
   red-green. Exige adaptar os consumidores dos vetores públicos sem quebrar
   suas APIs nem aumentar materializações no caminho quente.
2. Fingerprints semânticos por categoria e dependências de consultas por escopo.
   O fingerprint atual de significado do header continua conservador: macros,
   templates, corpos inline e contexto de compilação não podem ser ignorados.
3. Checkpoints de todas as passagens do formatter e convergência após uma edição.
   O cache por versão não substitui essa implementação.

Tokens permanecem AoS compactos. O experimento SoA mede uma passagem que lê
apenas `kind`; não basta para justificar a duplicação de colunas ou a migração
de todo o lexer/parser. A AST permanece SoA e não ganhou um objeto alocado por nó.
