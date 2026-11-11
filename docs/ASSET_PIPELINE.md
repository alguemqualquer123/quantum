// (c) SR VINIX
// SPDX-License-Identifier: MIT

# Quantum asset pipeline

Esta primeira fatia adiciona um inventário incremental e headless para projetos.
Ela não importa nem copia conteúdo externo: o diretório local de assets do projeto é
a única fonte analisada. O pacote Mojang em `templates/voxelworld-main` continua fora
do pipeline e não é lido nem derivado por estes comandos.

## Banco de assets

`quantum-editor-cmd assets index --project <dir>` grava o artefato derivado:

```text
<project>/.quantum/assetdb.json
```

O formato é JSON UTF-8, `format: 2`, com uma lista `files` ordenada por caminho.
Leitores continuam aceitando bancos `format: 1`:

```json
{
  "format": 2,
  "engine": "quantum",
  "manifestSha1": "...",
  "files": [
    {
      "path": "assets/models/hero.glb",
      "kind": "model",
      "size": 1234,
      "mtime": 0,
      "sha1": "...",
      "dependencies": ["assets/models/hero.bin", "assets/textures/hero.png"],
      "missing": []
    }
  ]
}
```

O hash do conteúdo só é recalculado quando tamanho ou `mtime` mudam. Dependências
textuais são descobertas em cenas, scripts, materiais, shaders, JSON/YAML e grafos;
glTF/GLB usa as URIs estruturadas de `buffers`/`images`, com decodificação de escapes
percentuais, e FBX usa referências de textura em ASCII ou em cadeias imprimíveis do
contêiner binário. URLs, `data:` URIs e caminhos fora do projeto são ignorados. A
existência delas é reavaliada em cada varredura, para que restaurar um arquivo remova
imediatamente o diagnóstico quebrado. O manifesto não contém data de execução nem
caminho absoluto, portanto duas varreduras sobre os mesmos bytes são reprodutíveis.

Comandos:

```text
quantum-editor-cmd assets index --project <dir> [--force] [--json]
quantum-editor-cmd assets doctor --project <dir> [--json]
quantum-editor-cmd assets verify --project <dir|project.yaml> [--deep] [--json]
quantum-editor-cmd assets import --project <dir|project.yaml> [--assets <dir>] [--force] [--json]
quantum-editor-cmd doctor --project <dir>
```

`assets doctor` retorna `0` quando não há dependências quebradas, `1` quando encontra
assets quebrados e `2` para erro de uso ou projeto inválido.

`assets verify` é uma auditoria somente leitura do banco já persistido. Ele carrega
`.quantum/assetdb.json`, recalcula o SHA-1 de cada arquivo registrado, sinaliza arquivos
removidos ou alterados e verifica a existência de cada dependência registrada. Não executa
`index` implicitamente nem regrava o banco. Retorna `0` quando o projeto coincide com o
índice, `1` quando há divergência e `2` quando o projeto ou o banco não pode ser lido.
Com `--json`, o resultado inclui `records`, `checked`, `removed`, `changed`,
`missingDependencies` e a lista detalhada `issues`.

`--deep` mantém a auditoria somente leitura e adiciona validação estrutural dos formatos
conhecidos: assinaturas e dimensões de PNG/JPEG/BMP/HDR/EXR, GLB e glTF/JSON, RIFF/WAV,
Ogg, FLAC, MP3, fontes SFNT e modelos OBJ/FBX; cenas, materiais e grafos YAML também
são parseados. Falhas aparecem como `invalid-format`, e o JSON informa `deep` e
`deepChecked`. Isso não substitui os importadores completos nem compila shaders.
GLB também passa por validação de comprimento total, alinhamento e limites de chunks,
presença do primeiro chunk JSON e parse do objeto JSON antes de ser aceito.
FBX binário exige o magic completo, versão plausível e tamanho mínimo do cabeçalho;
FBX ASCII exige um cabeçalho numérico `FBXVersion:`.

O índice também persiste metadados estruturais determinísticos para modelos: glTF/GLB
registra contagens de accessors, animations, buffers, images, materials, meshes, nodes,
scenes, skins e textures, além de `primitiveCount`, `morphTargetCount`, `hasSkin`,
`materialTextureCount`, `extensionCount`, `generator` e do marcador `quantumFbxImport`
quando existirem; FBX registra `format`, `encoding` (ASCII/binário) e a versão declarada
quando o cabeçalho a expõe. Derivados FBX também registram `linkedSourceCount`,
`resolvedTextureCount` e `missingTextureCount`, distinguindo dependências externas e
texturas não resolvidas sem reimportar o modelo. Esses campos são apenas diagnóstico e
não substituem a conversão FBX nem o importador de geometria.

Para não acoplar o executável standalone ao editor, o comando `assets index` também
grava `.quantum/asset-metadata.v1`. É um sidecar determinístico e versionado com os
pares `path/chave/valor` em hexadecimal; o `quantum-resource-server` o lê quando
disponível, persiste uma cópia no próprio `resource-index.v1` e expõe `metadata` nas
respostas `index` e `entry`. O sidecar é opcional: o servidor continua funcionando
com hash e dependências quando o projeto ainda não foi indexado pelo CLI.

Durante o export, fontes de visual scripting `.graph` também são lidas, validadas e
compiladas automaticamente para o `.gdat` correspondente no diretório de saída. O YAML
continua disponível como fonte de autoria, mas o runtime/exportado consome o blob DGR1.
Assim, o projeto não depende de o usuário ter clicado em **Graph > Compile** antes de
gerar um build.

`assets import` é o passo headless incremental para modelos FBX. Ele percorre a raiz de
assets configurada pelo projeto (a raiz do projeto por padrão, ou `--assets`), consulta o marcador de dependências e a versão do importador e
grava os `.glb` derivados em `<project>/.quantum/imported/`, preservando a mesma relação
de caminhos usada pelo editor. Arquivos atuais são apenas reportados como `up-to-date`;
`--force` reprocessa todos. O modo `--json` retorna a lista por arquivo, avisos do
importador e erros sem abrir janela ou modificar a fonte FBX. O comando retorna `1`
quando uma conversão falha, para que CI/export possa tratar a falha como gate.

## Resource server

O índice incremental do `quantum-resource-server` agora expõe `dependencies` e
`missing` em `index`/`entry`, e `status` inclui `brokenAssets`. O manifesto do servidor
inclui essas relações além de caminho e SHA-1, então o aparecimento ou desaparecimento
de uma dependência também gera uma mudança observável. O serviço persiste um cache
versionado em `<project>/.quantum/resource-index.v1`: a gravação é atômica, entradas
com cache inválido são descartadas e a varredura sempre revalida tamanho, mtime,
dependências e contenção de caminho antes de reutilizar SHA-1. O cache só acelera o
reinício do servidor e não substitui a árvore do projeto.
Para arquivos `.gltf`, o servidor também resolve URIs estruturadas de `buffers` e
`images`; para `.glb`, ele lê somente o primeiro chunk JSON, sem carregar a malha.

## Pak reprodutível

`quantum-resource-compiler` é um CLI headless de desktop para Windows, Linux e
macOS. O dispatch dos subcomandos ocorre antes da inicialização do backend, então
`shaders`, `pak`, `inspect` e `graph` não abrem a janela do editor.

`quantum-resource-compiler pak` já ordenava as entradas por caminho. A derivação de
`key`/`shift` agora usa FNV-1a de 64 bits sobre caminho e tamanho, em vez de
`std::hash`, cujo resultado é específico da implementação. Assim, o mesmo conjunto
de arquivos produz o mesmo `resources.pak`:

```text
quantum-resource-compiler pak --project <dir> --out <out-a.pak>
quantum-resource-compiler pak --project <dir> --out <out-b.pak>
quantum-resource-compiler inspect --pack <out-a.pak> --verify
```

`inspect --verify` chama a verificação estrutural somente leitura do runtime antes
de listar o pack: magic, contagem, caminhos, duplicatas, limites, sobreposição,
metadados FNV de obfuscação, cobertura completa e leitura dos corpos. Novos packs
usam `DXPK2` e carregam SHA-256 por entrada; a verificação decripta cada corpo e
rejeita alteração arbitrária de bytes. Packs legados `DXPK1` continuam legíveis e
recebem a verificação estrutural disponível, sem digest histórico.

## Próximas fatias

- Adicionar uma etapa incremental de importação/conversão por tipo.
