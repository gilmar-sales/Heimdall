# Plano para manter a equivalência sem executar o compilador

## Conclusão e limite do objetivo

É possível remover a execução do compilador do fluxo do Heimdall, mas há uma distinção importante:

- **Equivalência para cada instalação, SDK e configuração real:** não é possível garantir isso sem consultar alguma fonte que reflita o ambiente concreto. Os diretórios de headers e várias macros são propriedades da instalação do compilador e da plataforma.
- **Equivalência definida por uma configuração capturada:** é viável se o ambiente fornecer esses dados — por exemplo, no `compile_commands.json`, em um arquivo de configuração gerado externamente ou em dados fornecidos pelo usuário. O Heimdall passaria a consumir esses dados, sem executar o compilador.

Assim, o plano é tornar explícitos e reproduzíveis os dados que hoje são descobertos pelo `CompilerProbe`, e medir o que ainda diverge. Não é substituir a execução por uma lista fixa de suposições e chamar isso de equivalência.

## O que precisa ser substituído

Há dois usos principais:

1. **Macros predefinidas.** `MacroEnvironment::Builtins` executa o compilador com `-dM -E` para obter os built-ins. Sem essa consulta, usa `FallbackBuiltins`, que fornece apenas um conjunto pequeno e aproximado. Isso afeta a avaliação de condicionais e, consequentemente, quais macros e headers o Heimdall considera ativos.
2. **Diretórios de headers do sistema.** `IncludeIndex::SystemIncludes` consulta a ordem de busca do compilador. Esses diretórios são usados tanto para resolver includes quanto para classificar headers como pertencentes ao sistema.

O `CompilerProbe` também adiciona timeout, cancelamento, cache e deduplicação de trabalho concorrente. Ao removê-lo, esses mecanismos deixam de ser necessários para subprocessos; caches dos dados de configuração ainda podem ser úteis, desde que tenham invalidação clara.

## Estratégia proposta: configuração de ambiente explícita

A fonte da verdade passa a ser um **manifesto de configuração semântico**, associado a cada configuração de compilação. Ele deve conter:

- macros predefinidas, incluindo seus valores e se são function-like;
- diretórios de include, na ordem de busca;
- diretórios de quote-include, quando aplicável;
- a classificação dos diretórios como quote, include, system ou framework;
- plataforma, arquitetura e variante relevante do SDK;
- padrão C++ e opções que afetam macros ou resolução de headers;
- identidade/versionamento do manifesto e a configuração de compilação a que ele corresponde.

O manifesto pode ser produzido pelo sistema de build ou por uma ferramenta externa de configuração, fora do processo do Heimdall. **O servidor e a CLI não devem invocar nem depender da execução do compilador para produzi-lo.** A geração externa pode usar informações conhecidas pelo build; se também for proibido executar o compilador durante a geração, os dados terão de vir do toolchain/SDK instalado ou de configuração explícita, com as limitações correspondentes.

### Etapa 1 — Definir o contrato dos dados

Antes de mudar a implementação:

1. Separar os dados hoje obtidos pelo probe em duas estruturas: **built-ins** e **caminhos de busca**.
2. Definir como cada conjunto se associa a uma entrada do `compile_commands.json`. A associação precisa considerar diretório de trabalho, arquivo, target/configuração, padrão C++, flags relevantes, macros `-D`/`-U` e opções de include.
3. Definir comportamento quando os dados estiverem ausentes, desatualizados ou forem incompatíveis: fallback explícito e diagnóstico, nunca uma alegação silenciosa de equivalência.
4. Documentar que equivalência é relativa à configuração capturada. Uma alteração de SDK, target ou flags invalida o manifesto correspondente.

**Critério de saída:** o formato distingue configurações que não podem compartilhar macros ou diretórios, e a ausência dos dados tem um comportamento determinístico.

### Etapa 2 — Aproveitar o que o projeto já conhece

O `CompileCommand` já contém argumentos e diretório de trabalho. O Heimdall também já interpreta parte das opções de include e macros. A primeira implementação deve reutilizar esses dados:

- `-D` e `-U` continuam sendo aplicados pela lógica existente;
- `-I`, `-iquote`, e formas equivalentes já reconhecidas continuam alimentando as buscas;
- as opções de padrão C++ e os macros explícitos do comando continuam tendo precedência conforme a semântica do preprocessor;
- o manifesto fornece apenas os dados ambientais que não estão representados no comando: principalmente built-ins e diretórios implícitos do sistema.

Não se deve inferir que `-std=` ou a identificação do driver bastam para reconstruir os macros restantes. Eles não descrevem, por exemplo, a versão exata do compilador, target, SDK ou opções implícitas do toolchain.

### Etapa 3 — Implementar carregamento e resolução do manifesto

Adicionar uma camada interna que:

1. Carregue o manifesto de maneira determinística, com validação de esquema e versão.
2. Associe a entrada ao comando correto, sem usar apenas o nome do executável.
3. Normalize caminhos considerando o diretório de trabalho do comando, mantendo a ordem original.
4. Rejeite ou sinalize dados incompatíveis e arquivos inexistentes, sem inventar diretórios alternativos.
5. Permita fornecer uma origem explícita pelo projeto, pela configuração do Heimdall ou por um campo documentado do banco de compilação.
6. Não introduza dependência nova no `heimdall_core`; a mudança deve permanecer no módulo semântico/configuração compatível com as dependências atuais.

A chave de cache precisa incluir a identidade do manifesto e a configuração associada, não somente o driver ou o texto do comando. Alterações no conteúdo ou versão do manifesto devem invalidar resultados semânticos derivados.

### Etapa 4 — Substituir os dois consumidores

**Macros**

- Fazer `MacroEnvironment::Builtins` ler a tabela do manifesto.
- Preservar a aplicação posterior de `-D`/`-U`, includes forçados e varredura normal já existentes.
- Manter `FallbackBuiltins` apenas como fallback declarado, não como substituto considerado equivalente.
- Remover a lógica de `RunProbe` e `ParseProbe` quando os testes mostrarem que o manifesto cobre o caso.

**Diretórios de include**

- Substituir `IncludeIndex::SystemIncludes` por consulta ao manifesto.
- Preservar a ordem, pois a ordem de busca altera qual header é encontrado.
- Atualizar os consumidores — resolução de headers, `SpellHeader`, análise de includes e `MacroEnvironment::Builder` — para usar a mesma configuração carregada.
- Evitar que diferentes chamadas recarreguem ou interpretem o manifesto de modos diferentes.

É preferível passar uma configuração resolvida aos componentes semânticos, em vez de manter uma função estática que tenta descobrir dados ambientais por conta própria.

### Etapa 5 — Cobrir formatos e toolchains sem prometer reconstrução impossível

O escopo deve avançar por etapas, com suporte explicitamente declarado:

1. GCC e Clang em configurações de build que forneçam manifestos.
2. Clang-cl e MSVC, incluindo target, arquitetura, SDK do Windows e diretórios de frameworks/SDK.
3. Cross-compilation e toolchains embarcados, onde o host não representa o target.
4. Configurações com módulos, sysroots, SDKs múltiplos ou flags específicas, conforme os dados disponíveis.

Para toolchains sem manifesto, o Heimdall deve reportar que está usando fallback e quais capacidades podem ficar incompletas. Não se deve preencher macros específicas com valores fixos — como versões arbitrárias do MSVC — e apresentar isso como equivalente à configuração real.

### Etapa 6 — Criar uma ferramenta de captura externa, se desejado

Para facilitar adoção, pode existir uma ferramenta auxiliar que gere o manifesto para cada configuração. Ela é opcional e separada do Heimdall:

- pode integrar-se com o sistema de build ou com a configuração do toolchain;
- gera um artefato versionado e passível de cache;
- registra a origem e os parâmetros usados para produzi-lo;
- não é executada automaticamente por CLI/LSP;
- se obtiver dados consultando o compilador, isso precisa ser declarado: o Heimdall deixa de usar o compilador em runtime, mas a captura ainda depende dele.

Se a exigência for **não executar o compilador em nenhuma etapa**, a captura deve usar metadados fornecidos pelo SDK/toolchain ou pelo usuário. Nesse modo, não há garantia de correspondência perfeita quando esses metadados forem incompletos.

### Etapa 7 — Verificar equivalência com testes diferenciais

Antes de apagar o `CompilerProbe`, usá-lo temporariamente como referência em testes e ferramentas de desenvolvimento:

- Para uma matriz de toolchains e configurações, capturar macros e diretórios retornados pelo probe.
- Comparar os dados capturados com o manifesto carregado.
- Construir `MacroEnvironment` com os dois conjuntos e comparar macros visíveis em vários offsets.
- Comparar headers resolvidos, ordem de busca, `#include_next`, headers forçados e classificação de arquivos de sistema.
- Incluir overlays de arquivos abertos, headers ausentes, caminhos relativos, symlinks quando relevantes, cancelamento e configurações concorrentes.

Esse comparador pode ser temporário ou permanecer como ferramenta de validação. **Não deve ser necessário para o funcionamento normal do produto.**

### Etapa 8 — Definir critérios de aceitação

Considerar a remoção segura quando:

- a mesma configuração de build produz um manifesto estável e identificável;
- macros e diretórios usados pelos componentes semânticos vêm de uma única fonte;
- testes diferenciais passam na matriz de plataformas/toolchains suportada;
- ausência ou incompatibilidade de dados gera fallback e diagnóstico verificáveis;
- mudanças em flags, SDK ou manifesto invalidam caches;
- CLI e LSP continuam funcionando sem iniciar subprocessos do compilador;
- testes cobrem resolução de includes e condicionais, além dos testes atuais do probe.

### Etapa 9 — Remover o mecanismo antigo

Só depois da migração e validação:

- remover `CompilerProbe.hpp/.cpp`, o registro correspondente em `semantic/CMakeLists.txt` e os testes específicos de subprocesso;
- substituir esses testes por testes de carregamento, associação, invalidação e uso do manifesto;
- remover referências a `probeCompiler` se a opção deixar de fazer sentido, ou redefini-la com nome e documentação claros caso permaneça um modo de fallback;
- atualizar a documentação do projeto e do `compile_commands.json` esperado.

## Riscos e decisão recomendada

O maior risco não é a execução de processos: é **dados incompletos ou associados à configuração errada**. Isso pode causar falsos resultados semânticos, como ignorar uma branch de `#if`, resolver um header de outro SDK ou classificar um header de projeto como sistema.

Recomendo primeiro implementar o manifesto e a medição diferencial, mantendo o probe apenas como referência durante a transição. Depois, remover o probe do runtime. Se a exigência incluir não usar o compilador nem mesmo para gerar os manifestos, a equivalência só pode ser garantida para configurações em que as fontes externas forneçam integralmente os built-ins e os caminhos de busca; para as demais, o produto deve assumir e expor comportamento aproximado.
