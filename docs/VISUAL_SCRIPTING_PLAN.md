# Quantum Engine - Plano de Scripting Visual (Blueprint)

Objetivo: permitir que lógica de jogo seja criada por um **editor de grafo de nós** dentro do
`quantum-editor`, com a mesma ergonomía básica de Unreal Blueprint (eventos, fluxo, variáveis,
funções, debug visual), mas **executando sobre a máquina de scripting que já existe** na engine
(Lua 5.4 + `FunctionSubscribe` + `ScriptComponent`), sem inventar um segundo runtime paralelo.

Formato do documento: decisões arquiteturais primeiro (com alternativas e por que foram recusadas),
depois o modelo de dados, o catálogo de nós, a UI, e por fim o plano em fases com checkboxes,
arquivos reais do repo, esforço e critérios de aceite.

> **Revisão v2 (pós-merge `suite-port`, commit `297f87f4`).** O checkout atual é a linha upstream
> `7d197c71` mergada com a suíte quantum (UILab/`libs/uilab`, cinco executáveis standalone, hub,
> `engine/slate`, quatro árvores de skills espelhadas). O que mudou para este plano: a árvore de
> shaders vive em `engine/shaderlib/` (não mais `engine/tools/shaderlib/`); o editor é Dear ImGui
> **1.93.0-docking** com o chrome vindo do `libs/uilab` via `editor/UILabHost`; os executáveis
> `quantum-reflector`, `quantum-resource-compiler`, `quantum-build-generator`, `quantum-engine` e
> `quantum-resource-server` já existem como alvos separados (`tools/*/*.cmake`); a geração de
> `engine_api_suggestions.h` ainda é o `generate_api_suggestions.py` (o `quantum-reflector` é o
> substituto pretendido; ver D4 para o estado real dele).

---

## 0. Estado atual (evidência, para não duplicar o que já existe)

| assunto | onde está hoje | fato relevante para o plano |
|---|---|---|
| Lua | `engine/core/script/LuaBinding.cpp:111-119` (cria o estado), `:461-476` (registra classes) | **um único** `lua_State` global; instância por entidade é tabela + `luaL_ref` (`:677-728`) |
| bindings | `engine/core/script/binding/*.cpp` (8 arquivos, ~4.5 mil linhas) | tudo que um nó precisa chamar já está exposto e é parseável por reflection |
| C++ scripts | `engine/core/script/ScriptBase.h:12-23`, `ScriptProperty.h:16-24` (`DPROPERTY` = marcador em comentário) | descoberta é **estática**, feita no editor (`editor/util/ScriptParser.cpp:360-383`) |
| attachment | `engine/core/component/ScriptComponent.h:11-31` (`ScriptType{CPP,LUA}`, `ScriptEntry{path,className,enabled,properties,instance}`) | o grafo deve morar **aqui**, não num componente novo |
| eventos | `engine/core/util/FunctionSubscribe.h:123-360`, macros `REGISTER_*_EVENT` `:31-97`; 29 eventos estáticos em `engine/core/Engine.h` | é o único primitivo de dispatch; o grafo vira **mais um assinante taggeado** |
| ciclo por frame | `engine/core/Engine.cpp:1035-1141`: fixed (≤100 passos) → `onUpdate` → `scene->update` → `onPostUpdate` → `onDraw` → `scene->draw` | scripts **não** rodam por entidade; rodam nos eventos globais |
| hot reload | `editor/Conector.cpp:68-156` (copia a DLL para nome único antes de `LoadLibrary`) | recarregar grafo em Play é possível com o mesmo truque de cache-bust |
| serialização | `editor/Stream.cpp:5318-5400` (encode/decode `ScriptComponent`), `editor/Factory.cpp:1456-1486` (scene vira C++ no build) | **engine não lê YAML**: `libs/yaml-cpp` só aparece no build do editor |
| reflection | `tools/reflector` (executável `quantum-reflector`, `reflect`/`check` + `--json`) e `build/generated/engine_api_suggestions.h` (4504 linhas hoje, gerado pelo `generate_api_suggestions.py`; `EngineAPISymbol{name,kind,detail,parent}`) | é a fonte pronta para gerar o **catálogo de nós**; o reflector ainda não tem paridade com o python (ver D4) |
| crash guard | `engine/core/util/CrashGuard.h:70/182`, handler no editor `editor/Project.cpp:7887-7905` | script que rebenta é removido da assinatura e pausa o Play — o grafo herda isso |
| limites | `engine/core/ecs/Signature.h:12` (`bitset<64>`), 53 componentes registrados (`EntityRegistry.cpp:8-60`) | sobram **11** slots de componente: não gastar um no grafo |
| intenções | `roadmap/IDEAS.md:13-16` e `:82`, `TODO_ENGINE.md:19` ("Sistema de eventos global"), `:186-206` | o plano fecha esses itens; export C++/Lua como fallback já estava previsto |

Não existe **nada** de node graph no repo (nem `libs/imnodes`, nem janela de grafo). Todo o subsistema é novo.

---

## 1. Decisões arquiteturais

### D1 - O grafo é um `ScriptEntry`, não um componente novo
`enum class ScriptType { CPP, LUA, GRAPH }` (`ScriptComponent.h:11-14`). Um grafo é identificado por
`path` (`assets/Scripts/Player.graph`), `className` vira o **nome do grafo** (usado em mensagens e
no event bus) e `properties` continua sendo a lista de variáveis expostas.

*Por quê:* o Inspector (`Properties.cpp:8186-8720`), o undo, o `Stream`, o `Factory`, o drag-and-drop
do Resources e o `updateScriptProperties` (`Project.cpp:5138-5140`) já funcionam em cima de
`ScriptEntry`. Um componente novo custaria um slot do `bitset<64>` e duplicaria esses cinco caminhos.

### D2 - Fonte da verdade em YAML (editor), payload em binário (runtime)
- Autoria: `assets/Scripts/<Name>.graph` (YAML, legível, versionável, diff-ável) — parseado **só no editor**, com yaml-cpp que já está em `libs/`.
- Runtime: o editor **compila** o grafo para um blob binário compacto (`.gdat`) escrito em
  `<projeto>/.quantum/graphs/<Name>.gdat`, carregado por `Data::open` (esquema novo `graph://`,
  espelhando `lua://` em `LuaBinding.cpp:372-404`) e empacotado no `resources.pak` pelo pipeline que
  já existe (`quantum-resource-compiler pak`).
- Builds exportados: `Factory.cpp` embute o mesmo blob como array estático na cena gerada (precedente
  exato: `builtin_model_bananaman.h` e `editor/resources/fonts/*_ttf.h`), então HTML5/Android **não**
  precisam de busca em disco.

*Por quê:* meter yaml-cpp no `quantum.dll` mudaria a ABI/dependências da engine por um ganho pequeno, e
YAML no runtime é caro. Manter o binário como artefato gerado dá paridade editor/build e não obriga a
engine a conhecer o formato de autoria.

*Regra anti-drift:* o YAML é lossy-free para o grafo; o `.gdat` é **derivado** e regenerado a cada
save/build, com um checksum do YAML gravado no cabeçalho. Nunca se edita `.gdat`.

### D3 - Um runtime: interpretador de IR com operadores já ligados
Execução = tree-walking sobre um IR "prepared":

- `GraphAsset` (dados) → `prepare()` → `ExecutableGraph` com:
  - `std::vector<BoundOp>`: cada nó impuro resolve **uma vez** seu alvo (ponteiro para membro de
    componente, ponteiro para método de subsistema, setter de `ScriptProperty`) e guarda um
    `void(const GraphContext&)`;
  - registro de valores: `std::vector<GraphValue>` densely indexed (o "register file"), sem alocação por execução;
  - arestas puras viram **avaliação demandada** (nó puro não tem entrada de execução; ele é chamado
    quando o consumidor pede), exatamente como em Blueprint — isso elimina um mundo de nós "Cast/Move" inúteis.

Nada de resolver propriedades por nome de string no hot path (`ScriptProperty` usa `std::variant` e a
busca hoje é linear por nome — aceitável para Inspector, inaceitável por frame).

*Alternativa recusada:* code-gen direto para Lua e executar via `LuaBinding`. Barato, porém o depurador
visual (breakpoint/pulse/inspeção de pin) passa a depender do Lua VM e o mapeamento nó→linha frágil.
**Mas** a code-gen sobrevive como **export one-way** (D5), não como runtime.

### D4 - Tipos: `GraphValue` = hoje `ScriptPropertyValue` + referências de classe
Base: `ScriptPropertyType` (`ScriptProperty.h:30-41`: Bool, Int, Float, String, Vector2/3/4, Color3/4,
EntityReference). O grafo acrescenta `GraphValueType` para: `Entity`, `Scene`, subsistema (`RenderSystem`,
`PhysicsSystem`, ...), tipos de valor do engine (Rect, Matrix4, Quaternion, AABB, Ray, Angle, Color),
`Texture`/`Material`/`Mesh`/`Sound` (por handle), enum (`EaseType`, `Key`, `ButtonState`, ...), e `Any`
somente em nós de escape (chamada dinâmica, ver D6).

A **tabela de tipos e assinaturas é gerada**: o `quantum-reflector` (standalone, `tools/reflector/`,
buildado por `tools/reflector/quantum_reflector.cmake`) já produz `Class/Method/Property/Event` com
assinatura (`tools/reflector/reflector.h:13-33`) e renderiza JSON (`:88-89`). Adicionar um modo
`--emit node-catalog` que gera `build/generated/engine_node_catalog.h` é o caminho de menor atrito e o
que mantém o catálogo vivo quando a API muda (a CI ainda regenera o header de sugestões pelo
`generate_api_suggestions.py`, root `CMakeLists.txt:409-420`; migrar para o reflector é o alvo).

*Estado do reflector (medido no merge `297f87f4`):* `reflect` completo gera 153 classes / 924 métodos
em ~9 s; `check` contra o header do python ainda difere em 2013+ linhas (**sem paridade**, logo o gate
de CI não está ligado); `SceneManager.h` é pulado com aviso (bug pré-existente de `std::regex`
`error_complexity` em corpo com template, agora guardado em `parse_headers.cpp` para não abortar o
run) — esse fix é pré-requisito para confiar no catálogo em nós de `SceneManager`.

### D5 - Export para Lua/C++ é **one-way e opcional**
`Graph → Lua` (e `Graph → C++`) texto legível, gerado sob demanda pelo botão "Export..." da janela de
grafo, para quem quer converter um grafo em código e abandoná-lo (era o "fallback" de `IDEAS.md:16`).
Não é o caminho de execução, então divergência de comportamento não quebra nada: um teste de paridade
roda o grafo interpretado e o Lua exportado no mesmo template e compara o log (`TODO` virou teste de CI).

### D6 - Chamada dinâmica só em nós "genéricos", nunca em nós comuns
Nó genérico (`Call Dynamic`) recebe `Any` + nome de método; existe para casos de borda e para o
editor de propriedades. Todos os nós normais são tipados e ligados em `prepare()`. Isso mantém o
custo por nó no nível de "uma indirect call", e dá mensagem de erro de tipo **na autoria**, não no Play.

### D7 - Nós latentes (Delay, Async load, Tween) com scheduler próprio
`FunctionSubscribe` não tem noção de "retomar". O runtime mantém um `LatentQueue`:
`{execId, nodeId, resumeTime, ctx}` varrido por um assinante único de `Engine::onUpdate` (e de
`onFixedUpdate` quando o nó pede tick fixo). Um `Delay` grava o estado e **termina a execução atual**
(sem stack pendurada), e o scheduler religa o pin de saída quando vence — mesma semântica observável
do Blueprint, sem coroutines. `ActionSystem` (`ECSClassesLua.cpp:128`, `actionStart/Stop/Pause`)
continua sendo o caminho recomendado para tween/interpolation; o nó de latente só cobre "esperar".

Regras: cancelamento por entidade (se o `ScriptEntry` é desativado/entidade destruída, pendências
morrem), e limite por grafo para não inchar (erro de validação se > 4096 latentes ativos).

### D8 - Threads: main thread, sempre
O grafo só executa em callbacks de engine já na main thread. Nenhuma API de worker é exposta em v1;
um nó "Run on Worker" fica fora de escopo. Justificativa: o `Scene` é protegido por
`sceneListMutex`/`drawSemaphore` (`Engine.cpp:1035-1141`) e adicionar execução off-thread a partir de
um grafo authored por designer é risco alto com ganho baixo.

### D9 - Hot recompile em Play
Salvar o grafo durante Play recompila o `ExecutableGraph` e reinstala assinaturas com a mesma tag
(`Type_address_METHOD`, `FunctionSubscribe.h:26-29`), preservando variáveis de instância (o `instance`
handle do `ScriptEntry`). Estado por entidade sobrevive porque fica no `GraphInstance` do entry, keyed
pelo `Entity`. (Lua tem comportamento equivalente hoje em `LuaBinding::clearLoadedProjectModules`.)

### D10 - Escopo do editor: um grafo por arquivo, abas como as do CodeEditor
A janela entra no contrato de janela do editor (`quantum-editor-dev` skill): `WINDOW_NAME`,
`show/setOpen/isOpen`, `menuToggle` no View menu, `DockBuilderDockWindow`, campo em
`PanelVisibilitySettings` e registro em `QUANTUM_EDITOR_SOURCES`. Abas de grafo seguem
`addNewCodeWindowToDock("###" + path)` (`App.cpp`), i.e. um grafo = uma aba dockável.

### D11 - Canvas de nós: decisão com gate na Fase 0
1. **spike**: vendor de `imnodes` em `libs/imnodes` (precedente de vendor in-tree: `libs/imgui`,
   `libs/nfd`, `libs/yaml-cpp`; MIT, 2 arquivos).
2. **risco conhecido**: ImGui é **1.93.0-docking** com o backend de texturas dinâmicas
   (`RendererHasTextures`, `libs/imgui/backends/imgui_impl_opengl3.cpp:624` usa `pcmd->GetTexID()`);
   imnodes precisa ser a revisão compatível, senão dá conflito em `ImTextureID`/`ImGuiPlatformIO`.
3. **fallback**: canvas próprio (~1,2-1,5 mil linhas) com `ImDrawList`, `Theme::dpi()` e os helpers
   `Widgets::addImage*` — só pan/zoom, rects de nó, bézier e drag-and-drop. Nada de dependência nova.
Custo de decisão: gate com build verde + um canvas vazio mostrando 3 nós ligados. Se o imnodes não
fechar em 1 dia de spike, vai para o canvas próprio (o repo não aceita dependência nova caprichosa).

---

## 2. Modelo de dados / IR

```
engine/core/script/graph/            (diretório NOVO, exclusivo deste trabalho)
  GraphValue.h/.cpp        variante de valor + coerções + hash para pins
  GraphTypes.h             enums: NodeType, PinKind, EdgeKind, ValueType
  Graph.h/.cpp             GraphAsset {nodes, edges, variables, functions, events, meta}
  GraphSchema.h/.cpp       validação de tipos/arestas (chamada na autoria)
  GraphSerializerYaml.h/.cpp  (editor) YAML <-> GraphAsset  [compilado no editor, não na engine]
  GraphCompiler.h/.cpp     GraphAsset -> .gdat (blob determinístico, versionado)
  GraphReader.h/.cpp       .gdat -> GraphAsset (runtime)
  ExecutableGraph.h/.cpp   prepare(): BoundOps + register file + ligação de eventos
  GraphRuntime.h/.cpp      execução, latentes, instâncias por entidade
  NodeCatalog.h/.cpp       catálogo de nós (gerado + nós nativos escritos à mão)
  NodeDefs*.h/.cpp         famílias: Flow, Math, Variables, Components, Subsystems, UI, Events, Debug
  GraphEventBus.h/.cpp     "Dispatch/Listen Custom Event" + o event bus global do TODO_ENGINE.md:19
```

Estrutura lógica (não é ABI, é o conceito serializado):

```
GraphAsset
  version: uint32          # schema, não o arquivo
  name: string
  variables: [] GraphVar {name, displayName, type, default, exposed(bool), replicated=false}
  inputs/outputs: [] GraphPin {id, name, type, default}
  nodes: [] GraphNode {id, kind, title, pin defaults, position(x,y), comment, node-specific data}
  edges: [] GraphEdge {from(node,pin), to(node,pin)}
  entryEvents: [] GraphEventBinding {engine event id / component event / custom event, entryNodeId}
  functions: [] GraphFunction {id, name, pure?, params, locals, entryNode, returnNodes}
```

`.gdat` (runtime) tem layout determinístico e **estável para diff**: tabelas de offsets, sem
ponteiros, enums fixos, strings em pool separado (para que o depurador mostre nomes sem carregar o
YAML). Versionamento: header com `magic "DGR1"`, `schemaVersion`, `sourceChecksum` (sha1 do YAML),
contagem por tabela.

Execução:

```
GraphContext { Scene*, Entity, ScriptEntry*, registers: [] GraphValue, scratch: [] GraphValue,
               callStack: [] Frame, budgetChecked }
GraphRuntime::fire(eventId, args...) -> prepara ctx -> executa a partir do entry node -> anda nos
pinos de execução; nós puros são avaliados sob demanda e memoizados por execução.
```

Orçamento: um nó de execução custa **1 indirect call + leitura de registradores**; um grafo de 100 nós
ativos por frame deve ficar abaixo de ~10 µs. Guarda de execução (`maxNodesPerFire`, default 50k)
impede loop infinito — erro de runtime reportado pelo crash guard existente e a execução aborta, sem
matar o editor.

---

## 3. Catálogo de nós da v1

Categorias e contrapartida real na API do repo (nada de API inventada):

1. **Eventos (entry)**
   - Engine: `Start/Play`, `Update`, `Fixed Update`, `Post Update`, `Draw`, `Pause`, `Resume`,
     `View Loaded/Changed/Destroyed`, `Shutdown` (`Engine.h:421-430`).
   - Input: key down/up (`onKeyDown(int,bool,int)`), char, mouse down/up/scroll/move, enter/leave,
     touch start/end/move/cancel, gamepad connect/button/axis (`Engine.h:431-448`), e nós de estado
     (`Is Key Down`, `Mouse Position`) via bindings de `Input` (`CoreClassesLua.cpp:709-717`).
   - Colisão: `Begin/End Contact 2D` e 3D (`PhysicsSystem.h:105,116,120`; Box2D
     `engine/core/util/Box2DAux.h:43-223`, Jolt `engine/core/util/JoltPhysicsAux.h:108-188`).
   - UI: 12 eventos de `UIComponent`, `Button.onPress/onRelease`, `Panel`, `Scrollbar`, `TextEdit`
     (`ECSClassesLua.cpp:354-456`), `focus` (`UISystem.cpp:1209,2773,2833`).
   - Componente: `Action onStart/onPause/onStop/onStep` (`ECSClassesLua.cpp:301-304`),
     `Action finished` (`ActionSystem.cpp:332,391,422,428`).
   - Custom: `Receive <evento>` (D-GraphEventBus).
2. **Fluxo**: `Sequence` (multi-out), `Branch`, `Switch` (Int/String/Enum/Entity), `ForLoop`,
   `ForLoop with Break`, `While`, `DoOnce`, `Gate`, `Delay (s, use fixed tick)`, `Is Playing`,
   `Try/Finally` simples.
3. **Variáveis**: `Get/Set` (com `Set and Node`), variável por entidade vs. por grafo (estática),
   arrays/map via `GraphValue` composto, `Increment`, constants, e **auto-refactor** ao arrastar
   variável para o canvas (menu `Get/Set/Is Valid`).
4. **Entidade/cena**: `Spawn` (com transform), `Destroy` (com `DestroyComponent` equivalente),
   `Find by Name/Tag`, `Get Scene`, `Set Parent`/hierarchy se existir hoje, `Add/Remove Component`
   (atrasado para a janela segura de mutação), `Has Component`, `Cast to <Script>` (chamar função de
   outro grafo na mesma entidade).
5. **Acesso a componentes/propriedades**: `Get <Component>` + pinos por propriedade (gerados do
   catálogo), `Set <prop>`, `Get World/Local Location/Rotation/Scale` + `Add Movement`, `Move/Rotate
   to` com interpolação via `ActionSystem`.
6. **Subsistemas** (mapeados 1:1 aos bindings): `MeshSystem` (trocar modelo/material — lembrar do
   caminho `builtin://` de `BuiltinModels`), `RenderSystem` (câmera, pós-process, screenshot),
   `PhysicsSystem` (impulso, query ray/overlap), `AudioSystem` (play/stop/volume/3D), `UISystem`
   (criar widget, set text), `ActionSystem` (tween), `SceneManager` (mudar cena), `Engine` estáticos
   (tempo, delta, pause, quit), `BundleManager`/`Data` (carregar asset), `ThreadPoolManager` (só
   contagem/progresso, sem execução off-thread de grafo).
7. **Puros/matemática**: vetores, quatérnions, matriz, `Rect/AABB/OBB/Ray`, ângulo, cor, `lerp`,
   `clamp`, `random` (seed estática), `string` (format/parse), casts explícitos com botão de
   conversão no pino, `Equals/Not/And/Or/Not`, `Make/Break Vector`, `Swizzle`.
8. **Debug**: `Print String` (sai no `OutputWindow` via `Out::info` no editor e `Log::info` na
   engine), `Assert`, `Breakpoint`, `Draw Debug Line/Box` (se `RenderSystem` já tiver debug draw —
   senão fica para a fase de ferramentas).

Regra de crescimento: **tudo que a reflexão enxerga pode virar nó** por um template de
"Method call node" (nó genérico tipado pelo catálogo, com botão "+" que adiciona pins de argumento).
Isto é o que torna o catálogo de Blueprint manejável; escrever nós à mão é a exceção (fluxo,
variáveis, eventos, auto-refactor).

---

## 4. Editor

### 4.1 Janela e integração
- `editor/window/GraphEditorWindow.{h,cpp}` seguindo o contrato de janela do editor
  (`WINDOW_NAME` estático, `show()`/`setOpen`/`isOpen()`, helpers privados de desenho) **e** o
  contrato de janela hospedada do chrome UILab: `drawContent()` separado de `show()`,
  `setHostedWindow(true)` e registro do nome + `openFlag()` como `uilab::HostPanel` via
  `editor/UILabHost` (o passo a passo está na skill `quantum-editor-dev`;
  `Structure`/`Properties`/`ResourcesWindow` são os modelos) + `editor/graph/*` para widgets
  reutilizáveis (palette, pins, minimap).
- Abertura: duplo-clique em `.graph` no Resources (hoje abre `CodeEditor`), botão **"Edit Graph"** no
  Script property do Inspector ao lado do caminho (mesmo lugar onde hoje existe botão de modelo),
  `Ctrl+Shift+G` no menu, e entrada no command palette (`buildPaletteCommands`, `App.cpp:421`).
- Undo/redo: `editor/command/type/GraphEditCmd.{h,cpp}` implementando `Command.h`
  (`execute/undo/mergeWith/affectsStructure`), com **merge de drag** (mover nó = 1 comando,
  arrastar contínuo = 1) e diffs estruturais (add/remove node/edge) guardados como patch binário do
  `GraphAsset` (não snapshot inteiro, senão a pilha de undo come RAM em grafos grandes).
- Persistência de layout/visibilidade: `PanelVisibilitySettings` (`AppSettings.h:14`) +
  `settings.yaml`.
- Registro: `QUANTUM_EDITOR_SOURCES` no `CMakeLists.txt` raiz (lista fixa; esquecer a entrada = janela
  que nunca aparece).

### 4.2 UX mínima (o que faz parecer Blueprint e não um diagrama morto)
- Pan/zoom com minimapa, grade, alinhamento/snap, multi-seleção por box, `Ctrl+D` duplica,
  `Delete` remove com confirmação quando corta arestas.
- Palette com busca fuzzy + categorias + "recentes" (reaproveitar a máquina de fuzzy do
  `CommandPaletteWindow`).
- Criar nó **puxando fio**: soltar um pin de execução num vazio abre o menu filtrado por tipo; soltar
  pin de dados abre "Get/constante/conversão".
- Coerção no pino: botão `+`/`-` para expandir/converter (int→float, vector→make), exatamente como UE.
- Aresta com tipo incompatível: recusado com tooltip explicando o tipo (validação, não "aceita e
  quebra no Play").
- nós de evento **não** podem ser duplicados mais que um por assinatura idêntica (aviso).
- Abas por grafo + "Find References" (`Ctrl+Shift+F`) e busca de nó (`Ctrl+F`).
- Comentários (caixa de texto), agrupamento, cor por categoria (tokens de `Theme::Colors`, nada de
  RGBA fixo — convenção do repo).
- Painel de variáveis (esquerda) com criar/rename/exposed; "Compile" (que é a `prepare()`) com
  diagnostics listados clicáveis; botão "Save & Compile" com atalho `Ctrl+S`.
- Overlay de execução em Play: animação de pulso na aresta, valor nos pins em tooltip ao passar o
  rato, highlight do nó que está a executar.

### 4.3 Depuração (fecha o gap "Debugger Lua" de `TODO_ENGINE.md:186`)
- Breakpoint por nó (clique no margen), "pause on error", step-over por nó de execução, continue.
- Watch: selecionar pin → mostra valor atual; `Inspect Entity`.
- Call stack de funções de grafo; "why is this running?" → mostra a cadeia de eventos.
- Erros: exceção de execução capturada pelo `CrashGuard` já existente, reporting no `OutputWindow`
  com `file:node` (a sessão atual já pausa o Play e loga `Script crash in scene ...`,
  `Project.cpp:7887-7905`) + **nó marcado em vermelho** na janela.

---

## 5. Integração com o pipeline existente (arquivos a tocar)

Engine:
- [ ] `engine/core/component/ScriptComponent.h` - `ScriptType::GRAPH`.
- [ ] `engine/core/script/graph/**` - tudo novo (diretório exclusivo, sem segundo escritor).
- [ ] `engine/CMakeLists.txt` - adicionar `.cpp` em `QUANTUM_SRCS` (lista fixa em `:829`).
- [ ] `engine/core/io/Data.cpp` / esquema `graph://` - abrir blob (espelhar `lua://`,
      `LuaBinding.cpp:372-404`).
- [ ] `engine/core/Engine.h/.cpp` - nada estrutural: apenas usar os `FunctionSubscribe` existentes
      com tags próprias do grafo (`Graph_<entity>_<node>`).

Editor:
- [ ] `editor/Stream.cpp` (`:5318-5400`) - encode/decode de `ScriptEntry` tipo GRAPH (caminho +
      variáveis expostas; o corpo não vai no YAML da cena, vai no asset).
- [ ] `editor/Factory.cpp` (`:1456-1486`) - embutir `.gdat` no projeto gerado.
- [ ] `editor/Generator.cpp` (`:537-700`) - incluir grafs no build do projeto; hook
      `ProjectBuild.cmake` intacto.
- [ ] `editor/window/Properties.cpp` (`drawScriptComponent`, `:8186+`) - botão "Edit Graph", badge de
      compilação, erros inline.
- [ ] `editor/window/ResourcesWindow.cpp` - tipo de ficheiro `.graph`, ícone, duplo-clique abre o
      editor de grafo, drag-and-drop cria nós? (fase de polish).
- [ ] `editor/App.cpp` - membro da janela, `show()`, dock, menu, atalho, `PanelVisibilitySettings`.
- [ ] `editor/command/type/GraphEditCmd.{h,cpp}` - undo.
- [ ] `tools/reflector/**` - modo `--emit node-catalog` (JSON + header); o tool já existe, faltam o
      modo e a paridade com o python.
- [ ] root `CMakeLists.txt` - fontes do editor (`:551-563`), alvo de geração do catálogo (ao lado do
      bloco do `generate_api_suggestions.py`, `:409-420`), e — quando houver paridade — trocar a
      geração para o `quantum-reflector` e ligar o `check` na CI.
- [ ] `editor/cli/ResourceCompilerCli.cpp` - `pak` deve incluir `assets/**/*.gdat` (e `--graph`
      opcional para compilar YAML→gdat headless).
- [ ] `templates/<...>` - um template novo `visualscript-basics` de exemplo.

---

## 6. Plano de implementação por fases

Esforço = dias de engenharia com o repo neste estado (sem testes unitários, ver §8).

### Fase 0 - Fundação e gate de decisão (Esforço BAIXO, 3-4 d)
> **Referência UE5**: `EdGraph`/`K2Node` (modelo) e `UEdGraphNode`
- [ ] RFC curto (2 páginas) com a D11 decidida; spike de render: 3 nós + 2 arestas + pan/zoom no
      canvas escolhido, dentro de uma janela dummy.
- [ ] `GraphTypes.h`, `GraphValue.h` (variant + coerções) **sem** qualquer UI.
- [ ] `GraphCompiler` stub + `.gdat` header (`DGR1`, `schemaVersion`, `sourceChecksum`).
- **Critério de aceite**: build MSVC x64 Debug+Release verde; o executável `quantum-engine` abre um
  `.gdat` de teste impresso por dump (ainda sem executar); nenhum arquivo novo fora de
  `engine/core/script/graph/` e `editor/graph/`.

### Fase 1 - IR, YAML, validador (Esforço MÉDIO, 8-10 d)
- [ ] `GraphAsset`, `GraphSerializerYaml` (round-trip byte-estável: YAML→asset→YAML==entrada, teste
      de golden file).
- [ ] `GraphSchema`/validador: tipo de pino, ciclo em puros, entrada de execução órfã, id
      duplicado; erros com `node:pin`.
- [ ] `GraphCompiler` real + `GraphReader`; rejeitar versão desconhecida com mensagem clara.
- [ ] CLI headless: `quantum-resource-compiler graph --project <p> [--check]` (compila todos os
      `.graph` e reporta; também serve de smoke em CI).
- **Critério de aceite**: um grafo JSON/YAML escrito à mão passa `--check`; 12 casos inválidos dão
  mensagens distintas; round-trip estável.

### Fase 2 - Runtime interpretado (Esforço ALTO, 12-15 d)
- [ ] `ExecutableGraph::prepare()` com `BoundOp` (nada de reflexão por string no hot path).
- [ ] `GraphRuntime::fire` + register file + memoização de puros + guarda de budget.
- [ ] Latentes/Delay (D7) e cancelamento por entidade.
- [ ] Assinatura de eventos Engine (Start/Update/FixedUpdate/Draw/Pause/Resume) e de componente,
      com `removeByTagSubstring` na limpeza de cena (`Engine.cpp:738-767`).
- [ ] Integração `ScriptEntry` (`ScriptType::GRAPH`) no ciclo de vida já existente
      (`initializeLuaScripts`-equivalente: novo `initializeGraphScripts(scene)` chamado dos mesmos
      sítios: `Generator.cpp:740`, `Project.cpp:9376/9457/9702`).
- [ ] Crash guard/mensagens de erro nomeando grafo::nó.
- **Critério de aceite**: template de teste roda 600 frames headless com um grafo que move um cubo,
  faz Delay, loga no `Update` e responde a tecla; custo medido < 10 µs/100 nós; sem vazamento de
  assinaturas ao recarregar cena (contagem antes/depois igual).

### Fase 3 - Editor de grafo utilizável (Esforço ALTO, 15-18 d)
- [ ] `GraphEditorWindow` com contrato de janela do repo (dock, menu, atalhos, settings).
- [ ] Canvas (nós, pins, arestas, seleção, mover, conectar), palette com busca, criação por drag,
      coerção de pino, comentários.
- [ ] `GraphEditCmd` (undo/merge) plugado no `CommandHandle` por cena.
- [ ] Painel de variáveis + Save/Compile + diagnostics.
- [ ] Abrir de Resources/Inspector; título com `*` quando sujo; auto-save do editor respeitando o
      formato (nunca escrever `.gdat` à mão).
- **Critério de aceite**: criar do zero um grafo que controla um personagem (movimento + pulo +
  colisão) sem tocar em código; undo/redo 50 operações sem corromper; reload de projeto preserva
  posições.

### Fase 4 - Variáveis expostas, Inspector e cena (Esforço MÉDIO, 6-8 d)
- [ ] `variables[].exposed` → linhas reais no Inspector (mapeamento `scriptPropertyTypeToRowPropertyType`,
      `Properties.cpp:824-836`) + `EntityReference` picker existente reutilizado (`:4564`).
- [ ] `Stream` (cena) guarda valores por instância; `Factory` gera o mesmo para o build.
- [ ] Copiar/colar grafo entre entidades; "Instance of graph" no Structure.
- **Critério de aceite**: designer altera velocidade no Inspector, Play reflete; valor sobrevive a
  save/load e a export.

### Fase 5 - Catálogo gerado por reflexão (Esforço MÉDIO, 8-10 d)
- [ ] `quantum-reflector --emit node-catalog` → `engine_node_catalog.h/.json` (classes, métodos,
      propriedades, eventos, enums, com assinatura `void(int,float,float)` etc).
- [ ] Nó genérico "Method Call" tipado pelo catálogo; "Get Property"/"Set Property" por componente;
      nós de evento UI/colisão derivados dos `Event` kinds.
- [ ] CI: regenerar o catálogo e failar se drift (mesmo padrão do header de sugestões,
      root `CMakeLists.txt:409-420`).
- **Critério de aceite**: adicionar um método novo a um subsistema + binding → o nó aparece no
  editor **sem** escrever código de nó; ≥ 300 nós disponíveis; busca por "impulse" retorna o de física.

### Fase 6 - Funções, custom events, event bus (Esforço MÉDIO, 8-10 d)
- [ ] `GraphFunction` (pura e impura), `Return`, call node com args/retornos, call stack no debugger,
      prevenção de recursão (profundidade máx).
- [ ] `Dispatch Custom Event` / `Receive Custom Event`, por entidade e global
      (**fecha** `TODO_ENGINE.md:19` "sistema de eventos global").
- [ ] `Cast to Graph` (chamar função de outro grafo na mesma entidade).
- **Critério de aceite**: um grafo "Health" emite `OnDeath` e dois outros grafos ouvem; função pura
  de matemática reutilizada em 5 sítios; profiler mostra custo por função.

### Fase 7 - Build/export/plataformas (Esforço MÉDIO, 6-8 d)
- [ ] `.gdat` no `resources.pak` (verificado com `quantum-resource-compiler inspect`).
- [ ] `Factory` embute blob no projeto gerado (HTML5/Android sem disco).
- [ ] `quantum-engine --project` executa grafos (paridade com Lua, que é o que existe hoje).
- [ ] Export one-way Graph→Lua e Graph→C++ (`IDEAS.md:16`) + teste de paridade de log.
- **Critério de aceite**: `Export` de um projeto com grafo roda standalone no Windows **e** no
  workflow emscripten da CI; export Lua do mesmo grafo produz o mesmo comportamento no harness.

### Fase 8 - Debugger e hot reload (Esforço ALTO, 10-12 d)
- [ ] Breakpoint por nó, pause/step/continue, watch de pin, call stack, "pulse" em Play.
- [ ] Recompilação em Play (D9) preservando estado por entidade; botão "Apply" na janela.
- [ ] Painel "assinaturas ativas" (quantos nós estão ouvindo cada evento, custo por evento/frame).
- **Critério de aceite**: depurar "inimigo não morre" pausando num nó, inspecionando HP, editando o
  grafo e aplicando sem reiniciar o Play.

### Fase 9 - Polish e conteúdo de exemplo (Esforço MÉDIO, 8-10 d)
- [ ] Auto-refactor ao arrastar (Get/Set/Constant), quick-casts nos pins, bookmarks, folding de nó
      composto, template de nó (criar "Dash" em 3 cliques), docs (`docs/visual-scripting.md`), e
      migration do `ScriptParser`-created project wizard.
- [ ] Template novo `templates/visualscript-basics` + **reescrever `charactercontrol-main` em grafo**
      usando o modelo embutido `builtin://BananaMan.glb` (amostra os dois caminhos num demo só).
- [ ] `roadmap/IDEAS.md:13-16` riscado / atualizado.
- **Critério de aceite**: um novato completa "andar, pulo, coletar item" em < 20 min seguindo o doc.

**Total estimado: ~75-95 dias de engenharia (~15-19 semanas com paralelismo parcial).**
MVP utilizável = Fases 0-5 (~45-55 d). Fases 6-9 são o que faz ser "Blueprint" e não "widget demo".

---

## 7. Cortes de escopo explícitos (v1 NÃO tem)

- **Material graph** (outro documento; `IDEAS.md` §4).
- **Behavior trees / state machines** (ficam uma extensão documentada: mesmo IR, nós compostos).
- **Replicação/networking** de grafos (sem `NetworkSystem` nem binding Lua hoje).
- **Execução off-thread** de grafo (D8).
- **Graphs cross-project** (biblioteca de grafos versionada) - só na v2 (`IDEAS.md:82` sugere um
  repositório de Blueprints simples; o template da Fase 9 é o embrião).

---

## 8. Verificação (o repo não tem testes unitários)

Convenção do repo: *compile + run* (`quantum-build-verify`). Para este feature, três níveis:

1. **Syntax-only/local**: build MSVC x64 Debug + Release, MinGW quando aplicável, e o guard
   `QUANTUM_BUILD_DIR` (uma árvore, `build/` por padrão).
2. **Harness headless**: scripts que criam projeto em `%TEMP%`, compilam `.graph` com
   `quantum-resource-compiler graph --check`, e rodam `quantum-engine --project <p> --frames 600`
   comparando o log (padrão já usado no resource server: 17 checks programáticos, sem GUI).
   **Nunca** validar via `quantum-editor.exe` disparado por agente/CI - o auto-save do editor já
   destruiu cena de template; verificação de UI é manual, pelo humano, com projeto em `%TEMP%`.
3. **Golden files**: YAML round-trip e `.gdat` determinístico (mesmo YAML → mesmos bytes), para o
   diff de PR ser legível.

CI: adicionar `graph --check` em todos os templates no workflow `cmake.yml` (falha cedo se um nó do
catálogo mudou de assinatura).

---

## 9. Riscos e mitigações

| risco | impacto | mitigação |
|---|---|---|
| imnodes incompatível com ImGui 1.93.0-docking (novo sistema de texturas) | retrabalho de UI | gate na Fase 0; fallback de canvas próprio orçado; nada de API ImGui além do público |
| interpretador mais lento que Lua para jogos grandes | Play com frame baixo | ops ligados, register file, budget guard, e opção real de "export para Lua/C++" (D5) para hot paths |
| drift YAML (autoria) vs `.gdat` (runtime) | bug que só aparece no build | `.gdat` sempre derivado, checksum no cabeçalho, CI regenera, `graph --check` obrigatório |
| HTML5/Android não têm `dlopen` nem disco | grafo não carrega | blob embutido via `Factory` (Fase 7) |
| assinaturas vazando ao recarregar cena | memória/custo crescente | tag única por (`entity`,`graph`,`node`) + `removeByTagSubstring`, teste antes/depois |
| cap do `bitset<64>` (53 usados) | não dá para adicionar componente | decisão D1: grafo dentro de `ScriptComponent`; auditoria antes de qualquer componente novo |
| escopo crescer (material graph, state machines) | nunca acaba | §7 explícito + MVP nas Fases 0-5 |
| editor de grafo travar com 1000+ nós | UX ruim | culling por viewport, cache de layout, limite de redraw (`didRenderScene`/redraw request do padrão do editor) |
| auto-save do editor sobrescrever `.graph` mal-formado | perda de trabalho | escrita atômica (tmp+rename) + `sourceChecksum` + backup `.graph.bak` no primeiro save da sessão |

---

## 10. Divisão para agentes (regra: 1 diretório = 1 escritor, tarefas curtas)

| ticket | dono/diretório | depende de | entregável |
|---|---|---|---|
| T0 spike canvas | `editor/graph/` | - | decisão D11 + protótipo que renderiza |
| T1 IR+YAML+validador | `engine/core/script/graph/` | - | Fases 0-1 |
| T2 runtime | `engine/core/script/graph/` (mesmo dono do T1) | T1 | Fase 2 |
| T3 catálogo/reflector | `tools/reflector/` | T1 | Fase 5 (paralelo a T2) |
| T4 janela do editor | `editor/window/` + `editor/graph/` | T1 | Fase 3 |
| T5 undo/commands | `editor/command/type/` | T4 | Fase 3 |
| T6 stream/factory/generator | `editor/` raiz (`Stream`, `Factory`, `Generator`) | T2 | Fase 4/7 |
| T7 pak/CLI/export | `editor/cli/` + `tools/resource-compiler/` | T2 | Fase 7 |
| T8 debugger | `editor/window/` + runtime | T2,T4 | Fase 8 |
| T9 template+docs | `templates/visualscript-basics`, `docs/` | T4 | Fase 9 |

Cada ticket precisa caber em ~100 turnos de agente (limite prático ~150): T2 e T4 são os únicos que
devem ser quebrados em dois. Wiring em `editor/App.cpp` e `CMakeLists.txt` é **sempre** feito por um
único escritor por vez (conflito real já observado nesta sessão), e o dono do `editor/window/` não é
o mesmo do `engine/core/script/graph/`.

---

## 11. Apêndice A - exemplo mínimo de `.graph` (YAML de autoria)

```yaml
graph:
  version: 1
  name: Bouncer
  variables:
    - { name: jumpSpeed, type: float, default: 6.5, exposed: true }
    - { name: grounded,  type: bool,  default: false }
  events:
    - { id: update,          node: 1 }
    - { id: beginContact2D,  node: 9 }
  nodes:
    - { id: 1, kind: event.update,          pos: [   0, 160] }
    - { id: 2, kind: flow.branch,           pos: [ 320, 160] }
    - { id: 3, kind: var.get, args: { name: grounded } }            # puro
    - { id: 4, kind: input.isKeyDown, args: { key: SPACE } }        # puro
    - { id: 5, kind: logic.and }                                     # puro
    - { id: 6, kind: math.makeVector2, args: { x: 0.0 } }           # puro
    - { id: 7, kind: var.get, args: { name: jumpSpeed } }           # puro
    - { id: 8, kind: body2d.setProperty, args: { prop: linearVelocity }, pos: [ 620, 160] }
    - { id: 9, kind: event.beginContact2D,  pos: [   0, 360] }
    - { id: 10, kind: var.set, args: { name: grounded, value: true } }
  edges:
    - { from: [1, out],  to: [2, in] }
    - { from: [3, out],  to: [5, a] }
    - { from: [4, out],  to: [5, b] }
    - { from: [5, out],  to: [2, condition] }
    - { from: [2, true], to: [8, in] }
    - { from: [6, out],  to: [8, value] }
    - { from: [7, out],  to: [6, y] }
    - { from: [9, out],  to: [10, in] }
  comments:
    - { rect: [ -60, 340, 520, 80 ], text: "grounded é setada no contato, não no update" }
```

## 12. Apêndice B - nó nativo novo (checklist para quem implementar)

1. `NodeType` em `GraphTypes.h` + entrada em `NodeDefs<Family>.cpp` (pins, defaults, categoria,
   tooltip, ícone).
2. Ligação em `ExecutableGraph::prepare()` (BoundOp), nunca no hot path.
3. `GraphSchema` (validação de tipos) + teste no `--check`.
4. Palette: nome, sinônimos ("jump" → impulso vertical) para a busca funcionar.
5. Se for gerável por reflexão: **não** escrever nó - expor no binding Lua/C++ e deixar o catálogo
   (`tools/reflector`) fazer o resto.
