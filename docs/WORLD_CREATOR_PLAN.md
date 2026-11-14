# Quantum Engine - Plano de Plugin System + World Creator

Objetivo: dar à engine um **sistema de plugins C++ real** (módulos dinâmicos, manifest, ciclo de
vida, dependências, compatibilidade de versão, settings, logs, registro de extensões no editor) e
usar esse sistema para entregar o primeiro plugin oficial: **Quantum World Creator**, um gerador
procedural de terrenos e mundos que escreve na cena através do `TerrainComponent` que **já roda**.

Formato do documento: estado atual com evidência (`file:line`), decisões arquiteturais com as
alternativas recusadas, o modelo de dados do plugin, e o plano em fases com critérios de aceite e
verificação headless.

> Este plano nasce da especificação de 12 etapas pedida pelo usuário (anexada na conversa) e das
> quatro imagens de referência (World Creator / Gaea). A spec pede "não apenas uma demo visual";
> tudo abaixo é medido por **executável que roda e imprime algo são**, porque aqui não há unit
> tests tradicionais (`ctest` é vazio).

---

## 0. Estado atual (evidência)

| assunto | onde está hoje | fato relevante para o plano |
|---|---|---|
| carregar DLL | `editor/Conector.cpp:70-100` (`LoadLibrary`/`dlopen`, `GetProcAddress`/`dlsym`), `:121-178` (copia para nome único antes de carregar) | **existe um loader**, mas é específico de scripts de projeto e só conhece 3 símbolos fixos (`initScripts`, `cleanupScripts`, `cleanupEntityScripts` em `:215-231`). Não há manifest, nem registry, nem ciclo de vida. |
| engine compartilhada | `CMakeLists.txt:72` força `set(QUANTUM_SHARED ON)`; `engine/CMakeLists.txt:1030-1037` monta `add_library(quantum ${QUANTUM_LIB_TYPE})`; exports em `engine/core/Export.h:7-18` (`QUANTUM_API`) | um plugin DLL pode linkar `quantum` e herdar a mesma ABI — é exatamente o que a lib de projeto gerada faz (`editor/Generator.cpp:1066` define `QUANTUM_SHARED QUANTUM_EDITOR JPH_SHARED_LIBRARY`) |
| alvo MODULE | **não existe** nenhum `add_library(... MODULE)` no repo | todo alvo compartilhado hoje é a engine, a lib de projeto e o APK Android |
| checagem de compat | `editor/Generator.cpp:890-911` (`getEditorPluginAbiCheck`) rejeita toolchain (MSVC vs MinGW) e bitness | o repo **já** valida ABI antes de carregar; o plugin system deve reusar essa regra, não reinventar |
| versão | `editor/EditorVersion.h.in:4` (`QUANTUM_EDITOR_VERSION`) gerado de git-describe em `CMakeLists.txt:5-34`; **não existe** constante de versão da engine | `minEngineVersion` do manifest precisa de uma versão nova e própria: `QUANTUM_ENGINE_VERSION` + `QUANTUM_PLUGIN_ABI_VERSION` |
| JSON | `engine/libs/json/json.hpp`, include exposto por `engine/CMakeLists.txt:761` | manifest em JSON **dentro da engine** é possível sem nova dependência |
| YAML | `libs/yaml-cpp` só entra no build do editor (`CMakeLists.txt:520`, linkado em `:851`) | engine **não** lê YAML; settings de plugin na engine são JSON |
| logging | `engine/core/Log.h:11-19` (`print/verbose/debug/warn/error`), editor `editor/Out.h:14-107` com sink em `<config>/editor.log` (`editor/App.cpp:4151-4154`) | o plugin loga por `Log::`; o editor mostra o buffer próprio do manager |
| exceções | `_HAS_EXCEPTIONS=0` vem PUBLIC de `engine/libs/joltphysics/CMakeLists.txt:547`; o engine se desliga disso em `engine/CMakeLists.txt:781-782`; `editor/Conector.cpp:46-52` **captura** exceção atravessando a fronteira | a fronteira plugin↔engine não pode lançar; `noexcept` nas bordas e erros por valor/string |
| ImGui | compilado dentro de cada executável do editor (`CMakeLists.txt:533-565`), sem alvo próprio; `quantum_imgui` é `INTERFACE` (`:564`); engine não linka ImGui | **um plugin DLL não pode desenhar UI própria** — duas cópias de ImGui = dois contexts. A UI de plugin tem que ser declarativa. |
| terreno | `TerrainComponent` é POD header-only (`engine/core/component/TerrainComponent.h:138-180`); malha por `MeshSystem::createTerrain` (`engine/core/subsystem/MeshSystem.cpp:3148-3240`) a partir de **PNG RED16**; CDLOD de verdade (`:3079-3122`, `RenderSystem::updateTerrain` `:8747-8765`) | o gerador escreve heightmap + blendmap e configura o componente; **nada de malha nova** |
| splatting | 3 blendmaps × 3 canais = até 9 camadas (`TerrainComponent.h:21-24`), tudo num `texture2DArray` (`engine/shaderlib/includes/terrain_fs.glsl:4`); triplanar por inclinação **existe** (`:25-26`, `:87-102`); blend por altura **existe** (`:29`, `:156`, `:277`) | o que **não** existe: *seleção automática* de camada por slope/altitude — hoje só o canal pintado escolhe |
| ruído | `engine/core/util/Noise.{h,cpp}` tem Perlin/Simplex/Value + fBm + turbulence, 16 octaves, determinístico por seed inteira (`Noise.cpp:12-58`), bound em Lua (`UtilClassesLua.cpp:58-80`) — mas **nenhum call site C++ na engine** | o World Creator é o primeiro consumidor C++ real; o editor tem um ruído *value-only* separado (`editor/util/TerrainNoise.h`) |
| água | `WaterSystem` registrado (`engine/core/Scene.cpp:38`); nível = Y do transform (`WaterComponent.h:34-35`); `bakeFloor` amostra o terreno (`WaterSystem.cpp:223-259`) via `MeshSystem::sampleTerrainSurface` (`:3421-3474`) | lagos/rios nascem de altitude acumulada + um `Water` por bacia; a integração com o terreno **já existe** |
| vegetação | foliage espalhada por `MeshSystem::updateFoliageLayer` (`:3567`) / `createFoliageEntity` (`:3302`), com `seed` e densidade (`TerrainComponent.h:119-136`), cap 8192 instâncias/chunk (`:3243-3245`) | distribuição procedural tem caminho pronto e **instanciado**; não criar mil entidades |
| limites | `MAX_TERRAINGRID = 16` → `rootGridSize ≤ 4` (`TerrainComponent.h:7`, guard `MeshSystem.cpp:6332-6340`); índices **uint16** sem guarda (`:2808-2814`) → `resolution ≲ 252`; 9 camadas; `HybridArray` cresce no editor e não cresce no game (`engine/core/util/HybridArray.h:15-73`) | "mundo grande" = vários `TerrainComponent`s, e isso hoje **não tem grade de mundo compartilhada** |
| anterior de "instalar extensão" | `editor/cli/ComponentLibrary.cpp:241-294` (descoberta), `:48-85` (metadado raspado de comentário), `:296-470` (cópia com overwrite guard) | é o formato que um *install* de plugin deve imitar — mas com manifest real em vez de comentário |

**Não existe** no repo: plugin manager, plugin interface, manifest de plugin, registry de extensões,
multi-chunk de terreno, ruído em GLSL, seleção automática de camada de material.

---

## 1. Decisões arquiteturais

### D1 - Plugin é uma DLL/SO de verdade, com fronteira C de três símbolos
`extern "C"`: `QuantumPluginAbi() -> unsigned`, `QuantumPluginCreate() -> void*` (um `IPlugin*`),
`QuantumPluginDestroy(void*)`. O manager checa o número de ABI **antes** de tocar no ponteiro.

*Por quê:* é a única forma de satisfazer "carregar/descarregar dinamicamente" sem virar simulação. O
repo já faz isso com DLLs de projeto (`editor/Conector.cpp`), já linka a engine como `quantum.dll`
(`CMakeLists.txt:72`) e já recusa toolchain/bitness incompatíveis (`editor/Generator.cpp:890-911`) —
a fronteira C++ com vtable é aceitável **porque** as duas pontas são compiladas pelo mesmo
compilador, mesmo `/MD`, mesma engine compartilhada. O macro `QUANTUM_PLUGIN_ENTRY(Classe)` esconde a
boilerplate.

*Recusado:* (a) plugins estáticos registrados em código do editor — não cumpre "instalar plugin
externo"; (b) fronteira 100% C com structs de tamanho fixo e buffers `char[N]` — mais pura, mas
obrigaria o plugin a reimplementar acesso à engine sem C++, e a engine já expõe C++ para os scripts
de projeto. Registrar como risco R1 a exigência de toolchain idêntico.

### D2 - Manifest em JSON, porque a engine já tem o parser
`plugins/<Nome>/plugin.json` com `id`, `name`, `description`, `author`, `version`,
`minEngineVersion`, `abiVersion`, `platforms[]`, `type`, `dependencies[]`, `libraries[]`,
`entryPoint`, `permissions[]`, `panel{}`, `settings{}`. Lido com `engine/libs/json/json.hpp`
(include já exposto em `engine/CMakeLists.txt:761`).

*Por quê:* a spec aceita "JSON ou o formato da engine". O formato da engine para dados de projeto é
YAML, mas **yaml-cpp não está linkado na engine** — só no editor (`CMakeLists.txt:520/851`). Um
plugin é carregado pela engine; exigir YAML ali significaria adicionar yaml-cpp ao `quantum.dll` e
mudar a ABI de tudo que linka a engine. JSON é o caminho que não toca em sistema existente.

### D3 - UI de plugin é **declarativa**: o plugin descreve, o editor desenha
O plugin implementa `buildUiSchema(UiSchema&)` devolvendo grupos e campos tipados (slider float/int,
bool, enum, cor, texto, botão, label) e o editor renderiza com o ImGui **dele**. Ação vai de volta ao
plugin por `executeCommand(nome, args, resultado)`.

*Por quê:* ImGui é compilado dentro de cada executável do editor (`CMakeLists.txt:533-565`) e a
engine não o conhece. Um plugin que chame `ImGui::BeginSliderButton` precisaria de uma segunda cópia
de imgui — dois contexts, duas atlases, crash. O schema é a única forma de um plugin de terceiro
ganhar painel sem código no editor, e continua sendo "registro de ferramentas e painéis" como pede a
etapa 2.

*Recusado:* painel do World Creator escrito em `editor/` e registrado pelo manifest. Custa menos e
fica mais bonito, mas cada plugin novo passaria a exigir merge no editor — o oposto de um sistema de
plugins. **Compensação aceita:** a árvore de nodes no estilo Gaea das imagens de referência vira
grupos + seções no schema; o grafo visual de nodes fica como trabalho futuro, não como FASE 1.

### D4 - O World Creator gera **assets de terreno**, não uma malha nova
O gerador produz heightmap PNG (RED16, a mesma codificação de `editor/ai/AiTerrainCapability.cpp:29-38`
e `editor/util/TerrainMapFileWriter.cpp:85-90`), blendmaps e um `FoliageLayer` por bioma, e configura
`Terrain` (`engine/core/object/Terrain.cpp:30/158/176/191/235/255/61/91`).

*Por quê:* o `TerrainComponent` é CDLOD funcional com LOD, culling, splatting de 9 camadas, triplanar
por inclinação, colisão Jolt (`PhysicsSystem.cpp:1443-1507`) e integração com água
(`WaterSystem::bakeFloor`). Um sistema de chunks próprio duplicaria tudo isso e perderia a integração
no primeiro slice. Registrar honestamente o limite: `rootGridSize ≤ 4` e `resolution ≲ 252` por patch
(uint16 sem guarda, `MeshSystem.cpp:2808-2814`) — mundo maior exige vários terrenos, e a grade de
mundo compartilhada é a FASE 7, não uma promessa da FASE 2.

### D5 - O manager mora na engine; o editor só o observa e dirige
`engine/plugin/PluginManager` é o dono do ciclo de vida e do buffer de log. O editor ganha janelas
(`PluginBrowser`, painel de settings por schema) que leem `PluginManager::instance()` — o mesmo
padrão de `App::getTerrainEditWindow()` (`editor/App.cpp:155`, menu em `:637`).

*Por quê:* um plugin precisa funcionar em build exportado e em `quantum-engine.exe` headless, não só
no editor. Colocar o manager no editor tornaria o plugin uma feature de IDE.

### D6 - Nada atravessa a fronteira lançando exceção
Bordas de plugin são `noexcept`; erro vira valor + string no registro. O manager envolve cada callback
em `CrashGuard` (`engine/core/util/CrashGuard.h:70/182`) como o editor já faz com scripts.

*Por quê:* `_HAS_EXCEPTIONS=0` chega PUBLIC do Jolt (`engine/libs/joltphysics/CMakeLists.txt:547`) e
o repo tem histórico de abort por throw em fronteira de task/future.

---

## 2. Modelo de dados do plugin

```
engine/plugin/
  PluginAbi.h          QUANTUM_PLUGIN_ABI_VERSION, QUANTUM_PLUGIN_EXPORT, nome dos símbolos
  PluginVersion.h/.cpp parse/compare de "1.2.3", QUANTUM_ENGINE_VERSION
  PluginManifest.h/.cpp  json.hpp -> struct; validação e erros por campo
  PluginLoader.h/.cpp    open/close/symbol + extensão de plataforma (imita Conector.cpp:70-100)
  PluginUiSchema.h/.cpp  UiControlType, UiField, UiGroup, UiSchema, UiValue, UiSchemaBuilder
  PluginInterface.h/.cpp IPlugin (ciclo de vida, schema, comando) + PluginBase + QUANTUM_PLUGIN_ENTRY
  PluginContext.h        o que o manager entrega ao plugin (versões, settings, log, registrar comando)
  PluginSettings.h/.cpp  chave/valor por plugin, persistido em JSON
  PluginRegistry.h/.cpp  extensões: painéis, comandos, ferramentas; estado e erro por plugin
  PluginManager.h/.cpp   descoberta -> manifest -> dependências -> compat -> load -> init -> activate
```

Estados de um plugin: `Discovered`, `Incompatible`, `Failed`, `Loaded`, `Initialized`, `Active`,
`Disabled`. Falha em qualquer etapa **para** naquele plugin e não impede os outros.

`plugin.json` mínimo:

```json
{
  "id": "quantum.hello",
  "name": "Hello Plugin",
  "version": "1.0.0",
  "minEngineVersion": "0.1.0",
  "abiVersion": 1,
  "platforms": ["windows", "linux", "macos"],
  "type": "engine",
  "dependencies": [],
  "libraries": ["quantum-hello"],
  "entryPoint": "QuantumPluginCreate",
  "permissions": ["log"],
  "panel": { "title": "Hello" },
  "settings": { "greeting": "Hello from a plugin" }
}
```

---

## 3. Fases

### FASE 1 - Infraestrutura de plugins (esta entrega)
- [x] análise da engine (seção 0 acima, com evidência)
- [x] `PluginManager`, `PluginInterface`, `PluginManifest`, `PluginLoader`, `PluginRegistry`, `PluginSettings`, `PluginVersion`, `PluginUiSchema`
- [x] carregamento dinâmico real (`LoadLibrary`/`dlopen`) com checagem de ABI, versão, plataforma e dependências
- [x] plugin de teste `plugins/hello` (painel por schema + comandos + settings)
- [x] plugin de teste `plugins/broken` (ABI 999) para provar a rejeição
- [x] probe headless `quantum-plugin-tests.exe` (doctest, padrão de `tests/quantum_model_tests.cmake`)
- [x] janelas do editor (`PluginBrowser` + settings por schema) — FASE 3

Critério de aceite: o executável roda sem GPU, descobre os dois plugins, **carrega de fato** a DLL do
hello, executa um comando, ativa/desativa, descarrega sem crash, e rejeita o broken com motivo
escrito. MSVC é a compilação autoritativa.

**Verificado em 2026-11-04** (`build/`, MSVC 14.38, `quantum-plugin-tests.exe`): 25 casos, 199 asserções,
`Status: SUCCESS!` em duas execuções seguidas. A DLL do hello mapeia, responde `hello.echo` /
`hello.ping` / `hello.calls`, expõe o schema do painel, descarrega e recarrega; a do broken é recusada
com `plugin abi 999, engine abi 1` antes de tocar na vtable. `quantum.dll` e os dois plugins linkam; o
`quantum-editor` **compila sem erro** (`error C` = 0) mas não foi linkado porque o editor do usuário
estava aberto (PID 19204) — nada foi morto, exceto dois `quantum-engine.exe` de outra execução, presos
numa caixa de abort do CRT desde 07:41 e autorizados para encerramento.

Defeitos que só apareceram em runtime, e que um compile limpo não diria:
o `PluginContext` vivia na pilha de `startInstance()` enquanto o `PluginBase` guarda o endereço que
recebeu em `initialize()` (SIGSEGV no primeiro comando do plugin); `discover()` marcava a própria
pasta como manifest duplicado em cada `loadAll()`, enchendo o log; e três membros do engine
(`PluginManifest::hasPermission/libraryFileName/errorsAsText`) não tinham `QUANTUM_API`, então sumiam
só no link de quem consome a DLL. Nos testes, cada caso precisou limpar seu diretório de settings uma
única vez por execução — limpar a cada `configureBuilt` apagava o arquivo que o caso de reload tinha
acabado de salvar, e o erro parecia ser do parser.

### FASE 1a - Os quatro pontos que faltavam para um plugin ser publicável
- [x] tick: `IPlugin::onUpdate(float dt)` chamado por `Engine::runFrame` no fim da fase variável
      (`Engine.cpp:1300`), só para plugin `Active` cujo manifest nomeia `"tick"`; delta é o capped
      frame delta; custo vai para `addSystemStats("plugin:<id>", ms, 1)`; `unload()` durante o tick é
      recusado em vez de liberar o record em que o loop está pisando
- [x] cena: `PluginContext::scene()` via `Engine::getMainScene()` (null-safe; `getScene()` indexa
      `scenes[0]` sem checar) e exige a permissão `"scene"`
- [x] porta de comandos: `quantum-editor-cmd plugins list|describe|run|enable|disable|check|install`
      (`editor/cli/PluginCommands.cpp`) e `Plugins.*` em Lua (`engine/core/script/binding/PluginClassesLua.cpp`)
- [x] `permissions` aplicadas de verdade: `"log"`, `"scene"`, `"tick"` são gateadas pelo engine via
      `PluginContext::require()` (um aviso por capacidade, não por frame); `"settings"`, `"files"`,
      `"ui"` continuam declarações; nome de permissão desconhecido é reportado no `discover()`

**Verificado em 2026-11-04** (`build/`, MSVC, alvo `quantum-editor-cmd` linkado e `quantum-editor`
compilado sem erro): `quantum-plugin-tests.exe` = 30 casos / 238 asserções / `SUCCESS!`, agora com o
tick real sobre `quantum-hello.dll` e os gates de permissão num contexto in-process. O CLI roda sem
janela: `plugins list` mostra `quantum.broken [incompatible] :: plugin abi 999, engine abi 1` e
`quantum.hello [active]` com os quatro comandos; `describe` imprime a árvore de schema que o editor
desenharia; `run hello.ping repeat=3` devolve a saudação três vezes; `check` de uma pasta sem DLL sai
com código 1. A superfície Lua é provada por um `quantum-engine.exe` real num projeto de verdade
(`tests/plugin/lua-project`, 20 checagens, sai com 0), onde o log mostra
`[TICK] the plugin ticked 3 frame(s)` — ou seja, `runFrame` → `PluginManager::update` → `onUpdate` →
`Plugins.call` atravessou a fronteira do módulo.

O que parecia bloqueio alheio **não é** (corrigido na FASE 2): toda rodada headless de projeto sem
bundle em `assets/shaders` sai com 1 porque `RenderSystem.cpp:11730` encontra variantes ausentes e
dá `exit(1)` — não é work paralelo de `SokolUIRenderer`. Projetos com bundle (`templates/daynight`,
`templates/waterfall`) rodam com 0 e escrevem PNG. A solução do `--screenshot` da FASE 2 foi dar ao
probe um bundle cobrindo as variantes `Ter`, não esperar conserto de ninguém.

### FASE 2 - Terrain core
- [x] `quantum::Noise` ganhou as formas que faltavam: `ridged`/`ridged3` (multifractal `(1-|s|)²`,
      cristas afiadas, `[0,1]`), `warp2`/`warp3` (domínio deslocado por um campo simplex de 3
      oitavas, seeds derivadas por XOR de constantes fixas) e `warpedFbm`/`warpedFbm3`. `fbm` e
      `turbulence` passaram a compartilhar o mesmo laço de oitavas (`OctaveShape`), então um novo
      formato não duplica a soma.
- [x] o gerador e a erosão **mudaram de dono**: `TerrainErosion` e `PngWriter` saíram de
      `editor/util/` para `engine/core/util/` (com `TerrainMapRegion`, que eles assinam na
      assinatura) e `TerrainGenerator.{h,cpp}` é a nova peça do engine. Justificativa técnica: o
      plano exige geração procedural a partir de script, plugin e CLI, e o engine não pode chamar o
      editor. Nada no editor foi reescrito — ele só passou a incluir os headers do engine.
- [x] `TerrainGenerator`: 5 presets como **conjuntos de parâmetros** (não algoritmos separados),
      `TerrainGenParams` como dados puros, `generate` → fBm + ridged misturado + curva de relevo +
      amortecimento sub-maré + máscara de borda de ilha + erosão hidráulica e térmica;
      `encode16` com a mesma quantização do pincel do editor (`lround(clamp01(h) * 65535)`,
      little-endian); `generatePng` escreve o PNG cinza-16 que `TerrainComponent` lê como
      `ColorFormat::RED16` → `SG_PIXELFORMAT_R16`; `waterHeight` devolve o `y` do plano d'água.
- [x] CLI: `quantum-editor-cmd terrain presets|generate|info MAP.png` (`editor/cli/TerrainCommands.cpp`,
      despachado em `editor/main.cpp:81`) — sem projeto, sem janela, sem GPU.
- [x] Lua: `WorldGen.generate/presets/params/waterHeight` (`UtilClassesLua.cpp:269`) como funções
      cruas `int f(lua_State*)`, porque a opção é uma tabela e o binding precisa da VM;
      `Noise.ridged/ridged3/warpedFbm/warpedFbm3` entraram na classe `Noise`.
- [x] probe de cena `tests/worldcreator/probe` (projeto de verdade, cena montada em Lua)
- [x] documentar a superfície nova: o reflector passou a enxergar namespace de funções
      (`tools/reflector/parse_bindings.cpp:160`), então `WorldGen` e `Plugins` saem em `lua_api.md`
      e em `Types.lua`; o capítulo escrito à mão ganhou a seção 23 com um exemplo completo.

**Verificado em 2026-11-04** (`build/`, MSVC; `quantum.dll`, `quantum-editor-cmd.exe`,
`quantum-engine.exe`, `quantum-reflector.exe` linkados):

- `terrain generate --preset island --size 512 --seed 7313 --erosion-seed 99` escreve 524.948 bytes
  e relê idêntico (`terrain info`: min 0,000 / média 0,373 / máx 0,741; 35% do mapa abaixo da linha
  d'água). Duas execuções seguidas com a mesma seed dão o mesmo md5.
- O probe headless fecha **48 checagens, 0 falhas**, `exit=0`, e inclui a travessia de fronteira:
  os bytes que `WorldGen.generate` escreve são **idênticos** aos que o CLI escreveu para a mesma
  seed, e um reroll da mesma seed é idêntico também. Trocar o heightmap de um terreno vivo
  (`setHeightMap` + `createTerrain`) continua construindo a malha.
- O frame capturado (`--screenshot ... --capture-frame 140`) mostra o CDLOD deslocado de verdade:
  ilha com cordilheira, enseadas recortadas na cota da água e o plano do mar no `waterHeight`
  calculado. 126 mil triângulos de média no relatório de frames.

Decisões que a fase forçou e que valem registro:

- **A cena do probe é montada em Lua, não em `.scene`.** Uma cena YAML escrita à mão abortou o
  processo com código 3 (CRT `abort`, sem mensagem) antes de qualquer frame; o caminho Lua é o que
  os templates `waterfall` e `daynight` já provam, e mantém os números do mapa e do terreno no mesmo
  arquivo que os gera.
- **A câmera entra antes do terreno.** Um terreno criado por script sem câmera de cena passa por
  `MeshSystem::resetTerrainToUnbuilt` e limpa `needUpdateTerrain` para sempre.
- **Um projeto headless só escreve PNG se tiver `assets/shaders`.** O parágrafo "bloqueio alheio"
  da FASE 1a estava errado: não é work paralelo — `RenderSystem.cpp:11730` dá `exit(1)` quando o
  bundle do projeto não cobre os variantes usados, e projetos com bundle (`templates/daynight`,
  `templates/waterfall`) rodam com 0. O probe usa o bundle do template waterfall, que já traz as
  variantes `Ter`.

Pendente de FASE 2, assumidamente: o terreno ainda é **sem material** (cor do mesh; blend map e
camadas de detalhe são FASE 4), o mapa é **um só** (sem chunks, FASE 7), `AiTerrainCapability`
continua com seu próprio campo de value noise em vez de chamar `TerrainGenerator`, e o `maxHeight`
real de um preset fica abaixo de 1,0 (a máscara de ilha e a erosão achatam o topo) — a água é
posicionada por `waterHeight`, não por intuição.

A referência da API nova — `Noise` ampliado, `TerrainGenerator`, erosão, o contrato do heightmap, os
verbos do CLI, as funções Lua `WorldGen`/`Noise`, os presets e a lista do que não é suportado —
está em [WORLD_CREATOR_TERRAIN.md](WORLD_CREATOR_TERRAIN.md).

### FASE 3 - World Creator no editor (entregue 2026-11-04)

Duas janelas, um painel guiado por schema e um comando de undo, registrados em todos os pontos que o
editor exige (palette, menu View, dispatch de `AppMenuCommand`, dock, `show()` por frame,
persistência em `settings.yaml`, `QUANTUM_EDITOR_SOURCES`).

| Peça | O que é | Arquivo |
|---|---|---|
| painel de schema | `UiSchema` -> ImGui com os 11 controles; `applyEdits` grava as settings e chama `onSettingChanged` | `editor/window/PluginSchemaPanel.{h,cpp}` |
| Plugins | discover / load / unload / reload / enable / install, o painel acima por plugin, e o log dele | `editor/window/PluginBrowserWindow.{h,cpp}` |
| World Creator | presets, seed, tamanho, erosão, preview em relevo sombreado, grava o heightmap no projeto e aplica no terreno selecionado | `editor/window/WorldCreatorWindow.{h,cpp}` |
| undo | troca de caminho do heightmap, no padrão de `TerrainMapPatchCmd.cpp:69/80`, com `TerrainMapUtils::refresh` | `editor/command/type/WorldGenApplyCmd.{h,cpp}` |
| probe | `quantum-editor-cmd worldcreator selftest` | `editor/cli/WorldCreatorCommands.{h,cpp}` |

Decisões:

- A janela de plugins não recebe `Project*` — plugin é por diretório, não por projeto. World Creator
  recebe, porque escreve em `Project::getTerrainMapsDir()`.
- Registro em `App.cpp` pelo padrão de globais locais da TU (`g_pluginBrowserWindow`,
  `g_worldCreatorWindow`): é o caminho que não encosta em `App.h`, o arquivo mais disputado do editor.
- `WorldCreatorWindow::writeHeightmap` é **static e pública**. A UI fica na janela e a escrita vira um
  serviço que o CLI (e um plugin) pode chamar; o botão Generate chama exatamente essa função, então o
  probe prova o código do botão em vez de uma cópia dele.
- O valor de um campo `Enum` viaja como **texto** da opção (`"blit"`), não como índice. Settings são
  persistidas: um índice gravado apontaria para a opção errada no dia em que o plugin inserir uma
  opção no meio da própria lista.
- O valor de um campo `Color` é o `"#rrggbbaa"` **inteiro**, e a formatação mudou de dono: agora mora
  em `engine/plugin/PluginUiSchema.cpp` (`uiColorToText`/`uiColorFromText`), e o painel delega. Antes
  o builder semeava só o canal vermelho, então um plugin lia `float` até o usuário encostar no swatch
  e `string` depois — dois formatos para o mesmo campo.
- Um `Button` vai apenas para `pressed`, nunca para `edited`: apertar botão não é valor a persistir,
  e antes ele entrava no `settings.yaml` com valor nulo e disparava `onSettingChanged`.

**Verificado em 2026-11-04** (`build/`, MSVC, `quantum-editor` e `quantum-editor-cmd` linkados com
`error C` = 0), sem abrir o editor: `quantum-editor-cmd.exe worldcreator selftest` = **52 checks, 0
falhas**, exit 0. O painel é desenhado num contexto ImGui **sem renderer**, dirigido por eventos de
mouse reais, o que provou de fato:

- os 11 `UiControlType` desenham, e um clique produz exatamente o que cada um deve produzir — editam
  os dois sliders e o checkbox; inputs, combo, cor e texto multilinha apenas focam; botão só pressiona
  (e não vira setting);
- o clique no slider carrega o número para onde a empunhadura foi (`5` num `0..10`), e o do checkbox
  deixa em `field.value == false` + `changed` — é o valor que `applyEdits` grava;
- `applyEdits` -> `PluginSettings` -> arquivo em disco -> recarga: 4 campos editados chegam ao plugin
  por `onSettingChanged` uma vez cada, um id desconhecido e um id vazio são ignorados, e um record sem
  instância grava o arquivo semcallback;
- `writeHeightmap` produz um PNG que o `stbi_load_16` devolve `64x64x1`, com os texéis 16-bit
  exatamente as alturas geradas (`628..53973` dos dois lados), e o mesmo seed regrava os mesmos bytes;
  seed diferente, bytes diferentes; `size` abaixo de `MIN_SIZE` é normalizado (8x8) em vez de falhar;
  os cinco presets escrevem;
- as duas janelas novas abrem e layoutam três frames cada, com o manager real vendo os dois plugins do
  build (`quantum.hello [active]`, `quantum.broken` recusado por ABI).

Três defeitos reais apareceram só porque o probe clicou de verdade, e nenhum compile limpo os diria:
o `Checkbox` recebia `const bool*` (não linkava), o botão era gravado como setting, e o `ButtonBehavior`
só entrega hover à janela da frente — um probe que criasse uma janela por controle veria clique nenhum
e passaria pelos motivos errados.

**Só a GUI prova** (regra 1 de `quantum-safety-rules`; pedido ao usuário ao abrir View -> Plugins e
View -> World Creator): pixels dos dois painéis e do preview sombreado, o dock das duas janelas, o
seletor de alvo (pede uma cena parada com um `Terrain` selecionado) e o undo de um mapa aplicado.
Geração continua no frame do clique — 512 com erosão custa ~0,5 s; o caminho assíncrono é FASE 7.

### FASE 4 - Materiais e shaders
Biblioteca PBR (grass/dirt/sand/rock/cliff/snow/ice/gravel/moss/volcanic), seleção automática de
camada por slope/altitude no `terrain_fs.glsl` (o triplanar e o blend por altura já existem),
detail procedural. **Licenças:** baixar de ambientcg/polyhaven/cgbookcase só com verificação de
licença por arquivo; sem download nada entra no repo. Ver risco R4.

### FASE 5 - Geração de mundo
Montanhas/planícies/vales/lagos/rios com acumulação hidrológica (D8) sobre o heightmap; `Water` por
bacia; o `WaterSystem::bakeFloor` já cuida do acoplamento.

### FASE 6 - Vegetação e biomas
Regras de distribuição sobre `FoliageLayer` (cap 8192 instâncias/chunk), biomas com paleta + vegetação
+ temperatura/umidade simuladas.

### FASE 7 - Performance
Chunk grid com costuras, cache de geração, regeneração parcial, geração assíncrona no
`ThreadPoolManager` (sem throw na fronteira — D6).

### FASE 8 - Integração final
Export (PNG/mesh/seeds/biomas), projeto de demonstração, documentação das APIs novas, testes.

---

## 4. Riscos

- **R1 - toolchain**: um plugin de terceiro compilado com outro compilador/outra run-time quebra em
  tempo de carga, não em tempo de compilação. Mitigação: checar ABI + toolchain + bitness como
  `editor/Generator.cpp:890-911`, e dizer o motivo no registro.
- **R2 - `dlclose` não descarrega de fato**: o próprio repo documenta isso em `Conector.cpp:121-128`.
  Descarga é *best effort*; re-carga usa cópia com nome único, igual ao loader de scripts.
- **R3 - árvore contestada**: 968 caminhos modificados/sem track no checkout atual. Toda mudança aqui
  é aditiva (diretório novo `engine/plugin/`, `plugins/`, `tests/plugin/`) e o único arquivo
  compartilhado tocado é a lista de fontes em `engine/CMakeLists.txt`.
- **R4 - assets**: baixar texturas exige verificar licença por arquivo; o repo já tem precedente de
  fronteira de licença (`templates/voxelworld-main/minecraft-assets-26.3`) e de material default sem
  licença conhecida. Nada é baixado sem o usuário autorizar.
- **R5 - limites de terreno**: prometer "mundo 4096²" sobre o `TerrainComponent` atual é falso
  (uint16, `rootGridSize ≤ 4`). A FASE 2 gera dentro dos limites e a FASE 7 os expande.

## 5. Divisão de trabalho sugerida

| fatia | camada mais barata que resolve | prova |
|---|---|---|
| núcleo de plugin | `engine/plugin/` + `engine/CMakeLists.txt` | `quantum-plugin-tests.exe` |
| gerador procedural | C++ sobre `Noise.cpp` + PNG writer do editor | `quantum-engine.exe --screenshot` |
| splatting automático | `terrain_fs.glsl` + bit de variant | frame capturado antes/depois |
| UI | schema do plugin + `editor/window/PluginWindow` | **não verificável sem o usuário** |
