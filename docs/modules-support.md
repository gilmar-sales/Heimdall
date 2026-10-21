# Suporte a C++20 modules

Estado em 2026-10-20. Cobre parser, formatter, destaque de sintaxe, outline, navegação, regras
e análise entre arquivos. O que **não** está coberto está na seção "Limites e próximos passos".

## 1. Diagnóstico inicial (antes deste trabalho)

| Área | Situação |
|------|----------|
| Parser | `module X;` / `import X;` / `export module` reconhecidos por texto, **sem contexto**: `module::T x;` e `module = 1;` no escopo global virariam declaração de módulo. |
| `export` | Só tratado antes de `template`. `export namespace`, `export { ... }`, `export class`, `export int f()` caíam em `Declaration` genérica (sem `NamespaceDefinition`/`RecordDefinition` e, portanto, sem outline, navegação ou semântica). |
| Lexer | `module` e `import` eram palavras-chave fixas (`Tok::KwModule/KwImport`), então `int module = 1;` era destacado como keyword. São *identificadores contextuais* no padrão. |
| Formatter | `export module a:b;` virava `a: b`, `import :p;` virava `import: p`, `module :private;` virava `module: private` (o `:` era tratado como rótulo/herança). |
| Arquivos | O CLI ignorava `.cppm`, `.ixx` e variantes (`unsupported file`). O LSP só conhecia `.cppm`/`.ixx`. |
| Projeto | Nenhuma noção de unidade de módulo, de quem fornece um módulo, de ciclos ou de ordem de build. |

## 2. Modelo

Seguimos [module.unit] / [cpp.module]:

| Forma | `ModuleUnitKind` |
|-------|------------------|
| `export module a.b;` | `Interface` (interface primária) |
| `module a.b;` | `Implementation` |
| `export module a.b:p;` | `PartitionInterface` |
| `module a.b:p;` | `PartitionImplementation` |
| `module;` | fragmento de módulo global (`hasGlobalFragment`) |
| `module :private;` | fragmento privado (`hasPrivateFragment`) |

Imports: `import a.b;`, `import :p;` (partição do módulo corrente), `import <h>;` / `import "h";`
(*header units*), `export import ...;` e imports dentro de `export { ... }` (contam como exportados).

## 3. Arquitetura

```
core/ (STL-only)
  GrammarParser    contexto de module/import, ExportDeclaration (marcador ou bloco)
  ModuleUnit       ExtractModuleUnit(tree) -> ModuleUnitInfo (unidade, imports, problemas)
  RuleEngine       cpp/module-structure (erros de um único arquivo)
  Formatter        dois-pontos de partição, [[attr]] após o nome
  SyntaxHighlight  module/import contextuais
  SymbolOutline    OutlineKind::Module
semantic/
  ModuleGraph      resolução, dependências, ordem de build, ciclos, duplicatas, partições
src/ (CLI)
  ModulePass       passagem de projeto: grafo -> diagnósticos por arquivo
lsp/
  Server           go-to-definition em `import`
```

Decisões:

- **`module`/`import` são identificadores contextuais.** Saíram da tabela `Tok`; o parser decide
  por `IsModuleUnitHead` (o que vem depois: `;`, `:`, identificador, `<`, string). `module::T`,
  `module = 1`, `import(...)` continuam nomes comuns. `export` segue sendo keyword real.
- **`GrammarKind::ExportDeclaration`.** `export { ... }` é um nó com os filhos exportados;
  `export <decl>` gera um nó-marcador de um token seguido da declaração normal como irmã. Assim a
  declaração exportada mantém o seu tipo (`NamespaceDefinition`, `RecordDefinition`, ...) e todo
  consumidor existente (outline, índice, semântica) continua funcionando sem mudança.
- **Problemas de um arquivo vs. problemas entre arquivos.** `ExtractModuleUnit` só olha uma árvore
  e produz `ModuleUnitInfo::problems`; o `ModuleGraph` precisa de vários arquivos. O primeiro vira a
  regra `cpp/module-structure` (sempre ligada); o segundo, regras `project`.
- **Sem exceções/RTTI/terceiros no core**, de acordo com a política de `heimdall_core`.

## 4. Regras

| Código | Severidade | Camada | O que detecta |
|--------|-----------|--------|---------------|
| `cpp/module-structure` | error | syntactic | declaração antes de `export module`; declarações no fragmento global; fragmento global sem módulo; mais de uma declaração de módulo; fragmento privado fora da interface primária; `export` em unidade que não é interface ou dentro do fragmento privado; módulo importando a si mesmo |
| `cpp/module-import-cycle` | error | project | cada `import` que fecha um ciclo (Tarjan) |
| `cpp/module-duplicate-interface` | error | project | duas interfaces primárias do mesmo módulo |
| `cpp/module-partition-export` | warning | project | partição de interface não reexportada pela interface primária |
| `cpp/module-unresolved-import` | error, **opt-in** | project | `import` de módulo que nenhum arquivo analisado fornece (`std` e `std.compat` são externos) |

As regras `project` só rodam em `lint`/`check` e enxergam os arquivos **da mesma execução**. Por
isso `cpp/module-unresolved-import` é opt-in (um arquivo sozinho sempre importaria "módulos
desconhecidos"); ciclo e duplicata são reais mesmo com informação parcial. Cada arquivo usa as
próprias opções de regra (`.heimdall.json` mais próximo, severidade e supressões).

## 5. Recursos de editor (LSP)

- **Outline / `workspace/symbol`:** a declaração do módulo aparece como símbolo `Module` (LSP
  `SymbolKind.Module`), com o nome qualificado `app.core:part`. O índice marca se a unidade é uma
  interface (`SymbolView::exported`).
- **Go to definition em `import`:** `import app.util;` vai para a interface primária no workspace;
  `import :detail;` vai para a partição `app.core:detail`. Implementações (`module app.util;`) não
  são alvo. Header units não são tratados (não há `#include` correspondente).
- **Destaque:** `module`, `import` e `export` só são keyword onde iniciam uma declaração de módulo.
- **Formatter:** `export module a.b:p [[attr]];`, `import :p;`, `module :private;`.

## 6. Arquivos

`.cppm`, `.ixx`, `.mpp`, `.ccm`, `.cxxm`, `.c++m` (`IsModuleUnitExtension`) são fontes no CLI, no
LSP e no glob da extensão VS Code.

## 7. Testes

- `ModuleParseSpec.*`, `ModuleUnitSpec.*` — parser e extração (`test/src/ModuleUnitSpec.cpp`).
- `ModuleGraphSpec.*` — grafo (`test/src/ModuleGraphSpec.cpp`).
- `FormatterSpec.*Module*`, `SyntaxHighlightSpec.*Module*`, `SymbolOutlineSpec.*Module*`,
  `RuleEngineSpec.*Module*`, `WorkspaceSymbolIndexSpec.FindsModuleUnits...`,
  `FileDiscoverySpec.TreatsModuleInterfaceExtensions...`, `WorkspaceFilesSpec.IncludesEveryModule...`.
- `HeimdallCheckModulesCli` (`test/CheckModulesCli.cmake`, fixtures em `test/fixtures/modules`) —
  CLI de ponta a ponta.
- `LspModuleSmoke` (`test/LspModuleSmoke.py`) — outline, definition e `workspace/symbol` via LSP.

## 8. Limites e próximos passos

Ainda **não** feito, de forma deliberada:

1. **LSP sem diagnósticos entre arquivos.** O `ModuleGraph` existe e é reutilizável, mas o servidor
   não o alimenta; ciclos/duplicatas só aparecem no CLI. `cpp/module-structure` já aparece no editor.
2. **Semântica através de módulos.** O Typer e o `ProjectSymbolIndex` continuam tratando unidades de
   módulo como "sintaxe não suportada" (recusam rename/refatoração em vez de arriscar). Visibilidade
   de `export` e símbolos vindos de `import` não entram no binder.
3. **Header units** (`import <vector>;`) são reconhecidos mas não resolvidos para os macros/declarações
   do header; `import std;` não expõe símbolos de `std`.
4. **Compile database.** Flags de módulo (`-fmodules-ts`, `-fmodule-file=`, `-fmodule-mapper=`,
   `/reference`, `-fdeps-format`) não são interpretadas; a ordem de build vem do `ModuleGraph`, não
   de P1689. Um scanner `-fdeps-format=p1689r5` poderia alimentar o grafo.
5. **Completion** de nomes de módulo após `import ` (o `ModuleGraph`/índice já têm os nomes).
6. **Macros em linhas de módulo** (`import MACRO;`, `export module NAME;` expandido pelo
   preprocessador) não são expandidos.
