# Índice semântico de símbolos de projeto

Implementação inicial em `heimdall_analysis`, separada do `ProjectIndex` de exports/navegação aproximada. A API está em `analysis/include/Heimdall/ProjectSymbolIndex.hpp`.

## Uso e escopo

1. Configure a compile database no `Workspace`.
2. Registre buffers abertos com `Open`/`Update`.
3. Chame explicitamente `LoadProjectSources` para importar TUs fechadas e seus includes transitivos. Buffers registrados têm prioridade sobre o disco, inclusive headers ainda não salvos.
4. Capture um snapshot e consulte `snapshot.SymbolIndex()`.

```cpp
const auto imported = workspace.LoadProjectSources();
// Verifique imported.error() e imported->unavailable antes de prosseguir.
const auto snapshot = workspace.Snapshot();
const auto indexed = snapshot.SymbolIndex();
if (indexed)
{
    const auto& index = **indexed;
    for (const auto entity : index.Named("api::calculate"))
    {
        const auto& symbol = index.Entities()[entity];
        for (const auto occurrence : symbol.occurrences)
        {
            const auto& location = index.Occurrences()[occurrence];
            // location.document, offset/length, role e resolution.
        }
    }
}
```

O importador lê os arquivos antes de publicar uma nova revisão. Cancelamento, limites ou uma mudança concorrente do workspace não publicam uma importação parcial. Arquivos indisponíveis são reportados; o índice registra os gaps correspondentes. Os arquivos importados permanecem fixados no workspace até `Update`/`Close`: a API **não é um watcher do disco**. Includes de sistema só são carregados quando seus diretórios estão explicitamente disponíveis no comando; não há descoberta implícita do SDK nessa API.

## Identidades e ocorrências

- Identidades de funções livres usam namespace, tipos escalares builtin dos parâmetros e configuração. Nomes de parâmetros e defaults não fazem parte da identidade. Sinônimos comuns como `signed int`/`int` são normalizados.
- Declarações e definições compatíveis em header/source compartilham uma entidade. Tipos de retorno conflitantes e definições duplicadas são reportados, não reconciliados por heurística.
- Símbolos externos simples, namespaces, entidades internas por arquivo e locais por posição escrita são distinguídos. Parâmetros de protótipos são declarações próprias, não o parâmetro de uma definição futura.
- Chaves são strings delimitadas por comprimento, com comparação de igualdade completa. Não são hashes de IDs locais do Binder.
- `EntityId` e `SymbolId` pertencem à instância que os emitiu. `EntityFor(document, symbol)` traduz um ID local do **mesmo snapshot**; não compare IDs densos entre revisões.
- `At(document, offset)` consulta ocorrências por posição de byte. Adaptadores LSP continuam responsáveis pela conversão UTF-16.
- Lookup considera apenas declarações visíveis no arquivo e seus includes, respeitando ordem de inclusão e shadowing de namespaces. Um nome único em um arquivo não incluído não é uma referência resolvida.

Cada ocorrência registra `Resolved`, `Unresolved`, `Ambiguous`, `Dependent` ou `Unsupported`, e um papel de declaração, definição, referência ou chamada. Candidatos ambíguos são preservados sem atribuir uma entidade resolvida. O índice não reutiliza a escolha aproximada do Binder para certificar uma chamada a overloads de mesma aridade.

## Cobertura e limites conhecidos

`Complete()` cobre somente os documentos/configurações registrados. Comandos da compile database sem TU carregada, includes ausentes, variantes não analisadas, erros de parse e sintaxe não modelada são gaps explícitos. `HasCompleteCoverageFor(entity)` é um **pré-requisito**, não uma prova de preservação semântica de uma transformação: colisões de nomes, efeitos, lifetime, API/ABI e aplicação de edições ainda exigem validação própria.

O primeiro subconjunto canônico de funções usa parâmetros escalares builtin, sem aliases, ajuste cv/ref, variádicos, retorno deduzido ou `noexcept`. Definições dentro de blocos de namespace são suportadas; definições qualificadas fora do bloco (`int ns::f(...)`) ainda não são certificadas. Classes/membros, herança/overrides, templates, lambdas, using-declarations, módulos e linkage C não são certificados. Calls com argumentos de tipos desconhecidos/de classe não recebem prova de ADL. Overload sets são indexados com identidades separadas, mas chamadas ambíguas continuam bloqueadas.

Diretivas além de includes e macros predefinidas marcam cobertura de pré-processamento incompleta, inclusive include guards. Includes dentro de namespaces/funções/classes e entidades internas de headers precisam de modelagem por TU. Um header é analisado com uma configuração registrada; variantes não são fingidas como um único modelo completo.

O índice é lazy, imutável e sincronizado por snapshot. Atualizações de conteúdo, opções, dependências e versões geram um novo cache; snapshots antigos preservam seus dados. Builds cancelados ou acima dos limites não retornam um índice parcial. `Metrics()` expõe contagem/tempo de construção, e `Memory()` inclui uma estimativa de seu armazenamento.

## Integração com refatorações

O índice e o carregamento de projeto estão disponíveis na biblioteca. Esta entrega **não habilita rename global no LSP**, não muda o índice de navegação existente e não adiciona um aplicador de arquivos fechados. Antes de usar essas ocorrências para editar um projeto, são necessários validação de planos multi-arquivo, cobertura por configuração, pré-condições de conteúdo dos arquivos fechados e rebind/validação da transformação.

`test/src/ProjectSymbolIndexSpec.cpp` cobre identidade header/source, overloads, visibilidade, linkage interno, shadowing, buffers sujos transitivos, importação de arquivos fechados, configuração, conflitos, determinismo, concorrência, cancelamento e limites.

Validação em 2026-10-08: build com GCC no Windows e **971/971 testes CTest passaram**, incluindo os 17 testes novos do índice. Os testes de política arquitetural também passaram; core/semantic continuam sem depender da camada analysis.
