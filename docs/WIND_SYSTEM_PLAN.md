# Quantum Engine - Plano do Sistema de Vento

> Request do usuário (2026-11-05): *"queria que tivesse um sistema de vento na física da água
> e etc..., o vento pode balançar folhas, e até a água, fazendo ondas e pequenas ondas, dando
> um balanço mais verdadeiro para água"*.
> Escopo aprovado nas perguntas de alinhamento: **vento no motor (C++), global na cena +
> zonas por volume, consumido pela água (Gerstner dirigido pelo vento)**. Folha/grama era fase
> cortada; em 2026-11-06 o pedido voltou explicitamente para **1M de plantas num terreno gerado com
> muita folhagem**, então a fase 5 entrou no round. Ondinhas capilares no heightfield e força em
> corpos seguem nas fases 6-7, documentadas e **não implementadas** neste round.

---

## Estado atual (provado por definição + call site, 2026-11-05)

| Coisa | Realidade |
|---|---|
| `engine/components/WindZone.lua` | existe, é Lua pura: calcula `windDir`/`currentStrength` de senoides (`:50-53`, `:103`), empurra `WaterWaves:setWindDirection` (`:129-130`) e faz `body:applyForce` (`:162`). Nenhum C++ involvement. |
| `engine/components/WaterWaves.lua` | escreve `waveHeight/waveLength/waveSpeed/waveDirection/waveSteepness/crestFoam` por frame (`:142-153`). "Vento" aqui é um número de direção derivando. |
| `WindComponent` / `WindZone` em C++ | **não existe**. Os hits de `Wind` em `engine/**` são `WindingOrder` (`engine/core/render/Render.h:24`) e classes de `Window`. |
| `u_wind` / `windStrength` em GLSL | **não existe** em `engine/shaderlib/**`. Todo hit de "wind" é "window" ou "winding". |
| Vento na água | **não há entrada**. As ondas vêm de `getWaterWaves` (`engine/core/subsystem/RenderSystem.cpp:7285-7304`) com um único ângulo autorado `water.waveDirection` dividido em 4 por offsets hardcoded (`:7286-7287`); a velocidade de fase é `sqrt(9.81*k) * water.waveSpeed` (`:7318-7321`). |
| Sway de folhas/grama | **não existe** nenhum deslocamento de vértice de vento. Os únicos hooks de animação de vértice do `mesh.vert` são skinning e morph targets (`:111,119,132,146`). |
| Física | força global = gravidade apenas (`PhysicsSystem.cpp:469-474`, `:586-590`). Não há loop de força por corpo no motor; `applyForce` vem de fora (`Body3D.cpp:826-837`). |
| Planos | `TODO_ENGINE.md:135` `- [ ] Controllers (buoyancy, wind)` - único item, aberto. |

Conclusão honesta: hoje o "vento" do projeto é um script que move números de ondas. Nada no
motor sabe o que é vento. Este plano cria esse conceito.

---

## Decisões

### D1. Vento é uma configuração de cena, não um componente
O ar que se move é do mundo, não de uma entidade: é o análogo do `gravity3D` que já vive em
`SceneSettings` (`engine/core/SceneSettings.h:138-139`) e é lido pelo `PhysicsSystem` no init.
Um `WindComponent` por cena seria o mesmo que um `GravityComponent` por cena.

**Campos novos em `SceneSettings`** (depois de `gravity3D`, `:139`):

```cpp
// --- wind ---
// The air movement of the world. Off by default: no scene authored before this exists
// changes its water, its foam or its frame times. A WindZone multiplies it locally.
bool windEnabled = false;
float windSpeed = 6.0f;         // metres per second, measured 10 m above the water
float windDirection = 0.0f;     // around Y, degrees, 0 = +X, the same convention as waveDirection
float windGustiness = 0.35f;    // 0 = steady air, 1 = the speed swings from calm to double
float windChange = 0.2f;        // how fast the gust pattern renews, in cycles per second
```

`windDirection` usa o mesmo `Angle::defaultToRad` que `water.waveDirection`
(`RenderSystem.cpp:7294`) para que authorar as duas coisas seja a mesma linguagem.

### D2. O mar responde ao vento com inércia - e isso é o "balanço mais verdadeiro"
Ondas não nascem quando o vento vira: elas crescem enquanto o vento sopra e sobrevivem a ele.
Sem esse estado, o vento vira um multiplicador de amplitude e a superfície "pisca" com as
rajadas - que é exatamente o que hoje o `WaterWaves.lua` faz e o que o usuário sente como falso.

Cada `WaterComponent` ganha um integrador de *sea state* (runtime, **não persistido**, como
`wavePhase`): o quanto o mar atual está perto do mar que o vento de agora sustentaria.

### D3. Física das ondas: fetch-limited, não uma curva de gosto
Usamos as escalas clássicas do *wind sea* em água profunda (SMB / Pierson-Moskowitz, adimensionais
por `g` e `U`), que dão altura e período a partir de **velocidade do vento** e **fetch** - a
distância sobre a qual o vento empurra a água. Fetch é o que faz um lago pequeno ter onda pequena
com o mesmo vento de um oceano. É a única forma de o vento "parecer verdadeiro" sem tuning manual
por cena.

Com `U` = velocidade do vento em m/s, `X` = fetch em m, `g` = 9.81:

```
g*X/U^2      = número de fetch (adimensional)
g*Hs/U^2     = 0.0016 * (g*X/U^2)^0.66     -> altura significativa   (saturado em 0.0051)
g*Tp/U       = 0.72  * (g*X/U^2)^0.34      -> período de pico        (saturado em 7.13)
Lp           = g*Tp^2 / (2*PI)             -> comprimento de onda de pico em água profunda
```

Mapeamento para os campos que o motor já tem (documentado no código, calibrado no probe):
`waveHeight` alvo = `Hs/2` (a soma das 4 ondas em fase chega a `waveHeight`, contrato de
`getWaterWaves:7302`), `waveLength` alvo = `Lp`, `waveSpeed` alvo = 1.0 (a dispersão física
`sqrt(g*k)` já está em `getWaterWaveFrequency:7320`; `waveSpeed` é hoje um multiplicador de
gosto - com vento ele vai a 1.0), e `crestFoam` ganha um termo de whitecap que só aparece acima
de ~8 m/s (`water.frag:437` lê `crestFoam`, com o trap já documentado do `* 3`).

`windInfluence` (novo, persistido na água) é a mistura entre o mar autorado e o mar dirigido
pelo vento: 0 = exatamente o que existe hoje, 1 = o vento manda. **Default 1.0**, porque só tem
efeito quando `windEnabled` é true, e aí é isso que o usuário quer.

### D4. Rajadas: determinísticas, sem `rand()`
`engine/core/util/Wind.{h,cpp}` avalia o vento num ponto do mundo e num instante com o mesmo
xorshift64* de `WaterSystem::nextRandom` (`WaterSystem.cpp:147`) e a mesma disciplina do campo
de água: **dois passos iguais produzem bytes iguais em qualquer plataforma**, o que é o que
permite a um probe headless comparar números. A rajada é um valor suavizado no tempo (e no
espaço, para uma rajada *cruzar* o lago em vez de piscar nele).

### D5. Zonas: um componente, no molde do `WaterExclusion`
`WindZoneComponent` (`engine/core/component/WindZoneComponent.h`) é o volume onde o vento muda:
forma BOX/SPHERE, `strength` multiplicador, `direction` override (-1 = segue o global). Copia a
estrutura do `WaterExclusionComponent` (`:23-40`) inclusive o cap (`MAX_WIND_ZONES 8`, análogo de
`MAX_WATER_EXCLUSIONS 8` `:15`) e o padrão de busca por ponto.

**Fase 4 aplica a zona no CPU** (o sea state de cada água é dirigido pelo vento avaliado no
centro dela), o que já dá "abrigo atrás da montanha" para cenas com mais de uma água e para o
Lua. **Fase 6** leva a zona para o vértice (`water.vert`) com a espelho CPU em `sumWaterWaves`
(`RenderSystem.cpp:7324`) - o contrato do arquivo: *mudou um, mudou o outro* - e aí exige bump
da versão do cache de shaders (`App::getUserShaderCacheDir()`, v40 hoje).

### D6. Escala: 1M de **plantas**, não 1M de entidades
Medido na árvore (2026-11-06): `MAX_STORED_ENTITY = (1u << 20) - 1` = 1.048.575
(`engine/core/ecs/EntityManager.h:40`) - contar um milhão de entidades cabe no tipo, mas o resto não.
Uma entidade de mato "de verdade" custa `Transform` ≈ 376 B (quatro `Matrix4`, `Transform.h:14-45`)
+ um `MeshComponent` com `HybridArray<Submesh, 4>` embutido, cada `Submesh` carregando
`ObjectRender`, `Material` e quatro blocos de uniformes (`MeshComponent.h:123-126`, `:180`) -
megabytes por parcela, **um milhão de draw calls** (o caminho de mesh normal faz `beginDraw`+`draw`
por submesh, `RenderSystem.cpp:4213,4238-4272`) e um loop de `transforms->size()` por câmera por pass
(`:11561-11605`). Isso não é cena, é despejo de memória.

O caminho que já existe é o outro: `MeshSystem::createFoliageEntity` (`:3398`) cria **uma**
entidade `Transform+Mesh+Model+InstancedMesh` por *chunk* de placement, com
`mergeStaticMeshes = true` e `maxInstances` = capacidade; a instância é dada de vértice
(`i_matrix_col1..4`, `i_color`, `mesh.vert:98-107`) e o draw é um por bucket de LOD
(`MAX_MESH_LODS 4`). Um campo de um milhão de plantas = **centenas de entidades e ~1-3 mil draws**.

A lei do scatter, lida direto do código (`foliageCellAttempts` `:3366`, `updateFoliageLayer`
`:3663-3692`, aceitação `:3621`), usada ao contrário para calibrar o harness:

```
tentativas/celula = ceil( clamp(density * 64, 0, 8192) )   # celula de 8 m; SATURA em density=128/m²
maxBatchSide      = floor( sqrt(8192 / tentativas) )       # 8192 e o teto de instancias por entidade
batchSide         = clamp( ceil(D / (8*2)), 1, maxBatchSide )   # D = drawDistance da camada
capacidade/chunk  = tentativas * batchSide^2               # mesh do chunk inteiro cabe junto
celulas na visao  ~= ( 2*ceil(D/chunkSize) + 1 )^2         # janela QUADRADA, nao circular
                  (e nao passa de terrainRadius = ceil(S/2/chunkSize), S = terrainSize)
plantas           = celulas * capacidade * media(densityMap)
```

Três consequências que derrubam a intuição:

- **`density` acima de 128/m² não planta nada.** O `clamp` em 8192 é o teto, e ao saturá-lo
  `maxBatchSide` cai para 1 - ou seja, o chunk vira exatamente uma célula. Passar de 128/m² só
  compra chunks de 1 célula (menos draws? não: mais entidades pelo mesmo chão).
- **Quem controla a escala é o `drawDistance`, não a densidade.** Saturado, o teto é
  `8192 * (2*ceil(D/8)+1)²` tentativas. A 60 m sobre um terreno de 179,5 m: 17x17 = 289 chunks,
  2.367.488 tentativas; cobertura média 0,359 da density map **pintada a partir do heightmap**
  (senão cresce grama no mar) = ~850 mil plantas. `D = 128` no mesmo terreno dá 576 células
  = 4,7 milhões de tentativas, porque `terrainRadius` (12) corta antes da camada.
- **Não existe "1M de entidades" nessa cena, nem pode existir.** As 289 entidades são o teto da
  janela; o `density: 50000` do preset, que lido à letra seriam 50.000 plantas/m², vira 8192 por
  célula - o clamp converte o pedido absurdo em cenário caro, não em crash.

Medido ao vivo (2026-11-06) no terreno do `templates/fps-fps` com uma camada saturada:
`avgInstances 1.324.725` sobre 312 entidades, 654-705 draw calls. Um milhão de plantas custa
algumas centenas de entidades. É essa a razão de o harness ser um terreno + uma camada, e não um
milhão de `Entity`.

Dois portões que explicam "não aparece nada":
- `updateTerrainFoliage` volta sem plantar nada se a cena não tem render camera (`:3899-3901`) e
  **destrói** qualquer camada com `densityMap` vazio (`:3915`). Num `--screenshot` de cena sem
  `mainCamera`, o mato simplesmente não existe.
- O orçamento por frame é **tempo, não contagem** (`:3360-3363`, re-armado em `:6616` com
  4 ms): um tufode grama tem 129 mil triângulos e dois deles no mesmo frame mediram 222 ms de
  hitch. Consequência prática: os 289 chunks não nascem no frame 1 - o harness precisa de
  centenas de frames de warmup antes do capture, senão fotografa terreno pelado.

### D7. Sway de vegetação no GPU, atrás de um bit de variante (fase 5)
Sem CPU por instância: um milhão de matrizes reescritas por frame é o custo, não o draw. O balanço
nasce no vértice, e só para as camadas que pedirem.

- **`USE_WIND_SWAY`** = bit **30** do `mesh` (bits 0-27 usados, 28 é `DIRECTIONAL_LIGHTS_ONLY`
  aplicado fora da tabela, 29 é `HAS_HEIGHT`; tabela em `editor/shader/ShaderBuilder.cpp:457-485`) e
  bit **9** do `depth` (`:487-496`, hoje até 8). Código de 3 letras `Sws`. Bits são por tipo de
  shader - nunca reusar índice (`quantum-shaders`).
- **Bloco novo `u_vs_windParams`** = `WIND_VS_PARAMS`, **append** no fim de
  `UniformBlockType` (`engine/core/render/Render.h:128-204`; o enum é chave de lookup por nome em
  `ShaderData.cpp:150-250`, insertion no meio desloca valores - não fazer), com o nome registrado no
  `getUniformBlockName`. Membros na ordem (a ordem **é** o layout de bytes): direção do vento na
  superfície (xy), velocidade, `time`, altura da planta usada para normalizar a flexão, amplitude,
  frequência da rajada e o tique do campo.
- **Slots** `slotVSWind` ao lado de `slotVSFade` (`RenderSystem.cpp:3223`) e `slotVSDepthWind` ao lado
  de `slotVSDepthFade` (`:3384`) - **isso muda `sizeof` de `Submesh`**, ou seja: todos os `.obj` que
  incluem o header precisam ser recompilados junto, senão o processo fica com dois layouts da mesma
  classe (foi exatamente o crash de boot que a Fase 3 deixou registrado).
- **GLSL** em `engine/shaderlib/includes/wind_sway.glsl`, chamado por `mesh.vert` e `depth.vert`
  **antes** da projeção, para manter o `invariant gl_Position` idêntico entre os dois (contrato do
  depth pre-pass). Flexão ∝ (altura local / altura da planta)², fase vinda da **posição mundial da
  instância** (senão o campo inteiro sobe e desce como um pistão), rajada por hash advectado
  **amostrando a montante** - a mesma lei que o probe da Fase 2 pinou em `Wind.cpp`.
- **Opt-in** por camada: um campo em `TerrainFoliageLayer` (provavelmente `windSway`, 0..1) lido em
  `MeshSystem::updateFoliageLayer`, que liga o bit na variante do chunk. Aquece `Stream.cpp`
  (encode/decode da camada, `:5859`/`:5879`), `Catalog.cpp:1417` (`getTerrainFoliageLayerPropertyFast`)
  e `Properties.cpp` - as listas paralelas de sempre.
- **Custo real medido antes de prometer:** o gargalo de 1M não é o draw, é
  `mainInstanceViewDirty` - qualquer movimento de câmera re-culpa e re-ordena **todas** as instâncias
  (`RenderSystem.cpp:3879-3881`, `:4016-4067`). O sway adds ALU de vértice e nada de CPU.
- Cache bump obrigatório (`App::getUserShaderCacheDir()`, v40 hoje) + `generate_shaders`.

**Colisão de terreno**: `MeshSystem.cpp`, `TerrainComponent.h`, `Terrain.cpp` e
`ObjectClassesLua.cpp` estão modificados **e não commitados** (00:48-01:20 de 2026-11-06) - é o
trabalho de folhagem de outro agente. A Fase 5 pisa exatamente nesses arquivos. Antes de escrever a
linha de opt-in, confirmar que esse trabalho assentou (e ver `git status`), ou a feature de alguém
desaparece no meu commit.

---

## Fases

| # | Entrega | Arquivos principais | Prova headless | Dias |
|---|---|---|---|---|
| 1 | Vento como setting de cena: `SceneSettings` + YAML + Catalog + rows do editor + Lua | `engine/core/SceneSettings.h`, `editor/Stream.cpp:2932-2964`/`:3029-3138`, `editor/Catalog.h:305/637/867`, `editor/window/Properties.cpp:14506-14800`, `engine/core/script/binding/CoreClassesLua.cpp:994-1006` | probe: seta `Scene.windSpeed`, salva, recarrega, imprime os 5 campos idênticos | 0,5 |
| 2 | `engine/core/util/Wind.{h,cpp}`: avaliação determinística com rajada; `WindSystem`-lite como função estática + Lua `Wind.sample(point)` | arquivos novos + `engine/CMakeLists.txt` + `roadmap` de registro | probe: 1000 amostras em 60 s, imprime min/max/média e o hash FNV da sequência - roda duas vezes, bytes iguais | 0,5 |
| 3 | **Mar dirigido pelo vento**: sea state inercial por água, `windInfluence`/`fetch` persistidos, `getWaterWaves`/`getWaterWaveFrequency`/`getWaterSteepness` lendo o vento, foam de whitecap | `RenderSystem.cpp:7285-7321`, `:7732-7753`, `WaterComponent.h:88-92`/`:105`, checklist de propriedade (`Catalog`, `Stream`, `Factory`, `Properties`, `ObjectClassesLua`) | probe + **PNG**: `--screenshot` do mesmo lago com `windSpeed` 0 / 6 / 25; altura de crista medida por CPU (`getWaterSurfaceOffset`) subindo com o vento e **caindo devagar** depois que o vento para | 1,5 |
| 4 | `WindZoneComponent` (BOX/SPHERE, cap 8) com avaliação CPU por ponto; registro completo de componente novo | `engine/core/component/WindZoneComponent.h`, `engine/core/registry/EntityRegistry.cpp`, `engine/core/Quantum.h:78-82`, `editor/Catalog.cpp`, `editor/Stream.cpp`, `editor/Factory.cpp`, `editor/window/dialog/ComponentAddDialog.cpp:128-135`, binding Lua | probe: dois pontos, um dentro e um fora da zona, `Wind.sample` imprime direções/forças diferentes; água dentro da zona de abrigo fica mais lisa (PNG) | 1,5 |
| 5 | **Sway de folhas/grama no `mesh.vert`** (D7) + harness de escala `tests/wind/forest`: terreno gerado + 1M de plantas instanciadas medidas por `--stats-out` | `engine/shaderlib/mesh.vert`, `depth.vert`, `includes/wind_sway.glsl`, `InstancedMeshComponent`/`Submesh` slots, `ShaderBuilder.cpp` (mesh bit 30, depth bit 9), `Render.h`, `ShaderData.cpp`, opt-in em `TerrainFoliageLayer` | `--stats-out` do projeto de teste: `avgInstances` ≈ 1.000.000, `avgDrawCalls` na ordem de algumas centenas, `entities` ~800; PNG do mesmo campo com `windEnabled` off/on e `windSpeed` 4/12/25 | 2 |
| 6 | *(CORTADA)* zona de vento por vértice na água (sombras de vento no mar) + cache bump | `water.vert`, `sumWaterWaves`, `ShaderData.cpp`, `Render.h` | - | 1 |
| 7 | *(CORTADA)* força de vento em `Body3D`/`Body2D` com `windAffinity`, e deriva das partículas de chuva/poeira | `PhysicsSystem.cpp:3128-3190` (molde de `applyBuoyancy3D:867-925`), `Body3DComponent.h:107` | - | 1 |
| 8 | Demo e docs: `demo_windy_lake`, `engine/components/README.md`, migração do `WindZone.lua` para ler o vento do motor em vez de calcular o próprio | `engine/components/demos/`, docs | demo roda headless e imprime o sea state por frame | 0,5 |

Total neste round: **5 fases, ~6 dias** (a fase 5 entrou por pedido). Fases 6-8 documentadas, não
começadas.

### Status (execução)

- **Fase 1: provada.** Os sete arquivos de código + as rows do editor. A tabela acima
  estava errada em um ponto: **`editor/Factory.cpp` também é touchpoint** (`:2696`, logo depois de
  `setGravity3D`) - ele cospe as fontes C++ da cena de um build enviado, então um setting de cena que
  não passa por lá existe no editor e desaparece no jogo. O conjunto completo de um setting de cena é
  `SceneSettings.h` → `Scene.{h,cpp}` → `Stream.cpp` → `Catalog.h` → `Properties.cpp` →
  `CoreClassesLua.cpp` → `Factory.cpp`.
- **Fase 2: provada.** `engine/core/util/Wind.{h,cpp}` + `engine/CMakeLists.txt` + o binding Lua em
  `UtilClassesLua.cpp` (`Wind.sample/speed/direction`, ao lado do bloco de `Noise`). O campo é
  determinístico: duas rodadas do probe imprimem a mesma sequência de 1000 amostras (mesmo hash FNV).
  A advecção é `F(point - wind * t)` - amostrar a montante. Com `+` o sinal, a rajada se movia contra
  o vento e o teste "the same gust arrives downwind" falhava por 0,5 m/s.
- **Fase 3: escrita e compilando limpa, ainda não provada.** Onze arquivos: a lei SMB/PM em
  `RenderSystem.cpp` (`SEA_*` `:7297-7319`, `advanceWindSea` `:7353`, os quatro `getDrawnWave*`
  `:7400-7423` por onde passa todo consumidor de onda), `windInfluence`/`fetch` persistidos e
  `seaHeight`/`seaLength`/`seaAngle`/`seaWind`/`seaMix` de runtime em `WaterComponent.h`, wrappers e
  binding de `Water`, as quatro listas do editor e a row no inspector. O valor calibrado: 6 m/s com o
  fetch mínimo dá `seaHeight` 0,411 m e comprimento 38,7 m; 200 km de fetch no mesmo vento bate o teto
  de mar plenamente desenvolvido, 0,886 m. `cmake --build build --target quantum quantum-engine
  quantum-editor-cmd` e `--target quantum-editor`: **zero `error C`** nos dois logs; o que falta é só o
  link do `quantum.dll`, que o editor aberto tem mapeado (`LNK1168`), e depois a FASE 3 do probe e as
  três capturas do mesmo lago a 0 / 6 / 25 m/s.
- **Escala de folhagem: medida e fotografada, sem relink.** O bloqueio do `quantum.dll` era só da
  rota Lua (`Terrain:addFoliageLayer`); a rota **export** não precisa dele, porque o `Factory.cpp`
  cospe o `foliageLayers` como C++ e o jogo exportado compila o motor do zero na própria árvore.
  `tests/wind/forest/scripts/make_live_project.py` deriva o terreno autoral do `templates/fps-fps`
  (câmera adicionada e nomeada em `mainCamera`, density map pintada a partir do heightmap do próprio
  terreno, linha d'água 0,4635, cobertura média 0,359) e hoje imprime as duas escalas lado a lado:
  saturada **289 chunks de 8 m → 848.780 plantas**, renderável **49 chunks de 24 m → 121.425 plantas**.
  O que decide não é a contagem de instâncias, é o triângulo por instância medido no `.glb`
  (`Kinnikinnick` 129.344, `BuffaloGrass` 24.078, `Grass_Sprigs` 96, `BuffaloGrass_Sprigs` 26): com o
  `BuffaloGrass` de 24 mil triângulos o milhão deu **~3 s por frame** (medido no heartbeat, 100 frames
  em 300 s) e com o sprig de 96 a cena inteira é 11,7 Mtri e fotografa. PNG real, sem chrome do
  editor: `C:/Users/Admin/AppData/Local/Temp/foliage/grass_low.png` (`FrameCapture` do próprio jogo).
  Três buracos do `export` apareceu no caminho, todos contornáveis à mão e nenhum corrigido:
  (1) o CMake gerado referencia `plugin/PluginCatalog.cpp` e a cópia **não leva `engine/plugin/`** -
  o configure falha; (2) o `assets/shaders/*.sdat` **não é empacotado** e o runtime não compila shader,
  então o jogo morre com exit 1 e a lista de variantes que faltam no log (é um loop: rodar, ler
  `Missing shaders:`, rodar `quantum-editor-cmd shaders --out assets/shaders`); (3) **diretórios
  simbólicos de assets são pulados** na cópia - tudo que é junção/symlink no projeto some do export.
- **Probe:** `tests/wind/probe` (projeto de verdade, roda com `quantum-engine.exe --project`).
  30 checks, todos PASS (reconfirmado na manhã de 2026-11-06 com o DLL de 22:43 - o par exe/DLL está
  coerente): 19 de armazen/clamp e 11 do campo. A FASE 3 (11 checks do mar: alvo,
  monotonia, decaimento mais lento que a subida, `windInfluence` 0 = exatamente os números de autoria)
  está escrita e depende do DLL novo para rodar. Ao tentar rodá-la com o DLL velho o probe expôs um
  erro meu: `scene:setBackgroundColor` não tem overload de 1 argumento - agora é
  `scene.backgroundColor = Vector4(...)`, como no probe do WorldCreator.
- **Prova YAML da Fase 1 (os dois sentidos, headless):** `quantum-editor-cmd.exe export --project
  <cópia do template no TEMP> --backend opengl` decodifica o `.scene`, ressava o YAML e regrava a
  fonte C++ da cena. Chaves escritas à mão (`windSpeed: 17.5`, `windDirection: 233.0`,
  `windGustiness: 0.75`, `windChange: 0.5`, `windEnabled: true`) voltam idênticas no YAML reescrito e
  em `.quantum/generated/scene_Main_Scene_1.cpp:923-927` (`scene->setWindSpeed(17.500000f);` …). É o
  único caminho headless que exercita `Stream.cpp` **e** `Factory.cpp`; o motor não lê YAML.
- **Bloqueio encontrado e diagnosticado:** todo boot do motor segmentava, inclusive em cena sem
  vento (`Scene()` vindo de Lua). Causa: `SceneSettings` cresceu 5 campos e o build incremental do
  MSBuild deixou **128 `.obj` de `build/engine/quantum.dir/Debug` mais antigos que o header** - o
  `quantum.dll` ficou com dois layouts de `Scene` dentro. Sintoma de Árvore: `quantum.dll` com mtime
  antigo enquanto `quantum.lib` é regravado ("Refreshing the engine import library timestamp" não
  relinca o DLL). Teste: `find build/engine/quantum.dir/Debug -name "*.obj" ! -newer <header>`; se
  houver obj mais velho que o header que ele inclui, apague-os e reconstrua antes de culpar o código.
  Regra prática que sobra disto: mexer em **fonte** durante o build de outro agente é seguro (MSBuild
  recompila obj mais velho que a fonte); mexer em **header que muda sizeof** não é.

---

## Checklist de touchpoints (nenhum registry é automático)

Fases 1 e 3 tocam listas paralelas escritas à mão - uma linha esquecida compila e simplesmente
não aparece:

- **setting de cena**: `SceneSettings.h` (default = fonte de verdade) → `editor/Stream.cpp`
  encode **e** decode → `editor/Catalog.h` três tabelas (`getSceneProperty<T>` `:305`,
  `getScenePropertyDefault` `:637`, setter `:867`) → rows em `editor/window/Properties.cpp`
  (`drawScenePropertyRow`, bloco `:14506-14800`) → `CoreClassesLua.cpp` para Lua.
- **propriedade de componente** (`windInfluence`, `fetch`): `WaterComponent.h` →
  `editor/Catalog.cpp:549-598` (nome/tipo/propriedades) → `editor/Stream.cpp:6513-6519` write /
  `:6574-6580` read → `editor/Factory.cpp:1657-1663` → `editor/window/Properties.cpp:8855-8890`
  → `engine/core/script/binding/ObjectClassesLua.cpp:465-476` → wrappers em
  `engine/core/object/environment/Water.cpp`.
- **componente novo** (fase 4): `ComponentType` **apenas acrescentado no fim** (o ordinal é bit
  de máscara e `Signature` tem teto de 64), `EntityRegistry.cpp` (`registerComponent<`),
  `engine/CMakeLists.txt`, `engine/libs/.../project.pbxproj` do Xcode, `QUANTUM_EDITOR_SOURCES`,
  `ComponentAddDialog.cpp`, binding Lua, `engine_api_suggestions.h`.

---

## Riscos

1. **Mudar o default de uma cena existente.** Mitigado por `windEnabled = false` e por
   `windInfluence` só ter efeito com vento ligado. Toda cena `.scene` pré-existente precisa
   renderizar byte-identica antes e depois - o probe da fase 3 captura isso.
2. **`crestFoam` já é enganoso**: `water.frag:437` multiplica por 3 antes do clamp, então um
   `crestFoam` acima de ~0.33 deixa toda célula molhada branca. O termo de whitecap entra **no
   máximo**, não somado sem teto.
3. **`sumWaterWaves` é um espelho CPU do `water.vert`** (`RenderSystem.cpp:7324`, contrato em
   `water.vert:9-10`). Qualquer mudança de forma de onda sem atualizar os dois faz a física
   flutuar num lugar e o pixel desenhar noutro. Fase 3 mantém os dois; fase 6 é onde isso morde.
4. **Cache de shaders**: a fase 6 exige bump de versão em `App::getUserShaderCacheDir()`; sem
   isso o usuário vê o shader velho de um `.sdat` em disco e o bug parece não existir.
5. **`_HAS_EXCEPTIONS=0`** (`engine/libs/joltphysics/CMakeLists.txt`, PUBLIC): nada no `Wind`
   pode lançar através de fronteira de task/future.
6. **Escala**: o sim de água é em **metros**; `windSpeed` também é em m/s. Misturar com
   `pointsToMeterScale2D` (64, não serializado) num projeto 2D produz números sem significado.

---

## Divisão de trabalho para agentes

- **Agente A (fases 1+2)**: só `SceneSettings`/`Wind.{h,cpp}`/registro de setting + probe. Não
  encosta em `RenderSystem.cpp`.
- **Agente B (fase 3)**: `RenderSystem.cpp` bloco de água + `WaterComponent` + checklist de
  propriedade + captura PNG. Começa **depois** do A entregar, porque lê os campos do setting.
- **Agente C (fase 4)**: `WindZoneComponent` e o registro de componente novo - o maior número de
  arquivos, nenhum deles nos do A/B, exceto `Catalog`/`Stream`, que precisam de merge explícito.
- Escrita simultânea no mesmo diretório de skill/registry é proibida: verificar `mtime` antes
  (regra da casa) e nunca `git add -A`.
