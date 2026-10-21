# Comparação de tokens: análise e plano de implementação

## Execução de 20/10/2026

Arquivo: `core/src/SyntaxHighlight.cpp`, 637 linhas. Exercita classes, enums,
containers STL, templates, lambdas, parâmetros, variáveis locais, membros,
chamadas estáticas e referências a declarações em headers.

- HEAD: `98b9a5aad4274f598c6515c8cf9a9a36d6c7edb4`.
- SHA-256 do fonte: `316cee8fb5c9ccd24225f4f11d9f14d26f6e9be7ac0ff82d6cdb37fd5bd7985a`.
- Heimdall: executável Release existente em `build/lsp/heimdall-lsp.exe`,
  modificado em 19/10/2026 às 22:16 no horário local; não recompilado nesta análise.
- Referência: clangd 22.1.0, plataforma `x86_64-pc-windows-msvc`.
- Relatório original: `build/token-comparison-complex.json`.
- Relatório com banco compatível: `build/token-comparison-complex-compatible.json`.
- Banco de comparação: `build/token-comparison-input/compile_commands.json`.

```powershell
python bench/TokenComparison.py core/src/SyntaxHighlight.cpp --heimdall build/lsp/heimdall-lsp.exe --compile-commands build/token-comparison-input/compile_commands.json --timeout 60 --output build/token-comparison-complex-compatible.json
```

O banco compatível contém a entrada original deste arquivo, removendo apenas
`-fmodules-ts`, `-fmodule-mapper=...` e `-fdeps-format=p1689r5`, rejeitados pelo
clangd. Diretório, compiler driver, includes, PCH textual, defines e padrão C++26
foram preservados. O banco original não foi alterado. Os dois servidores receberam
o mesmo banco em cada execução. Os relatórios original e compatível são idênticos.

O log `build/token-comparison-clangd-compatible-check.log` não contém diagnósticos
de argumentos desconhecidos ou de parsing. Seu resumo ainda contabiliza 17 falhas
do teste de aplicabilidade de `ExtractFunction` sobre `break`/`continue` sem o loop:
não interpretar esse total como 17 erros de compilação. A referência usou os headers
MSVC descobertos pelo clangd, enquanto o projeto é compilado com GCC/MinGW; portanto
esta execução não demonstra equivalência entre as bibliotecas padrão dos toolchains.

## Resultado medido

| Medida | Quantidade |
| --- | ---: |
| Tokens Heimdall | 1.570 |
| Tokens clangd | 1.470 |
| Iguais em intervalo, tipo e modificadores | 236 |
| Mesmo intervalo, mesmo tipo, modificadores distintos | 595 |
| Mesmo intervalo, tipos distintos | 164 |
| Apenas no clangd (`missing`) | 475 |
| Apenas no Heimdall (`extra`) | 575 |

Existem 995 intervalos comuns. Neles, 831 têm o mesmo tipo (83,5%), sem considerar
modificadores. A cobertura de intervalos do clangd é 995/1.470 (67,7%). A igualdade
completa é 236/1.470 (16,1%). São medidas desta execução, não uma nota geral de
correção: a taxonomia e a cobertura lexical dos servidores são diferentes.

Dos 475 ausentes: 249 `variable`, 88 `operator`, 73 `enumMember`, 54 `class` e
11 `enum`. Nem todos representam lacunas confirmadas; operadores e identificadores
da biblioteca precisam de triagem contextual.

Dos 575 extras: 289 `keyword`, 113 `string`, 76 `operator`, 42 `namespace`,
28 `number`, 19 `comment`, 7 `macro` e 1 `property`. Não remover keywords, strings
ou comentários apenas para aproximar contagens. Muitos extras são cobertura lexical
intencional do Heimdall; os 42 namespaces e os operadores exigem inspeção.

Entre os tipos diferentes predominam `type → enum` (81) e `operator → bracket`
(38). Há ainda `keyword → class` (9), `method → enumMember` (8),
`property → variable` (6) e `parameter → property` (5).

## Evidências e causas a investigar

Linhas abaixo usam numeração humana, baseada em um. O JSON usa posições LSP baseadas
em zero e unidades UTF-16.

| Exemplo | Heimdall | clangd | Interpretação |
| --- | --- | --- | --- |
| `token`, linhas 134–135 | ausente | variable | Declaração e referências locais sem realce |
| `i`, linhas 187–189 | ausente | variable | Variável de laço e usos sem realce |
| `HighlightKind`, linha 17 | type | enum | Categoria de tipo externo perdida |
| `Count`, linha 17 | ausente | enumMember | Referência qualificada a enumerador externo |
| `offset`, linha 149 | parameter | property | Nome de membro confundido com parâmetro |
| `kind`, linhas 282 e 287 | parameter | property | Colisão entre nome de parâmetro e membro |
| `Comment`, linha 182 | method | enumMember | Enum usado como argumento classificado como chamada |
| `Extract`, linhas 125 e 623 | method | function | Verificar convenção para métodos estáticos |
| `string_view`, linha 463 | namespace | class | Heurística de nome minúsculo antes de `::` |
| `<`, linha 17 | operator | bracket | Taxonomia de delimitadores de templates |

`core/src/SyntaxHighlight.cpp::ClassifyIdentifier` usa nome textual, o token seguinte
e heurísticas para acesso a membro, tipo e chamada. O mapa de nomes não identifica
cada referência por símbolo e escopo. Isso explica caminhos capazes de produzir as
divergências, mas cada correção deve começar por uma reprodução mínima.

`lsp/Server.cpp::LanguageServer::SemanticTokensRequest` complementa o realce com `Binder::Bind`,
mas aplica correções principalmente a parâmetros e parâmetros de template.
Há uma varredura textual do corpo para nomes iguais aos parâmetros e uma varredura
de todos os tokens para cada nome de parâmetro de template. A primeira exclui o
designador `.membro =`, mas pode sobrescrever outros acessos a membros e sombras.

A legenda em `lsp/SemanticTokens.cpp` só anuncia `declaration` e `inactive`.
`HighlightSpan::modifiers` é `uint8_t`. O modelo já possui `SymbolKind`, referências
resolvidas e flags como `Static` e `Definition`; deve-se aproveitar esses dados antes
de criar outro resolvedor de nomes.

## Ordem de implementação

### 1. Tornar a medição auditável

Ampliar `bench/TokenComparison.py` e `test/TokenComparisonSpec.py` para registrar
versões/hash dos executáveis, hash dos fontes, comandos, legendas, banco utilizado,
diagnósticos e estado de prontidão do documento. A ausência de publicação de
diagnósticos deve ser registrada como desconhecida, nunca como fonte limpa.

Anunciar uma lista real de tipos/modificadores aceitos pelo cliente em vez de listas
vazias. Sincronizar com a análise da versão aberta antes da coleta, com deadline e
falha explícita. A execução atual pede tokens logo após `didOpen`; a coincidência
entre dois relatórios é evidência de repetibilidade, não prova de prontidão.

Separar métricas de cobertura, divergência de tipo, divergência de modificador e
cobertura lexical. Manter o relatório bruto. Se houver perfis de equivalência,
torná-los explícitos e opcionais; não esconder `struct/class` ou `operator/bracket`.

Aceite: testes de notificações fora de ordem, documentos/versionamentos diferentes,
ausência de diagnósticos, timeout, falha do servidor, negociação de capacidades e
legendas com ordens diferentes. Duas coletas prontas devem ser iguais para fonte e
executáveis fixos. Registrar limitações de toolchain.

### 2. Corrigir colisões de nomes e aplicar classificação por símbolo

Extrair o enriquecimento semântico do handler LSP para uma unidade testável na camada
semântica. O core mantém o realce lexical/sintático e sua dependência exclusiva da STL.
A camada semântica recebe spans e usa `Symbols()`, `Refs()` e os escopos do modelo
para produzir substituições por intervalo/token, sem depender apenas da grafia.

Priorizar os cinco casos `parameter → property`: membros homônimos, acessos com `.`
e `->`, variáveis que sombreiam parâmetros e lambdas com nomes repetidos. Substituir
as varreduras textuais por referências resolvidas; um fallback deve ser conservador
quando o binder não resolver o símbolo. Preservar `inactive` e `declaration` ao
enriquecer spans, em vez de reconstruí-los sempre com modificadores zero.

Aceite: uma reprodução mínima que falhe antes de cada correção, teste de declaração
e uso, sombra em bloco/lambda, membro homônimo e designadores. Nenhum intervalo
duplicado ou sobreposto. Manter o smoke test existente de parâmetros.

### 3. Cobrir variáveis locais e referências a enums

Usar declarações e referências resolvidas para emitir `variable`, `enum` e
`enumMember`. Priorizar locais, variáveis de laço e referências a enums já disponíveis
no modelo. Investigar também parâmetros de template e nomes qualificados.

Os 249 `variable` ausentes incluem símbolos externos; separar os casos locais dos
dependentes de headers antes de definir a meta. Não transformar todo identificador
desconhecido em variável. Corrigir o reconhecimento de chamada que produz
`method` para enumeradores como `Comment` e `Identifier`.

Aceite: fixtures mínimas com laços, structured bindings, lambdas, enums locais e
qualificados. Reduzir as lacunas confirmadas sem aumentar falsos positivos ou
regredir arquivos incompletos.

### 4. Integrar símbolos provenientes de headers

Avaliar o modelo semântico/indexação existente para transportar a identidade e a
categoria de tipos, aliases, enumeradores e membros externos até o enriquecimento.
Se o modelo atual não fornecer esses dados, estender seus registros/consultas antes
de adicionar heurísticas ao highlighter. Cobrir `HighlightKind::Count`, tipos STL,
aliases e membros estáticos; evitar classificar pelo uso de maiúsculas/minúsculas.

Aceite: fixtures com dois headers, nomes iguais em namespaces distintos, alias e
enum externo. Mudanças em header ou compile database devem invalidar o resultado.
Não tornar o clangd uma dependência de runtime do Heimdall.

### 5. Ampliar modificadores e revisar a taxonomia

Definir o contrato antes de alterar a legenda. Implementar primeiro `definition`,
`readonly` e `static`, usando evidência do símbolo/tipo, distinguindo const do objeto
de const do pointee. Depois avaliar escopos e demais extensões do clangd.

Se forem necessários mais de oito bits, ampliar o armazenamento de modificadores e
testar a codificação LSP e compatibilidade da legenda. Evitar copiar extensões como
`usedAsMutableReference` ou `defaultLibrary` sem dados suficientes para sustentá-las.
As 595 diferenças apenas de modificadores não serão necessariamente eliminadas
pelos três primeiros modificadores.

Decidir explicitamente sobre `typeParameter`, `concept`, `bracket`, métodos estáticos
e `struct/class`. Não mudar o contrato de tokens apenas para igualar a saída do clangd.

Aceite: testes de declaração versus definição, const de variável versus pointee,
função estática e instância; conferir a legenda anunciada e os bits emitidos juntos.

### 6. Consolidar o corpus e acompanhar regressões

Criar fontes pequenas determinísticas para cada lacuna confirmada em
`test/src/SyntaxHighlightSpec.cpp`, `test/src/SemanticTokensSpec.cpp` ou specs da nova
unidade semântica, além dos testes LSP necessários. Registrar novos fontes nos
`CMakeLists.txt` explícitos. Formatar C++ alterado com o executável do build selecionado.

Manter este arquivo complexo como caso de integração e acrescentar casos sem STL
para isolar diferenças do ambiente. A comparação real com clangd deve ser opcional
e registrar sua versão; os testes comuns não devem exigir sua instalação.

Aceite final: testes focados e suíte aplicável passando, fixtures sem lacunas
conhecidas dentro do perfil definido e relatório atualizado com justificativa para
divergências intencionais. Preservar os caminhos SoA, fazer passagens lineares e
reutilizar buffers. Medir custo adicional com benchmark pertinente antes de afirmar
ganho de desempenho ou aceitar cache novo.

## Escopo desta entrega

Este documento preserva o baseline e o planejamento original. As seis etapas foram
implementadas; detalhes, contrato e validação estão em
`docs/TOKEN_COMPARISON_IMPLEMENTATION.md`. Os artefatos em `build/` são locais e podem
ser removidos por uma limpeza do build.
