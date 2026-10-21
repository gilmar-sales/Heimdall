# Ferramentas de análise do Heimdall

## Comparação de tokens com clangd

`bench/TokenComparison.py` compara os tokens semânticos dos dois servidores LSP
para os mesmos arquivos e banco de compilação. Requer Python 3, clangd e o
`heimdall-lsp` compilado; usa somente a biblioteca padrão do Python.

```powershell
python bench/TokenComparison.py core/src/Lexer.cpp --heimdall build/lsp/heimdall-lsp.exe --clangd clangd --compile-commands build/compile_commands.json --output tokens.json
```

Aceita vários arquivos, `--root` para a raiz do workspace e `--timeout` em segundos
por requisição (padrão: 30). O JSON usa posições LSP com linha e coluna baseadas
em zero, em unidades UTF-16, e inclui o texto de cada token divergente.
As legendas próprias dos servidores são decodificadas por nome; os modificadores
são ordenados antes da comparação. `missing` indica token apenas no clangd,
`extra` apenas no Heimdall e `classification` indica tipos ou modificadores distintos
no mesmo intervalo. Extensões diferentes aparecem como um token ausente e um extra.

O código de saída é 0 para igualdade, 1 para diferenças e 2 para falha de execução.
Diferenças representam divergências de realce semântico, não necessariamente bugs:
o clangd não publica todos os tokens léxicos, e os servidores têm taxonomias próprias.
Não compara o lexer bruto. O relatório preserva essas diferenças sem equivalências
implícitas. O banco deve ter o nome `compile_commands.json`, como exigido pelo clangd.

O relatório versão 2 registra hashes dos executáveis, fontes e banco de compilação,
comandos dos servidores, `serverInfo`, legendas, diagnósticos versionados e snapshots
completos dos tokens. As métricas distinguem cobertura de intervalos, concordância
de tipos, diferenças apenas de modificadores e extras lexicais.

A coleta exige diagnósticos da versão aberta. No Heimdall, aguarda também completion
completa, para sincronizar a indexação dos headers. No clangd, a publicação de
diagnósticos versionados e a resposta de tokens completos formam a barreira; completion
pode continuar incompleta por truncamento de sugestões mesmo com a AST pronta.
Ausência de diagnósticos é estado desconhecido e leva a timeout, não a uma conclusão
de fonte limpa. Diagnósticos publicados com erros ficam no relatório para triagem.

As flags do banco precisam ser aceitas pelo servidor de referência. Flags de módulos
específicas do GCC podem exigir um banco separado de comparação. O comparador não
altera automaticamente as opções de compilação e não pressupõe bibliotecas padrão
equivalentes entre os toolchains.

```powershell
python test/TokenComparisonSpec.py
python test/TokenParitySmoke.py --heimdall build/lsp/heimdall-lsp.exe --clangd clangd
```

O smoke test normal do CTest não exige clangd. Para habilitar o diferencial opcional,
configure o build existente com `-DHEIMDALL_TEST_CLANGD_TOKENS=ON`; o teste chama-se
`TokenClangdDifferential`. O corpus está em `test/fixtures/token_parity/`.

## Triagem de erros

`group_heimdall_errors.py` usa somente a biblioteca padrão do Python 3. Aceita um
ou mais arrays JSON produzidos por `heimdall lint --json`, inclusive arquivos já
filtrados para erros. Ignora warnings e remove erros repetidos entre entradas.

```bash
python3 tools/group_heimdall_errors.py \
  /tmp/opencode/frigga-heimdall-20261013/build-errors.json \
  /tmp/opencode/frigga-heimdall-20261013/semantic.json \
  /tmp/opencode/frigga-heimdall-20261013/assimp-fbx-parser.json \
  --root /home/gilmar/dev/Frigga \
  --output /tmp/opencode/frigga-heimdall-20261013/new-triage
```

O diretório de saída deve ser novo: resultados existentes não são sobrescritos.

- `summary.md`: grupos em ordem decrescente de ocorrências, hipóteses,
  componentes e exemplos com linha de código.
- `groups.json`: todas as ocorrências, IDs estáveis dos grupos, mensagens
  normalizadas, contagens e status de leitura das fontes.
- `--examples N`: quantidade máxima de exemplos por grupo (padrão: 5).
- `--no-source`: não ler fontes; a classificação fica restrita ao diagnóstico.

As categorias são heurísticas de investigação, não confirmação de defeito.
O script não executa o Heimdall nem modifica fontes. Lê os arquivos de origem
referenciados no JSON para contextualizar erros; use `--no-source` caso não queira
essa leitura. As linhas atuais podem divergir da versão analisada anteriormente.
Arquivos sem fonte disponível continuam no relatório. `build/other` significa
apenas caminho em build fora de `_deps`, não comprova código gerado.

O JSON completo é carregado em memória: prefira `build-errors.json` em vez do
baseline com mais de um milhão de diagnósticos quando warnings não forem úteis.
Erros de aridade são agrupados independentemente do nome do callee e das
contagens, mas os diagnósticos originais ficam preservados no JSON detalhado.

Testes isolados:

```bash
python3 -m unittest discover -s tools -p 'test_group_heimdall_errors.py' -v
```
