# Plano: syntax highlight via LSP semantic tokens

## Objetivo

Servir realce de sintaxe pelo `heimdall-lsp` com `textDocument/semanticTokens`, para qualquer
cliente LSP (VS Code, Neovim, Helix...), sem depender de gramática TextMate.

## Camadas

1. **Léxica** (`heimdall_core`): palavras-chave, números, strings, comentários, operadores e
   diretivas de pré-processador, direto do buffer de tokens lossless. Não precisa de compile
   database e funciona em código incompleto.
2. **Sintática**: nomes declarados em `SymbolOutline` (namespace, tipos, funções, campos,
   enumeradores) classificam a declaração e os usos do mesmo nome no arquivo. Identificadores
   seguidos de `(` viram função, e `.`/`->` indicam método/propriedade. Macros vêm de `#define`.
3. **Semântica** (fora desta etapa): resolver usos entre arquivos pelo índice de projeto e
   marcar locais/parâmetros pelo `Binder`.

## Entrega

| Peça | Local |
|------|-------|
| Classificação (STL only, sem exceções/RTTI) | `core/include/Heimdall/SyntaxHighlight.hpp`, `core/src/SyntaxHighlight.cpp` |
| Codificação LSP (delta, UTF-16, quebra de tokens multilinha) | `lsp/SemanticTokens.{hpp,cpp}` |
| Handlers `full` e `range` + capability/legend | `lsp/Server.cpp` |
| Testes | `test/src/SyntaxHighlightSpec.cpp`, `test/src/SemanticTokensSpec.cpp` |

## Decisões

- Legend fixa, na ordem de `HighlightKind`; modificador único `declaration`.
- Tokens multilinha (comentários de bloco, raw strings) são divididos por linha, pois nem todo
  cliente suporta `multilineTokenSupport`.
- Identificadores sem classificação confiável (locais, parâmetros) não são emitidos, para o
  tema/TextMate do cliente continuar valendo. Um nome com declarações de tipos conflitantes no
  mesmo arquivo é tratado como ambíguo e não é classificado pelo nome.
- `semanticTokens/full/delta` fica para depois, caso a medição mostre necessidade.

## Atualização

As macros agora vêm do `MacroEnvironment` (ver `docs/macro-support-plan.md`): o realce reconhece macros definidas em qualquer header, `-D` ou builtin do compilador, e marca código excluído por `#if` com o modificador `inactive`.

A cobertura é total: identificadores em grupos inativos mantêm sua classificação
mais o modificador `inactive` (nomes sem classificação confiável saem como
`variable` só ali, para não deixar buracos), e as linhas das diretivas
condicionais (`#if`/`#else`/`#endif`, ...) nunca são esmaecidas. O servidor também
publica `textDocument/inactiveRegions` (mesmo formato do clangd) a cada
`publishDiagnostics`, com fallback para as faixas do próprio arquivo quando o
ambiente de macros não existe; a extensão aplica uma decoração com opacidade
(`heimdall.dimInactiveCode`, padrão ligado).
