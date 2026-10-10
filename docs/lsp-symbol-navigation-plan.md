# Plano: navegação de símbolos no LSP

## Objetivo

Oferecer navegação padrão do VS Code para:

- **Outline** do arquivo atual e seção *Symbols* do Explorer, via `textDocument/documentSymbol`.
- **Go to Symbol in Workspace** (`Ctrl+T` por padrão), via `workspace/symbol`.

Não registrar atalhos próprios: os comandos e atalhos devem continuar sendo os nativos do VS Code. O servidor anuncia as capacidades LSP e o cliente apresenta os resultados.

## Estado atual e componentes

- `lsp/Server.cpp` monta as capacidades durante `initialize` e despacha requests LSP; ainda não há `documentSymbol` nem `workspace/symbol`.
- `lsp/Server.hpp` mantém documentos abertos, snapshots, parse cache e compile database.
- `vscode-extension/package.json` não precisa de contribuição de comandos/atalhos para Outline ou Ctrl+T.
- `analysis/include/Heimdall/ProjectSymbolIndex.hpp` oferece índice semântico de projeto, mas exige que as fontes sejam registradas/importadas explicitamente; não é um índice de workspace pronto para ser consultado sem custo no caminho interativo.

## Decisões de desenho

1. **Outline usa a sintaxe do documento aberto.** Derivar símbolos da `ParseTree` já cacheada e do snapshot preso à versão do request. Não depender de `enableSemantic`, compile database ou importação do projeto para mostrar declarações locais.
2. **Ctrl+T começa com um índice sintático incremental do workspace.** Indexar símbolos declarados nos arquivos do workspace em background, com origem/intervalo/nome/tipo e caminho; atualizar arquivos abertos de seus buffers atuais. Não executar `ProjectSymbolIndex::Build` a cada tecla: sua construção/importação pode ser ampla, cara e sujeita a gaps semânticos que não são necessários para localizar declarações.
3. **Semântica não é requisito de descoberta.** Sobrecargas e símbolos com mesmo nome devem poder aparecer como resultados distintos. O Ctrl+T localiza declarações, não resolve referências nem promete identidade semântica entre arquivos.
4. **Respostas LSP determinísticas e seguras.** Converter offsets de byte para ranges LSP UTF-16 usando o índice de linhas; limitar resultados; respeitar cancelamento; retornar resultados vazios quando não houver símbolos/índice disponível, nunca bloquear a thread de I/O.

## Entrega por fases

### 1. Inventário de declarações e conversões LSP

- Identificar no AST quais nós representam namespace, tipo, enum, função/método, variável/field, alias e enumerador, e como obter nome, qualificação, range da declaração e range do nome.
- Definir mapeamento para `SymbolKind` LSP; omitir nós sem nome utilizável e decidir explicitamente se parâmetros/variáveis locais aparecem no Outline (recomendação: não incluir locais; parâmetros só quando fizerem sentido como filhos de callable).
- Reutilizar a conversão posição/range e serialização JSON existentes; acrescentar testes para UTF-16, CRLF e identificadores fora do BMP se a conversão atual permitir esse caso.

### 2. `textDocument/documentSymbol` (Outline)

- Anunciar `documentSymbolProvider` em `initialize`.
- Implementar handler usando o documento/snapshot capturado no recebimento do request e `CachedParse`.
- Responder como `DocumentSymbol[]` hierárquico, mantendo contenção pai-filho (namespace → tipo → membros, por exemplo), ranges válidos e ordenação pela posição no arquivo.
- Em erro de parse, retornar os símbolos recuperáveis do parse disponível ou lista vazia, sem falhar a sessão.
- Cobrir namespaces aninhados, classes/structs, enums, funções, campos/variáveis, overloads, símbolos anônimos, documento vazio e ranges.

### 3. `workspace/symbol` (Ctrl+T) – primeira versão

- Definir escopo de descoberta: fontes C/C++ sob a raiz explícita do workspace; ignorar diretórios de build/VCS e respeitar limites de arquivos/tamanho. Não tratar `workspaceDiagnostics` como fonte de verdade, pois pode estar desabilitado.
- Construir índice leve em background a partir dos arquivos e atualizar/reindexar em `didOpen`, `didChange`, `didClose` e alterações no disco. Buffers abertos prevalecem sobre disco; fechar um documento restaura conteúdo do disco.
- Não varrer ou parsear o workspace durante a resposta ao request. Publicar snapshots imutáveis do índice; request lê o último snapshot disponível e filtra pela query (nome e, se viável, nome qualificado), com resultados limitados e estáveis.
- Anunciar `workspaceSymbolProvider` como objeto com `workDoneProgress`/`resolveProvider` somente se implementados; inicialmente usar provider simples sem `resolve`.
- Retornar `SymbolInformation[]` (ou `WorkspaceSymbol[]` com `location.uri` e `range`), incluindo caminho/contêiner para distinguir resultados homônimos. Converter posições com o conteúdo correspondente à revisão indexada.
- Tratar cancelamento, falhas de leitura/parse e limites sem bloquear requests ou publicar índice parcial inconsistente.

### 4. Qualidade, desempenho e documentação

- Adicionar testes de protocolo LSP para capacidades, mensagens, seleção por query, URI/range e atualização após edição/fechamento.
- Acrescentar smoke test de extensão/manual: abrir Outline; usar Ctrl+T; selecionar resultado abre o arquivo no local certo.
- Medir custo de varredura, memória, latência de Ctrl+T e atualização em arquivos grandes; estabelecer limites e validar que requests servem snapshots antigos enquanto novo índice é construído.
- Documentar cobertura: primeira versão é sintática, limitada ao workspace explícito e pode omitir construções não recuperáveis pelo parser; headers externos/SDK e símbolos gerados ficam fora salvo decisão posterior.

## Dependências e riscos

- Antes da implementação, confirmar que AST/ParseTree fornece ranges de declaração e relações de escopo suficientes. Se não, expor metadados mínimos no parser, sem acoplar `heimdall_core` a LSP ou `analysis`.
- O maior risco do Ctrl+T é indexar workspace sem bloquear inicialização nem revarrer tudo por edição. Fazer descoberta/build em worker, com limites, cancelamento e snapshots publicados atomicamente.
- `ProjectSymbolIndex` é uma possível evolução futura para nomes/identidade semântica mais ricos, mas sua cobertura explicitamente parcial e custo de importação não devem limitar a primeira versão sintática.
- O protocolo LSP não define o atalho Ctrl+T; a experiência depende de anunciar `workspace/symbol` corretamente e da integração do cliente VS Code.

## Critérios de aceite

- Outline exibe hierarquia útil para o arquivo aberto, atualiza após edição e navega para a posição correta.
- Ctrl+T encontra símbolos de múltiplos arquivos do workspace e abrir um resultado navega para sua declaração.
- Ambos funcionam com semântica desativada; nenhuma varredura/análise custosa acontece na thread de I/O ou durante cada tecla de busca.
- Requests canceláveis, resultados determinísticos e testes cobrindo as capacidades, protocolo, posições e atualizações.

## Estado da implementação

As quatro fases foram implementadas. Onde a realidade divergiu do plano está anotado.

### O que foi feito

- **Inventário (fase 1):** `core/include/Heimdall/SymbolOutline.hpp` / `core/src/SymbolOutline.cpp`
  extraem as declarações da `ParseTree` sem dependências novas (STL apenas). Cada
  `OutlineSymbol` tem nome, `detail`, contêiner qualificado, tipo, intervalo do nome e da
  declaração (inclusive o cabeçalho `template <...>`) e o índice do pai. Locais e parâmetros
  ficam de fora; declarações adiantadas e `friend` também.
- **Correção no parser:** `inline namespace x { ... }` era lido como `Declaration`; agora é
  `NamespaceDefinition` (`GrammarParser.cpp`, teste `ParseTreeSpec.InlineNamespaceIsANamespaceDefinition`).
- **Outline (fase 2):** `textDocument/documentSymbol` usa o snapshot preso ao request e o
  `CachedParse`. Responde `DocumentSymbol[]` hierárquico, ou `SymbolInformation[]` quando o
  cliente não declara `hierarchicalDocumentSymbolSupport`.
- **Ctrl+T (fase 3):** `workspace/symbol` lê o índice de `WorkspaceSymbolIndex`, nunca varre nem
  parseia no request. O índice tem duas camadas (disco e buffers abertos; o buffer prevalece),
  com entradas imutáveis trocadas inteiras.
  - Disco: um worker descobre os arquivos (`WorkspaceFiles`) e os indexa em até 4 threads.
  - Buffers: a pass de diagnósticos publica os símbolos da árvore que já parseou, sem parse extra.
  - `didClose` devolve o arquivo ao disco; `workspace/didChangeWatchedFiles` (a extensão registra
    o watcher) cobre criação, alteração, remoção e pastas removidas.
  - Opção `workspaceSymbols` (padrão `true`) liga/desliga o índice; o Outline não depende dela.
- **Feedback de carregamento:** o servidor envia `window/workDoneProgress` (criar, `begin`, um `report` por
  percentual, `end`) enquanto indexa o disco, só para clientes que declaram `window.workDoneProgress`;
  a extensão mostra "starting language server" na barra de status até o `initialize` responder.
- **Extensão:** watcher de fontes, opção `heimdall.workspaceSymbols`, README. Nenhum comando ou
  atalho próprio foi registrado.

### Decisões que divergem ou completam o plano

- Buffers abertos fora da raiz do workspace também são pesquisáveis (somem ao fechar); só o
  disco é limitado à raiz.
- Escopos anônimos são transparentes na busca (`a::(anonymous namespace)::f` aparece como `a::f`).
- Sem consulta (vazia), o resultado é vazio. O limite é 200 resultados.
- `SymbolInformation` (não `WorkspaceSymbol`) com o intervalo do **nome**, para o cursor cair no
  identificador. Sem `resolve`.

### Cobertura conhecida

- Reconhecido: namespaces, classes, structs, unions, enums e enumeradores, funções e métodos
  (cada sobrecarga), construtores, destrutores, operadores, campos, variáveis, `using X = ...`,
  `typedef` simples, concepts, templates.
- Limitações herdadas do parser: `typedef struct Tag { ... } Alias;` (o `Alias` não aparece),
  declaradores depois de uma definição de tipo (`struct S { ... } instance;`), e o que o parser
  não recupera em código incompleto.
- Fora do escopo: cabeçalhos do sistema/SDK, símbolos gerados por macro, identidade semântica
  entre arquivos (`ProjectSymbolIndex` fica como evolução futura).

### Medições (Release, Windows, 28 núcleos; `bench/src/SymbolBench.cpp`)

| Caso | Resultado |
| --- | --- |
| Extrair o outline de um documento de 18 mil linhas já parseado | 0,17 ms |
| Parse + extração do mesmo documento | 11 ms |
| Converter 430 símbolos para posições LSP | 0,53 ms |
| Trocar o buffer de um arquivo no índice | 0,1 µs |
| Varredura inicial, 1 000 / 5 000 arquivos | 0,8 s / 6 s |
| Varredura inicial, 20 000 arquivos (limite) | 34 s, 87 MB |
| `workspace/symbol`, 5 000 arquivos (205 mil símbolos) | 2–8 ms (p95 ≤ 9 ms) |
| `workspace/symbol`, 20 000 arquivos (820 mil símbolos) | 6–60 ms (p95 ≤ 60 ms) |

Antes da otimização do índice (colunas contíguas, contêineres internados, máscara de caracteres,
seleção por heap limitado), a mesma consulta a 20 000 arquivos levava 72–173 ms.

Limites: 20 000 arquivos, 2 MiB por arquivo, 200 resultados. Um request cancelado devolve
`RequestCancelled`; enquanto o índice novo é construído, as consultas servem as entradas já publicadas.

### Testes

- `SymbolOutlineSpec`, `SymbolProtocolSpec` (UTF-16, CRLF, JSON válido, hierarquia),
  `WorkspaceSymbolIndexSpec` (ranking, determinismo, camadas, cancelamento, concorrência),
  `WorkspaceFilesSpec`.
- `test/LspSymbolSmoke.py`: ponta a ponta com o servidor real, semântica desligada, sem compile
  database (Outline hierárquico e plano, edições, buffers abertos, arquivos monitorados,
  cancelamento, opção desligada).
- Verificação manual no VS Code (não automatizada): abrir o Outline e usar `Ctrl+T`; escolher um
  resultado abre o arquivo na posição do nome.
