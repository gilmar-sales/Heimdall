# Triagem de erros do Heimdall

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
