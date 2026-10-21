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
grava ao encerrar o servidor. A CLI grava ao encerrar. O caminho aparece no stderr. Buffer: 256 MB
(`HEIMDALL_TRACE_BUFFER_MB`). Abra o `.pftrace` em <https://ui.perfetto.dev>.

## Arquitetura

- `core/include/Heimdall/Trace.hpp`: macros e `trace::Sink` (ponteiros de função), só STL.
- `profiling/`: único lugar que inclui `perfetto.h`; instala o sink e grava o arquivo.
- `vendor/perfetto/`: SDK amalgamado.

Categorias: `startup`, `config`, `compiler`, `macro`, `include`, `parse`, `index`, `semantic`,
`lsp`, `cli`. Threads nomeadas: `lsp-io`, `index-main`, `index-helper`, `workspace-scan`,
`cli-worker`. Contadores: `indexed files`, `workspace files`, `input files`.

## Adicionar pontos de medição

```cpp
HEIMDALL_TRACE_SCOPE(Index, "Nome");                      // nome literal
HEIMDALL_TRACE_SCOPE_DETAIL(Index, "Nome", path.string()); // detalhe só é avaliado com sink
```

Arquivos que usam esses macros não passam na verificação de tokens do rename do Heimdall
(a expansão difere do texto escrito); por isso `src/cli.cpp` usa `trace::Scope` diretamente.
