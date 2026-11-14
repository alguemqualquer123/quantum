# Quantum Plugins & Packages — plano do marketplace

Referência do formato: `WORLD_CREATOR_PLAN.md` (FASE 1 é o núcleo de plugins que já existe).
Este documento cobre o que falta para o sistema de plugins virar um **catálogo instalável** e para um
plugin conseguir **criar janelas e interagir com a engine** como faz Unreal e Unity.

## 0. Estado medido (2026-11-04), não assumido

Existe e está ligado:

- `engine/plugin/` (11 arquivos): `PluginManager` descobre uma pasta por plugin com `plugin.json`,
  resolve a DLL (pasta, `lib/`, ou ao lado de `quantum.dll`), checa plataforma → ABI do manifest →
  ABI do binário (`QuantumPluginAbi`) → `minEngineVersion` → dependências em ponto fixo
  (`PluginManager.cpp:417`, `:445`), monta o schema, ativa, faz tick (`:640`) e descarrega.
- `IPlugin` (`PluginInterface.h:21`): `initialize/shutdown/activate/deactivate/buildUiSchema/
  executeCommand/onUpdate/onSettingChanged`.
- `PluginContext` (`PluginContext.h:35`): `manifest/settings/registry/directory/resourcePath/
  engineVersion/hasPermission/require/scene/log/registerCommand/registerPanel/registerTool`.
- Permissões **cobradas pelo engine**: `log`, `scene`, `tick` (`PluginManifest.cpp:203`).
  **Declaradas sem cobrança**: `settings`, `files`, `ui`.
- CLI `quantum-editor-cmd plugins list|describe|run|enable|disable|check|install`
  (`PluginCommands.cpp:431`), Lua `Plugins.list|commands|state|describe|isEnabled|setEnabled|call`
  (`PluginClassesLua.cpp:167`), janela `PluginBrowserWindow` com master/detail e settings por schema,
  `quantum-plugin-tests.exe` (30 casos).
- O editor **já tem** um assets browser: `ResourcesWindow` ("Resources") com scan assíncrono,
  thumbnails, filtro por nome, grade/árvore, recortar/copiar/colar, import externo, delete com undo.

Não existe (grep confirmado, zero ocorrências): índice remoto, formato de pacote, checksum,
uninstall, update, "outdated", progress, diálogo de import de pacote, janela própria de plugin, item
de menu de plugin, consumo de `registerTool`/`panels()` fora do CLI, API de arquivo para plugin, e
pasta de plugins do projeto. `engine/CMakeLists.txt` não referencia zip/sha/inflate próprio; o único
inflate disponível é `ufbx_inflate` (`libs/ufbx/ufbx.h:5409`, ligado no editor) e o único deflate é
`stbi_zlib_compress` (`engine/libs/stb/stb_image_write.h:895`, já usado por `PngWriter.cpp:135`).

## 1. Como Unreal e Unity fazem, e o que copiamos

| Conceito | Unreal | Unity | Quantum (este plano) |
|---|---|---|---|
| descritor | `.uplugin` (JSON: `FileVersion`, `VersionName`, `FriendlyName`, `Description`, `Category`, `CreatedBy`, `DocsURL`, `MarketplaceURL`, `EnabledByDefault`, `CanContainContent`, `IsBetaVersion`, `Modules[]`, `Plugins[]`) | `package.json` (`name`, `version`, `displayName`, `description`, `unity`, `dependencies`, `editor`/`type`) | `plugin.json` já existe; acrescenta `category`, `keywords`, `docsUrl`, `marketplaceUrl`, `supportUrl`, `icon`, `preview`, `enabledByDefault`, `canContainContent`, `isBetaVersion` |
| onde procura | `Engine/Plugins/**` **e** `<Project>/Plugins/` | `<project>/Packages/` + cache + registry | `<pluginsDir>` (engine-wide) **e** `<project>/Plugins/` (por projeto, vence em conflito de id) |
| origem remota | Fab/Marketplace (download `.zip`) | registries npm-compatíveis + Asset Store | `registries.json` com `[{name,url,scopes[]}]`; `index.json` por registry; **padrão offline é um catálogo no repositório** |
| pacote | `.zip` | `.tgz` (tar+gzip) | **`.dplugin` = ZIP** (stored + deflate), lido com `ufbx_inflate`, escrito com `stbi_zlib_compress` |
| integridade | hash do store | `dist.integrity` (npm) | SHA-256 próprio + CRC-32 por entrada ZIP. **Não é assinatura**: verificação de corrupção, não de autoria |
| instalação | editor escreve em `Plugins/`, pede restart | Package Manager escreve dependências no manifest do projeto | `installed.json` (lock): id → versão, caminho, sha256, registry, conteúdo copiado |
| UI | Plugin Browser: ícone, versão, autor, link, categoria com contagem, busca, checkbox Enabled | Package Manager: escopo (In Project / Registry / Unity Registry), busca, detalhes, Install/Update/Remove | `PackageBrowserWindow` com exatamente esses controles |
| plugin cria janela | tab/spawnable window do Slate | janelas do EditorWindow, `MenuItem` | `registerWindow` + `IPlugin::onDrawWindow(id)` imediato, `registerMenuItem`, `registerTool` consumido |
| plugin registra componente | módulo com `StartupModule` | assembly + `[InitializeOnLoad]` | **NÃO suportado** (seção 5) |

## 2. Decisões

**D1 — o catálogo vive no editor, o runtime só lê pastas instaladas.**
Empacotar, baixar e descompactar é trabalho de ferramenta (Unreal e Unity fazem igual: o build de um
jogador nunca fala com a store). Assim `engine/plugin/` ganha só metadados (`catalog`, `lock`,
`search paths`) e nada de HTTP/zip, e um jogo shipped continua sem dependência de rede. O código de
download reaproveita `editor/ai/HttpClient::downloadToFile` (WinHTTP/libcurl) e o descompactador fica
em `editor/util/PackageZip`.

**D2 — pacote = ZIP com extensão `.dplugin`.**
ZIP porque é o formato do Fab/Unreal e o que qualquer ferramenta sabe abrir; `.tgz` exigiria tar +
gunzip. Leitor aceita *stored* e *deflate* (inflate via `ufbx_inflate`, que já está ligado no editor e
exposto como `ufbx_abi`). Escritor emite *deflate* com `stbi_zlib_compress` tirando o invólucro zlib
(2 bytes de cabeçalho + 4 de adler32 no fim), que é exatamente o stream cru que o método 8 do ZIP
pede, e *stored* quando não compensa. CRC-32 próprio por entrada.

**D3 — registry é um diretório ou uma URL que serve `index.json`.**
```json
{ "registry": "local", "generated": "2026-11-04T00:00:00Z",
  "packages": [ { "id": "com.quantum.worldcreator", "name": "World Creator",
    "category": "Terrain", "keywords": ["terrain","noise"], "author": "SR VINIX",
    "versions": [ { "version": "1.0.0", "file": "packages/com.quantum.worldcreator-1.0.0.dplugin",
      "size": 12345, "sha256": "…", "minEngineVersion": "…", "dependencies": ["com.quantum.noisekit"],
      "permissions": ["scene","ui","files"], "canContainContent": true,
      "description": "…", "docsUrl": "…", "preview": "preview.png" } ],
    "latest": "1.0.0" } ] }
```
É o desenho do packument npm/UPM reduzido ao que a engine usa, e **declaradamente não é
compatível com npm**: endpoint, campos e autenticação são nossos. Um registry local (`file://` ou
caminho absoluto) funciona offline desde o primeiro dia, e o repositório traz
`plugins/catalog/index.json` como o registry embutido — é o que prova o fluxo inteiro sem rede.

**D4 — escopo de resolução.**
`install <id>[@versão]`: procura o id em cada registry (`scopes` mais específico vence, como no UPM),
pega a versão pedida ou `latest`, baixa para `<pluginsDir>/.cache`, confere tamanho + SHA-256,
descompacta, valida o `plugin.json` do pacote com o **`check` que já existe**, e só então move para
`<pluginsDir>/<id>/` e escreve o lock. Uma etapa que falha não toca na instalação anterior (a cópia é
feita em `.staging` e promovida).

**D5 — uninstall apaga o que o pacote copiou e nada mais.**
O lock grava a lista de arquivos de conteúdo instalados no projeto; `uninstall` remove por essa lista
e não por heurística de pasta. Arquivo que mudou de tamanho/hash desde a instalação é reportado e
preservado — perder trabalho do usuário é o pior erro possível aqui.

**D6 — janelas de plugin são imediatas e hospedadas pelo editor.**
O plugin não pode chamar ImGui: o contexto ImGui, os tokens de tema e o loop de draw são do editor, e
um `.dll` de terceiro linkando imgui daria duas cópias do contexto. Então `IPlugin` ganha
`onDrawWindow(const char* windowId)` e recebe um `PluginDrawList`-lite? Não: recebe **o ponteiro do
contexto ImGui é proibido**; em vez disso o editor desenha o que o schema descreve e, para desenho
arbitrário, o plugin devolve *comandos de desenho* (texto, botões, sliders, imagem, retângulo) por um
builder de frame — é o mesmo contrato do schema, aplicado por frame. Isso mantém a ABI 2 pequena e
testável headless (o schema de um frame pode ser impresso no CLI).

**D7 — ABI 2.**
Adicionar métodos virtuais a `IPlugin` muda a vtable, então `QUANTUM_PLUGIN_ABI_VERSION` vai a 2 e os
dois plugins de exemplo são recompilados; a recusa de ABI continua sendo a primeira checagem, com a
mensagem dizendo qual ABI o binário reporta.

## 3. Fases

**P1 — catálogo, pacote, install/uninstall (CLI, headless).**
`engine/plugin/PluginCatalog.{h,cpp}` (parse do index, busca, resolução de versão, comparação de
versão reusing `PluginVersion`), `engine/plugin/PluginLock.{h,cpp}` (`installed.json`),
`engine/core/util/Sha256.{h,cpp}` e `Crc32` no mesmo arquivo, `PluginManager::addSearchDirectory`
para `<project>/Plugins`, `editor/util/PackageZip.{h,cpp}` (read/write/list/extract + verify),
`editor/util/PackageManager.{h,cpp}` (orquestra fetch→verify→extract→promote→lock), verbos
`catalog add|remove|list|refresh`, `search`, `install`, `uninstall`, `outdated`, `update`,
`package <dir> -o <zip>`, `verify <zip>`. Toca em: `engine/CMakeLists.txt`,
`CMakeLists.txt` (sources do editor + cmd), `editor/main.cpp` (dispatch),
`editor/cli/PluginCommands.cpp`.
Probe: `quantum-editor-cmd plugins catalog selftest` num diretório temporário — empacota `hello`,
serve por `file://`, instala, desconferindo o sha (deve falhar), reinstala, desinstala, e o
`installed.json` sobrevive a um novo `PluginManager`.

**P2 — plugin cria janela, menu, tool e usa arquivos.**
ABI 2: `registerWindow(id,title,category,icon,initialSize)`, `IPlugin::onDrawWindow(id, UiFrame&)`,
`registerMenuItem(label, command)`, `files` e `ui` cobrados de verdade (`readFile`/`writeFile`
restritos ao diretório do plugin, com proteção de traversal). Host no editor:
`editor/window/PluginWindowHost.{h,cpp}` desenha as janelas registradas, abre item no menu e
consome `registerTool`. Touchpoints: `AppMenuCommand`, `App.cpp` (draw + menu), `AppSettings`
(visibilidade), dock, as duas árvores de fontes do exe.

**P3 — `PackageBrowserWindow` ("Packages").**
Como o Plugin Browser do Unreal: árvore de categorias com contagem à esquerda, busca, seletor de
escopo (In Project / Installed / cada registry), lista com ícone+versão+autor+estado, painel de
detalhes com preview, dependências, permissões, URLs e os botões Install/Update/Remove/Enable.
Registro nos ~14 pontos de uma janela do editor (padrão provado em FASE 3d) + probe headless de draw.

**P4 — pacotes de conteúdo (o lado "Asset Store").**
`canContainContent` + `content/` no pacote, import para o projeto atual com diálogo de árvore
(marcar/desmarcar pastas), lock rastreando os arquivos copiados, origem "Package: x" no
`ResourcesWindow`, verbos `content install|remove|list`.

**P5 — os plugins que se usam.**
Nativos, sobre API que **já existe** na engine (nada de API inventada): `com.quantum.worldcreator`
(geração + erosão + presets), `com.quantum.materiallibrary` (PBR procedural da FASE 4a),
`com.quantum.noisekit` (texturas de detalhe tileáveis via `quantum::Noise`), `com.quantum.watersheds`
(rios/lagos por acumulação), `com.quantum.biomes` (regras de biome → splat + folhagem),
`com.quantum.foliagescatter` (instanciamento sobre terreno), `com.quantum.meshkit` (geometria
paramétrica como entidades), `com.quantum.minimap` (captura top-down do relevo em PNG),
`com.quantum.heightio` (16-bit import/export **com geometria**, nunca só imagem), `com.quantum.gridnav`
(A* sobre custo de terreno) e dois pacotes de conteúdo. Cada um com `plugin.json`, painel por schema,
uma janela própria e ao menos um comando chamável por CLI e Lua.

**P6 — docs.**
`PLUGIN_MARKETPLACE.md` (formato completo, CLI, Lua, como escrever um plugin com janela), exemplo
andado passo a passo, `lua_api.md`/`lua_types.h` regenerados, fatos espelhados nas quatro árvores de
skills.

## 4. Verificação

Sempre headless, e o editor **não é linkado** enquanto a janela do usuário estiver aberta (LNK1168 é
lock, não erro de código): os alvos `quantum-editor-cmd`, `quantum-plugin-tests`, `quantum-engine` e o
probe de catálogo provam o caminho. Uma renderização é provada com PNG capturado, não com compilação
limpa. O que só a GUI prova (a janela Packages desenhando, a janela de plugin dockando) é declarado
como não verificado e entregue para o usuário olhar.

## 5. O que declaradamente não é suportado

- **Um plugin adicionar um componente C++ serializável.** Os registros do editor, do `Stream`, da
  `Factory`, do `Catalog` e do Lua são listas paralelas mantidas à mão e o ordinal do `ComponentType`
  é máscara de bit; um plugin não tem como acrescentar isso em runtime sem corromper cenas.
  Um plugin registra **comandos, painéis, janelas, tools, itens de menu, scripts Lua e assets** — e é
  assim que Unreal (módulos exigem rebuild) e Unity (pacotes de código C# compilam no domain reload)
  também limitam.
- **Autenticação.** SHA-256 detecta corrupção; não há assinatura de pacote nem cadeia de confiança.
  Instalar de um registry desconhecido é confiar no dono dele — a mesma nota que o UPM faz.
- **Isolamento.** O plugin roda no processo, com o mesmo direito do editor. A permissão é checagem de
  contrato, não sandbox; `isEnforcedPermission` existe para isso ser honesto no log.
- **Hot reload garantido no Windows.** `FreeLibrary` falha se a DLL ainda tem thread rodando; o
  botão Reload já recusa durante o tick e continua sendo melhor effort.
- **Pagamento, conta, avaliação.** Um catálogo é um diretório com `index.json`; loja comercial é outro produto.

## 6. Riscos

1. `ufbx_inflate` é público mas foi escrito para streams de FBX — o selftest de P1 precisa de um ZIP
   real gerado fora (git bash `zip`) para provar o leitor com deflate de terceiro.
2. Promover a pasta instalada sobre uma em uso pela DLL carregada exige unload antes; o unlock do
   `LoadLibrary` no Windows nem sempre é imediato, e o fluxo tem que dizer "reinicie o editor" em vez
   de deixar um erro confuso.
3. Conteúdo copiado para o projeto é arquivo do usuário a partir daí; sem o lock exato, `uninstall`
   vira destruição de dados. Daí a regra de não remover arquivo cujo hash mudou.
4. Concorrência de agentes nesta árvore: `plugins/quantum_plugins.cmake` e `CMakeLists.txt` são
   compartilhados; os plugins novos entram por pastas próprias, um por vez, e o CMake é editado por
   mim em uma passada só.
