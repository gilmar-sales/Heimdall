# Profiling com Perfetto

Instrumentação opt-in das etapas de carregamento (CLI e LSP). Sem `HEIMDALL_PROFILING` os
macros `HEIMDALL_TRACE_*` expandem para nada e o Perfetto não é compilado.

## Habilitar

```bash
cmake -G Ninja -S . -B cmake-build-profiling -DHEIMDALL_PROFILING=ON
cmake --build cmake-build-profiling --target heimdall heimdall-lsp
```

Com `HEIMDALL_PROFILING=ON`, CLI e LSP gravam automaticamente `heimdall.pftrace` na
raiz do projeto aberto. No LSP, usa a primeira pasta do workspace informada pelo editor
(ou `rootUri`/`rootPath`). Na CLI, procura `.git`, `.heimdall.json` ou `CMakeLists.txt`
no diretório de execução e seus ancestrais; sem esses marcadores, usa o diretório de
execução. Não é necessário definir `HEIMDALL_TRACE`; seu valor não altera o destino.
Cada execução sobrescreve o arquivo anterior ao encerrar. Execuções simultâneas usam o
mesmo destino; a última gravação prevalece.

```bash
./cmake-build-profiling/src/heimdall lint --compile-commands build/compile_commands.json src/cli.cpp
```

O LSP encerra a captura e grava o arquivo ao concluir a indexação inicial, antes de
anunciar o índice pronto, sem precisar fechar o editor. Se a indexação não terminar,
grava ao encerrar o servidor. A CLI grava ao encerrar. O caminho aparece no stderr.
Abra o `.pftrace` em <https://ui.perfetto.dev>.

## Categorias e cenários de investigação

`HEIMDALL_TRACE_CATEGORIES` seleciona as categorias **antes da captura**, não apenas
na visualização do Perfetto. Categorias desabilitadas não emitem eventos nem avaliam
os detalhes dos escopos. A seleção é lida no início de cada sessão; reinicie o LSP
após mudar o ambiente que inicia o servidor/editor. Alterar somente uma variável no
terminal integrado não altera o ambiente de um editor que já está aberto.

Sem a variável, ou com valor vazio, usa `overview`: fases gerais, sem eventos por
condição de macro, por include, por cache ou por lock.

| Categoria | O que mede |
|---|---|
| `startup`, `config`, `compiler` | Inicialização, configuração e consultas externas ao compilador |
| `macro`, `include` | Preparação do ambiente e construção dos perfis de includes |
| `parse`, `index`, `semantic`, `lsp`, `cli` | Fases principais de análise e processamento |
| `scheduler` | Execução das tarefas da pool e contador `task queue delay ns` |
| `header` | `ScanHeader`: varredura recursiva, incluindo o trabalho dos descendentes |
| `macro.detail` | `EvaluateCondition` e `ExpandIncludeOperand`; pode gerar muitos eventos |
| `include.detail` | Resolução individual de includes |
| `cache` | Consultas/atualizações dos caches de efeitos e resultados hit/miss/accepted/rejected |
| `cache.detail` | `ValidateEffect`, `ApplyEffect` e `ReplayEffects`; potencialmente volumoso |
| `io` | Leitura de headers e consultas aos metadados dos arquivos |
| `lock.wait` | Tempo até adquirir os mutexes instrumentados |
| `lock.hold` | Tempo com esses mutexes adquiridos, terminando antes do unlock |

Presets:

- `overview`: categorias gerais, incluindo `scheduler` (padrão).
- `macros`: overview + `header,macro.detail`.
- `includes`: overview + `header,include.detail,cache,cache.detail,io`.
- `contention`: overview + `cache,io,lock.wait,lock.hold`.
- `all` (ou `*`): todas; `none`: nenhuma, mantendo apenas metadados da sessão.

Combine nomes e presets separados por vírgulas. `-nome` exclui uma categoria ou
preset; a ordem é significativa. Uma seleção iniciada com exclusão parte de
`overview`. Nomes desconhecidos e itens vazios são erros, sem fallback silencioso.

Exemplos em PowerShell, antes de iniciar a CLI ou o editor:

```powershell
# Investigar o PCH sem eventos para cada condição de macro:
$env:HEIMDALL_TRACE_CATEGORIES = 'overview,header,cache,lock.wait,lock.hold'

# Investigar contenção, incluindo leitura de arquivos:
$env:HEIMDALL_TRACE_CATEGORIES = 'contention,header'

# Investigar apenas parsing e filas de tarefas:
$env:HEIMDALL_TRACE_CATEGORIES = 'parse,scheduler'

# Captura detalhada, excluindo os dois grupos mais volumosos:
$env:HEIMDALL_TRACE_CATEGORIES = 'all,-macro.detail,-cache.detail'
```

No caso dos headers, compare os intervalos `lock.wait` com `lock.hold` de
`EffectCache::Find`, `EffectCache::Store`, `HeaderContentCache::Find`,
`HeaderContentCache::Store` e `BuiltinTableCache`. O cache de probes também instrumenta
`CompilerProbeCache::Find/Store`. São pontos instrumentados específicos, não todos os
locks do processo. Um `ScanHeader` longo, isoladamente, não comprova contenção.

Para investigar espera na publicação de progresso ou transporte:

- `lock.wait` / `lock.hold`: `IndexProgress::Update`, `IndexProgress::Snapshot` e
  `JsonRpc::Send` distinguem aquisição e posse dos respectivos mutexes.
- `lsp`: `JsonRpc::Send` e `PublishIndexProgress` abrangem o envio completo.
- `io`: `JsonRpc::Write` e `JsonRpc::Flush` medem escrita e flush no stdout.
- `index`: `UpdateIndexActivity` mede a atualização do progresso **fora** do intervalo
  da etapa seguinte. Assim, esperar para atualizar a atividade não aparece como parsing.

Use `contention,header` para esses eventos com as fases gerais. Um `JsonRpc::Flush`
longo aponta para o transporte; um `lock.wait/JsonRpc::Send` longo aponta para espera
por outro remetente. Isso não mede CPU: são intervalos de tempo decorrido.

Para identificar parsing repetido, os escopos `WorkspaceDiagnosticsFile`,
`ParseIndexFile` e `ParseDocument` carregam o caminho/URI do arquivo e distinguem
diagnósticos, indexação e parsing requisitado pelo editor. Seus descendentes mostram
`ParseTree::Build` e `Lex`; compare o mesmo arquivo e as opções antes de concluir
que o trabalho é redundante.

## Threads do LSP

Documentos, parsing, requisições e auxiliares da indexação compartilham uma única
`ThreadPool`, com lanes FIFO por documento e barreiras para snapshots. Não existem
mais pools independentes para documentos e parsing nem novas threads por auxiliar
de indexação. Com dois ou mais workers, tarefas background podem ocupar no máximo
N-1 workers, deixando capacidade para requisições interativas. Isso não preempta uma
tarefa já em execução nem elimina toda espera por locks ou parsing.

A indexação agenda uma tarefa `SymbolIndexFile` por arquivo, com uma janela limitada
por `HEIMDALL_INDEX_THREADS` (máximo de quatro). Um arquivo lento não impede os outros
slots de avançar; a pool reavalia as prioridades entre arquivos, sem enfileirar todo o
workspace de uma vez. `IndexDispatchWait` e `IndexCompletionWait`, em `scheduler`,
mostram as esperas do coordenador por slots e pela conclusão das tarefas.

Workers apenas atualizam o último estado de progresso em memória. O reporter copia
esse estado sob o lock e libera o mutex antes de montar/enviar as notificações. Envia
no máximo uma atualização a cada 250 ms, com heartbeat de um segundo se nada mudou;
marcos de início/fim não seguem esse limite. Nenhuma resposta a requisições é descartada.
O trace é salvo após a indexação e antes de aguardar o reporter encerrar; uma publicação
ainda bloqueada aparece como intervalo cortado pela captura. O envio continua síncrono:
stdout bloqueado ainda pode atrasar respostas e o encerramento,
mas não mantém adquirido o mutex usado pelos indexadores para atualizar o progresso.

O transporte de I/O e os coordenadores de indexação, varredura e diagnósticos continuam
separados: seus loops/esperas não são colocados dentro da pool. O coordenador da
indexação descobre os arquivos, despacha as tarefas e verifica a completude. A varredura
de diagnósticos mantém seu fluxo próprio; portanto, uma única pool não significa que
todo o trabalho do processo seja executado exclusivamente por seus workers.

## Integridade da captura

- O buffer do serviço (256 MB por padrão, configurável por `HEIMDALL_TRACE_BUFFER_MB`)
  é drenado para um arquivo temporário a cada 100 ms. A captura completa não fica
  retida em um ring buffer até o fim, evitando sobrescrever o início em sessões longas.
- O produtor usa 32 MB de memória compartilhada para absorver rajadas das threads.
  O SDK de TrackEvent ainda pode descartar pacotes se esse buffer se esgotar; disco
  lento, cheio ou indisponível também pode impedir uma captura íntegra. O aumento
  de capacidade e a drenagem periódica não são uma garantia absoluta contra perdas.
- Ao parar, novos eventos são bloqueados, os intervalos ativos são fechados na sua
  trilha original e o pipeline recebe um flush síncrono antes do encerramento. Intervalos
  cortados pela captura têm o argumento `capture_cutoff = true`: o fim indica o limite
  da gravação, não que a tarefa tenha terminado.
- O arquivo temporário é copiado para `<projeto>/heimdall.pftrace` ao finalizar.
  Em caso de falha de exportação/flush, o temporário é preservado e seu caminho aparece
  no stderr para recuperação.

No SQL do Perfetto, verifique perdas e intervalos não finalizados:

```sql
SELECT name, value FROM stats WHERE severity = 'data_loss' AND value != 0;
SELECT name, COUNT(*) FROM slice WHERE dur = -1 GROUP BY name;
```

Resultados vazios são esperados nas capturas de regressão. Isso não substitui a
validação com a carga real de indexação de cada projeto.

## Arquitetura

- `core/include/Heimdall/Trace.hpp`: macros e `trace::Sink` (ponteiros de função), só STL.
- `profiling/`: único lugar que inclui `perfetto.h`; instala o sink e grava o arquivo.
- `vendor/perfetto/`: SDK amalgamado.

Threads nomeadas: `lsp-io`, `index-main`, `lsp-worker`, `workspace-scan`,
`cli-worker`. Contadores: `indexed files`, `workspace files`, `input files`.

## Adicionar pontos de medição

```cpp
HEIMDALL_TRACE_SCOPE(Index, "Nome");                      // nome literal
HEIMDALL_TRACE_SCOPE_DETAIL(Index, "Nome", path.string()); // detalhe só se a categoria estiver ativa
HEIMDALL_TRACE_LOCK(mutex, "NomeDoCache");                 // espera e posse em categorias distintas
```

Arquivos que usam esses macros não passam na verificação de tokens do rename do Heimdall
(a expansão difere do texto escrito); por isso `src/cli.cpp` usa `trace::Scope` diretamente.
