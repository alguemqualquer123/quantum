# Quantum Engine — Plano do Sistema de UI Retained-Mode ("Quantum UI")

Objetivo: um módulo **isolado** de UI retained-mode orientada a dados (árvore de elementos,
flexbox, estilos USS, marcação UXML, eventos/foco, data binding, renderer próprio) para **UI de
jogo** (HUD, menus, inventário, diálogos) e, futuramente, UI retained-mode do editor.
O Dear ImGui **continua** como está (ferramentas do editor), e a UI antiga de cena
(`engine/core/object/ui/*`: Panel/Button/Image/Text/…) **não é removida nem alterada** — o módulo
novo convive com ela.

Formato do documento: decisões arquiteturais primeiro (com alternativas recusadas e motivo),
depois estrutura, interfaces, integração, e o plano em fases com critérios de aceite, esforço e
arquivos reais do repo.

> **Status: PLANO — nenhum código foi escrito ainda.** Este documento (e as duas decisões de
> dependência nas seções 2/D1 e 8) aguardam o OK.

---

## 1. Estado atual (evidência, para não duplicar o que já existe)

| assunto | onde está hoje | fato relevante para o plano |
|---|---|---|
| Linguagem / build | `CMakeLists.txt:740` (`CXX_STANDARD 17`), `engine/CMakeLists.txt` | C++17; plataformas: win, linux, mac, ios/tvos, android, emscripten (`engine/platform/*`) |
| Render | `engine/renders/sokol/*` (`SokolShader`, `SokolTexture`, `SokolFramebuffer`, `SokolObject`, `SokolCmdQueue`…) | todo o runtime renderiza via **sokol_gfx**; backends compilados do `engine/shaderlib` → GLSL410 (GL 4.1+ desktop), GLES3, HLSL5, MSL, SPIRV |
| Loop / sistemas | `engine/core/Engine.cpp` (update fixo → `scene->update` → `onDraw` → `scene->draw`), `engine/core/subsystem/*` (`ActionSystem`, `AudioSystem`, `MeshSystem`, `NetworkSystem`, `PhysicsSystem`, `RenderSystem`, `UISystem`) | sub-sistemas são por cena (`scene->getSystem<T>()`); input chega ao `UISystem` via `Engine.cpp` (~1217–1435) |
| UI antiga (fica intacta) | `engine/core/object/ui/*` (Panel, Image, Text, Button, Container, UILayout, Progressbar, Scrollbar, TextEdit, Polygon), `engine/core/component/UI*.h`, `subsystem/UISystem.*` | objetos de cena com âncoras estilo uGUI (`UILayout`), eventos por callback no `UIComponent`, scissor por layout (`UILayoutComponent.scissor`), shader `ShaderType::UI` |
| Texto existente | `engine/core/util/STBText.h` | **HarfBuzz** (shaping) + **stb_truetype** (raster) + atlas de glifos por tamanho + **cadeia de fallback de fontes** (um atlas só); `engine/libs/sheenbidi` (bidi) já vendorado |
| Canvas / escala | `Engine::setCanvasSize`, `Engine::setScalingMode` (`Scaling::FITWIDTH`…), `Engine::getViewRect()` | já existe um conceito global de canvas de referência — reaproveitar |
| Input | `engine/core/Input.h` | mouse/teclado/touch/**gamepad** prontos; `MouseMode`, `setMouseCursor(CursorType)` |
| Gating UI ↔ jogo | `Engine::isUIEventReceived()` / `setIgnoreEventsHandledByUI` | flag oficial de "a UI consumiu o evento" — o módulo novo participa dela |
| Recursos | `engine/core/io/Data.*`, `FileUtils`, assets por caminho relativo ao projeto (`project/main.cpp`: `image.setTexture("quantum.png")`) | UXML/USS/fontes/imagens seguem essa resolução; `ResourcePack` para build shipped |
| Libs reusáveis já vendoradas | `engine/libs/*` | tinyxml2 (UXML), nanosvg (SVG/ícones), harfbuzz, sheenbidi, stb, lua + luabridge3 (scripting), json, sokol |
| Shaders | `engine/shaderlib/*.vert|frag` (inclui `ui.vert/ui.frag` da UI antiga), `engine/core/pool/ShaderPool.*`, `ShaderData`/`.sdat` | pipeline: fontes → shadercompiler → dados por backend + cache versionado |
| Projetos de jogo | `templates/*` (dados + Lua), `.quantum/engine-api`, `CMakeLists.txt` gerado pelo editor, `generated/main.cpp` | projeto de jogo = runtime + cenas/scripts/assets; backend default `glcore` |
| Testes | — | **não existe** framework de testes para a engine |
| Dear ImGui | builds do editor | permanece intocado (editor-only) |

---

## 2. Decisões de arquitetura (com alternativas recusadas)

### D1 — Layout: **Yoga vendorizado** + camada própria de âncoras/grid
- **Escolha**: vendorizar o **Yoga** (Meta, MIT) em `engine/libs/yoga/` no mesmo padrão já usado
  para o HarfBuzz amalgamado (`engine/libs/harfbuzz/CMakeLists.txt`), como uma lib estática
  interna, e escrever por cima a camada Quantum (`ui/layout/`): âncoras/pivot estilo uGUI
  (CanvasPanel/RectTransform), grid (GridPanel/GridLayoutGroup), CanvasScaler e os "measure
  functions" de texto.
- **Motivo**: o requisito é "flexbox **completo**" (wrap, grow/shrink/basis, justify/align, gap,
  min/max, %, relative/absolute). O Yoga é maduro, incremental (mark-dirty por nó), tem measure
  callback (`YGNodeSetMeasureFunc`) — exatamente o que o texto precisa — e MIT (compatível com a
  licença do repo). O custo de um flexbox próprio correto é alto e não é onde está o valor deste
  projeto (renderer/eventos/texto são).
- **Alternativa recusada**: flexbox próprio zero-dependência (~2.5–3.5k LOC, conformidade parcial,
  +4–8 dias-dev, casos-limite sem testes da comunidade). Se você preferir zero-dep, é viável —
  só me diga; o cronograma da Fase 2 alonga.
- **Nota**: Yoga **não tem grid**; grid e âncoras são nossos de qualquer forma.

### D2 — Renderer: interface `UIRenderer` + implementação **SokolUIRenderer**
- **Escolha**: interface abstrata `quantum::ui::UIRenderer` (o "IUIRenderer" do pedido, sem
  prefixo `I` para seguir o estilo da engine) + implementação em `engine/renders/sokol/` usando
  **sokol_gfx** (que no desktop é OpenGL 4.1+ core, e cobre GLES3/D3D11/Metal/Vulkan/Web de graça).
- **Motivo**: o runtime inteiro já é sokol_gfx; escrever GL cru quebraria os outros backends e o
  web/mobile. sokol_gfx dá o que o requisito pede: scissor (`sg_apply_scissor_rect`), stencil
  (`sg_stencil_state`), blend/opacidade, e buffers stream por frame. "Renderer próprio em OpenGL"
  fica atendido — no desktop a implementação **é** o backend OpenGL, via a abstração oficial.
- **Alternativa recusada**: GL puro — quebraria D3D11/Metal/Vulkan/Emscripten e a coerência do
  pipeline de shaders offline.

### D3 — Texto: reusar a base existente + SDF próprio (sem FreeType/msdfgen)
- **Escolha**: HarfBuzz (já vendorado) para shaping, **stb_truetype** para rasterização (já é a
  escolha da engine — `STBText`), SheenBidi para bidi; novo `TextRenderer` do módulo com
  `FontAtlasManager` (atlas por (fonte, tamanho), cache LRU), runs de rich text (cor/negrito/
  itálico), quebra/justify/ellipsis/line-height, e **geração de SDF própria (8SSEDT, ~300 LOC)**
  numa fase de polimento para nitidez em qualquer escala.
- **Motivo**: evita 2 dependências novas (FreeType, msdfgen); a fundação de shaping/raster já
  existe e é usada pela engine hoje.
- **Alternativa recusada**: FreeType+msdfgen — qualidade MSDF superior, mas é dependência nova;
  só com sua autorização explícita.
- **IME**: completo depende de mexer em `engine/platform/*` — fica **parcial** (o input de texto
  básico já existe via `eventOnCharInput`); completo só se você pedir (aviso antes).

### D4 — USS/UXML: parsers próprios
- **Escolha**: parser de **USS** próprio (subset CSS: seletores por tipo/#id/.classe,
  descendente/filho, pseudo-estados `:hover/:active/:focus/:disabled/:checked`, variáveis `--x`,
  cascata + especificidade, unidades px/%/em) e parser de **UXML** sobre **tinyxml2** (já no repo),
  com templates/slots e mapeamento de atributos; mais o **builder fluente** equivalente em C++.
  Hot reload (dev) por checagem de mtime no frame, reconstrução best-effort preservando estado.
  **Reordenação aprovada (out/2026)**: o **USS + hot reload + variáveis/tema** foi antecipado para
  a F2 (pseudo-estados ligados na F3); o **UXML** (templates/slots/builder) fica na F4.
- **Motivo**: nenhum formato existente no repo é CSS-like; CSS é o padrão mental da UI Toolkit e o
  parser é pequeno (~1–1.5k LOC com testes). XML já tem parser pronto no repo.

### D5 — Nomes e localização: namespace `quantum::ui` em `engine/core/ui/`
- **Escolha**: tudo novo em `engine/core/ui/` no namespace `quantum::ui` (`ui::VisualElement`,
  `ui::Button`, `ui::Label`…). A UI antiga continua em `quantum::` (`quantum::Button`, `quantum::Text`).
- **Motivo**: zero colisão de nomes com a UI atual e com a engine; quando a UI antiga for
  aposentada (futuro), é só promover o namespace.

### D6 — Input: roteador com pilha de camadas (estilo Common UI)
- **Escolha**: `ui::InputRouter` + `ui::LayerStack` (pilha de "activables": HUD → menu → modal;
  modal bloqueia input das camadas abaixo), na frente de tudo dentro do módulo. Prioridade global
  (no editor): ImGui → UI nova → UI antiga de cena → jogo. A UI nova marca
  `Engine::setIgnoreEventsHandledByUI`-style através do flag existente (`uiEventReceived`) para o
  jogo saber que o evento foi consumido.
- **Motivo**: requisito explícito de "roteamento de input por camada" (Fase 7) e convivência
  declarada com o ImGui sem disputa de foco.

### D7 — Sem alocação por frame / orçamento de CPU
- **Escolha**: arena por frame (`std::pmr::monotonic_buffer_resource`, C++17 — sem dep nova),
  pools de nós/comandos, **dirty flags separadas** (layout / estilo / pintura) e batching de draw
  por (textura, estado, scissor). Listas grandes (ListView) virtualizadas por reciclagem.
- **Meta**: 5.000 elementos visíveis a 60 FPS com **< 1 ms** de CPU de UI por frame (medido com
  benchmark desde a Fase 1, cobrado na Fase 8).

### D8 — Testes: **doctest vendorizado** (dev-only) — depende do seu OK
- **Escolha proposta**: `libs/doctest/` (single-header, MIT) + alvo `quantum-ui-tests` (option de
  CMake, fora do build de jogos).
- **Motivo**: o requisito pede testes unitários de layout/USS/UXML/eventos; o repo não tem
  framework. doctest é header-only, sem dependência de runtime, não entra no binário do jogo.
- **Alternativa**: mini-framework próprio (~150 LOC, asserts + runner) se você preferir zero-dep.
- **Nota**: os testes serão GPU-free (testam layout, parsers, dispatcher e **montagem do command
  buffer** — sem contexto gráfico).

### D9 — Demo: alvo dev `quantum-ui-demo` agora; template de projeto a partir da Fase 4
- **Escolha**: nas Fases 1–5 a demo roda num alvo de desenvolvimento `quantum-ui-demo`
  (`engine/ui_demo/`, mesmo padrão do `engine/voxel_demo/CMakeLists.txt`), com iteração rápida.
  A partir da Fase 4, um template `templates/ui-demo/` roda pelo **fluxo real de projeto**
  (UXML/USS em `assets/`, carregados por um script Lua via binding mínimo do `UIDocument`).
- **Motivo**: o fluxo de projeto passa pelo editor (gera `CMakeLists.txt`/`main.cpp`); levar isso
  para a Fase 1 só adicionaria atrito. A serialização do `UIDocument` como objeto de cena
  editável no editor fica para depois (editor retained-mode futura).

### D10 — World-space UI (Fase 7): reuso do caminho existente
- **Escolha**: renderizar o documento num `Framebuffer` (já existe) e desenhar um quad com o
  shader/mvp (o caminho já usado pela UI antiga) — UI no mundo 3D sem caminho de render novo.

---

## 3. Estrutura de pastas

```
engine/core/ui/                  # módulo novo (namespace quantum::ui)
  core/        VisualElement, Widget, ElementId, Classes, Property<T>, DirtyFlags, FrameArena
  layout/      LayoutNode (Yoga), GridLayout, Anchors (uGUI), LayoutGroups (H/V/Grid),
               CanvasScaler, Measure (texto)
  style/       USSLexer, USSParser, Selector, Specificity, StyleResolver, Transition,
               Animation, Easing, ComputedStyle
  markup/      UXMLParser, Template/Slot, Builder (fluente), HotReloader
  events/      Event, EventType, Dispatcher (capture→target→bubble), PointerEvent, KeyEvent,
               FocusEvent, NavigationEvent, WheelEvent, DragDropEvent, PointerCapture,
               FocusController
  controls/    Label, Image, Button, Toggle, Slider, ProgressBar, TextField, Dropdown,
               RadioGroup, ScrollView, ListView (virtualizada), TreeView, TabView, Foldout,
               Tooltip, ContextMenu, Popup, Modal
  binding/     BindingPath, Binding (1-way/2-way), ViewModel, Converter
  render/      UIRenderer (interface), CommandBuffer, RenderState, Batcher, TextureAtlas
               (shelf + LRU), NineSlice, Brush (rounded/border/gradient/shadow), TextRenderer,
               SDF (8SSEDT)
  common/      LayerStack, Activatable, InputRouter, InputAction, DeviceIconSet
  integration/ UIDocument (Object de cena), DocumentSystem (SubSystem), DevWatcher

engine/libs/yoga/                # vendor MIT (mesmo padrão de engine/libs/harfbuzz)
engine/renders/sokol/            # + SokolUIRenderer.{h,cpp}
engine/shaderlib/          # + ui_brush.vert / ui_brush.frag (+ includes/ui_sdf.glsl)
engine/ui_demo/                  # alvo dev quantum-ui-demo (fases 1–5)
tests/ui/                        # alvo quantum-ui-tests (doctest ou mini-framework próprio)
templates/ui-demo/               # template de aceite (fase 4+)
libs/doctest/                    # só se aprovado (D8)
```

---

## 4. Interfaces principais (esboço)

```cpp
namespace quantum::ui {

// ---- núcleo ------------------------------------------------------------
class VisualElement {
    ElementId id;                    // #nome para USS/UXML
    std::vector<std::string> classes;
    void addToClassList(std::string_view);
    void setVisible(bool); void setEnabled(bool);
    VisualElement& addChild(std::unique_ptr<VisualElement>);
    void removeFromHierarchy();
    Rect  layout;                    // bounds após o layout (leitura)
    void markDirty(DirtyFlags);      // Layout | Style | Paint
    // estilo: setters tipados que alimentam o ComputedStyle
};
class Widget : public VisualElement { /* onAttach/onDetach, estado, interação */ };
template <typename T> class Property {           // valor + notificação
    Property& operator=(T);  const T& get() const;
    Subscription onChanged(std::function<void(const T&, const T&)>);
};

// ---- layout ------------------------------------------------------------
class LayoutNode {                                // 1:1 com YGNode
    static LayoutNode flex(); static LayoutNode grid();
    LayoutNode& direction(FlexDirection); LayoutNode& grow(float); LayoutNode& basis(Length);
    LayoutNode& gap(float); LayoutNode& absolute(); LayoutNode& percent(...);
    LayoutNode& minMax(Length, Length);
};
struct Anchors { Vector2 min, max, pivot, anchoredPos, sizeDelta; };   // uGUI
class CanvasScaler { ReferenceResolution ref; ScaleMode mode; float match; }; // ConstantPixelSize | ScaleWithScreenSize
class LayoutEngine { void compute(VisualElement& root, Vector2 available); };

// ---- estilo / marcação ---------------------------------------------------
class USSParser { static StyleSheet parse(std::string_view src, Diagnostics&); };
class StyleResolver {                       // cascata + especificidade + pseudo-estados
    ComputedStyle& computed(VisualElement&, uint32_t pseudoStateMask);
};
class UXMLParser  { static UXMLTemplate parse(std::string_view xml); };
class Builder     { static std::unique_ptr<VisualElement> root(); };      // API fluente
class HotReloader { void watch(std::string_view path, std::function<void()>); };

// ---- eventos ------------------------------------------------------------
struct Event { EventType type; VisualElement* target; VisualElement* currentTarget;
               enum Phase { Capture, Target, Bubble } phase; void stopPropagation(); };
class Dispatcher { void dispatch(Event&, VisualElement& target); };       // 3 fases
class FocusController { void setFocus(VisualElement*); void navigate(Direction);
                        void setTabOrder(std::span<VisualElement*>); };
class PointerCapture { void capture(VisualElement&, int pointerId); void release(); };

// ---- renderer (interface desacoplada) ------------------------------------
class UIRenderer {                           // implementação: SokolUIRenderer
    virtual void beginFrame(Vector2 viewSize, FrameArena&);
    virtual void addRect (const Rect&, const Brush&);        // cantos, borda, gradiente
    virtual void addImage(const Rect&, Texture&, const NineSlice*);
    virtual void addShadow(const Rect&, const Shadow&);
    virtual void addText(const TextRun&);
    virtual void pushClip(const Rect&); virtual void popClip();
    virtual void pushTransform(const Matrix3&); virtual void pushOpacity(float);
    virtual void endFrame();                                  // flush: batching + submit
};

// ---- integração com a engine ---------------------------------------------
class UIDocument : public Object {           // entidade de cena
    void setSource(std::string_view uxmlPath);   // + .uss irmão
    void setStyleSheet(std::string_view ussPath);
    VisualElement& root();
    enum class Space { Screen, World };
};
class DocumentSystem : public SubSystem {    // tick: layout sujo + animações + roteamento de input
    bool eventOnPointerDown(float, float); bool eventOnPointerMove(float, float); /* … */
};

} // namespace quantum::ui
```

Exemplos-alvo (UXML/USS equivalentes à API C++):

```xml
<ui:UXML>
  <ui:VisualElement class="hud">
    <ui:Label text="Vida" class="hp"/>
    <ui:ProgressBar name="hp-bar" value="0.75"/>
  </ui:VisualElement>
</ui:UXML>
```
```css
.hud { flex-direction: row; gap: 8px; padding: 12px; }
.hp  { color: #ff5555; font-size: 18px; }
.hp:hover { color: #ffffff; transition: color 120ms ease-out; }
```

---

## 5. Integração com a engine e arquivos compartilhados tocados

Pontos de toque em arquivos existentes — **mínimos e append-only**:

1. `engine/CMakeLists.txt` — `add_subdirectory` do yoga; fontes novas do módulo; alvo dev
   `quantum-ui-demo`; option do alvo de testes.
2. `CMakeLists.txt` raiz / `copy_headers.cmake` — exportar os headers públicos novos para
   `.quantum/engine-api` (API dos projetos).
3. `engine/core/Engine.cpp` — encaminhar input ao `ui::DocumentSystem` nos mesmos pontos que hoje
   chamam `UISystem::eventOnPointer*/eventOnKey*` + um tick/draw. (Ordem: ImGui → UI nova → UI antiga.)
4. `engine/core/Scene.cpp/.h` (onde os sub-sistemas são instanciados) — registrar
   `ui::DocumentSystem`.
5. `engine/core/render/Render.h` + `engine/core/pool/ShaderPool.*` — **append** de
   `ShaderType::UI_BRUSH` no fim do enum (o próprio enum documenta "keep appending").
6. `engine/shaderlib/` — **novos** `ui_brush.vert/frag` (+ `includes/ui_sdf.glsl`);
   regeneração do cache de shaders pelo pipeline existente.
7. `engine/core/script/binding/*` — novo arquivo de binding Lua do `UIDocument` (Fase 4+).

**Não será tocado**: `engine/shaderlib/mesh.frag` (outro trabalho), `VISUAL_SCRIPTING_PLAN.md`,
`RENDERING_UPGRADE_PLAN.md`, `Projects/`, templates de terceiros, artefatos de `build/`.
A UI antiga e todo o caminho do Dear ImGui ficam intactos.

---

## 6. Fases (uma por vez: compila, testa, demonstra)

> **Reordenação (a pedido, out/2026)**: o estilo "CSS" chega cedo — a **F2** agora junta
> layout flexbox + **USS com variáveis/tema** + **hot reload** + **acabamento** (gradientes,
> box-shadow, texto SDF), para a demo já sair bonita e personalizável. Os **pseudo-estados**
> (`:hover/:active/...`) entram na **F3** junto com os eventos, e o **UXML** + template na **F4**.
> A F2 pode rodar em dois incrementos verificáveis (F2a layout → F2b USS+acabamento), mantendo
> o ritual de compilar/testar a cada incremento.

| # | Fase | Entrega principal | Aceite |
|---|---|---|---|
| 1 ✅ | Núcleo + renderer + texto básico | árvore `VisualElement`, arena/pools, `CommandBuffer` + `SokolUIRenderer` (retângulos, cantos, borda, imagem, **9-slice**, **clipping** por scissor), `TextRenderer` básico (shaping HarfBuzz + atlas) | `quantum-ui-demo`: tela com painéis aninhados desenhados, texto e imagem 9-slice clipada |
| 2 | Layout flexbox + **USS + tema + acabamento** + DPI | Yoga integrado (dirty incremental), âncoras/painéis UMG (Overlay, H/VBox, GridPanel, WrapBox, ScrollBox, SizeBox, ScaleBox, Border), RectTransform/âncoras + LayoutGroups/ContentSizeFitter, CanvasScaler; **parser USS próprio** (subset CSS: tipo/#id/.classe, descendente/filho, cascata + especificidade, `--variáveis`, px/%/em) **+ temas por variáveis** (exemplos dark/neon) **+ hot reload dev** (mtime no frame); **gradientes (linear/radial), box-shadow e texto SDF (8SSEDT)** no renderer | demo com **menu definido 100% em `.uss`**, tema por variáveis, gradiente + sombra + texto nítido em qualquer escala; janela redimensionada responde certo (incl. % e absolutos); editar o `.uss` reflete **sem recompilar** |
| 3 | Eventos, foco, input (**+ pseudo-estados**) | pipeline capture→target→bubble, Pointer/Keyboard/Focus/Navigation/Drag&Drop, pointer capture, tab order, navegação direcional por gamepad; **`:hover/:active/:focus/:disabled/:checked` ligados ao style resolver da F2** | botões com hover/active/focus **vindos do `.uss`**; navegação **só** por gamepad funciona; roteamento por camada respeita `isUIEventReceived` |
| 4 | UXML + templates + fluxo de projeto | parser **UXML** (tinyxml2), templates/slots, builder fluente, template `templates/ui-demo` rodando pelo **fluxo real de projeto** (UXML/USS em `assets/`, carregados por script Lua via binding mínimo do `UIDocument`) | menu do template definido **100% em UXML/USS** rodando pelo editor/export |
| 5 | Controles avançados | TextField, Dropdown, RadioGroup, ScrollView, ListView virtualizada, TreeView, TabView, Foldout, Tooltip, ContextMenu, Popup/Modal (focus trap) | tela de inventário com **10.000 itens** fluida (virtualização medida) |
| 6 | Animações + data binding | transitions (`@keyframes`, curvas de easing), `Property<T>` + Binding 1/2-way com conversores | HUD ligado a um ViewModel (vida/munição) atualiza sem código manual |
| 7 | Camadas (Common UI) + world-space | `LayerStack`/`Activatable`, roteamento por camada, `InputAction` abstrata, ícones por dispositivo; UI em textura para o mundo 3D | pilha menu→modal com input roteado certo; HUD world-space num quad 3D |
| 8 | Otimização, testes, docs | benchmark 5k elementos (meta < 1 ms CPU), suíte `quantum-ui-tests`, README por controle com exemplo mínimo | número medido batendo a meta + suíte verde |

Esforço aproximado (dias-dev, 1 pessoa): F1 8–12 (feita) · F2 **12–18** (a maior: layout + USS +
acabamento) · F3 6–8 · F4 3–5 · F5 8–10 · F6 5–7 · F7 6–8 · F8 5–8. Total ~53–76.

**Fora de escopo (por agora)**: editor visual de UXML/USS dentro do `quantum-editor`
(a "UI de editor retained-mode futura" — o módulo fica preparado), IME completo
(depende de `engine/platform`), binding C# (a engine usa Lua), screen-reader accessibility
(acessibilidade aqui = teclado/gamepad).

---

## 7. Requisitos não funcionais (como serão garantidos)

- **Zero alocação por frame**: arena `std::pmr` resetada por frame; pools de nós/comandos;
  buffers de vértice/índice reutilizados (stream). Checado no benchmark da F8.
- **Recálculo só do que sujou**: dirty flags separadas + dirty marking do Yoga; style resolver
  cacheado por (elemento, máscara de pseudo-estado).
- **Batching**: merge de comandos por (textura, shader, scissor); atlas dinâmico com eviction LRU.
- **Testes**: layout (casos flexbox com retângulos esperados), USS (seletores/especificidade/
  cascata), UXML (estrutura/templates), eventos (ordem de fases, foco/navegação), command buffer
  (montagem sem GPU). Rodam no alvo `quantum-ui-tests`.
- **Documentação**: cada controle com exemplo mínimo no README do módulo.

## 8. Riscos

| risco | mitigação |
|---|---|
| Conflito com o trabalho paralelo de render (`mesh.frag`/RENDERING_UPGRADE) | toques append-only (1 enum + shaders NOVOS); sequenciar a regeneração do cache de shaders com quem estiver mexendo |
| Yoga é dependência nova | sua decisão (D1); alternativa zero-dep documentada |
| stb_truetype não tem SDF nativo | SDF próprio (8SSEDT) antecipado para a F2 (a pedido); MSDF só com sua autorização |
| IME completo | explicitamente parcial até você pedir |
| Erros de conformidade CSS/UXML | suíte de testes desde a fase do parser; subset documentado |

---

## 9. Aprovações

1. **D1 — Layout**: ✅ **Yoga vendorizado** (aprovado).
2. **D8 — Testes**: ✅ **doctest vendorizado** (aprovado).
3. **Reordenação do visual/CSS**: ✅ **USS + tema + acabamento na F2** (aprovado out/2026).

**Fase 1 entregue**: núcleo (`engine/core/ui/`), `SokolUIRenderer`, texto (HarfBuzz + atlas),
demo `quantum-ui-demo` e suíte `quantum-ui-tests` (12 casos / 105 asserções); diff agrupado e
**não commitado** (você comita).

**Fase 2a entregue**: layout flexbox (Yoga v2.0.1 vendorizado, `engine/core/ui/layout/YogaLayout.*`,
`YGConfigSetUseWebDefaults` + `flexShrink` 0 para a semântica UMG), API de estilo em `VisualElement`
(`core/Style.h`), painéis (`layout/Panels.*`: HBox, VBox, WrapBox, GridPanel, ScrollBox, SizeBox),
medida de texto (`FontAtlas::measure` + override no `Label`) e `setPosition`/`setSize` virando
açúcar sobre posição absoluta/tamanho fixo. `quantum-ui-tests` com 22 casos / 164 asserções; demo
com VBox/HBox/WrapBox/Grid/Scroll verificada em screenshot. Commit `632efb2e`.

**Fase 2b (a seguir)**: USS (subset CSS com variáveis/tema) + hot reload, gradientes,
box-shadow, texto SDF (8SSEDT), 9-slice/bordas; âncoras/CanvasPanel/Overlay/ScaleBox/Border
("F2a2") entram junto.
