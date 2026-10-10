# Acompanhamento de recursos C++26

A [tabela de feature-test macros](https://en.cppreference.com/cpp/feature_test) é a
referência para os valores mínimos. `core/include/Heimdall/Cpp26Features.hpp`
expõe uma seleção de recursos C++26 relevantes ao projeto: valor anunciado
(`0` quando a macro não existe), valor mínimo e disponibilidade. Para adicionar
outro recurso, inclua sua macro e seu valor mínimo na lista `Cpp26Features()`.
Macros de biblioteca são consultadas após incluir `<version>`.

Execute `./build/test/Tests_run --gtest_filter='Cpp26FeaturesSpec.*'` para
imprimir a disponibilidade **do compilador que construiu o teste**. Esse
relatório não descreve necessariamente o compilador do código analisado pelo
Heimdall, nem prova que toda a implementação de um recurso está correta.
Alguns recursos, como reflection no GCC, ainda precisam de flags adicionais.

## Reflection (P2996R13 e propostas associadas)

`__cpp_impl_reflection >= 202506L` indica suporte anunciado pelo front-end;
`HEIMDALL_HAS_CPP26_REFLECTION` reflete esse valor **na unidade compilada**.
Não habilita reflection automaticamente. Em GCC 16, a flag experimental
`-freflection` é necessária: o teste comum reporta `0` porque não usa a flag.

Ao configurar com testes, CMake tenta compilar uma sonda com
`-std=c++26 -freflection`, `<meta>`, `^^` e `identifier_of`. Se passar, o alvo
opcional `Cpp26ReflectionProbe` executa testes unitários de nomes, membros,
parâmetros, enumeradores e splice de tipos. Execute
`cmake --build build --target Cpp26ReflectionProbe` e
`ctest --test-dir build -R Cpp26ReflectionSpec --output-on-failure`.
`HEIMDALL_TEST_REFLECTION=OFF` desliga apenas essa sonda; em compiladores sem
suporte, ela não é registrada. A macro não garante que **todas** as operações
dos papers P2996R13, P3096R12, P3293R3, P3394R4 e P3795R2 funcionem; a
sonda testa explicitamente apenas as operações citadas acima. A flag também
não é propagada para os outros alvos do Heimdall.

Para completion de `std::meta::`, o Heimdall indexa o `<meta>` **real da
toolchain** fornecida pelo `compile_commands.json`; não mantém uma lista fixa
de métodos. `-freflection` precisa estar nos argumentos daquele comando.
O lexer e o parser reconhecem `^^` como operador prefixo, e a camada semântica
infere `std::meta::info` para seu resultado, inclusive ao refletir tipos
fundamentais e entidades. Essa inferência não substitui a validação de
operandos pelo compilador.
O parser também reconhece `static_assert` e splices `[: expressão :]` usados
pela sonda. No hover sobre `^^`, o LSP mostra `std::meta::info`, o operando
refletido e a declaração correspondente quando o índice consegue resolvê-la.
O teste `IncludeIndexSpec.MetaHeaderOffersReflectionFunctionsByQualifiedName`
verifica funções de diferentes grupos da API, `access_context::current`,
todos os nomes indexados no cabeçalho, filtragem por prefixo e ausência de
sugestões fora do namespace. Isso valida disponibilidade no completion, não a
semântica de execução de cada função. O teste
depende de uma toolchain que exponha os diretórios de headers do sistema.

Para proteger código que usa `#embed`:

```cpp
#include <Heimdall/Cpp26Features.hpp>

#if HEIMDALL_HAS_CPP26_EMBED && defined(__has_embed)
    #if __has_embed("dados.bin") == __STDC_EMBED_FOUND__
constexpr unsigned char data[] = {
    #embed "dados.bin"
};
    #endif
#endif
```

`__cpp_pp_embed >= 202502L` indica a diretiva C++26; `__has_embed` verifica
o recurso específico e seus parâmetros (`__STDC_EMBED_EMPTY__` indica arquivo
vazio). O teste usa um arquivo binário de quatro bytes, inclusive `0x00` e
`0xFF`, para verificar a inclusão real. Se a ferramenta não oferecer `#embed`,
somente esse teste de execução é ignorado; o relatório continua disponível.
