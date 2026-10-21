# Plano de otimização e previsibilidade da indexação

## Objetivo e escopo

Reduzir o tempo até o índice do projeto estar utilizável, o tempo até a cobertura
completa e a variação de latência entre arquivos e execuções. Preservar a resolução
de macros, os includes transitivos, os buffers não salvos e as configurações reais
de compilação. Este documento planeja o trabalho; as otimizações ainda não estão
implementadas.

Tempos de processos novos e de caches aquecidos são populações distintas. Não há
garantia de tempos idênticos: cache do sistema operacional, armazenamento, carga de
CPU e antivírus interferem. A meta é remover trabalho duplicado e picos evitáveis,
medir a variação restante e manter o editor responsivo.

## Evidências disponíveis

Medição isolada em Release, com os comandos de `build/compile_commands.json`:

| Arquivo | Headers transitivos | Macros/includes, primeira passagem | Parsing |
|---|---:|---:|---:|
| `mutex.h` | 452 | 3,37 s | 14,4 ms |
| `log.h` | 154 | 0,51 s | 0,8 ms |
| `json_reporter.cc` | 507 | 1,66 s | 15,4 ms |

Na segunda passagem do mesmo processo, a preparação levou aproximadamente
0,94 s, 0,49 s e 0,32 s. Essas são amostras exploratórias, não percentis nem prova
de ganho. A execução LSP sobre 1176 arquivos concluiu a indexação dos arquivos em
aproximadamente 72 s; a confirmação de completude posterior deve ser medida
separadamente. O tempo mostrado na notificação é acumulado.

O código atual demonstra:

- `LanguageServer::IndexDiskSymbols` prepara um ambiente de macros por arquivo.
  O cache de ambientes por documento não é preenchido nesse caminho.
- `MacroEnvironment::Builder::Read` mantém conteúdos somente durante aquele build.
- `Resolve` recria caminhos de busca e consulta a existência de candidatos.
- `EffectCache` é global e armazena uma única versão por chave de configuração e
  header; contextos de macros diferentes podem substituir versões úteis.
- `Valid` verifica leituras de macros e consulta tamanho/data dos arquivos de cada
  efeito. `Absorb` e `Apply` concatenam dependências sem deduplicação explícita.
- Ao chegar a 8192 entradas, `EffectCache::Store` apaga o cache inteiro.
- `Builtins` e `SystemIncludes` liberam o mutex antes de executar o processo externo;
  solicitações simultâneas da mesma chave podem repetir essa execução.
- Os arquivos são ordenados por caminho. Assim, `build/_deps` recebe processamento
  antes de vários arquivos do projeto.
- O token de cancelamento da indexação chega ao parser, mas o caminho atual de
  preparação do ambiente usa o token do contexto de requisição, inexistente no worker.

O impacto quantitativo de cada item ainda precisa ser demonstrado. Em especial,
não está demonstrado que o limite de 8192 seja atingido nesta reprodução.

## 1. Instrumentação e linha de base

Adicionar métricas opt-in, agregadas por worker e consolidadas ao final, sem escrita
por token e sem inserir logs em stdout do protocolo LSP. Relatório JSON deve incluir:

- Descoberta, espera na fila, configuração, consulta ao compilador, resolução de
  includes, leitura, validação/replay de efeitos, scan de macros, parser, outline,
  shard, publicação e confirmação final de completude.
- Contagens de consultas de existência e metadados, arquivos lidos, bytes, headers
  únicos, dependências repetidas, hits/misses, motivos de invalidação e evicções.
- Builds compartilhados, esperas por builds em andamento, subprocessos iniciados,
  tempo de locks, memória residente e bytes retidos por cache.
- Identificação da configuração, corpus, compilador, opções, número de workers,
  revisão e indicação explícita de processo novo ou processo aquecido.

Estender `bench/src/MacroBench.cpp` e criar um driver de inicialização LSP para o
workspace completo. Registrar início até projeto disponível e até índice completo;
não usar apenas o último contador de arquivos como conclusão de todas as etapas.

Matriz: fixtures determinísticas e o próprio Heimdall; 1, 2 e 4 workers; processo
novo; repetição no mesmo processo; edição de header comum; edição sem dependentes;
alteração de configuração; criação/remoção de include; pressão de memória.
Repetir execuções pareadas, alternando baseline e candidato. Usar pelo menos 30
amostras para p50/p95 de inicialização e 100 antes de interpretar p99. Não anunciar
cache de disco frio quando apenas o processo foi reiniciado. Não limpar caches do
sistema ou desligar antivírus como requisito de uso.

Entrega: relatório reproduzível, comandos e ranking de custos. Essa etapa decide
se as fases seguintes precisam mudar de prioridade.

## 2. Contexto imutável por configuração de compilação

Preparar uma vez e compartilhar diretórios normalizados, ordem de busca, identidade
do compilador, builtins e definições da linha de comando. Preservar diretório de
trabalho, `-D`/`-U`, `-iquote`, ordem dos `-I`, forced includes, padrão e flags que
alteram o ambiente. Não usar somente o caminho do arquivo como identidade.

Coalescer consultas de builtins e includes de sistema: um produtor por chave,
consumidores aguardam sem segurar o mutex global. Falha ou cancelamento não pode
deixar uma entrada permanentemente em andamento. A chave deve observar mudanças do
toolchain e do ambiente que afetam a busca. Não compartilhar configurações apenas
porque usam o mesmo executável.

Consultas externas precisam de encerramento e prazo controlados pela abstração de
processo suportada pelo projeto. Cancelar um consumidor não deve destruir trabalho
ainda necessário a outros consumidores. Não substituir silenciosamente resultados
reais por fallback para atingir uma meta de tempo.

## 3. Conteúdo, metadados e resolução compartilhados

Introduzir um serviço interno da camada semântica, com ownership explícito e dados
imutáveis, para compartilhar conteúdos e revisões entre builds do ambiente.
Representar dependências por IDs e revisões; materializar caminhos para protocolo e
relatórios. Manter a dependência do core exclusivamente na STL.

Primeiro, reutilizar observações dentro de um único build/snapshot, evitando `stat`
repetido do mesmo header durante a validação de vários efeitos. Depois, compartilhar
entre builds apenas quando houver um contrato de invalidação implementado.

O cache de resolução deve distinguir nome, includer, busca quoted/angled,
`include_next`, posição de busca, configuração e revisão dos caminhos do overlay.
Um resultado negativo também depende dos diretórios pesquisados: criar um header ou
adicionar um arquivo em diretório de maior prioridade deve invalidá-lo. TTL sozinho
não é garantia de correção.

Buffers abertos prevalecem sobre disco. Edição, fechamento, renomeação, mudança de
compilação, eventos de watcher perdidos e overflow exigem invalidação ou revalidação.
O primeiro incremento deve continuar revalidando disco em limites seguros. Só
reduzir essa revalidação após garantir cobertura dos watchers ou um fallback
conservador. Não prometer detectar alterações de conteúdo com tamanho e timestamp
idênticos apenas com metadados; definir verificação de conteúdo nos casos exigidos.

## 4. Cache de efeitos previsível

Substituir `clear()` global por evicção incremental com orçamento em bytes e limite
secundário de entradas. Começar com uma política simples e mensurável; dados em uso
continuam vivos por ownership compartilhado e entram na contabilidade de memória.

Deduplicar a coluna de dependências usada para validação, preservando a ordem das
operações de macros e os efeitos de include guards/pragma once. Validar cada revisão
de arquivo no máximo uma vez por snapshot de análise.

Permitir variantes limitadas por header e contexto de macros lidas, em vez de
substituir continuamente uma única variante. Guardar a assinatura das leituras e
conferi-la integralmente; hash é um filtro, não prova de equivalência. Limitar
variantes e contabilizar custo de busca e memória.

Completude e limites fazem parte do contrato: efeitos construídos com outro limite
de profundidade, macros ou tamanho não podem furar os limites da solicitação atual.
Resultados cancelados, incompletos ou dependentes de consultas não representadas
não podem virar hits completos. Examinar também dependências de `__has_include`.

Não aplicar uma trava de produção por header recursivo: grafos com ciclos podem
produzir deadlock entre workers. Começar compartilhando leituras e consultas ao
compilador; só coalescer builds completos com chave de contexto adequada, estados
explícitos e testes de ciclos, reentrada, falha e cancelamento.

## 5. Reutilização incremental de ambientes

Após conteúdo e invalidação estarem corretos, avaliar cache limitado de ambientes
completos por conteúdo principal, configuração e revisões transitivas. Não ativar
simplesmente `remember=true` para todos os 1176 arquivos: isso pode reter muitos
checkpoints e criar inconsistência entre cache de disco e de documentos abertos.

Uma mudança invalida somente os dependentes conhecidos, usando arestas reversas.
Ausência de prova exige recomputação. Publicar um snapshot novo de forma atômica;
requisições antigas mantêm suas revisões. Não guardar ponteiros de comandos como
identidade persistente ao recarregar `compile_commands.json`.

## 6. Agendamento e responsividade

Priorizar documento aberto e fontes do projeto antes das dependências, com ordem
estável dentro de cada grupo. Manter cobertura completa das dependências; expor
separadamente projeto utilizável e índice completo. Isso melhora disponibilidade,
mas não será contabilizado como redução do trabalho total.

Medir 1/2/4 workers antes de alterar o padrão. Mais threads podem aumentar colisões
de cache e I/O. Limitar trabalho pesado e pressão de memória; evitar que consultas
interativas fiquem atrás de uma fila inteira de indexação. Não depender da ordem
de término dos workers para obter resultados semânticos consistentes.

Propagar o stop token até preparação do ambiente, resolução, scan e esperas.
Manter progresso periódico independente do arquivo que estiver lento. Só anunciar
ready após a confirmação final de completude; cancelamento deve ter estado próprio.
Uma etapa longa deve expor sua duração, além do tempo acumulado.

## 7. Persistência entre inicializações: fase posterior

Considerar somente após medir o ganho dos caches em memória. Persistir artefatos
imutáveis com formato versionado, identidade do toolchain, configuração, conteúdo e
dependências validadas. Usar gravação atômica e orçamento de disco. Cache ausente,
corrompido ou incompatível deve provocar reconstrução segura. Não persistir buffers
não salvos. A implementação inicial pode ficar sem persistência se atingir as metas.

## Testes e critérios de aceitação

Ampliar `MacroEnvironmentSpec` com equivalência entre cache desligado, primeira
execução e replay, observando snapshots por offset, origens, diretivas, diagnósticos,
arquivos e completude. Cobrir alteração transitiva, overlay, include criado/removido,
shadowing por ordem de busca, `include_next`, guards, pragma once, múltiplos contextos,
limites, cancelamento e falhas. Aproveitar os testes existentes de invalidação.

Adicionar testes determinísticos de consultas concorrentes com barreiras e contagem
de produtores; sem asserts de milissegundos em GoogleTest. Exercitar evicção acima
do orçamento sem esvaziamento global e sem descartar dados emprestados. Exercitar
ciclos e consumidores cancelados sem deadlock. Testar toolchain fake para controlar
consultas externas, falhas, timeouts e mudanças de identidade.

No LSP, testar watcher, buffers abertos, recarga da compile DB, shutdown durante
preparação e correspondência dos resultados com execução sem reutilização. O índice
completo deve conter os mesmos símbolos; métricas e ordem de conclusão podem mudar.

Metas iniciais de avaliação, a confirmar após a linha de base repetida:

- Nenhuma regressão semântica e nenhum ganho obtido descartando dependências.
- Uma consulta externa por chave simultânea e revisão válida do toolchain.
- Uma observação de revisão por arquivo por snapshot, sem validações redundantes.
- Nenhum esvaziamento global por atingir capacidade; memória limitada e reportada.
- Alvo de reduzir pelo menos 50% da mediana do preparo aquecido nos três arquivos
  observados e 30% da mediana de inicialização completa no mesmo hardware.
- Alvo de p95/p50 <= 1,3 em execuções controladas do mesmo cenário; investigar
  outliers por etapa antes de flexibilizar a meta. Não misturar frio e aquecido.
- Meta provisória de cancelamento interno <= 250 ms, excluindo apenas operações
  externas explicitamente identificadas; essas também terão timeout e encerramento.
- Requisições interativas durante indexação não devem piorar seu p95 baseline;
  budgets absolutos serão fixados com os benchmarks existentes de latência LSP.

## Sequência de entregas e revisão

| Entrega | Alterações principais | Condição para avançar |
|---|---|---|
| 1 | Métricas, driver LSP, matriz e baseline | Ranking e variação reproduzidos |
| 2 | Contextos e consultas ao compilador coalescidas | Produtor único, falhas e cancelamento testados |
| 3 | Reuso de observações e conteúdo imutável | Equivalência e invalidação comprovadas |
| 4 | Evicção incremental e dependências deduplicadas | Memória limitada e ausência de queda global |
| 5 | Variantes de efeitos e reutilização incremental | Contextos e alterações transitivas corretos |
| 6 | Prioridades, stop tokens e estados de progresso | Editor responsivo e shutdown previsível |
| 7 | Benchmark completo e eventual persistência | Metas avaliadas com relatórios pareados |

Arquivos centrais: `semantic/src/MacroEnvironment.cpp`,
`semantic/include/Heimdall/MacroEnvironment.hpp`, `semantic/src/IncludeIndex.cpp`,
`lsp/Server.cpp`, `lsp/Server.hpp`, `lsp/WorkspaceFiles.cpp`, `lsp/SymbolProtocol.*`,
`vscode-extension/src/extension.ts`, `bench/src/MacroBench.cpp` e os specs e smokes
correspondentes. Novos arquivos entram nas listas explícitas do CMake; novas classes
seguem PascalCase e a política de dependências. Formatar C++ com o Heimdall selecionado.

Cada entrega terá relatório de correção, contagens de trabalho, latência e memória.
Executar specs focados durante desenvolvimento e a suíte aplicável antes de fechar
cada entrega. Validar em Windows e em outro ambiente suportado antes de generalizar
resultados. Manter comparação com cache desligado no harness; não criar opções de
produto temporárias sem necessidade. Não implementar todas as fases de uma vez:
o perfil após cada entrega decide o próximo investimento.
