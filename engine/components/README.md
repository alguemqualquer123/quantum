# Biblioteca de componentes prontos (Lua)

61 scripts de gameplay prontos para arrastar na cena e ligar sem escrever código, mais 10 demos que
usam esses componentes para montar praia, oceano, cachoeira, céu completo com sol, lua e nuvens, ciclo
de dia/noite, tempestade (chuva, neve, granizo e relâmpago com trovão atrasado), fogo que pega e se
espalha, física real (corda, estilingue, queda, zona de gravidade, captura com o mouse), um pack de
assets e um personagem montado em osso — cabelo, sobrancelha e partes penduradas num esqueleto
animado, com mola.

Ela vive **dentro da engine**, em `engine/components/`, e por isso vai junto com o editor (o alvo
`copy_quantum_editor_engine` copia `engine/` inteiro ao lado do `.exe`). Projeto novo já recebe a
biblioteca; projeto existente recebe por um comando, sem abrir o editor.

Nenhum componente é um tipo C++ novo: cada arquivo é um script Lua comum do quantum — só que escrito
para ser **ligado pelo Inspector**, com campos que recebem entidades arrastadas. São 937 propriedades
declaradas no total.

Cada componente é autossuficiente (não usa `require`): o editor executa o arquivo para descobrir as
propriedades, e um `require` quebrado faria todas as linhas do Inspector sumirem em silêncio. Os
pequenos helpers duplicados entre arquivos são intencionais. Os **demos** são a exceção: eles usam
`require` e precisam da biblioteca instalada. `Burnable` é a única exceção entre os componentes, e
usa `require` só dentro de `pcall`, chamando na hora de acender — nunca no topo do arquivo, que é
onde o scan de propriedades do editor leria o erro.

## Instalar no projeto

**Projeto novo** — automático: `createNewProjectAt` chama `installReadyMadeComponents` logo depois de
`copyEngineApiToProject`. Os 61 arquivos aparecem em `<raiz Lua>/<subdir>/` e os 11 arquivos de demo
(10 `demo_*` + o driver comum `kit_demo.lua`) em `<raiz Lua>/demos/`, e os sprites, sons, mapas de
terrain e shaders que a biblioteca usa vão para `<raiz do projeto>/assets/` — incluindo o fork
`assets/shaders/sky_daynight.{vert,frag}` **com o `.sdat` já compilado ao lado**, porque o runtime não
compila shader (armadilha 28).

**Projeto existente** — na linha de comando:

```bash
build/Debug/quantum-editor-cmd.exe components list
build/Debug/quantum-editor-cmd.exe components install --project "C:/caminho/MeuProjeto"
```

`install` nunca escreve por cima de um arquivo que o projeto já tem (a cópia do projeto é do usuário
a partir daí); use `--overwrite` para substituir.

| Flag | O que faz |
|---|---|
| `--lua <dir>` | raiz Lua onde instalar (padrão: `luaDir:` do `project.yaml`) |
| `--into <subdir>` | subpasta dentro da raiz (padrão: `Components`) |
| `--only FollowCamera,Health` | instala só alguns — **e não instala demos**, porque um demo precisa de componentes que o projeto não teria |
| `--no-demos` | instala os 58 sem os demos |
| `--overwrite` | substitui arquivos que o projeto já tem |

## Rodar um demo sem abrir o editor

```bash
mkdir -p /tmp/KitLive/lua
cp -r engine/components /tmp/KitLive/lua/Components        # ou use `components install`
cp engine/components/demos/*.lua /tmp/KitLive/lua/demos/
printf 'name: KitLive\ncanvasWidth: 640\ncanvasHeight: 360\nluaDir: lua\n' > /tmp/KitLive/project.yaml

KIT_DEMO_FRAMES=300 build/Debug/quantum-engine.exe --project /tmp/KitLive \
    --scene demos/demo_beach --window 1280x720
```

Duas coisas que já pegaram ninguém:

- **`quantum-engine` não lê `luaDir:` do `project.yaml`.** A cena é resolvida em
  `<projeto>/lua/<cena>.lua`. Num projeto de template, que vem com `luaDir: .`, instale com
  `--lua lua` para conseguir rodar demo pela linha de comando — ou rode pelo editor, que respeita o
  `luaDir`.
- **`KIT_DEMO_FRAMES` limita a duração** e faz o demo chamar `System.quit()` sozinho. Sem isso, a
  janela roda para sempre e, se você matar o processo por tempo, o `stdout` fica no buffer e o log
  sai vazio — o erro existiu mas você não vê.

Os sete demos: `demo_beach` (praia de um script só), `demo_ocean` (mar aberto, caixas que flutuam
e nadador), `demo_daynight` (relógio + tempestade + balsa + mastro que cai na água), `demo_storm`
(precipitação visível com chuva/neve/granizo, relâmpago com trovão atrasado, névoa e teclas),
`demo_fire`
(fogo que se espalha por combustível),
`demo_asset_pack`
(carrega um modelo texturado de cada categoria do pack da engine e confere as maps do chão),
`demo_rope`
(corda, estilingue, caixa que cai, zona de gravidade, *grabber*). Os sete fazem
`require("demos.kit_demo")`: o driver comum que cria as entidades da cena, segura os wrappers
(armadilha 1) e limita a duração. Ele é copiado junto porque sem ele nenhum demo roda.

`demo_waterfall` é o oitavo e **não** usa `kit_demo`: ele é o próprio projeto `templates/waterfall`
e precisa dos dois mapas gerados por `scripts/gen_terrain_maps.py`, então roda assim:

```bash
WATERFALL_SHOT=wide WATERFALL_PROFILE=1 build/Debug/quantum-engine.exe \
    --project templates/waterfall --window 1280x720 --frames 200 --capture-frame 195 \
    --screenshot /tmp/waterfall.png
```

## Anexar e configurar

No editor, com a entidade selecionada:

1. **Inspector → `+ New component`**: os 61 componentes da biblioteca estão no fim da lista,
   agrupados sob **Ready-made scripts** (busque por nome — `rope`, `hud`, `swim`…).
2. Clicar instala `Components/<Nome>.lua` no projeto quando ainda não houver e cria a linha de
   script com a seção **Properties** já montada; numa entidade sem `ScriptComponent` ele é criado
   junto. Tudo isso é um comando só, então **Ctrl+Z** desfaz o anexar.
   O caminho de sempre continua valendo: **Scripts → `+ Add Script` → `Lua Script`** e escolher o
   arquivo em `Components/`.
3. Arraste a entidade do slot (Target, Camera, Ground, Water, Pool…) — é assim que tudo se liga.

**O nome do arquivo é o nome da classe** (`FollowCamera.lua` ⇒ módulo `FollowCamera`). Renomear o
arquivo desliga o componente. Se você editar um componente, reabra a cena ou clique no script para
o editor reler as propriedades.

## O que chega em Lua quando você arrasta uma entidade

Um slot vira, em `self.<nome>`:

| Declaração no `properties` | O que a script recebe |
|---|---|
| `type = "entity"` | a **tabela do primeiro script habilitado** da entidade-alvo |
| `type = "Health"`, `"WaterWaves"`, `"CameraShake"`, `"Spawner"`, `"ScreenFade"`, `"SoundOnEvent"`, `"ImpactEffect"`, `"ParticleBurst"`, `"PatrolPath"` | a **instância exatamente desse componente** |
| `type = "camera"`, `"text"`, `"progressbar"`, `"body3d"`, `"terrain"`, `"object"`… | o **handle do motor** para aquele componente |
| `type = "EntityReference"` | ⚠️ nada reconhecível: um handle genérico **e um aviso no log** |

Última linha é a armadilha real: `EntityReference` é o nome do tipo C++, não um dos nomes aceitos,
então o motor não sabe o que entregar. Use `"entity"`, o nome do seu componente, ou o nome do
handle. Toda esta biblioteca usa só esses três formatos, e por isso os componentes aceitam qualquer
um deles:

```lua
-- Forma universal de ler um slot, usada em todos os arquivos:
local function entityId(reference)
    if type(reference) == "number" then
        if reference == 0 then return nil end
        return reference
    end
    if reference and type(reference.entity) == "number" then
        if reference.entity == 0 then return nil end
        return reference.entity
    end
    return nil
end
```

Para chamar um método de outro componente, verifique antes — o slot pode estar vazio:

```lua
local m = self.target and self.target.takeDamage
if type(m) == "function" then self.target:takeDamage(25, self.entity) end
```

## As armadilhas provadas

Estas não são teoria: cada uma derrubou um script ou um teste nesta biblioteca, e a prova está nos
comandos do fim do arquivo.

**1. O wrapper Lua é dono da entidade.** `EntityHandle(Scene*)` marca `entityOwned = true` e o
destructor chama `scene->destroyEntity(entity)` (`engine/core/object/EntityHandle.cpp:37-56`, o
construtor com `Entity` passado é que é `entityOwned = false`). Então:

```lua
local water = Water(scene)          -- cria a entidade e a possui
local id = water.entity             -- se só isto sobreviver, a entidade morre no próximo collect
```

Depois do GC, todo acesso àquele id vira `(ERROR): Retrieving non-existent component` com
`invalid unordered_map<K, T> key` (`engine/core/ecs/ComponentArray.h:231`). Mantenha o **wrapper**
referenciado, não só o número. Os demos fazem isso com `Kit.hold(...)`.

**2. A engine entrega userdata, não tabela.** `Vector2/3/4`, `Color`, handles de entidade e o `Scene`
chegam como userdata do LuaBridge. `type(v) == "table"` nunca é teste de valor, e indexar userdata
por membro que não existe **dá erro** (não `nil`). Todo leitor de valor desta biblioteca aceita os
dois: primeiro `v.x`, depois os chamadores `v:getX()`, `v:X()`, `v:get(x, y, z)`, `v:toTable()`, e só
então `v[1]`.

**3. `Body3D` expõe damping persistente.** `linearDamping` / `angularDamping` são propriedades
Lua armazenadas no componente, serializadas na cena e aplicadas ao corpo Jolt. Valores negativos
são normalizados para zero; corpos estáticos conservam o valor autoral, mas não possuem
`MotionProperties` para atualização em tempo real.

**4. `Joint3D`/`Joint2D` preservam `EntityHandle` no binding.** Os dois usam
`deriveClass<..., EntityHandle>` e podem ser construídos com `scene, entity`; a criação e a
simulação de joint fixo 3D têm smoke runtime. O caminho Lua deve manter o componente `Joint3D`
na entidade para que o sistema carregue a junta durante o passo de física.

**5. `Scene:setPostProcessUniform` retorna `void`** e loga `(ERROR): Post-process pass N does not
exist` a cada chamada contra um pass que a cena não tem (`CoreClassesLua.cpp:1008` lista os passes).
Consulte `#scene.postProcessPasses` antes de escrever, como `ScreenFade:passAvailable()` faz — se
não, é uma linha de erro por frame.

**6. Tipo de corpo antes de `loadBody3D`.** `Body3D:setType` chama `SetMotionType(Activate)` do Jolt
num corpo **já adicionado**, o que dá assert no engine Debug e derruba o processo (exit 3, com o
`stdout` perdido). Passe `false` para estático, `BodyType.KINEMATIC` para plataforma, nada para
dinâmico — a helper `Kit.addBoxBody(scene, id, w, h, d, bodyType)` já resolve isso.

**7. Projeto sem shaders pré-compilados não sobe.** Faltando as variantes `.sdat` em
`assets/shaders`, o `quantum-engine` sai na hora com exit 1 e o bloco `Quantum is missing precompiled
shaders`. O `kit_check` não roda num projeto vazio por isso — o fixture precisa de `assets/shaders`.

**8. Só `init()` é chamado pelo motor**, e slot vazio não entrega `nil` — entrega um handle. Por isso
todos os guards aqui são `type(x) == "table" and type(x.m) == "function"`.

**9. `Camera:setTarget` só aceita três números.** O binding registra um único overload,
`void (float, float, float)` (`engine/core/script/binding/ObjectClassesLua.cpp:275`); o setter de
`Vector3` é a **propriedade** `camera.target` (`:274`). Um `camera:setTarget(Vector3(...))` morre por
frame com `Error decoding argument #3: The lua object can't be cast to desired type`. O `Types.lua`
gerado mostra os dois caminhos, então copie dele só a propriedade. O harness agora recusa o método
com argumento único.

**10. O Lua da engine é 5.4 sem `LUA_COMPAT_MATHLIB`.** `math.atan2`, `math.pow`, `math.cosh`,
`math.log10`, `table.getn`, `loadstring`, `setfenv` respondem `nil` ali dentro mesmo que um `lua54`
de distribuição (5.4.8 com compat) os tenha — o interpretador de teste não é a engine. Use
`math.atan(y, x)` e `x ^ y`. A falha aparece como `attempt to call a nil value (field 'atan2')`, uma
por frame.

**11. Handle de componente não é o componente.** `Camera(scene, id)` e `Body3D(scene, id)` só
    carimbam o par (cena, entidade): nenhuma propriedade existe até a entidade ter o
    `CameraComponent`. Ler/escrever uma propriedade de componente numa entidade que não o tem cai em
    `ComponentArray::getComponent`, que loga `(ERROR): Retrieving non-existent component: invalid
    unordered_map<K, T> key` **e relança** — virando erro Lua (`engine/core/ecs/ComponentArray.h:225-233`).
    Como não há "tem componente?" em Lua (`Signature` não é bound), o teste é um `pcall` lendo uma
    propriedade exclusiva do componente, e ele tem de rodar **uma vez no `init`**, nunca por frame:
    é o que `FollowCamera:cameraOf()` faz, e a razão de o script aceitar slot de câmera, entidade
    própria, `scene.camera` e nome, nessa ordem.

**12. O chão de um terrain não é a origem da entidade.** Um `Terrain` com `terrainSize 100` e
    `maxHeight 5` deslocado em `y = -5` tem superfície entre -5 e 0 conforme o heightmap, então usar a
    altura da entidade como altura do chão afunda o personagem (ou o deixa flutuando) em até
    `maxHeight` metros. A sonda é um raio para baixo — `Ray(o, d):intersects(scene, RayFilter.BODY_3D)`
    — que só responde se o terrain **tiver um Body3D estático com shape `heightfield`** (source
    `entity_heightfield`). `CharacterController3D` faz isso e guarda o último valor válido
    (`groundHeight`) para não cair quando o raio perde a referência.

**13. A entidade de um `Particles` não tem Transform.** Quem ganha Transform é quem deriva de
`Object`: `Points`, `Lines`, `Light`, `Mesh`, `Terrain`, `Model`. `Action(scene)` e
`Particles(scene)` só adicionam `ActionComponent`/`ParticlesComponent` (`engine/core/object/Action.cpp`,
`Particles.cpp`), e `Sound(scene)` tampoco — o formulário que adiciona é `Sound(scene, true)`. Escrever
`position` na entidade da ação dá `(ERROR): Retrieving non-existent component: invalid
unordered_map<K, T> key` **uma vez por frame**. Em `Precipitation`, por isso, o `followCamera` move a
entidade **`Points`**, e `particles.localSpace = true` é o que faz os drops vivos viajarem com ela.

**14. O pool de partículas só existe depois que a ação começa.** `ActionSystem::particleActionStart`
(`engine/core/subsystem/ActionSystem.cpp:1121`) é o único lugar que aloca `particles.particles` e
`points.points`, e ele dimensiona os dois por `particles.maxParticles` — que vale **100** por padrão
(`engine/core/component/ParticlesComponent.h:237`) e é copiado para `points.maxPoints` (`:1171-1172`).
Consequências provadas: `rate` alto com pool pequeno rende exatamente 100 drops; e um emissor montado
sem `restart()` nunca aloca nada (fica em `0 drops`). Por isso `Precipitation:apply()` termina em
`restart()`, e `maxParticles` foi exposto em Lua (`ActionClassesLua.cpp`, ao lado de `rate`).

**15. Uma `Camera` criada por script não é a câmera da cena, e é de UI com *far clip* de 10.**
`CameraType::CAMERA_UI` e `DEFAULT_ORTHO_FAR` (`engine/core/component/CameraComponent.h:37,63`) são os
padrões do componente, e nada no engine os corrige para uma cena montada em Lua. Um demo 3D sem
`setPerspective` renderiza o mundo inteiro além de 10 metros fora da tela. `Kit.camera3D(camera, yfov,
near, far)` resolve: chama `Camera::setPerspective` (que **desliga** `autoResize`,
`engine/core/object/Camera.cpp:73-86`) e religa, então a proporção continua acompanhando a janela.
E falta a outra metade: a cena renderiza pela câmera que `Scene::getRenderCamera()` devolver
(`engine/core/Scene.cpp:161-163`), que é a câmera default criada por `createDefaultCamera()` — a de UI
com *far* 10 — até alguém chamar `scene:setCamera(camera)`. Sem isso a sua câmera 3D existe, responde
`.farClip`, move-se, e **nada** a usa: os testes de distância de renderização medem a câmera errada.
Os demos chamam; `probe_bench` e `probe_render_distance` também chamam.

**16. Terrain feito por script precisa de câmera **e** de heightmap.** `MeshSystem::createOrUpdateTerrain`
recusa sem câmera na cena (`engine/core/subsystem/MeshSystem.cpp:6300`) e recusa com
`terrain.heightMap.empty()` (`:6340`); as duas saídas chamam `resetTerrainToUnbuilt`, que zera
`needUpdateTerrain` **para sempre** (`:3136`). O resultado não é um terrain plano: é
`(WARN): Mesh of 'Terrain' has failed to load 8 times` rodando para sempre. Ordem certa:
`scene:setCamera(camera)` → `terrain:setHeightMap("...png")` → deixar o laço do engine gerar (ele
chama `createOrUpdateTerrain` por frame em `:6510-6521`, então `createTerrain()` manual não é
necessário e, chamado cedo demais, é justamente o que queima a flag). O **relevo** é deslocamento no
vertex shader; a malha CPU é o plano subdividido.

**17. Um número solto é lido como id de entidade.** Os helpers de slot aceitam tabela-script, handle
userdata e id cru, e `entityId()` trata **qualquer número ≠ 0** como id. `Lightning:strike(120)` virava
busca pela entidade 120 — `(ERROR)` por raio. Por isso `strike()` resolve na ordem: número = distância,
tabela com `x` = ponto no chão, resto = entidade, nada = escolha aleatória.

**18. Ler um componente que a entidade não tem loga um ERROR **antes** de lançar.**
`ComponentArray::getComponent` faz `Log::error("Retrieving non-existent component: %s")` e depois
`throw` (`engine/core/ecs/ComponentArray.h:224-263`). O `pcall` do Lua engole o lançamento, **não** a
linha de log. Foi assim que o `demo_fire` imprimiu 252 erros: `tint` ligava a sonda de `Mesh.color` a
cada passo do carvão numa entidade que só tinha `Transform`. Consequência de projeto: uma sonda de
capacidade tem que **se desligar sozinha no primeiro falho**, senão o spam é proporcional ao tempo de
jogo. É o que `Burnable:charTick()` faz (`self.tint = false` + um aviso).

**19. `Health` recusa o segundo hit dentro de `invulnerableTime` (0,2 s).** Um DPS contínuo que chama
`takeDamage(1)` a cada frame vira 5 hits por segundo, não 60. `Fire:heatTick` e `Burnable:selfDamageTick`
acumulam o dano e pagam em blocos de ≥0,25 s; quem escrever outro dano por área tem que fazer o mesmo.

**20. Da `Scene` só existem `getEntityName` e `isEntityCreated`.** `getName`/`isCreated` **não** estão no
binding (`CoreClassesLua.cpp`), e chamados dentro de `pcall` devolvem nil em silêncio — o que é pior do
que um erro. O laço de propagação do `Fire` percorre o registro e usa `isEntityCreated` para podar; sem
ele, uma confusão de nome faz o fogo esquecer todos os combustíveis do cenário. Os stubs de
`checks/kit_harness.lua` agora respondem os dois exatamente por isso: um harness que não os tem deixa
passar justamente essa classe de bug.

**21. Combustível se anuncia por um global, `_G.__quantumFire.burnables`.** Lua não tem consulta de overlap
nem de cena, e um slot por vizinho seria autoraria inviável. O preço: um estado de script sobrevive à
cena que o preencheu, então **cada** entrada é checada contra a cena antes de uso, e uma morte fora da
lista marca a tabela inteira como velha. Componente nenhum deve presumir que uma entrada ali é viva.

**22. `addInstance` não dá colisão, nem corpo, nem identidade.** A cópia vive num
`InstancedMeshComponent` da entidade hospedeira (`engine/core/object/Mesh.cpp:780`), então ela é desenhada
mas não existe para a física nem para `Ray`: uma multidão de caixas instanciadas é atravessável, e um
inimigo que atira nela acerta o nada. Use instância para cenário/efeito e entidade para o que interage.
Segundo detalhe: `maxInstances` não é um teto que descarta — estourar dobra o valor e marca o `Mesh` para
recarregar (`:786-790`). Terceiro: as linhas de ajuste (`maxInstances`, `cullDistance`, `distanceFade`) só
colam depois de `createInstancedMesh()`, e um lote vazio **não desenha nada** — a ordem certa é criar,
dimensionar e encher no mesmo passo.

**23. Um corpo estático não tem *motion properties*, e os setters da engine agora recusam em vez de
morrer.** `Body::SetAllowSleeping`/`SetMass` da Jolt dereferenciam `mMotionProperties` sem checagem
(`Body.cpp:74`), e a Jolt só aloca isso para corpo dinemático/cinemático — num `STATIC` o ponteiro é
null e a escrita cai em endereço 0. Prova: um `MeshCollider` estático que definia `allowSleeping` depois
de `loadBody3D` derrubava o processo inteiro (`quantum-engine.exe` saía 139). Hoje `Body3D::setAllowSleeping`
/log::verbose/ e `isAllowSleeping` devolvem o padrão, e `setMass` avisa e ignora. Consequência prática: em
`isStatic = true`, `mass` e `antiTunnel` não fazem nada — são propriedades de quem se move.

**24. `Kit.attach` não inscreve duas vezes, mas só se o componente avisar.** O `init()` de todo
componente chama `RegisterEngineEvent(self, ...)` (regra do motor: só `init` é despachado sozinho), e o
`Kit.attach` inscrevia `onFixedUpdate`/`onUpdate` por cima — `RegisterEngineEvent` não reporta duplicata,
então o handler rodava duas vezes por passo e qualquer retry cronometrado queimava o orçamento dobro. Um
componente que se inscreve no `init()` registra `self._registeredEvents = { onFixedUpdate = true }` e o kit
respeita a marca; `CapsuleCollider` e `MeshCollider` já o fazem. Os outros 54 ainda rodam o handler duas
vezes num demo do kit — inocuo hoje porque são idempotentes, mas é armadilha para quem conta tentativas.

**25. `position` não é `rotation`.** O `Transform` do motor devolve `Vector3` para um e `Quaternion`
para outro, e ambos são *userdata* com campos `x, y, z`. Uma sonda que achata os dois numa tabela só e
depois monta um quaternion com `w` + os `x, y, z` daquela tabela está construindo uma rotação a partir
da **posição** — o resultado é um quaternion de norma em metros, que o `toMatrix` da engine normaliza
por divisão e devolve NaN/zero. O `AABB::setExtents` (assert `min <= max`) é onde isso aparece: uma
rotação degenerada escrita num modelo *skinned* derruba o bound por frame. No `HairSwing` a comparação
de quietude usa a tabela achatada, mas o quaternion vem sempre de `qof(object.rotation)`.

**26. Quaternion lido da engine pode não ter norma 1** — `getWorldRotation()` de um osso animado é a
composição de várias rotações misturadas, e `log()`/`exp()` só significam ângulo num quaternion unitário.
Normalizar na leitura (`qof`) custa uma raiz por frame e é o que mantém o resto da matemática correto.

**27. `mode = "follow"` precisa reescrever todo frame.** Diferente de `attach`, que reparenteia e deixa
a hierarquia fazer o trabalho, em `follow` o transform copiado **é** o vínculo: o `apply()` roda em
`onUpdate` sempre. Com o resync preso numa flag opcional, a peça era posicionada uma única vez — na
maioria das vezes antes do `RenderSystem::updateTransform` ter preenchido o transform mundial do osso
(lerada 2: script roda antes do draw) — e ficava parada na origem, 1,55 m abaixo da cabeça, para sempre.

**28. O runtime não compila shader — ele só lê `.sdat`.** `quantum-engine` não instala um
`ShaderPool::setShaderBuilder`, então `ShaderPool::get` resolve `shader://<arquivo>.sdat` dentro de
`<projeto>/assets/shaders/` e, se o arquivo não está lá, o processo imprime a lista de variantes
faltando e sai com 1. Um fork do céu precisa ser **embarcado já compilado ao lado do fonte**. O nome é
determinístico porque `ShaderType::SKYBOX` tem zero bits de propriedade:
`sky_<caminho-relativo-do-projeto-sanitizado>_glsl410.sdat`, não-alfanuméricos virando `_` — no caso do
kit, `sky_assets_shaders_sky_daynight_glsl410.sdat`. Para gerar:

```bash
# 1. declare o fork no shaderKeys: da cena YAML do projeto de trabalho
build/Debug/quantum-resource-compiler.exe shaders --project "C:/abs/caminho/Projeto" --force --verbose
# 2. copie o artefato do cache do usuário para dentro do projeto
cp "$LOCALAPPDATA/quantum/shaders/v40/sky_assets_shaders_sky_daynight_glsl410.sdat" \
   Projeto/assets/shaders/
```

O `--project` tem de ser **caminho absoluto**: relativo é juntado duas vezes e o compilador não
compila nada em silêncio. As variantes embutidas (`mesh`, `water`, `blit`, `depth`) saem direto no
projeto com a outra forma: `shaders --out "<abs>/assets/shaders" --backend opengl --shader
"mesh:Puc,Nor,Vc4,Fog"` (repita o `--shader` por variante).

**29. `customShader` é propriedade, não método.** O binding registra o par getter/setter
(`ObjectClassesLua.cpp:199`), não o setter C++ separado. `sky:setCustomShader("...")` dá `attempt to
call a nil value (method 'setCustomShader')`; a forma é `sky.customShader = "assets/shaders/sky_daynight"`
— sem extensão, sem `.sdat` no caminho.

**30. `Shape(scene, id)` é uma *visão*, não um construtor.** Vale para todo handle de componente
(armadilha 11), mas morde aqui porque `Shape` é a forma de criar primitivo por script:
`Shape(scene, id):createSphere(...)` lê um `ShapeComponent` que a entidade não tem e morre com o ERROR
de `ComponentArray::getComponent` **antes** de lançar. `Shape(scene)` é o construtor que **adiciona** o
componente — e passa a possuir a entidade, então vale o `Kit.hold`.

**31. `SkyComponent::color` continua valendo com um fork no lugar.** O motor calcula
`envColor = sRGBToLinear(sky.color) * globalIlluminationIntensity` independente do que o dome desenha,
e é disso que a água tira o reflexo do céu e a IBL ambiente. Por isso o `DayNightCycle` escreve a cor
do céu mesmo quando o céu procedural assumiu o desenho — remover essa linha deixa o lago refletindo
azul-de-dia às 23 h.

**32. Se você re-centrar o ponteiro, o delta mede contra o ponto do warp.** Em `CAPTURED`,
`Input.getMousePosition()` é a soma do *raw motion* e gira 360° sozinha — `FollowCamera` depende
disso e nunca warpa. Mas um olhar próprio que chama `Engine.setMousePosition` no centro para o giro
contínuo precisa saber que essa chamada **reescreve a posição que a engine reporta**
(`engine/core/Engine.cpp:535-549` → `SystemWin::setMousePosition` rebaseia o router,
`engine/platform/win/SystemWin.cpp:160-165`). Então `d = mp_N − mp_{N−1}` subtrai o warp do
deslocamento e dá 0 em movimento constante: varrer a mesa devagar não vira a câmera, mas um tremido
ainda vira — é assim que esse bug escapa do teste casual. O certo é
`d = mp_N − warp_anterior`. Só o voxelworld warpa (os componentes e demos usam a soma crua como
`FollowCamera`); a correção e a sonda estão em `templates/voxelworld-main/scripts/VoxelPlayer.cpp`
(`applyLook`).

## Convenções que valem para todos
- **Ângulos em graus.** `Engine.useDegrees` é `true` por padrão, e os componentes expõem tudo em
  graus (os `displayName` dizem). Yaw é em torno de Y, pitch em torno de X.
- Cada componente chama `RegisterEngineEvent(self, "onUpdate")` (ou `"onFixedUpdate"` para física)
  no `init()`. Scripts seus precisam fazer o mesmo. Se ele também for anexável pelo kit, marque
  `self._registeredEvents = { onUpdate = true }` antes — ver armadilha 24.
- `Engine.deltatime` é limitado a 0.05s por componente, para um frame longo não arremessar nada.
- Slots vazios **nunca dão erro**: o componente avisa no log (`[FollowCamera] …`) e segue com o
  fallback pelo nome da entidade, quando houver. O aviso de configuração é uma vez só por instância;
  o aviso de runtime repetitivo (ex. `Rope` sem Joint3D) é limitado a 1/s.
- Nada aqui escreve em `scene`/arquivos: um componente pode ser removido sem deixar rastro.
- Abaixo da linha de resumo, cada arquivo declara o seu grupo numa linha `-- category:` (ex.
  `-- category: Water & Shore`). É essa linha que posiciona o componente numa categoria do diálogo
  *New component* do editor e no `components list`; sem ela ele cai em "Scripts"/"Ungrouped". Os
  nomes usados hoje: `Water & Shore`, `Physics`, `Environment & Motion`, `Camera & Controllers`,
  `Interaction & Combat`, `Scene & HUD`, `Audio & Particles`, `Screen Effects`, `Characters`.

## Componentes

### Água e mar

| Arquivo | Onde vai | O que faz | Slots arrastáveis |
|---|---|---|---|
| `WaterWaves` | na entidade do **Water** | mar vivo: deriva de vento, rajadas, inclinação acoplada à altura; `setIntensity`/`gust` | `water` (entity) |
| `WaterRipples` | em quem mexe na água | soca perturvação no *shallow field* quando o sujeito acorda ou atravessa a superfície; dispara som e partículas de respingo | `water`, `follow`, `splashSound`, `splashParticles` |
| `BeachShore` | na entidade do **Water** | praia em um knob: preset + `clarity`/`wavePower`/`shorelineFoam` retocando tudo coerente | `water`, `terrain`, `sky`, `fog`, `waves` (WaterWaves) |
| `FloatOnWater` | em qualquer **Body3D** dinâmico | flutuação real: amostra superfície e corrente no centro de massa e chama `applyBuoyancyImpulse` | `water`, `onFloatEntity`, `onSinkEntity` |
| `SwimController` | junto de um **Body3D** | nada: sobe/mergulha com histerese, steering dentro d'água, devolve o drag ao sair | `water`, `waves` (WaterWaves), `splashEntity` |
| `Waterfall` | em qualquer entidade | a **queda visível**: lençol de partículas atiradas do lábio até o ponto de impacto, mais o volume que o `Water` de cima descarrega no de baixo. `plunge` é a velocidade para fora no lábio — sem ela a alturafield faz rampa e o lençol cai *dentro* da rocha. `setFlow`, `getMovedVolume`, `getCount` | `source`, `target` (Water) |

### Física real

| Arquivo | Onde vai | Requisitos | Observações |
|---|---|---|---|
| `CapsuleCollider` | em qualquer entidade | — | corpo cápsula do zero: escolhe eixo (Y por omissão, Unity `height` = cápsula inteira), densidade, fricção, `isTrigger`, escala correta; `apply()`/`rebake()` e `setSize()` recusa um corpo já cozinhado |
| `MeshCollider` | na entidade com **Mesh** ou **Model** | a malha do próprio corpo, ou os nós do modelo | casco convexo (o único que pode ser dinâmico) ou *triangle mesh* para cenário; coze quando a geometria chega, retry a cada 0.5 s até `maxRetries`; `rebake()` depois de trocar o modelo |
| `Rope` | na entidade com **Joint3D** | pivot: `anchor` + `subject` | junta Jolt de verdade; `ropeLike` = pivô+clamp de comprimento, off = junta de distância; `reel`, `getTension`, `breakLength` |
| `Slingshot` | no lançador | `subject` com Body3D | elástico invisível, força de mola real e um impulso no disparo; arrasta com o mouse ou tecla |
| `FallingObject` | no objeto que cai | Body3D | dorme até algo chegar perto, cai com gravidade e machuca na aterrissagem; anti-túnel, `respawnTime` |
| `GravityZone` | na zona | — | caixa ou esfera com gravidade própria: reescreve `gravityFactor` dos corpos dentro e restaura ao sair |
| `PhysicsGrabber` | na câmera | Body3D nos alvos | raio de tela escolhe o corpo e arrasta com impulso amortecido, então o arremesso continua colidindo |

### Ambiente e movimento

| Arquivo | Onde vai | O que faz |
|---|---|---|
| `DayNightCycle` | na entidade da **Light** direcional (ou ligue Light/Sky/Fog) | um relógio move sol, **lua**, céu, névoa e ambiente juntos. Fases nomeadas (`getPhase()` → `morning`/`day`/`afternoon`/`dusk`/`night`), ganchos `onSunrise`/`onSunset`/`onHour`/`onPhase`, e um `moonPhase` 0..1 que põe a lua no círculo do sol com o alongamento certo — então lua cheia nasce quando o sol se põe e a terminadora é calculada no shader, nunca um número que possa discordar da geometria. `fogNear`/`fogFar` são propriedades porque a distância certa é da cena, não do relógio |
| `Clouds` | na entidade do **SkyBox** (ou no slot `sky`) | a camada de nuvens **separada** do relógio: cobertura, altitude, escala, detalhe, suavidade, espessura, duas camadas (cúmulo + cirro), vento com rajada e tinta. Quatro presets (`clear`/`few`/`overcast`/`storm`) escritos nos mesmos uniforms que o `DayNightCycle` preenche, então os dois scripts podem morar na mesma cena sem um sobrescrever o outro por engano |
| `WeatherSystem` | em qualquer entidade | intensidade única que fecha a névoa, chove no *shallow field*, faz ondulação, liga partículas, ambiente e rajadas |
| `Precipitation` | em qualquer entidade | a **precipitação visível**: chuva, neve e granizo desenhados como sprites (`Points`) movidos por `Particles`, centrada na câmera e varrida pelo vento. Cria as duas entidades sozinho. Slots: `weather` (WeatherSystem). `setKind(0..2)`, `rain/snow/hail`, `setIntensity`, `setWind`, `start/stop`, `getCount` |
| `Lightning` | em qualquer entidade | relâmpago com trovão **atrasado pela velocidade do som**: flash de luz POINT com várias piscadas, zig-zag de `Lines` com garfos, estalos em rolada. Cria luz, raio e som sozinho; cutuca `CameraShake` e `ScreenFade` quando ligados nos slots `shake` e `fade`. `strike(alvo)`, `start/stop`, `getStrikeCount`, `getDistance` |
| `WindZone` | em qualquer entidade | fonte da verdade do vento: direção com deriva e rajadas, repassada ao `WaterWaves` e empurrada nos corpos ligados |
| `WindForceController` | em qualquer **Body3D** dinâmico | controller reutilizável que amostra `Wind.sample` por corpo e aplica força relativa ao vento, com área, arrasto, atenuação por altura e limite de velocidade |
| `WindForceController2D` | em qualquer **Body2D** dinâmico | versão 2D do controller de vento: amostra o campo determinístico, aplica arrasto relativo no centro de massa, atenuação por altura e limite de velocidade |
| `FloatOnWater2D` | em qualquer **Body2D** dinâmico | buoyancy 2D baseada em plano de água configurável, onda viajante opcional, arrasto submerso e limite de velocidade; não depende da água 3D |
| `MovePlatform` | na plataforma | vaivém cinemático entre duas entidades-âncora (ou um offset), carregando passageiros |
| `GamepadMover` | no player com Body3D | gêmeo de `PhysicsMover3D` para controle: stick esquerdo, sprint no gatilho, A pula |
| `Fire` | em qualquer entidade | **fogo de verdade, criado pelo próprio script**: chama `Points`+`Particles` em três camadas (chama, fumaça, brasa), uma `Light` POINT que pisca com três senos incomensuráveis, loop de crepitar em `Sound`, relógio de combustível, calor que machuca um `Health` e propaga para os `Burnable` do raio. A chuva apaga. Cria todas as entidades sozinho |
| `Burnable` | no objeto que pode pegar fogo | combustível: aceita o calor vizinho (`igniteChance`/`flammability`), acende o `Fire` **dele próprio** por `require`, escurece o `Mesh` conforme carvona, consome o combustível, some quando vira cinza (`burnAway`) e molha com a chuva |

### Câmera e movimento clássico

| Arquivo | Onde vai | Observações |
|---|---|---|
| `FollowCamera` | na entidade **que tem o Camera** | câmera que segue, orbita com o mouse (ou `CAPTURED`, com TAB liberando) e amortece; aceita `shake`; slot `camera` para por o script numa entidade qualquer |
| `PlayerController` | em qualquer entidade com **Transform** | preset completo: cria cápsula dinâmica, `builtin://BananaMan.glb`, câmera 3D e estados Idle/Walk/Run/Crouch/Jump/Fall; nomes de animação editáveis |
| `VehicleController` | na entidade com **Body3D** dinâmico | chassis com quatro pontos de suspensão, mola/amortecimento, raycast de chão, tração, aderência lateral, esterço, freio e limite de velocidade; usa WASD/setas |
| `OrbitCamera` | na entidade da câmera | *turntable* com zoom no scroll e giro automático |
| `CameraShake` | na câmera ou em qualquer entidade | trauma com ruído; publica `offset`/`roll` para as câmeras lerem |
| `Camera2DFollow` | na entidade da câmera 2D | segue ortho com zoom, *deadzone* e limites de mundo |
| `CharacterController3D` | no player | cinemático por Transform: ache o terreno com um Body3D `heightfield` embaixo, que a sonda de raio usa a altura real (`groundY` só é o chão quando não há colisor); paredes ele atravessa; eixos relativos à câmera |
| `PhysicsMover3D` | no player | steering por `linearVelocity`; `jumpHeight` em metros; coyote time |
| `TopDownController` | no player/inimigo | move em X/Z, olha para onde anda, altura travável |
| `PlatformerController2D` | no player 2D | velocidades em **unidades de mundo** (não m/s); pulo curto ao soltar a tecla; vira o sprite |

### Interação, fluxo e combate

| Arquivo | Onde vai | Notas |
|---|---|---|
| `Interactable` | na coisa que se usa | encosta e aperta a tecla: porta, alavanca, baú; widgets de prompt nos slots |
| `ScenePortal` | na porta/saída | pré-carrega o destino enquanto o player anda, só troca quando os assets estão prontos; usa `ScreenFade` |
| `PatrolPath` | no mover | rota entre waypoints nomeados ou arrastados; loop, pingue-ponde, pausa em cada |
| `LineOfSight` | no observador | visão com cone, alcance e raio contra os corpos no meio; pode perseguir |
| `DamageArea` | no perigo | lava/gás/espinhos por caixa ou esfera com dano por segundo, empurrão e efeitos |
| `Health` | em tudo que apanha | publica `current`, `percent`, `alive`; `takeDamage/heal/kill/setHealth`; invulnerabilidade, regeneração |
| `Projectile` | na entidade do tiro | acerto por **distância** ao `target`; `launch(position, direction, target)`; some ao expirar |
| `Spawner` | na entidade "arma" | pool = **entidades já na cena**; duplique o projétil e arraste esta entidade no slot `spawner` delas |
| `Lifetime` | em efeito/flecha | expira, some ou é destruído; pertencendo a um `Spawner`, devolve o slot ao pool |
| `Pickup` | no item | raio em torno de si; cura via componente `Health`; reaparece após `respawnTime` |
| `TriggerZone` | na zona | entrada/saída/permanência por raio (altura opcional); ganchos `onEnter`/`onExit`/`onStay` |

### Cenário e HUD

| Arquivo | Onde vai | Notas |
|---|---|---|
| `Rotator` | qualquer entidade com Transform | giro contínuo por eixo |
| `Bobber` | idem | sobe e desce em seno, com balanço lateral opcional |
| `LookAt` | idem | vira para o alvo com velocidade de giro, só yaw se preferir |
| `Timer` | entidade "cérebro" | chama `method` em `target` a cada `interval`; `start/stop/reset` |
| `HudValue` | entidade UI com **Text** | escreve `source.<field>` formatado (`prefix`, `suffix`, `decimals`, `asPercent`) |
| `HudBar` | entidade UI com **Progressbar** | preenche de `minValue` a `maxValue`, com amortecimento |

### Áudio e partículas

| Arquivo | Onde vai | Notas |
|---|---|---|
| `AmbientSound` | na entidade com **Sound** | loop 3D que some com a distância ao listener e aceita intensidade externa |
| `SoundOnEvent` | na entidade com **Sound** | um-shot que outros componentes cutucam: hit, passo, clique; jitter de pitch e cooldown |
| `MusicPlayer` | em qualquer entidade | trilha com crossfade em dois canais e ducking por estado (`setState("combat")`) |
| `ParticleBurst` | na entidade com **Particles** | emissor reutilizável; `emit(point)` de outro script, cor/tamanho/forma/cone configuráveis |

### GFX

| Arquivo | Onde vai | Notas |
|---|---|---|
| `ScreenFade` | em qualquer entidade | fade, blackout e flash de tela por uma entidade com **Image**, ou por uniforme de post-process (só float) |
| `PostEffectPulse` | em qualquer entidade | respira um uniforme float da cadeia do usuário, ou segue um `Health` e aperta quando a vida cai |
| `ImpactEffect` | em quem apanha | uma chamada para "levou dano": partículas + som + shake + flash por `play(strength, point)` |
| `TrailEffect` | em qualquer entidade | rastro reativo à velocidade: alimenta um **Particles** com taxa, vida e tamanho proporcionais |

### Personagem

| Arquivo | Onde vai | Notas |
|---|---|---|
| `AvatarPart` | na peça (cabelo, capacete, arma) | pendura a entidade num osso do corpo pelo **nome**: `mode = "attach"` reparenteia no osso (zero atraso), `mode = "follow"` copia o transform do osso (a peça solta do esqueleto, ex. cabelo cortado); `offset`/`rotationOffset`/`scaleFactor`, `aliases` para o mesmo osso escrito de outro jeito, `detach()`/`rebind()` para scripts de gameplay |
| `HairSwing` | na mesma entidade do `AvatarPart` (modo `attach`) | mola esférica em torno do osso: a peça atrasa ao girar a cabeça, volta ao repouso, e `maxAngle` limita o desvio; `droop` pende para baixo, `wind`/`flutter` com vento, `impulse(eixo, força)` para um soco/salto, `deviation()` para HUD e sondas |

## Receitas

### Praia completa (o que o `demo_beach` faz)

1. Entidade `Water` com o componente **Water** → `WaterWaves` (ondas vivas) e `BeachShore` (areia
   molhada, espuma, céu e névoa pelo preset).
2. Entidades de caixa: **Body3D** dinâmico + `FloatOnWater` + `WaterRipples` (elas mesma fazem a
   ondulação onde passam).
3. Céu e névoa: entidades **SkyBox** e **Fog** arrastadas nos slots do `BeachShore`.

### Corda / estilingue / queda (o que o `demo_rope` faz)

1. `Anchor` com corpo estático + `Ball` com corpo dinâmico + uma entidade que **tenha o componente
   Joint3D** → `Rope` com `anchor` e `subject`.
2. `Stone` dinâmico + `Slingshot` no lançador: puxe com o mouse, o tiro sai pela raio da câmera com
   `power × stretch`.
3. `FallingObject` na caixa (dormindo) + `Health` no boneco embaixo + `ImpactEffect` e `CameraShake`.
4. `GravityZone` na região e `PhysicsGrabber` na câmera para arremessar com o mouse.

### Câmera em terceira pessoa em dois passos

1. Entidade `Player` → `CharacterController3D`.
2. Entidade da câmera → `FollowCamera`, arraste `Player` no slot **Target**.

Pronto: WASD anda, Espaço pula, botão direito orbita. Com **Capture the mouse** ligado (e **TAB**
como tecla de troca), o ponteiro some e o movimento do mouse vira câmera — em `CAPTURED` a engine
entrega ao `Input.getMousePosition()` a soma do *raw motion* (`engine/platform/win/WindowWin.cpp:430`,
`WinInputRouter`), então dá para girar 360° sem o cursor bater na borda da janela. O chão: arraste a
entidade do terreno/chão em **Ground entity** (a altura dela manda) ou escreva `Ground height`; sobre
terrain, o chão real vem da sonda de raio, que exige o Body3D `heightfield` do terreno (armadilha 12).

### Inimigo com vida, visão e barra na tela

1. Inimigo → `Health` (máx 100) + `LineOfSight` (alvo = player, `occlude` ligado).
2. Atirador: `Projectile` na entidade da flecha, `target` = inimigo, `homing` ligado.
3. HUD: entidade com Progressbar → `HudBar`, `source` = inimigo, `field` = `percent`.
4. HUD numérico: entidade com Text → `HudValue`, `source` = inimigo, `field` = `current`.

### Tiro com pool

1. Crie a flecha (Transform + Mesh) e coloque **4 cópias** como filhas da entidade `Arma`.
2. Cada cópia: `Projectile` (ou `Lifetime`) com o slot **Pool that owns this shot** = `Arma`.
3. `Arma` → `Spawner`: `spawnPoint` = boca do cano, `target` = quem deve ser acertado,
   `spawnKey` = tecla, ou `autoSpawn` ligado para metralhadora.
4. Os projéteis se registram sozinhos no `init`; `spawn(n)` entrega os livres.

### Chuva que enche o mar (o que o `demo_daynight` faz)

`DayNightCycle` (relógio) + `WeatherSystem` (intensidade) + `Fog`/`SkyBox`/`Water`/`WaterWaves` nos
slots do `WeatherSystem` + `WindZone` ligado ao `WaterWaves`. Uma intensidade só fecha a névoa, chove
no *shallow field*, ondula a superfície, liga partículas e rajadas.

### Ciclo completo — manhã, dia, tarde, noite, sol, lua e nuvens (o que `demo_sky` faz)

Três peças, e só a primeira existia antes do céu procedural:

1. `DayNightCycle` numa entidade com a `Light` direcional. Ele procura `Light`, `SkyBox` e `Fog` por
   nome (`Sky`, `Fog`) ou pelos slots; sem um `Moon` ligado, a luz noturna volta a ser o próprio sol
   caindo até `moonIntensity` — o comportamento antigo, intacto.
2. `Clouds` na entidade do `SkyBox`. É **um script separado de propósito**: o relógio decide quando é
   dia, e a cobertura de nuvens não é uma função da hora. `setPreset("storm")` no meio da tarde tem de
   funcionar.
3. O fork `assets/shaders/sky_daynight` no `SkyBox`. É aqui que mora o céu: gradiente Rayleigh com
   banda anti-solar roxa, disco do sol, **disco da lua com terminadora ortográfica e mares**, estrelas
   com cintilação, e dois decks de nuvens com sombra interna marchada na direção da luz. O relógio
   alimenta os uniforms por nome (`sunDir`, `moonDir`, `cloudLayer`, …) — ver `quantum-shaders` para a
   costura `u_fs_customParams`.

Sem o fork, os itens 1 e 2 continuam funcionando: o `SkyBox` padrão mostra a cor que o relógio escreve
e os uniforms ficam sem leitor. O fork é um upgrade visual, não uma dependência.

Teclas do demo: `[` `]` hora, `,` `.` preset de nuvens, `-` `=` fase da lua, `1..4` enquadramentos,
`SPACE` pausa, `ESC` sai. Headless, as mesmas coisas entram por `SKY_HOUR`, `SKY_PRESET`, `SKY_MOON`,
`SKY_SHOT` e a captura é um PNG do frame apresentado.

**O céu procedural pode virar arquivo.** `demos/demo_cubemap.lua` desenha o mesmo dome seis vezes, uma
por face de cubo, e `scripts/assemble_sky_cubemap.py assemble <faces> <cross.png>` empacota os seis PNGs
no cross 4×3 que `TextureData::loadCubeMapFromSingleFile` lê de volta — `SkyBox:setTexture
("textures/sky/night_cross.png")` é o caminho inteiro. O projeto já traz um pronto: 23:00, lua cheia,
céu limpo, 2048×1536, 66 KB, em `templates/daynight/assets/textures/sky/`. Vale um cubo por dois
motivos: é o que um bake de lightmap ou uma sonda de reflexão offline precisa, e um cubo com as seis
faces preenchidas é o que `RenderSystem::updateSkyEnvironment` aceita para gerar o `IBLEnvironment`
(irradiância + pré-filtrado) — ou seja, trocar o dome por um cubo noturno dá ao cena luar especular de
verdade, coisa que o dome desenhado por shader não alimenta.

A convenção de linhas é medida, não deduzida: **a borda de cima de cada face é o `+Y` do mundo.** As up
vectors que o motor usa para montar o cubo de sombra de uma luz pontual (`RenderSystem.cpp:9557`, `-Y`
nas faces laterais) produzem um cross que round-tripa pixel por pixel e ainda assim mostra a lua
*abaixo* do horizonte — um teste de ordem de face não pega isso, só o "onde está a luz" pega. `CUBE_DIR
="dx dy dz ux uy uz"` existe para essa segunda checagem: uma vista diagonal a 54° de elevação emenda três
faces de uma vez, e dome e cubo precisam concordar nela.

O projeto `templates/daynight` **é** essa cena: o `lua/main.lua` dele só faz
`require("demos.demo_sky")`, então abrir o projeto e dar Play — ou rodar
`quantum-engine.exe --project templates/daynight` sem `--scene` — cai direto no ciclo rodando. E o
`scenes/main.scene` dele é a mesma coisa montada no editor, entidade por entidade (`Sky`, `Sun`,
`Moon`, `Fog`, `Terrain`, `Lake`, `Cycle`, `Weather`, `Camera`), com os 34 + 17 campos dos dois
scripts preenchidos no Inspector. É YAML escrito à mão: o `doctor` passa limpo (0 erros, 0 avisos),
mas o que ele prova é estrutura e caminhos de arquivo — **a cena aberta no editor não foi vista**,
porque nenhuma regra deste repo permite lançar o editor a partir de um agente.

### Tempestade completa (o que o `demo_storm` faz)

Quatro scripts, nenhum shader escrito e nenhuma textura baixada — os sprites vêm com a biblioteca em
`assets/textures/{rain,snow,hail}.png` e o trovão em `assets/sounds/thunder.ogg`:

1. `WeatherSystem` numa entidade chamada `Weather`, com `fog`, `water`, `sky` e `type`/`targetIntensity`
   /`rainAmount`/`windDirection`/`gustStrength`. É a fonte da verdade.
2. `Precipitation` com `weather = <WeatherSystem>`, `followWeather = true`, `kind = 0` e
   `texture = "textures/rain.png"`. É o que se **vê** cair; o `WeatherSystem` só molha o *shallow field*.
3. `Lightning` com `weather` + `followWeather = true` + `stormThreshold = 0.3` e os knobs de flash/raio.
   Ele cria a `Light`, o `Lines` e o `Sound` dele próprio; o trovão sai atrasado
   (`distance / speedOfSound`, ~343 m/s) com roladas de eco.
4. `CameraShake` e `ScreenFade` nos slots `shake`/`fade` do `Lightning` — sem eles o raio não tem
   trauma nem flash de tela, e é isso que faz uma descarga parecer uma descarga.

Trocar de modo é sempre o mesmo trio, junto, para as três fontes não brigarem:

```lua
weather.type = 0                    -- 0 chuva, 1 neve   (WeatherSystem)
weather.targetIntensity = 0.9
precip.streak = 7.0                 -- 0 desliga o esticão da gota
precip:setKind(0)                   -- 0 chuva, 1 neve, 2 granizo
precip:setWind(215, 6.0)            -- setWind re-executa os initializadores: vai por último
```

`setKind`/`setWind` reconstróem o visual inteiro (`apply()`), e `apply()` termina em `restart()` por
causa da armadilha 14. Teclas do demo: `1` chuva, `2` neve, `3` granizo, `4` calmaria, `L` força um
raio, `ESPAÇO` sacode a câmera.

### Cachoeira que chove, escorre, cai e enche o poço (o que `demo_waterfall` faz)

O relevo não é pintado, é **calculado**: `scripts/gen_terrain_maps.py` gera o *heightmap* e o
*blend map* da mesma função de altura, porque `WaterSystem::bakeFloor` amostra o *heightmap* no CPU
para fazer o leito do *shallow field*. As duas imagens têm que vir do mesmo lugar ou a água escorre
para onde a montanha não está.

1. `Terrain` com os dois mapas, mais um `Water` no platô (o rio de alimentação: `simRain` é a chuva
   que cai nele, `simEdgeDrain = true` é o lábio por onde a água sai) e um `Water` na bacia (o poço
   de impacto, cujo plano está na altura do **ponto mais baixo do caminho de saída** — com
   `simEdgeDrain`, o lago para de subir ali, não onde você semeou).
2. `Waterfall` entre os dois: `source` esvazia, `target` enche, e o lençol de partículas é mirado no
   ponto de impacto com `plunge` de velocidade para fora. Uma parede de *heightfield* é uma **rampa**,
   nunca um muro: a queda livre só acelera para baixo, então sem `plunge` o lençol entra na rocha. O
   script imprime a folga para cada velocidade — negativa é "dentro da pedra".
3. `WaterWaves` em cada massa (o `crestFoam` escala também a espuma de corrente, e é multiplicado por
   três antes do clamp: 0.45 deixa tudo branco), `WaterRipples` no poço, `FloatOnWater` num tronco
   para provar que a corrente carrega, `Precipitation` + `WeatherSystem` por cima.

Duas coisas que só a captura mostrou:

- `scene.globalIlluminationIntensity` vale **0.1** por default e o reflexo do céu na água é
  `cor do céu × esse número` (`RenderSystem.cpp:7835`). Com o default, um lago azul-cobalto sob um
  céu claro renderiza preto-tinta. Suba para ~0.55.
- A espuma de margem é a distância no *depth buffer* **ao longo do raio de visão**, então uma parede
  vertical que sai da água conta como "água de espessura zero" em tudo que é pixel perto dela: a
  linha d'água vira banco de neve. `shoreFoam` é o knob que aperta essa faixa (0.07 no poço, 0.02 no
  rio); o `depthFade` separado é a profundidade em que a cor escura assume, e tem que caber dentro da
  profundidade real do corpo ou a superfície inteira é a cor de fundo.

Sondas: `WATERFALL_PROFILE=1` imprime a caminhada pelo canal **e pelo lago** (profundidade e
velocidade célula por célula), `WATERFALL_SHOT=wide|pool|fall|lip|top` troca a câmera, e as teclas
`R`/`F`/`U`/`D`/`C` mexem na chuva e na vazão em tempo real.

### Fogo que pega e se espalha (o que o `demo_fire` faz)

Dois scripts, quatro arquivos novos no projeto — `assets/textures/{fire_puff,fire_smoke,fire_ember}.png`
e `assets/sounds/fire_crackle.wav`, todos instalados junto com a biblioteca:

1. `Fire` numa entidade no centro da fogueira. Ele cria **até oito** entidades: um par
   `Points`+`Particles` para a chama, mais um par para a fumaça e um para a brasa (`smoke`/`embers =
   false` desliga cada par), uma `Light` POINT com nome `<entidade>Light`, e um `Sound` em loop.
   Nenhuma delas precisa ser autorada.
2. `Burnable` em cada caixa, pneu ou palhoça que pode pegar. Ele faz `require` do `Fire` e acende o
   **dele próprio** na mesma entidade quando recebe calor — por isso o `Burnable` precisa estar na
   entidade que tem o `Mesh`, se você quer vê-lo escurecer (`tint`).
3. Raio de propagação: `spreadRadius` no `Fire` (e no `Burnable`, que passa adiante) mede do centro da
   chama até o centro do combustível, somado à meia-diagonal dele (`AABB` do `Mesh`, quando existe).
   Sem `spreadRadius` o fogo não pula; sem `spreadInterval` ele checa a cada 0,8 s.
4. Chuva: o mesmo `WeatherSystem` do demo de tempestade, ligado no slot `weather` de **cada** fogo e
   combustível. `rainExtinguishes` é o que faz a intensidade 1.0 apagar.

A fogueira do demo é acesa por script (`bonfire:ignite()` num frame contado), não por `startOnLoad`,
para que as duas origens de incêndio — chama autorada e chama que veio do vizinho — aconteçam numa
corrida só. `Fire` também machuca: `heatDamage` + `damageRadius` + `subject`/`health` é DPS contínuo
pago em blocos de 0,25 s (ver armadilha 19).

Variante de shader que a fixture precisa, além das três da tempestade: `points:Tex,Vc4,Txr`. O `Txr`
(`HAS_TEXTURERECT`, `editor/shader/ShaderBuilder.cpp:460`) é o que faz o sprite ler o retângulo da
`addSpriteFrame`; sem ele a brasa sai como um quadrado branco.

### Coisa morre → algo acontece

`Health` → slot **On death: entity** = a entidade que tem o método, **On death: method** = nome
(padrão `onDeath`). `TriggerZone`, `Pickup`, `Projectile`, `Lifetime`, `FallingObject`,
`ScenePortal`, `MovePlatform`, `PatrolPath`, `LineOfSight`, `WeatherSystem` e `MusicPlayer` seguem o
mesmo desenho: um slot de entidade + um campo de texto com o nome do método.

### Multidão com uma draw call (instancing puro do motor)

Nenhum componente da biblioteca faz isso — é API do `Mesh`, herdada por `Model` e `Sprite`, e é o que
segura mil objetos scriptados sem derreter o frame.

```lua
local crowd = Kit.hold(Model(scene))          -- uma entidade
crowd:loadModel("Models/MapKit/mapkit_cube.glb")
crowd:createInstancedMesh()                   -- antes disto, nenhuma linha de ajuste cola
crowd.maxInstances = N
for i = 0, N - 1 do
    crowd:addInstance(Vector3(x, 0.5, z))     -- sem Body3D: desenha, não colide
end
```

O `InstancedMeshComponent` agrupa as cópias por LOD e manda **uma** draw call por lote (uma por nível de
LOD quando o malha tem níveis) em vez de uma por cópia; `cullDistance`, `distanceFade` e
`instancedBillboard` afinam o resto. A sonda que prova a conta é `checks/probe_instances.lua`, e ela
compara os dois mundos possíveis — medido em Debug, 1000 cubos, 1280x720, 200 quadros:

| | draw calls | triângulos | `drawScene` | `update` | frame médio | fps |
|---|---|---|---|---|---|---|
| 1000 entidades (`plain`) | **1000** | 12 000 | 12,83 ms | 5,19 ms | 19,67 ms | 51 |
| 1 lote de 1000 (`batched`) | **1** | 12 000 | 1,32 ms | 0,56 ms | 6,90 ms | 145 |

Os triângulos são os mesmos nos dois — o que some é o custo de submeter, não o de rasterizar.

```bash
KIT_INST_MODE=plain   KIT_INST_N=1000 quantum-engine.exe --project <dir> --scene checks/probe_instances \
    --window 1280x720 --frames 200 --warmup 60 --stats-out plain.json
KIT_INST_MODE=batched KIT_INST_N=1000 quantum-engine.exe --project <dir> --scene checks/probe_instances \
    --window 1280x720 --frames 200 --warmup 60 --stats-out batched.json
```

Duas coisas que a sonda quase perdeu: o variante `mesh:...,Ist,...` precisa estar no `assets/shaders` da
fixture (senão o runner aborta antes de desenhar), e `maxInstances`/`cullDistance`/`distanceFade` só colam
num lote que já existe — chame `createInstancedMesh()` antes, porque um lote **com zero instâncias não é
desenhado de jeito nenhum** (`selectMeshDraw` devolve `false` quando `instanceCount` é 0).

Uma cena autorada também pode carregar o componente — `instances` é serializado
(`Stream::encodeInstancedMeshComponent`) — mas nada no editor transforma um `Mesh` comum em lote sozinho.
O caminho de script é o de cima, e é ele que segura uma cidade procedural.

## Métodos públicos (para chamar de outra script)
Assinaturas conferidas uma a uma contra os `function <Classe>:<método>` dos arquivos.

| Componente | Métodos |
|---|---|
| `Health` | `takeDamage(amount, source)` `heal(amount, source)` `kill(source)` `setHealth(value, source)` `getPercent()` `isAlive()` |
| `Spawner` | `spawn(count)` `register(member)` `recycle(member)` `idleCount()` `activeCount()` |
| `Projectile` | `launch(position, direction, target)` `reset()` `targetPosition()` |
| `CameraShake` | `addTrauma(amount)` |
| `Lifetime` | `expire()` `reset()` |
| `Pickup` | `collect()` `respawn()` `setVisible(value)` |
| `Timer` | `start()` `stop()` `reset()` |
| `HudValue` / `HudBar` | `show(value)` (`HudValue` também `format(value)`) |
| `TriggerZone` | `isInside()` |
| `Rope` | `attach(anchorEntity, bodyEntity)` `detach()` `setLength(value)` `reel(delta)` `getTension()` `anchorPoint()` |
| `Slingshot` | `load(reference)` `pull(direction, amount)` `release()` `reset()` `getStretch()` `aimDirection()` |
| `FallingObject` | `victims(point, damage)` `fireImpact(speed, point)` `restart()` |
| `GravityZone` | `setFactor(value)` `setDirection(value)` `contains(reference)` `getActiveCount()` `center()` |
| `PhysicsGrabber` | `screenRay()` `pick(ray)` `grab(reference)` `release()` `isGrabbing()` `setStrength(value)` |
| `FloatOnWater` | `setBuoyancy(value)` `getSubmerged()` |
| `SwimController` | `enterWater()` `leaveWater()` |
| `WaterWaves` | `setIntensity(zeroToOne)` `gust(strength)` `setWindDirection(degrees)` `getWaveHeight()` `getWindDirection()` |
| `WaterRipples` | `fireSplash(position)` |
| `BeachShore` | `applyPreset(index)` `setMood(zeroToOne)` `wetSand(width)` |
| `DayNightCycle` | `setTime(hour)` `addHours(amount)` `getHour()` `getHourText()` `getPhase()` `getSunElevation()` `getMoonElevation()` `getMoonPhase()` `setMoonPhase(value)` `getIllumination()` `getDayProgress()` `getSunDirection()` `getMoonDirection()` `setPaused(value)` |
| `Clouds` | `setPreset(name)` — `clear`/`few`/`overcast`/`storm`, retorna `false` para nome desconhecido. Todo o resto é propriedade lida e escrita no `onUpdate` |
| `WeatherSystem` | `setIntensity(value)` `rain()` `snow()` `clear(seconds)` `getIntensity()` |
| `Precipitation` | `setIntensity(value)` `setKind(value)` `rain()` `snow()` `hail()` `setWind(direction, speed)` `start()` `stop()` `isActive()` `getCount()` `restart()` |
| `Lightning` | `strike(target)` (número = distância, tabela com `x` = ponto, entidade = onde ela está, nada = aleatório) `setEnabled(value)` `start()` `stop()` `getStrikeCount()` `getDistance()` |
| `WindZone` | `setGust(scale)` `calm()` `getDirection()` `getStrength()` |
| `Fire` | `ignite()` `extinguish()` `setIntensity(value)` `addFuel(seconds)` `soak(amount)` `restartLayer(layer)` `isBurning()` `getIntensity()` `getFuel()` `getWet()` `getParticleCount()` `layerCount(layer)` `position()` `ownerBurnable()` |
| `Burnable` | `ignite()` `igniteFrom(source)` `extinguish()` `burnOut()` `soak(amount)` `douse()` `addFuel(seconds)` `setIntensity(value)` `isBurning()` `getFuel()` `getChar()` `getIntensity()` `getWet()` `getFire()` `distanceTo(reference)` `position()` |
| `MovePlatform` | `setProgress(t)` `pause()` `resume()` `nextWaypoint()` `getDelta()` |
| `PatrolPath` | `gotoWaypoint(target)` `next()` `pause()` `resume()` `getCurrent()` `getProgress()` `refresh()` |
| `LineOfSight` | `canSee(reference)` `isSeen()` `getDistance()` `refresh()` `pursue(id)` |
| `Interactable` | `trigger()` `canUse()` `isInRange()` `isLooking()` `setPrompt(text)` `showPrompt(value)` `setWidgets(visible)` |
| `DamageArea` | `setRate(value)` `setEnabled(value)` `isActive()` `inside()` `overlaps(centre, point)` `push(point, centre)` |
| `CapsuleCollider` | `apply()` `setSize(radius, height)` (recusa depois de cozinhado) `setTrigger(value)` `isLoaded()` `getShapeIndex()` `shapeCount()` |
| `MeshCollider` | `bake()` `rebake()` `setTrigger(value)` `isLoaded()` `getShapeIndex()` `shapeCount()` |
| `ScenePortal` | `preload()` `travel()` `beginTravel()` `cancel()` `release()` `getProgress()` `setDestination(name)` |
| `ScreenFade` | `fadeIn(seconds)` `fadeOut(seconds)` `flashTo(color, alpha, seconds)` `setLevel(value)` `isDone()` `skip()` `complete()` |
| `PostEffectPulse` | `pulse(seconds)` `setLevel(value)` `setTarget(fromValue, toValue)` `write(name, value)` `stop()` `getLevel()` |
| `ImpactEffect` | `play(strength, point)` `setEnabled(value)` `getStrength()` |
| `ParticleBurst` | `emit(pointOrEntity, scale)` `setEmitter(point)` `stop()` `reset()` `setColor(value)` `setCount(value)` `setEnabled(value)` |
| `TrailEffect` | `burst()` `setRate(value)` `setEnabled(value)` `getSpeed()` `clear()` |
| `AmbientSound` | `setIntensity(value)` `rampTo(target, seconds)` `fadeIn(seconds)` `fadeOut(seconds)` `stop()` `resume()` `disable()` |
| `SoundOnEvent` | `load(path)` `play(pointOrNil)` `playAt(path, position)` `setEmitter(point)` `stopAll()` `setVolume(value)` |
| `MusicPlayer` | `play(indexOrName)` `start(channel, track, rate)` `switchTo(track)` `setState(name)` `pause()` `resume()` `stop()` `next()` `previous()` `setVolume(value)` `getTrack()` `getLength()` `getPlayingTime()` |
| `Camera2DFollow` | `setZoom(value)` `setBounds(minX, maxX, minY, maxY)` `snapToSubject()` `clampToBounds(value, low, high)` |
| `GamepadMover` | `getInputVector()` `readAxis(axis)` `isActive()` |
| `CharacterController3D` / `PhysicsMover3D` | `toCameraSpace(x, z)` (auxiliar do eixo da câmera) |
| `Rotator` / `Bobber` / `LookAt` / `OrbitCamera` / `FollowCamera` / `TopDownController` / `PlatformerController2D` | nenhum — só propriedades |

## Limites (nada disso existe no motor, então os componentes não prometem)

- **Lua não cria entidades.** O pool é autorado na cena; `Spawner` só reposiciona e reativa.
- **Sem consulta de overlap/shape-cast a partir de Lua** — só raio único
  (`Ray(o, d):intersects(scene, RayFilter.BODY_3D)`), e ele enxerga colisores, não transforms. Os
  acertos, zonas e "quem está na plataforma" aqui são distância, e por isso quase tudo tem slot de
  entidade ou lista de nomes.
- **Altura do terreno tem de ser colisão**: com um `Body3D` estático `heightfield` no terrain,
  `CharacterController3D` acha a superfície sozinho (armadilha 12); sem ele, o chão é `groundY` e o
  personagem atravessa o relevo.
- Movimento por Transform não colide com **paredes**: anda através de malha e caixa. Para cenário
  sólido use o `PhysicsMover3D` com Body3D — o terreno é a exceção, porque a sonda de raio cobre ele.
- `Pickup`/`TriggerZone`/`DamageArea` medem a partir do centro da entidade, não do colisor.
- **A cadeia de post-process do usuário leva um float por uniforme e não ganha passes novos por
  Lua.** `ScreenFade` e `PostEffectPulse` só mexem em float — não existe bloom/HDR por aqui.
- Joints: `Rope` e afins precisam do componente `Joint3D` **na cena** (armado pelo Inspector); do
  lado Lua não dá para criar junta (ver armadilha 4).
- **Gota visível não é física.** `Precipitation` desenha e move `Points`; ele não empurra corpo, não
  colide e não molha nada. Quem enche o *shallow field* é o `WeatherSystem` (`rainAmount`), e os dois
  só conversam pela intensidade (`followWeather`).
- **O raio é `Lines` + uma `Light` POINT.** Não existe luz volumétrica nem glow no motor
  (`quantum-bloom`, `quantum-volumetrics`), então o feixe não ilumina fumaça nem sangra — ele é uma
  polilinha com cor e alpha, e é isso que faz a cena ler como relâmpago.
- **Trovão é amostra, não síntese.** Sem `assets/sounds/thunder.ogg` no projeto a tempestade lampeja
  muda; o `Lightning` avisa uma vez (`no thunder sample at ...`) e segue. A instalação da biblioteca
  copia o `.ogg` que vem com ela.

## Verificação

Sem suíte de testes no repositório, então a prova são checadores headless em `checks/` mais os demos.

**1. Sintaxe + comportamento simulado** (`lua`/`luac` 5.4 comuns, sem engine):

```bash
cd engine/components
for f in *.lua demos/*.lua; do luac54 -p "$f" || echo "SYNTAX FAIL $f"; done   # 65 arquivos
lua54 checks/kit_harness.lua "$(pwd)"     # 61 components, 0 failures
```

O harness monta cada componente como o `LuaBinding` faz (tabela com `__index` no módulo, propriedades
vindas dos `default`, `init()`, 240 frames de `onUpdate`/`onFixedUpdate` com teclado, mouse e scroll
variando) contra stubs que imitam a API real — inclusive a assinatura de `Object:setPosition(Vector3)`,
que é onde um stub mais tolerante já escondeu um bug.

**2. Formas de chamada contra o binding real** — o `lua_api.md` renderiza assinaturas C++, então um
setter sobrecarregado parece certo lá e é rejeitado em Lua. Este roda dentro da engine:

```bash
cp checks/api_probe.lua <projeto>/lua/
build/Debug/quantum-engine.exe --project <projeto> --scene api_probe --window 320x240 --log-level 2
# [api] 116 calls probed against the real bindings, 0 failures, 0 traps now valid
```

As linhas `(ERROR)` no fim são as *traps* — probes que **devem** ser recusados; se uma virar `ok`, o
binding ganhou o formato e o arquivo é que está errado.

**3. Carga no estado Lua do motor** — o mesmo caminho que o Inspector percorre:

```bash
cp checks/kit_check.lua <projeto>/lua/
timeout 70 build/Debug/quantum-engine.exe --project <projeto> --scene kit_check \
    --window 320x240 --log-level 2 | grep '^\[kit\]'
# 57 components checked in the engine Lua state, 0 failures
```

`System.quit()` dentro do `QUANTUM_INIT init()` não encerra o runner (o loop de frames começa depois),
por isso o `timeout` — e lembre de matar o processo, que fica órfão segurando `quantum.dll` e trava o
próximo link (LNK1168).

**4. Instalação e demos ponta a ponta:**

```bash
build/Debug/quantum-editor-cmd.exe components install --project <projeto>   # 56 + 9 em demos/
KIT_DEMO_FRAMES=300 build/Debug/quantum-engine.exe --project <projeto> \
    --scene demos/demo_beach --window 320x240 --log-level 2                # e demo_ocean/daynight/rope/storm/fire
```

**5. Tempestade ponta a ponta** — o `demo_storm` é o único que exercita partícula + luz + `Lines` +
som num scene, então ele é também a prova de que um projeto de fixture tem as variantes certas. Um
projeto vazio precisa de `mesh`/`depth` com `Ter` para o terrain e `points:Tex,Vc4` + `lines:Vc4` para
chuva e raio:

```bash
build/Debug/quantum-resource-compiler.exe shaders --out assets/shaders --backend opengl \
    --shader "mesh:Puc,Nor,Fog,Ter" --shader "depth:Ter" \
    --shader "points:Tex,Vc4" --shader "lines:Vc4"
build/Release/quantum-engine.exe --project <projeto> --scene demos/demo_storm \
    --window 1280x720 --frames 600 --warmup 20 --name storm --stats-out storm.json
grep -c '(ERROR)' storm_out.txt    # 0
grep -c '(WARN)' storm_out.txt     # 0
```

Saída esperada: `2080 drops` — o pool segue `maxParticles`, que agora é ligado em Lua
(`Particles.maxParticles` em `ActionClassesLua.cpp`); sem esse binding a armadilha 14 ancora o número
em `100 drops`. Os modos trocam em `1/2/3/4` e `strike` aparece no fim. Rodado em
`build/Debug/quantum-engine.exe` em 2026-10-25: `EXIT=0`, `(ERROR)` 0, `(WARN)` 0, 4 descargas em 120
frames. Para provar que o laço automático de raios existe sem esperar minutos de tempestade, suba a
taxa:

```bash
cp checks/probe_storm_auto.lua <projeto>/lua/demos/
build/Release/quantum-engine.exe --project <projeto> --scene demos/probe_storm_auto --window 640x360 --frames 900
# [probe] done: 44 strikes, last 302.7 m, 30 thunder waits queued
```

Em 600 frames (~4,5 s) a 30 raios/minuto o `0` automático é estatística, não bug: a taxa esperada é
uma descarga a cada ~3,5 s. A 600/min a sonda conta 44.

**6. Fogo ponta a ponta** — `Fire` é o componente que mais cria entidades sozinho (três pares
`Points`+`Particles`, uma `Light`, um `Sound`), então o probe verifica as seis coisas de que um fogo é
feito, uma a uma, e não "o demo pareceu estranho":

```bash
cp checks/probe_fire.lua <projeto>/lua/checks/
build/Debug/quantum-engine.exe --project <projeto> --scene checks/probe_fire --window 1280x720 \
    > probe_out.txt 2>&1
grep '^\[probe\]' probe_out.txt
```

Saída esperada, rodada em 2026-10-26 com `build/Debug/quantum-engine.exe`:

```
[probe] fire: 19/19 checks passed - 347 sprites on the bonfire, char 0.25
[probe] ALL PASS
```

As 19 linhas cobrem: três pools que enchem, a luz que **realmente entrou na cena** (um `build()` que
morresse no meio deixaria `lamp` nil e os números continuariam saindo), o piscar, o calor que atravessa
3 m e não atravessa 24 m, o vizinho que acende o próprio fogo, combustível que vira carvão, o `Mesh` que
escurece, chuva que apaga, combustível que acaba, `burnAway` que esconde, combustível queimado que recusa
fósforo, inflamabilidade 0 que recusa, combustível molhado que recusa, e o dano que chega no `Health`.
O probe tem **um** `(ERROR)` esperado: a sonda que confirma a armadilha 18 — `tint` num entity sem
`Mesh` tem que se desligar no primeiro falho.

O demo ao lado dele roda limpo:

```bash
KIT_DEMO_FRAMES=1200 build/Debug/quantum-engine.exe --project <projeto> --scene demos/demo_fire \
    --window 1280x720 > fire_out.txt 2>&1
grep -c '(ERROR)' fire_out.txt    # 0
grep -c '(WARN)' fire_out.txt     # 0
```

A fixture de fogo precisa de uma variante a mais que a tempestade: `points:Tex,Vc4,Txr` (o `Txr` é o
retângulo de sprite — sem ele a brasa sai quadrada). Junte ao `--shader` do bloco 5.

**7. Personagem montado em osso ponta a ponta** — `demo_character_creator` é a sonda do `AvatarPart`
e do `HairSwing`; ela **mede** em vez de afirmar, e cada veredito é a diferença entre duas amostras do
mesmo par: uma peça que não está realmente pendurada no osso deriva, e a deriva é o que é reportado.

```bash
QUANTUM_PACK_DIR=C:/quantum2/engine/assets   build/Debug/quantum-engine.exe --project <projeto> --scene demos/demo_character_creator   --window 320x240 > cc_out.txt 2>&1
grep -c 'avatar\] ok' cc_out.txt     # 8
grep -E 'avatar\] (PASS|FAIL)' cc_out.txt
```

Os oito vereditos: `attach` (as cinco peças penduradas no mesmo osso), `alias` (nome errado cai no
fallback `Head`), `track` (passo por frame de cabelo vs. osso idêntico — 0,00000 m durante giro,
`Dance_Loop` e translação), `offset` (a distância sobrancelha→cabeça não deriva), `follow` (braço de
0,05 m mantido contra a posição do osso no frame **anterior**, porque o script lê antes do
`updateTransform`), `swing` (pico de 40° e volta a 1,6°), `droop` (o teto de 20° segurou) e `detach`
(a peça abandonada ficou 1,813 m para trás). O `QUANTUM_PACK_DIR` é obrigatório: sem ele a sonda escolhe
`assets/engine/assets`, que resolve relativo ao CWD do processo e não relativo ao projeto.

A fixture precisa das variantes `mesh:Uv1,Puc,Nor,Nmp,Ski,Aop[,Dlo]` (o `Ski` é o skinning dos packs de
personagem) além das dos blocos 5 e 6.

**8. Ciclo de céu ponta a ponta** — `demo_sky` é a sonda do `DayNightCycle`, do `Clouds` e do fork
`sky_daynight` juntos. Ela percorre uma hora por fase nomeada e escreve um PNG do frame apresentado,
então a prova é a lista de fases *e* a imagem:

```bash
for h in 6.5 12.5 17.6 19.6 23.0; do
  SKY_HOUR=$h SKY_PRESET=few build/Debug/quantum-engine.exe --project <projeto> \
      --scene demos/demo_sky --window 640x360 --screenshot /tmp/sky-$h.png --capture-frame 30
done | grep '^\[sky\] [0-9]' | awk '{print $3}' | sort -u    # afternoon day dusk morning night
```

As cinco fases nomeadas têm de aparecer, cada linha tem de trazer sol e lua em elevações **opostas**
(com `SKY_MOON` 0.5, lua cheia: `06:30 sun +6.9 moon -6.9`, `23:00 sun -62.4 moon +62.4`), e nenhum
run pode imprimir `missing precompiled shaders`. Os PNGs são o que prova o resto: o disco do sol no
horizonte às 17:36, a faixa de claridade às 19:36 (o céu escurece **em rampa**, não num degrau — um
`dayFactor` que zera na linha do horizonte faz o crepúsculo virar noite em um frame), e a lua com a
terminadora desenhada às 23:00.

Os nomes de método/propriedade do motor usados pelos componentes foram conferidos um a um contra
`build/generated/lua_api.md` e contra as cadeias de `LuaBinding` em
`engine/core/script/binding/*.cpp`.

O que **não** foi verificado: comportamento visual no editor e nos jogos — câmera, sensação de
movimento, altura de pulo, HUD, aparência das ondas e o arrasto dos slots precisam de uma passada
sua na cena. Os demos provam os caminhos de script e de evento, não a imagem — com duas exceções: o céu
(bloco 8) e a água, esses sim com PNG do frame apresentado lido com olho. O que continua sem prova é o
Inspector: nenhuma linha de propriedade acima foi conferida na janela do editor.
