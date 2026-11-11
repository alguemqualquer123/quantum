# VoxelWorld — um Minecraft-like feito com a engine Quantum

Projeto de demonstração voxel em `templates/voxelworld-main`. Mundo infinito gerado por ruído,
streaming de chunks, iluminação estilo Minecraft (skylight + blocklight), água que **cai em coluna** (o
espalhamento lateral está escrito e nunca roda — `#220`),
salvamento em disco, jogador em primeira pessoa com colisão por bloco, multiplayer TCP, som 3D
gerado por script, ciclo dia/noite e sobrevivência com vida, fome, sede, dano ambiente e respawn, com
as três modas do Minecraft (sobrevivência, criativo e espectador) governando quem pode voar, editar e
ferir-se, com o jogador e os mobs vestidos por um rig de seis cubos animado pelas UVs de uma skin
64×64 — o layout do formato de skin, para que uma skin que alguém já tem sirva sem ser redesenhada —
e com o bloco minerado **caindo no mundo** como um cubo girando que se pega por contato.
Todo o código de jogo está em `scripts/` (C++,
`quantum::ScriptBase`), sem Lua, para caber no orçamento de frame que um mundo voxel exige.

Este arquivo é o **roadmap vivo** do projeto: atualize o estado das fases aqui a cada corte
entregue.

## Estado

| # | Fase | Estado |
|---|------|--------|
| 0 | Atlas de texturas 16×16 gerado por script (licença limpa) | ✅ `textures/voxel/atlas.png`, 78 slots (68 blocos + 10 estágios de rachadura) + `panel.png` para o HUD |
| 1 | Tabela de blocos: 58 tipos, flags, shapes de colisão, luz, dureza, tool level | ✅ `scripts/VoxelBlocks.{h,cpp}` |
| 2 | Chunk: storage plano (`blocks`/`light`/`height`/`biome`), serialização própria | ✅ `scripts/VoxelChunk.{h,cpp}` |
| 3 | `VoxelWorld`: streaming orçamentado, hooks de gen/light/water/save, edições | ✅ `scripts/VoxelWorld.{h,cpp}` |
| 4 | Mesher: UVs no atlas, AO, luz em vertex color, índices de 32 bits, 2 entidades/chunk | ✅ `scripts/VoxelMesh.{h,cpp}` |
| 5 | Geração de mundo: biomas, altura, cavernas, minérios, árvores, oceano/lago | 🟡 `VoxelGen.cpp` — sonda: 9 biomas, alturas 18..126, 28,4% submerso, 4% de caverna escavada, 4,3 árvores/chunk, 1,22 ms/chunk, 49 chunks re-gerados byte-idênticos |
| 6 | Iluminação voxel: skylight por coluna + BFS de blocklight, propagação entre chunks | ✅ `VoxelLight.cpp` — 12 checks na sonda (`VOXEL_PROBE`): sala selada escura, poço de luz grátis, 1 nível por bloco lateral, tocha = tabela, parede segura a luz, re-solve muda 0 de 32768 bytes, curva dia/noite amostrada |
| 7 | Bloco de água: nível 0..15, **coluna por gravidade**, lava, fontes finitas | 🟡 `VoxelWater.cpp` — 8 checks: fonte nunca se esvazia, riacho para no 7º, coluna cai antes de espalhar, pedra bloqueia, lava 4× mais lenta, água+lava vira rocha, piscina assenta e não muda mais. O **lateral não roda**: o ramo de queda retorna antes do laço vizinho (`VoxelWater.cpp:309-333` vs `:349-367`) e `isFree` conta água como livre — ver `#220` |
| 8 | Salvamento: um arquivo por chunk em `world/`, cache LRU, I/O assíncrono | ✅ `scripts/VoxelSave.cpp` — 15 checks na sonda (`VOXEL_PROBE`): a edição vira arquivo no flush de saída (98 844 bytes para um payload de 98 836), a releitura fria volta **byte-idêntica** (blocos, luz, topo e bioma), coluna quente é respondida sem abrir o arquivo, `index.txt` traz versão + contagem + a linha da coluna, e os quatro modos de dano são recusados em vez de lidos: byte virado (CRC), arquivo pela metade, versão estranha com CRC válido, conteúdo íntegro morando no nome errado. O journal fecha o ciclo: a linha `<cx cz x y z id>` escrita, e uma releitura fria que a reproduz no mundo |
| 9 | Jogador FPS: colisão AABB contra shapes de bloco, pulo/água/fly, reach por raycast DDA | ✅ `VoxelPlayer.cpp` + `VoxelCollide.cpp` — 13 checks de colisão + o resto do módulo (`player 27`), inclusive o passo em meia-altura medido na coluna do jogador, o `respawnAt` que recusa uma coluna ainda não streamada em vez de teletransportar para dentro dela, e os **3 do olhar com o ponteiro capturado**: um varrimento de 12 px por frame vira os 7,680° de yaw que a sensibilidade promete, a mão que para não puxa a vista de volta e a mesma viagem em vertical dá 5,760° de pitch — ver "Ponteiro capturado" |
| 10 | Quebrar/place por item: dureza + tool level, drop, colocação sem sobrepor o jogador | 🟡 dentro de `VoxelPlayer.cpp` — a sonda prova raycast → break → place → recusa sobre o corpo. A **conta** está certa e medida (`miningSeconds` `:386-397`: mão 0,11 s dirt / 2,19 s stone / 23,9 s obsidian, picareta 0,35 s stone), mas o jogador nasce com picareta de madeira invisível (`VoxelPlayer.h:146-147`) e o HUD nomeia o bloco, nunca a ferramenta (`VoxelGame.cpp:598-600`) — ver `#222` |
| 11 | Entidades: mobs com gravidade voxel, caminho no terreno, spawn/despawn por distância | ✅ `VoxelEntity.cpp` — 14 checks na sonda: nasce apoiado (0 encaixado, 0 flutuando), pool com teto duro (64 pedidos → 32 vivos), anda com a malha no mesmo ponto do sim (deriva 0), cai quando o chão sob os pés some e pousa no próximo, nada em água (subiu 1,34 bloco em 86 frames molhados), é culled + escondido + slot livre, areia/cascalho descem célula a célula pelo caminho **alto** (o save e a rede veem), atravessam água e param no sólido, e a sonda devolve pool, fila e cada voxel cavado |
| 12 | Multiplayer: host autoritativo + clientes, TCP com length-prefix, snapshot + diff | ✅ `scripts/VoxelNet.cpp` — round-trip provado por **dois processos reais** em `scripts/voxel-net-roundtrip.sh`: 15 checks, 0 falhas, os dois blocos flutuando nos dois PNGs (`host.png`/`client.png`, região dos blocos com **0 de 34 800** pixels diferentes) |
| 13 | HUD: hotbar, crosshair, nome do bloco mirado, FPS/tris/chunks carregados | ✅ `VoxelGame.cpp` — cena de UI própria (`Engine::addSceneLayer`), placas translúcidas atrás do overlay e de cada slot, selected em âmbar. Provado em `vhud7.png` |
| 14 | Dia/noite: sol orbital, skylight multiplicado pelo céu, fog noturno, tochas brilham | 🟡 o host possui `Light` + `Fog` e re-horneia a luz dos chunks. A **curva** está provada (`daylightFactor`: 0,05 à meia-noite, 1,0 ao meio-dia, dawn == dusk); o re-horneio por frame não |
| 15 | Cena: `Main Scene.scene` com `VoxelWorld` script, luz direcional, câmera e céu | ✅ 4 entidades: MainCamera, Sky, VoxelWorld (`VoxelGame`), Player (`VoxelPlayerScript`) |
| 16 | Build headless: `generate` → `configure` → `build` → exe roda e captura PNG | ✅ `.quantum/build-standalone/VoxelWorld.exe`, 0 avisos MSVC, 64 fps / 81 chunks / 226 388 triângulos no frame 240 |
| 17 | Substituir o `ChunkGenerator` antigo (mesh quebrado, sem UV, índice 16 bits) | ✅ movido para `legacy/` |
| 18 | Quebrar com interação: 10 estágios de rachadura sobre o bloco, partículas, cancelamento | ✅ `scripts/VoxelBreak.{h,cpp}` — cubo recortado sobre o bloco alvo, `textureRect` trocado por estágio, 10 tiles de rachadura **1-bit** no atlas, partículas por dureza, some ao trocar de alvo ou soltar o botão. 10 checks na sonda + `vb15_stage.png` |
| 19 | Sons: WAVs gerados por script (licença limpa) + `VoxelAudio` com vozes 2D/3D | ✅ `scripts/VoxelAudio.{h,cpp}` + pack de 35 WAV PCM gerado por `scripts/generate_voxel_sounds.py` (raiz do repositório) (1 926 946 bytes, digest `3104914fad8fa324`) — anel fixo de 12 vozes 3D + 3 camas em loop, mineração na cadência da lasca, quebra/place pelo hook do controlador, splash na borda de entrar na água, passo cobrado por distância, crossfade dia/noite e abafamento quando o olho está submerso; o F3 ganha a linha `Sound: 12/12 voices (21 plays, 8 steals, 0 missing)`. 20 checks na sonda (`audio 20`) + `VoxelAudio: 12 voices + 3 beds, 0 samples missing, 3d 1` |
| 20 | Vida, fome e sede: decaimento, queda, afogamento, veneno, regen, morte/respawn | ✅ `scripts/VoxelLife.{h,cpp}` — **28 checks** na entrega, **37** hoje na sonda (`life 37`, com os 9 do portão de modo): as barras queimam exatamente pela tabela do header (100 s parado = 5 de comida, 8 de água), andar ×2 e correr ×3 medidos **contra** o ocioso, o pulo cobra uma vez e nadar não cobra, queda de 3 blocos não fere / de 10 fere 7 / na água não fere / saída de voo não fere, o ar acaba em 10 s e depois dói a 2 por segundo, lava a 6, cacto a 1, barra vazida mata (sede antes de fome), cura a 0,4/s só com comida acima do portão **e paga comida por ela**, a morte é **um** evento com causa nomeada, corpo morto não decaiu em 100 s de caminhada, e levantar devolve meio dia com a queda que matou esquecida. O F3 ganha `Life: 20 hp  20 food  20 water  air 10.0  dead 0`, `G` come o item do slot e `H` bebe |
| 21 | Modos sobrevivência / criativo / espectador, com as regras por modo | ✅ `VoxelConfig.h` (vocabulário) + `VoxelPlayer.cpp` (dono) + `VoxelLife.cpp` (informado) — **16 checks novos na sonda** (`player 14`, `life 37`, total **143 / 0 falhas**): os três nomes resolvem para os conjuntos de regras que nomeiam e um nome desconhecido é sobrevivência, sobrevivência **ignora** o toggle de voo em vez de limpá-lo, criativo está no ar exatamente quando o toggle diz, espectador está no ar sem toggle e **os seus cliques não mudam nada** (voxel intacto, progresso 0, pilha intocada), mineração instantânea e pilha que não gasta são do criativo, e as barras voltam de outra moda do jeito que foram deixadas. `[M]` cicla e escreve na linha `Game Mode` da cena; hotbar e crosshair somem no espectador, provado nos dois PNGs (`voxel_mode.png` / `voxel_spectator.png`) |
| 22 | Modelos do player e dos mobs: rig de cubos animado, UVs no layout de skin 64×64 | ✅ `scripts/VoxelModel.{h,cpp}` + `scripts/generate_voxel_skins.py` (raiz do repositório) + `textures/skins/default.png` — **20 checks** na sonda (`model 20`): as seis áreas de mapa são as do layout e **não dividem um texel** (uma skin pinta seis corpos), a face da cabeça está em (8,8), coroa (8,0), nuca (24,8), torso (20,20), braço direito (44,20), cada área tem **o tamanho da própria face**, o corpo pisa o assoalho e a coroa fecha em 2 unidades, um jogador mede **1,8 bloco igual à sua hitbox**, o braço em repouso **pende** (cotovelo 60,750 contra a linha do quadril 60,750, 0,375 para o lado) em vez de abrir T-pose, o ciclo fecha em 2π, antífase braço/perna medida (−17,1 / +17,1), amplitude cresce com a velocidade e **para em 55°**, a cabeça gira **relativa** ao corpo, e um rig é **6 entidades vivas** desenhadas onde a pose manda — um flag esconde as seis, um teardown não deixa entidade. Do lado do jogador **+4** (`player 14 → 18`): a cabeça olha exatamente onde a câmera olha (`dot 1.00000` em yaw 37 / pitch −14), o olho publicado **continua sendo a cabeça** e só a câmera recua 3,20, a coroa do modelo é o topo do colisor (65,800), e uma parede atrás **encurta** a câmera (2,22) em vez de enterrá-la. Do lado do mob **+1** (`entity 14 → 15`): o rig pendurado no corpo em que o sim está, quadril e pescoço nas alturas prometidas. `[V]` liga a terceira pessoa, `VOXEL_THIRD=1` faz o mesmo headless; visto em `voxel_third.png`, `crop_mobs.png` e `crop_mine.png` |
| 23 | Sistema de skin: PNG 64×64 do usuário, legado 64×32, fallback procedural | ⬜ — hoje um **PNG só**: não há mais uma skin por modelo; o pack traz `textures/entity/player/{slim,wide}` com 9 skins oficiais 64×64, o que torna 23 "apontar o caminho" em vez de desenhar |
| 24 | Inventário: 36 slots, pilhas, drop por bloco, crafting | 🟡 `VoxelDrop.{h,cpp}` — o drop **no mundo** está entregue e medido (10 checks, `#201`): cubo do tile do bloco, 90°/s, bob, merge em pilha, ímã de 1,8, recusa que deixa o item onde está e pilha de 64 na hotbar. O storage são **9** slots (`VoxelPlayer.h:126`), sem grid e sem UI (`#223`), e crafting **não existe** — `BLOCK_CRAFTING_TABLE` é um bloco colocável sem receita (`#224`) |
| 25 | UI: tela de criação de mundo, menu ESC e inventário aberto | 🟡 a **camada** está entregue e medida (`scripts/VoxelUi.{h,cpp}`, 18 checks, `#215`/`#216`): sprites do pack resolvidos em runtime, nine-slice pela borda do `.mcmeta`, modal que bloqueia input e reclama o ponteiro, botão de quatro texturas, request de texto e confirmação com callback disparável headless, rótulo com fonte opcional, corações/comida/ar vestindo a arte original e a hotbar re-bindada a `container/slot.png`. Falta o **chamador**: as três telas (`#198`) e os ícones de item nos slots (`#204`/`#225` — hoje a hotbar desenha dígitos). Um bug de HUD achado na captura do usuário: `#226` |
| 26 | Assets reais do pack local (`minecraft-assets-26.3/`) como entrada de build/runtime | 🟡 `#202`/`#203` entregues (costurador + atlas ligado ao mesher e ao HUD, `atlas 7` na sonda), `#204`-`#212` abertos. O pack é **da Mojang** e está no `.gitignore` junto de tudo que for costurado dele; a fronteira está em "Licença dos assets" |
| 27 | Corte de diagnóstico: as cinco queixas de 2/11 medidas uma a uma | ✅ nenhuma linha de código — a entrega é a verdade sobre cada queixa, em "O que a reclamação de 2/11 mediu". **Texturas: o atlas do pack é o desenhado e é fiel** (recorte manual tile a tile), o que falta é tinta por bioma, log da escolha e sonda de pixel (`#227`). Água lateral: o ramo está morto (`#220`); submerso: não existe visual (`#221`); ferramentas: a conta está certa, a ferramenta é invisível (`#222`); inventário/crafting: 9 slots e zero (`#223`/`#224`); placa cinza: `#226` |

Legenda: ✅ entregue e compilado · 🟡 escrito, faltando compilar/provar · ⬜ não começado

### A frente atual (23..26) e o que cada fase tem que provar

Ordem por dependência, não por desejo: 22 fechou sobre malha-em-memória — seis cubos com UVs no layout
de skin, uma pose aritmética escrita em transform por frame e nenhum vertex buffer tocado depois da
criação; 23 é a mesma geometria lendo o PNG 64×64 **do usuário** em vez do gerado pelo script; 24
fornece os dados que a tela de 25 desenha; e 26 é a frente nova — o usuário colocou
`minecraft-assets-26.3/` (28 111 arquivos, o dump da Mojang) ao lado do projeto, e ela troca os
*placeholders* gerados por script pelos pixels e dados reais, **sem mover um byte deles para o
repositório**. Dependência dentro de 26: `#202` (o costurador) antes de `#203`/`#204`/`#206`/`#207`,
porque todos desenham do mesmo atlas.

O que já foi conferido no código da engine antes de escrever qualquer linha:

- **Som** é SoLoud (`engine/core/pool/SoundPool.cpp:13` inclui `soloud_wav.h`), então WAV PCM
  funciona e um script pode gerá-los do zero — a mesma disciplina do atlas de texturas. O módulo
  **não** usa `quantum::Sound` (`engine/core/object/sound/Sound.h`): escreve o `SoundComponent`
  direto, porque o wrapper cria uma entidade por som e `Sound::getObject()` **lança**
  (`Sound.cpp:33-38`) quando o som é 2D, num build com `_HAS_EXCEPTIONS=0`. As armadilhas reais do
  caminho estão em "Som: o pack e as vozes".
- **Partículas** vivem em `ParticlesComponent` (`engine/core/component/ParticlesComponent.h`) com
  `bursts`, `rate`, inicializadores/modificadores por curva de easing, e são avançadas pelo
  `ActionSystem` (`engine/core/action/Particles.cpp`). A receita de uso do lado do script está em
  `engine/components/ParticleBurst.lua` — inclusive que **não há `stop()`**: limpar bursts e o flag
  de emitter é o silêncio.
- **UI de dentro do jogo em C++** é a mesma que o HUD já usa (`VoxelGame.cpp:300-375`), e o clique
  é alcançável: `UIComponent::onClick` é um `FunctionSubscribe<void(float,float)>`
  (`engine/core/component/UIComponent.h:59`) e o `UISystem` faz `ui->onClick.call(localX, localY)`
  em `engine/core/subsystem/UISystem.cpp:3054`. `FunctionSubscribe::add(tag, std::function)` aceita
  lambda (`engine/core/util/FunctionSubscribe.h:265`), então um script C++ assina sem Lua.
  A cena-layer precisa de `setEnableUIEvents(ENABLED)` para reivindicar o ponteiro
  (`engine/core/Scene.h:24-28`) — o HUD hoje é `DISABLED` de propósito (`VoxelGame.cpp:306`).
- **Tecla e caractere**: `Input::isKeyPressed(D_KEY_*)` / `isMousePressed` / `getMouseScroll`
  são polling, sem borda — o controlador já faz borda com bools próprios. Para digitar o nome do
  mundo existe `Engine::onCharInput` (`engine/core/Engine.h:535`, `void(wchar_t)`).
- **Ponteiro capturado: o `Engine::setMousePosition` reescreve a posição que a engine reporta**
  (`Engine.cpp:536`, e o router é rebaseado junto em `SystemWin.cpp:160-165`). Capturado, o
  `WM_MOUSEMOVE` é engolido sem reportar (`WinInputRouter.cpp:61-69`) e só o `WM_INPUT` entrega
  movimento. Consequência, e foi ela que derrubou o olhar: **a baseline do delta tem de ser o ponto do
  warp, nunca a leitura do frame anterior.** Lida contra a leitura anterior, a conta vira
  (viajou no frame N) − (viajou no frame N−1) — a diferença entre duas viagens em vez da viagem — e um
  varrimento constante, que é exatamente o que girar para os lados é, se cancela e o yaw não anda.
  Prova headless em `VoxelPlayer.cpp` (módulo `player`, 27 checks), e `VOXEL_LOOK=<frames>` imprime o que a camada de
  ponteiro realmente entregou (`mp`, `base`, `d`, yaw/pitch) quando a dúvida for do lado da máquina.
- **Malha e textura da memória**: o mesher de chunk já faz os dois (`VoxelMesh.cpp:172-212`), e
  `Mesh::setTexture(id, TextureData)` + `TextureData(w,h,size,format,channels,data)` existem para
  uma skin vinda de bytes. O caminho mais simples para a skin do usuário é por caminho de arquivo
  (`textures/skins/<nome>.png`), sem depender do stb dentro do script.

**A fronteira de licença, escrita uma vez e valendo para 22/23.** Texturas, skins e modelos do
Minecraft original (Steve, Alex, texturas de mob, GUI atlas) são da Mojang/Microsoft e **não são
baixados nem embutidos** neste projeto. O que se entrega é um *sistema de skin*: a geometria é
original (um rig de cubos nosso), a **organização** dos 64×64 é o layout do formato — que é
especificação de arquivo, não arte — e os pixels vêm ou de placeholders gerados por script
(`generate_voxel_skins.py`, licença limpa como o atlas) ou do **arquivo do próprio usuário**,
colocado em `textures/skins/`. A skin que é sua funciona; a que é da Mojang não vem conosco.

## Arquitetura

```
VoxelConfig.h   números compartilhados: CHUNK_SX/SZ 16, WORLD_HEIGHT 128, SEA_LEVEL 42,
                id = 9 bits de tipo + 7 bits de estado, budgets de frame por geração/mesh,
                e o vocabulário das modas (enum + nome ↔ enum, um só lugar para os três lerem)
VoxelTiles.h    atlas: enum de slots + voxelTileRect() com meio texel de inset  (gerado)
VoxelBlocks     tabela estática por tipo: tiles por face, flags, luz emitida, filtro de sol,
                dureza, ferramenta, shapes de colisão em dezesseis avos, cor de tinta
VoxelChunk      um cilindro 16×128×16 de blocos, luz empacotada (sky<<4|block) e topo por coluna.
                É dados puros — uma worker thread pode possuí-lo
VoxelWorld      dono do mapa de chunks e do streaming em volta do jogador. expõe get/set/
                breakBlock/placeBlock e cinco std::function hooks (generate, relight, waterTick,
                loadChunk, saveChunk): um módulo ausente simplesmente não é instalado
VoxelMesh       constroi a geometria de um chunk: quad por face visível, AO 3×3, luz horneada em
                vertex color, textura no atlas. duas entidades: opaca (MASK) e liquida (BLEND)
```

Regras que sustentam o design:

- **Nada de ECS fora da main thread.** `Chunk` é dados puros e `ChunkRender` guarda só ids de
  entidade; o gerador e o save loader podem rodar em worker sem tocar em `Scene`.
- **Nada lança exceção através de uma future** (`_HAS_EXCEPTIONS=0` no build da engine): um módulo
  que falha devolve estado (`loaded=false`, `ok=false`), nunca `throw`.
- **Custo por frame é orçamentado**: `GEN_BUDGET_MS`/`MESH_BUDGET_MS` limitam quantos chunks um
  frame gera e malha; o resto fica para o próximo, ordenado por distância ao jogador.
- **Mesh = função dos blocos.** Quem muda um bloco chama `setBlock`, que suja o chunk e os
  vizinhos na borda (`DIRTY_SPREAD`); o mesher reconstrói a entidade. Iluminação é horneada no
  vertex color, então re-luzir também suja o mesh.
- **Determinismo.** `chunkSeed(worldSeed, cx, cz)` + `quantum::Noise` (hash inteiro) geram o mesmo
  mundo em MSVC e MinGW. Árvore que cruza a borda de um chunk é escrita pelo chunk vizinho, porque
  a altura do terreno é função pura de (x,z).

## Como compilar e rodar

```bash
# 1. regera o CMakeLists do projeto (fazendo uma vez depois de mexer em scripts/)
"$QUANTUM_BUILD_DIR/Debug/quantum-build-generator.exe" generate --project "C:/quantum2/templates/voxelworld-main"
# 2. build standalone de verdade (o hotreload do editor não liga ws2_32 nem expõe o exe)
cmake -S . -B .quantum/build-standalone -DQUANTUM_EDITOR_PLUGIN=OFF
cmake --build .quantum/build-standalone --config Release --target VoxelWorld
# 3. roda
./.quantum/build-standalone/VoxelWorld.exe
```

O passo 1 merece a armadilha: `generate` ressincroniza `<projeto>/.quantum/engine-api` a partir da engine
**instalada** (`build/Debug/engine/`, escrita pelo alvo `copy_quantum_editor_engine`) e não de
`C:/quantum2/engine/`. Editou um header da engine e foi direto ao `generate`? Ele sobrescreve a cópia
manual com o snapshot velho e o erro aparece no **projeto**, como `'hasFocus' is not a member of
'quantum::Engine'` — o header que você acabou de editar está certo no disco e invisível para o jogo.
A ordem é `cmake --build "$QUANTUM_BUILD_DIR" --target copy_quantum_editor_engine --config Debug` e só
depois `generate`. O mesmo `generate` quer caminho **absoluto** do projeto: com caminho relativo ele
dobra o prefixo e responde `project has no scenes to generate` com exit 1.

`VOXEL_SHOT=arquivo.png VOXEL_FRAMES=240` fazem o exe sair capturando um frame — é assim que o
trabalho gráfico é provado sem abrir o editor. `VOXEL_FRAMES` sozinho também fecha o run (sem
captura), porque um orçamento de frames que só encerra quando alguém pediu uma foto deixa toda
invocação só-de-sonda queimando CPU para sempre.

Os knobs da cena ficam no ambiente, nunca no arquivo `.scene` — assim o mesmo commit roda offline,
como host ou como cliente sem estado de máquina em git: `VOXEL_NET=host|client|off`,
`VOXEL_PORT`, `VOXEL_ADDR`, `VOXEL_SAVEDIR` (vazio desliga o save em disco), `VOXEL_NETLOG=1` liga o
trace do módulo de rede (sem ele o `VoxelNet` não imprime nada) e `VOXEL_NETEDIT="dx dy dz id"`, que
escreve **um** bloco relativo aos olhos do jogador — é o que torna o protocolo testável sem dois
humanos apertando teclas. `VOXEL_MODE=survival|creative|spectator` pinola a moda do processo inteiro e
**manda mais que a linha `Game Mode` da cena**, porque um run headless não tem inspector; um nome que a
build não conhece é sobrevivência. `VOXEL_THIRD=1` começa o run já vestindo o corpo — é o `[V]` de quem
não tem teclado, e a única forma de o avatar entrar numa captura. Do lado do som, `VOXEL_NO_SOUND=1` desliga o módulo inteiro e
`VOXEL_NO_AMBIENCE=1` deixa só os efeitos, sem as três camas em loop — é assim que se prova que um
ruído na captura é o ruído e não a cama que veio junto.

Controles com o módulo de jogador compilado (`VoxelPlayer.cpp`): mouse para olhar (a captura do
ponteiro começa no primeiro clique, `Esc` solta), `WASD`/setas, `Espaço` pula/nada, `Shift` agacha,
`Ctrl` corre (e desce no ar), `F` ou dois toques em `Espaço` alternam o voo — só no criativo, porque
em sobrevivência o flag não ganha ar e um toggle que o física ignora é um flag que mente. `M` cicla
sobrevivência → criativo → espectador e escreve na própria linha `Game Mode` da cena; no espectador
`Espaço`/`Ctrl` sobem e descem e o olhar pilota em três dimensões. `V` leva a câmera para trás da cabeça
quando o módulo de modelo está compilado: o que anda é a câmera, não o olho — alcance, afogamento e o
início de uma queda continuam lendo exatamente a mesma cabeça de antes. `1..9` e a roda escolhem o
slot, botão esquerdo quebra (segure para minerar), direito coloca, meio pega o bloco mirado,
`Q` troca a ferramenta e `R` o tier dela. Com o módulo de vida compilado, `G` come o que está no
slot (melão e abóbora alimentam; qualquer outra coisa o host devolve sem gastar) e `H` bebe quando a
cabeça está na água ou o bloco mirado é água — as duas só em sobrevivência, e o F3 só anuncia o que a
moda permite. Sem o módulo, o host fica com `1..9`, a roda e `R` (regenera o mundo). Em qualquer build
`F3` liga/desliga o overlay de diagnóstico.

## A sonda de comportamento (`VOXEL_PROBE`)

Cada módulo exporta `probeX(VoxelWorld&, voxel::Probe&)` e o host roda todos numa passada só:

```bash
VOXEL_PROBE=1 VOXEL_FRAMES=260 VOXEL_SHOT=/tmp/vprobe.png ./.quantum/build-standalone/VoxelWorld.exe \
    | grep PROBE
# PROBE TOTAL: 225 passed, 1 failed   (mesh 2, atlas 7, collide 13, light 12, water 8, player 33, life 43, break 10, audio 20, entity 15, drops 10, model 20, ui 18, save 15)
```

Somado ao harness de rede, que roda fora do processo (12, dois processos): **238 checks, 1 falha** — a
do vidro, listada como item aberto no fim deste arquivo.

**A sonda roda sobre o seed, não sobre o último mundo jogado.** O default do projeto salva em
`world/`, e um chunk lido de lá traz as edições que o processo anterior escreveu — inclusive as que a
própria sonda fez. Dois runs do mesmo binário deram totais diferentes (184 vs 182) por causa de um
bloco minerado pela run de antes, e nada no código mudou. Com `VOXEL_PROBE` ligado e sem
`VOXEL_SAVEDIR` explícito, o host redireciona o salvamento para
`<temp>/quantum-voxel-probe-world`, esvaziado na entrada: o mundo é o que a seed descreve, o módulo de
save continua instalado (parar de salvar **custaria** os 15 checks de save, que fazem `skip` sem
diretório) e o lixo da prova nunca pisa o `world/` do jogador. `VOXEL_SAVEDIR` manual ainda vence,
porque um teste de salvamento deliberado é dono do seu caminho.

**Desligar o salvamento de todo (`VOXEL_SAVEDIR=""`) muda o resultado, e não é instabilidade do
código.** O run dá `176 passed, 2 failed` em vez de `193 / 0`: os 15 de save somem (esperado) e mais
dois aparecem — `the run starts on dry ground at the waterline` (o começo cai em `-7,72`, chão 41 com
mar a 42, ou seja, dentro d'água) e `the eye is turned toward the coast the start chose` (o yaw 37,8°
aponta `(-0.59,-0.77)` enquanto a costa rebuscada do column atual está em `(-0.71,0.71)`). É a
reabertura do `#199`: a busca de praia aceitava uma coluna abaixo do nível do mar e, rebuscada a partir
da coluna em que o corpo já está, devolve outra costa — o número não mudou porque um chunk foi lido de
algum lugar, mudou porque **não havia mundo salvo para ler**. Os dois agora passam com
`VOXEL_SAVEDIR=""` (`start -10,76: ground 42, sea 42, 7 of 8 under, by water 1`; o olho mira
`(-0.71,0.71)` e a costa escolhida é `(-0.71,0.71)`, alinhamento 1,00), porque a busca foi reescrita
para recusar coluna submersa, para ranquear pelo mar aberto que a coluna vê e para partir de `(0,0)` do
mundo — ver "Água que se vê". A medição de referência continua sendo a receita de cima, com o diretório
frio da sonda.

O harness é `scripts/VoxelProbe.h` (header-only de propósito: a lista `SCRIPT_SOURCES` do projeto é
gerada, e um `.cpp` novo exigiria regerar o `CMakeLists.txt`). Ele dá três coisas a quem escreve a
sonda: `check(afirmação, ok, detalhe)` com o **estado medido** na linha — um FAIL tem que bastar para
diagnosticar sem debugger, porque não existe teste unitário aqui —, `VoxelPatch`, que lembra o id
original de cada célula no primeiro toque e devolve tudo ao sair (anda na ordem inversa), e
`carveArena()`, que abre um quarto de ar com chão de pedra na coluna em volta do jogador e devolve
`false` se a coluna ainda não streamou — para a sonda dar `skip` em vez de medir o vazio.

Duas regras que a sonda impõe:

- O andaime é escrito por `setBlockQuiet`, nunca por `setBlock`: o journal de salvamento e o broadcast
  de rede leriam o andar de prova como se fosse jogada do jogador. Uma derramada de água é a exceção
  (o simulador só vê edições pelo journal) e o probe limpa `pendingEdits()` depois.
- Uma afirmação só vale se ela sobreviver ao que o solver **realmente** faz. A de vidro dizia "a
  célula de vidro guarda o nível que chegou nela"; medida, a célula guarda 13 com 14 de ar acima.
  O motivo está no código: a passada por coluna cobra o filtro ao **sair** do bloco (`skyDown`) e o
  flood fill cobra ao **entrar** (`cost`), e num mundo com copa de árvore acima o flood ganha. O que
  não é ambíguo é o quarto debaixo do vidro — exatamente `filter` abaixo do céu em que o telhado fica,
  do teto ao chão. A sonda agora afirma isso, e para a segunda camada afirma monotonicidade (vidro a
  mais nunca deixa nada mais claro), que não depende de qual convenção vence a coluna.

Escrever as sondas achou defeitos que compilar nunca acharia. Os dois primeiros são de sonda: o
`static_assert` do formato do chunk estava engolido dentro de uma linha de comentário, então nunca
rodou, e a sonda do jogador ligava o voo criativo **antes** de checar se a coluna tinha streamado —
um run que respondia só "skip" deixava o boneco flutuando no menu.

Os outros quatro são do salvamento, e nenhum deles apareceria sem medir comportamento:

- **Nenhum arquivo de coluna era escrito, nunca.** `dirtySave` só nascia `true` no construtor do
  `Chunk` e na reposta do journal; `markDirty()` — o caminho de toda edição do jogador — não tocava
  nele, e o `seen` do estado só ganhava quem voltasse de uma leitura **bem-sucedida**, num mundo novo
  em que toda coluna chega como "nada guardado". O resultado medido: `index.txt` = `1 0` para sempre
  e um `edits.log` crescendo uma linha por bloco minerado, sessão após sessão.
- O journal de salvamento colhia as edições do **andaime de outra sonda**: as sondas escrevem alto de
  propósito (o simulador de água e a rede só veem edição pelo journal), e a lista `pendingEdits()` do
  mundo é compartilhada. A sonda de save agora a descarta antes de instalar o diretório próprio.
- `writeFileAtomic` chamava `write(nullptr, 0)` quando o journal ficou vazio, e `journalAppendNew`
  abria em modo texto enquanto a reescrita ia por binário: o mesmo `edits.log` traria `CRLF` e
  `LF` dependendo de quem escreveu por último. Medido como `17 bytes, expected 16`.
- Uma sonda não pode indexar um arquivo que o módulo pode não ter produzido. Um `dmg[1024]` sobre um
  vetor vazio virou um range check do MSVC Debug, um `abort()` e — sem console — uma janela modal de
  "Debug Error!" que pendurou o run para sempre. Agora todo flip passa por um helper com limite, e um
  módulo que não escreveu arquivo colhe **FAILs honestos** em vez de travar a suíte.

O quinto defeito não é do salvamento nem da sonda: é da **colisão**, e nada menos que a afirmação da
cabeçalho do módulo. `moveBoxVoxel` media o chão `VOXEL_GROUND_PROBE` (0,02) abaixo dos pés em todo
frame, inclusive num corpo que estava **subindo**. Uma nadada a 1,15 m/s sobe 0,019 por passo — menos
que a própria sonda —, então o snap devolvia o mob ao fundo, respondia `VOXEL_HIT_GROUND`, e o
integrador zerava a velocidade vertical. Medido antes da correção: `pés 62,000 → 62,000, in liquid 1`
em 90 frames, i.e. nenhum ser em água jamais subiu à superfície; ele andava debaixo d'água no chão.
O assentamento agora só vale para quem não está subindo (`dy <= 0`), a sonda de colisão crava o caso
na fonte (`62,0000 → 62,0190, flags=0`) e a de entidades prova o comportamento do lado do mob.

Duas das afirmações da sonda de entidades foram **reescritas** porque mediam a minha armação, não o
módulo: o buraco era cavado no assoalho da arena, mas `spawnVoxelMobs` apoia o mob no topo sólido de
qualquer coluna do raio — com frequência o morro não escavado dois blocos acima —, e a foto do último
frame pegava o bicho no meio de um pulo de parede. Agora o buraco abre sob os pés **do mob que o pool
devolveu**, e o que se afirma é o ponto mais baixo da queda e quantos frames ele descansou ali.

Uma terceira caiu na fase 22, e é do mesmo tipo. A afirmação da queda também cobrava `escaped == 0` — o mob
não sair do buraco de 3×3 — mas o buraco é cavado **onde o pool colocou o mob**, e ao lado de uma encosta
há ombro baixo o bastante para se andar por ele: sair, ali, é o mob funcionando. Ficava vermelho **1 vez em
umas quinze corridas**, com a queda, o pouso e o descanso todos verdes. O descanso agora é contado
**dentro do buraco e sobre o chão que a sonda acabou de pôr ali** — e nenhuma daquelas frames pode estar
dentro de um bloco —, enquanto o quanto o bicho andou para fora virou **detalhe impresso**: `12 frames
descansando no buraco, 0 dentro de um bloco, 0 andou fora`, e na corrida que antes falhava, `8 descansando,
0 wedged, 113 do lado de fora` continua verde. Não é um número perdoado: a exigência passou a estar amarrada
ao chão que a sonda criou, e não a uma altura em que o descanso aconteceu por acaso.

A fase de vida achou um defeito de módulo e um de controlador, e os dois só apareceram porque a
afirmação exigia o **número** da tabela:

- O afogamento media `air 0.00, health 13.00` onde a tabela diz 14,00. Os termos de dano liam a barra
  **depois** de o passo esvaziá-la, então o intervalo que estoura o último bolho era cobrado como se o
  corpo já estivesse sem ar — com `dt` grande entrava afogamento antes da hora, e "quanto tempo tem
  dez segundos de ar" passava a depender do frame rate. Agora os termos leem o estado com que o
  intervalo **começou** (`wasAirless`/`wasStarving`/`wasParched`), e a sonda fecha em exatamente 3 s
  de dano depois de vazio.
- `respawnAt` escrevia `feetX/feetZ` **antes** de perguntar `getTop`. Numa coluna ainda não streamada
  o corpo ia parar no vazio e o foco de streaming — que segue os pés — seria arrastado para lá: o
  mundo começaria a gerar chão a ~500 blocos do jogador por causa de um respawn falho. A checagem
  passou para antes de qualquer escrita, e a sonda afirma que o corpo **não se moveu** quando a
  coluna não existe.

E duas afirmações da própria sonda de vida eram falsas antes de o módulo estar errado: as quedas
mediam uma queda de zero blocos (`air.feetY == y` com `grounded = false`, e o pouso no mesmo `y`), e a
morte de um corpo já morto comparava um contador lido **depois** do dano com o mesmo contador lido
depois do dano — tautologia verde. Corrigidas, as duas passaram a falhar de verdade, e aí revelaram
o terceiro: o `reviveVoxelLife` devolve as barras em 12/12, mas o frame ocioso que a sonda roda em
seguida queima 0,005 de comida, e o `==` exato com `%.1f` no detalhe escondia isso. A afirmação agora
tolera um frame de decaimento e imprime `%.3f`.

## A sonda de rede (`scripts/voxel-net-roundtrip.sh`)

Nenhuma sonda dentro de um processo prova o `VoxelNet`: o jogo é host-autoritativo sobre TCP, então
o que um `probeNet()` mediria é uma chamada de função. O harness sobe o **mesmo** `VoxelWorld.exe`
duas vezes na mesma máquina — um como host, outro como cliente — arma uma edição scripted por lado
(`VOXEL_NETEDIT`) e lê o log do *par*, casando pelas coordenadas:

```bash
sh scripts/voxel-net-roundtrip.sh            # [frames do cliente] [porta]
# PROBE net: 15 passed, 0 failed
```

As 15 afirmações: o host escuta na porta pedida, o cliente é `welcomed`, o host registra o par
`linked`, os dois gravam o próprio bloco, **o cliente recebe o bloco do host nas mesmas
coordenadas**, **o host aceita o bloco do cliente nas mesmas coordenadas**, nenhuma edição foi
`REFUSED` (as duas pontas têm o mesmo seed, então o `previous` otimista bate), os dois overlays
carregam a linha de rede, os dois lados concordam no seed, um par que sai não vira erro de socket,
os dois processos se fecham sozinhos e os dois PNGs existem.

A prova visual fecha o círculo que o log não fecha: o log diz que o pacote chegou, só a imagem diz
que a malha do receptor foi reconstruída. Os dois blocos flutuam à frente das duas câmeras e a
região deles é **byte-idêntica** nos dois frames — 0 de 34 800 pixels diferentes.

Escrever o harness achou quatro defeitos que nenhum teste de função acharia:

- Com `VOXEL_FRAMES` igual nos dois lados, o host fechava durante os ~2,5 s de boot do motor do
  cliente. O lado de fora espera `HOST_FRAMES = FRAMES + 600`, e a espera pela porta é por
  `netstat`, não pelo log: com stdout redirecionado para arquivo a escrita é **em bloco**, então uma
  linha "listening" não é sinal de nada que aconteceu agora.
- Um host que grava o bloco scripted **antes** de alguém entrar transmite para ninguém, e quem entra
  depois lê aquela coluna da própria geração. A porta de entrada da edição espera `netReady()` **e**
  um jogador remoto visível.
- Um par que morre sem `BYE` é um `WSAECONNRESET` (10054). O `recv` devolvia `socketErrorText` e o
  host carregava `last error: recv failed (winsock error 10054)` no overlay pelo resto da sessão
  depois de uma desconexão comum. Agora reset/abort caem na mesma frase de um fechamento ordenado,
  que `dropConnection` já mantém fora de `netError()`.
- Rodar a sonda sem `VOXEL_SHOT` revelou o quarto: `VOXEL_FRAMES` só fechava o processo quando havia
  uma foto pedida, então um run só-de-sonda queimava CPU para sempre e o log — escrito em bloco, com
  a última linha cortada no meio — parecia hang. O orçamento de frames agora fecha sozinho, com ou
  sem captura.

## O formato em disco (Fase 8)

```
world/<cx>_<cz>.vck   [versão u32 LE][Chunk::serialize()][CRC32 u32 LE do corpo]
world/index.txt       "1 <n>\n" e depois n linhas "cx cz bytes crc"
world/edits.log       <cx cz x y z id> por linha: toda edição, antes de qualquer arquivo
```

A ordem das recusas em `validateColumnFile` é o que dá a cada dano um portão só: tamanho → versão →
**CRC** → mágica → **coordenadas do arquivo contra o nome**. A versão fica fora do CRC, então virar o
byte 0 é o único jeito de testar o portão da versão; já um arquivo íntegro morando no nome errado só
é pego pela checagem de coordenada. O journal é o piso de durabilidade: uma coluna é reescrita quando
suja, quando cai do LRU ou na saída, e entre esses momentos a linha dela é a única cópia da edição —
por isso uma re-leitura fria reproduz o bloco no mundo e marca a coluna suja de novo.

## HUD: o que a engine obriga (Fase 13, tudo medido em `vhud7.png`)

- O HUD é uma `Scene` própria adicionada com `Engine::addSceneLayer`. Uma entidade `Text` na cena do
  mundo é um quad na câmera em perspectiva — posicionada em **blocos**, não em pixels. A camada precisa
  de `setEnableUIEvents(UIEventState::DISABLED)`, senão a câmera UI reivindica os eventos de ponteiro
  que o controlador do mundo usa.
- Preset de âncora e offset manual **não se misturam**: `applyAnchorPreset` reescreve os quatro offsets
  a toda passada de layout. `anchorBox()` no host define pontos e offsets juntos, e são os offsets que
  carregam o tamanho da caixa.
- `setPivotCentered(true)` não centraliza dentro da caixa: ele desloca o run por metade da **caixa**, e
  a engine nunca dimensiona a caixa ao texto. Centralizar de verdade é medir o avanço com
  `Text::getCharWidth()` (pixels no tamanho gravado da fonte, válido logo após `createText()`) e
  re-ancorar em torno do centro desejado. `getAABB()` não serve: vem do vertex buffer já construído.
- O avanço é fracionário. Truncar por glifo perde ~1/3 de pixel por caractere — numa linha de 62
  caracteres foram 23 px a menos, e a placa ficava curta. Some em `float` e arredonda no fim.
- Uma `Text` criada com `new Text(scene)` dentro de `buildHud()` cai na cena do **mundo** se `scene`
  for o membro herdado de `ScriptBase`. O `C4458` de shadowing era exatamente o sintoma desse bug; a
  cena local do HUD tem nome próprio (`hud`) por causa disso.
- UI não tem z-order nenhum: o pass de desenho anda pelas entidades na ordem de criação. Uma placa tem
  que nascer **antes** do texto que ela atrás.
- Placa translúcida custa um PNG 4×4 branco liso (`textures/voxel/panel.png`, emitido pelo mesmo
  gerador do atlas): `Image::setColor` multiplica a textura, então a tinta é a placa.
- `TextComponent::maxTextSize` é 100 por padrão. O bloco de diagnóstico passa disso no segundo frame e
  cada crescimento loga warning e reallocs o buffer — o host abre com 1024.

## Rachadura no bloco (Fase 18, tudo medido em `vb15_stage.png`)

O overlay é uma entidade própria: um cubo de 24 vértices inflado em `BREAK_INFLATE` (1,0045) sobre o
bloco alvo, `receiveLights=false` para a rachadura escurecer junto com o bloco não é o que se quer
numa caverna, e o estágio trocado escrevendo só `submeshes[0].textureRect` — a geometria nunca é
tocada de novo. Três coisas ahí dentro só se descobrem medindo:

- **`transparent` tem que ser forçado.** `usesAlphaBlend()` (`RenderSystem.cpp:209`) só conta
  `BLEND` — `MASK` não liga o `autoTransparency`, então o cubo ia no pass opaco. E aí o bloco embaixo
  perdia a corrida: onde o tile é transparente o quadro ficava com a **cor de fundo da cena**
  (`backgroundColor: [0.25, 0.45, 0.65, 1]`), um losango azul chapado de ~18 000 px ao lado da
  rachadura. `mc.transparent = true` resolve. O controle que provou: `VOXEL_NO_CRACK=1` roda o mesmo
  seed/posição e o azul **desaparece inteiro** — o diff dos dois PNGs é 39 358 px dentro da área do
  overlay e 265 px (só o texto de FPS) fora dela.
- **`ALPHA_MASK` não expressa meio-tom.** `mesh.frag:311-319` descarta abaixo de `alphaCutoff` e força
  `a = 1.0` no que sobrevive. O gerador do atlas escrevia alfas 110/137/193/250 nos dez estágios: o
  resultado era mancha preta grossa que **pipoca** de invisível a sólida ao cruzar o corte, em vez de
  afinar. O fade agora é 1-bit — o que existe é carregado, o resto é a espessura do traço (cabeça de
  raio em todo pixel, cauda em um de dois, `s % 3` para lasca clara do estágio 6 em diante). Os dez
  tiles ficaram com alfa **só 0 ou 255**, e a contagem de opacos cresce 8 → 21 → 28 → 40 → 51 → 61 →
  95 → 98 → 116 → 124.
- **A variante usada é `mesh:Ult,Uv1,Vc4,Txr,Fog,Ams`, bits `0x01001C03`.** Ela já está no bundle em
  `%LOCALAPPDATA%/quantum/shaders/v40` (`mesh_UltUv1Vc4TxrFogAms_glsl410.sdat`); mover o overlay para o
  caminho *lit* exigiria gerar um `.sdat` novo, porque não existe variante `Txr` acesa.

`VOXEL_MINE=<updates>` segura o botão sem mouse nenhum, `VOXEL_MINE_DELAY` atrasa a armação e
`VOXEL_MINE_SLOW=n` faz o relógio de mineração andar a 1/n — sem isso um bloco de terra morre em 4
updates e não há estágio que fotografar. `VOXEL_NO_DEBRIS=1` e `VOXEL_NO_CRACK=1` tiram uma camada por
vez: lasca e rachadura são escuras, caem no mesmo bloco e na mesma distância, então um PNG sozinho não
separa as duas.

**Runs não são determinísticos no que importa.** O fps medido variou 6/18/31/44/51 entre comandos
idênticos, e a posição do jogador e o bloco alvo mudam junto (`34.0 62.6 34.0` alvo `34,61,32` numa
run, `34.0 63.6 27.6` alvo `33,61,26` na outra). Comparação de pixel entre runs **não** é controle: a
atribuição tem que acontecer dentro de uma run só.

## Som: o pack e as vozes (Fase 19, tudo medido por sonda)

O pack é gerado do zero por `scripts/generate_voxel_sounds.py` (raiz do repositório): 35 WAV PCM
44,1 kHz / 16 bits / mono, 1 926 946 bytes, pico mestre −3 dBFS, soma `3104914fad8fa324` impressa no
`sounds/voxel/manifest.txt` que acompanha os arquivos. É a mesma disciplina do atlas: nenhum ruído
baixado, tudo determinístico — cada arquivo tem sua própria semente, então regerar reescreve os
mesmos bytes. O manifest traz zcr/s e a energia por banda (abaixo de 250 Hz, 250–2500, acima) por
amostra, que é como se confere que areia é chiado (0,82 na alta) e terra é grave (0,74 na baixa) sem
abrir o áudio.

O contrato de nome vive em dois lugares e a sonda é o que costura: `SoundMaterial` tem cinco valores
e o gerador nomeia os arquivos por esses cinco nomes. `samplePath()` decide o sufixo `_N` **por
evento**, não por contagem — baque e colocação são um arquivo só (`break_stone.wav`), golpe e passo
são sempre indexados (`dig_glass_0.wav`, mesmo com um grão só), porque um segundo grão adicionado
depois não pode renomear o primeiro. A checagem abre cada caminho com a própria camada de arquivos
da engine (`Data::open`), então deriva em qualquer direção falha um check em vez de logar "sample
missing" uma vez por frame.

O módulo escreve `SoundComponent` direto, e cinco comportamentos da engine só aparecem lendo o
código dela — estão gravados aqui porque cada um deles é um bug silencioso:

- **`filename` vazio destrói a amostra compartilhada.** Trocar o nome dispara o `destroySound` do
  sistema, que chama `SoundPool::remove` e tira do pool a amostra que **as outras vozes ainda estão
  usando** — um anel que alternasse entre 35 arquivos releria o disco a cada disparo. Por isso o
  módulo escreve o componente e reseta o `shared_ptr` (o carregador só consulta o pool quando ele
  está vazio), nunca limpa o nome.
- **`needUpdate` é a porta de tudo.** `AudioSystem::update()` empurra posição/velocidade/gain 3D só
  para quem tem o flag aceso; uma escrita sem flag é ignorada sem aviso.
- **`handle` reciclado não é voz viva.** A SoLoud reindexa handles; chamar `stop` em um handle
  fora do orçamento de tempo cala **outra** voz — a do próprio módulo, neste caso. Só para handle
  dentro do orçamento, e só quando o componente ainda se reporta vivo.
- **`state` nunca volta a `Stopped` sozinho.** Quando o sample termina a engine deixa o estado em
  `Playing`, então vivacidade é `Playing && length > 0 && playingTime > 0 && playingTime < length`,
  medida contra `Engine::getSystemTime()` — que é **segundos de relógio de parede** (`stm_sec`), não
  tempo de simulação, e é por isso que nada aqui é cronometrado por `dt`.
- **Sem câmera na cena não existe 3D.** O ouvinte é a câmera da cena, lida pelo `AudioSystem` a
  cada frame; sem ela o som degrada para 2D em silêncio. O probe afirma `3d 1` em vez de assumir.

O anel de tiros únicos é fixo (12 vozes) e **rouba voz em vez de crescer**: 15 disparos viram 15 plays e
os que não acham voz livre tomam o som mais antigo. A quantidade esperada de roubo é **medida** na
sonda, não assumida — a sonda do jogador roda antes e seu break/place criativo já deixam vozes
tocando. As três camas (dia, noite, submersa) nunca são roubadas; o crossfade é função do `dayLight`
e o mergulho abaixa as camas a `AMBIENCE_DUCK` (0,35) e os impactos a `UNDERWATER_DUCK` (0,62) — a
cama perde mais porque um golpe submerso ainda tem seu transiente para ser localizado, e uma cama
contínua é justamente o som que tem que parar de competir com o splash. A engine não tem equalizador
por voz, então é o gain que faz o papel do filtro. Passo é cobrado por **distância** (um a cada 2
blocos, `STRIDE_BLOCKS`), então ele desacelera com o jogador em vez de rodar em timer — e a sonda
cobra as duas metades dessa frase: três frames de exatamente uma passada cada disparam três passos
(o acumulador devolve a passada quando dispara, então a sobra nunca vira passo fantasma), e a mesma
distância gasta no ar não dispara nenhum.

O F3 mostra a linha `Sound: N/12 voices (plays, steals, missing)` — é o único jeito de *ver* o
módulo funcionando numa captura headless, e as duas primeiras palavras dela separam os dois modos
silenciosos que um jogador chamaria de "mudo": `Sound: off (VOXEL_NO_SOUND)` quando o ambiente
desligou o módulo, `Sound: silent (no voices built)` quando o módulo nunca se conectou à cena. A
sonda confere a linha, então um formato quebrado em `audioStats()` falha em vez de
sumir. A linha está no frame apresentado de `vaudio_shot2.png`, e a placa atrás do texto cresceu com
ela — o overlay é medido a partir do próprio texto, não de uma caixa fixa.

Quem decide o que soa é o host, como no resto do projeto: o controlador reporta (`blockBrokenFn`,
`blockPlacedFn`, os campos `footX/footY/footZ`/`footBlock`) e `VoxelGame` traduz em efeito — o som da
picareta inclusive cavalga a cadência da lasca em `updateBreakFx`, porque a picareta e as lascas
são o mesmo golpe e dois relógios divergiriam dentro de uma quebra. `VOXEL_NO_SOUND=1` desliga o módulo
inteiro, que é o controle A/B de qualquer captura.

**O que a sonda não prova:** nada foi ouvido. Ela prova que cada arquivo nomeado existe, que cada
material mapeia para uma superfície, que o anel não cresce e que os gains das camas seguem a
iluminação e a água. Mixagem — se o passo está alto demais, se o break mascara a cama — é ouvido por
humano, e os levels (`dig 0.55`, `break 0.9`, `place 0.7`, `step 0.35`) são arbitrados por
cálculo de repetição, não por escuta.

## Vida, fome e sede (Fase 20, tudo medido por sonda)

**O módulo é aritmética sobre um relatório.** `updateVoxelLife(body, seconds)` não lê o mundo, não lê o
player e não lê o relógio da engine: o host preenche um `VoxelBody` e entrega os segundos que
passaram. É isso que torna as 28 afirmações da entrega possíveis — a sonda gasta exatamente 100 segundos de pé,
o que um módulo que amostrasse o mundo sozinho só provaria com um humano andando eles. E é isso que
faz a afirmação de integrabilidade valer: dez segundos num passo ou em 500 frames dão o mesmo número
(`%.4f vs %.4f` comida).

As taxas moram no header, cada uma com o motivo do lado: escala 20 (um coração são 2 pontos, e a linha
do HUD é um número inteiro de ícones de meia barra), ocioso 0,05 comida e 0,08 água por segundo, ×2
andando e ×3 correndo, pulo 0,06 cobrado **uma vez**, queda grátis até 3 blocos e 1 por bloco depois,
lava 6/s, cacto 1/s, afogamento 2/s, fome 0,3/s e sede 0,5/s (sede mata antes), ar de 10 s recheado a
4×, cura de 0,4/s portada em comida ≥ 16 **e paga em comida** a 0,09/s — quase o dobro do custo de
ficar parado, e só com o corpo inteiro fora de dano: regenerar dentro da lava seria duas regras
brigando.

**A queda é medida de um pico auto-rastreado, não da velocidade.** `airPeakY` sobe enquanto o corpo
está no ar e a aterrissagem cobra `pico − pés`. O código de movimento do controlador não foi tocado, e
é por isso que "água tira a queda", "ser elevado pelo mundo não é queda" e "aterrissar fora de voo não
é queda" são verificáveis sem simular um frame de física.

**O dano contínuo é agregado por causa e aplicado como um número só**, e a morte leva o nome do maior
contribuinte do frame que terminou o corpo (medido: lava + cacto → `DAMAGE_LAVA`). A morte é **um**
evento, consumido pelo primeiro que perguntar; um corpo morto não aceita dano nem morte a mais, e não
decai.

**Quem amostra o mundo é o host** (`updateLifeFx`): um anel de 3×3 colunas em duas alturas (18 células
em volta do corpo) decide `inLava`/`touchingHazard` — o celular dos pés entra, porque lava na altura do
joelho queima; o `moved` horizontal é **topado em 1 bloco**, porque um respawn move o corpo a distância
toda até a coluna do spawn e isso não é uma passada que valha comida; o spawn é gravado uma única vez,
**quando o mundo tem um topo** para aquela coluna; e `respawnAt` é re-tentado a cada frame até a coluna
existir — um corpo posto onde ainda não há geração arrastaria o foco de streaming atrás de si.

**`[G]` come e `[H]` bebe a hotbar que já existe.** `voxelFoodNutrition(type)` devolve 6 para melão, 4
para abóbora e 0 para o resto; 0 faz o host **não gastar** o item, e `eatVoxelLife` também responde
`false` quando a barra já estava cheia — a melancia não vira nada de graça. Beber exige água: cabeça
submersa, corpo na água, ou o bloco mirado sendo água. A tabela de comida (e a de perigo, e a de
material de som) mora no módulo e não num campo de `BlockDef` porque `g_blocks` é 59 agregados
posicionais: uma coluna nova teria que ser escrita 59 vezes, e a linha que esquecesse dela herdaria
silenciosamente a linha de cima.

O F3 ganhou a linha `Life: 17 hp  20 food  12 water  air 9.0  dead 1` — valores rotulados, não fração,
porque a sonda relê a linha andando os números em ordem e um `17/20` entregaria o máximo junto; a
afirmação "the overlay's life line reports the bars it is holding" falha se o formato quebrar.

**O que a sonda não prova:** nada do que o host faz com o mundo foi medido. Nenhuma afirmação planta um
cacto ao lado do jogador e cobra o ponto de vida, nenhuma aperta `G` num processo real, e nenhum
jogador morreu num run capturado — o que está provado é o **cálculo** (taxas, bordas, integrabilidade,
a linha do overlay) e o `respawnAt` do controlador. As barras também não têm HUD: o que se vê é texto
no F3, e o coração de fase 25 ainda não existe. O portão de modo — criativo não decai, espectador não
tem corpo — veio depois, na fase 21 abaixo: esta entrega foi deliberadamente um módulo que sabe dizer
o que aconteceu, não um que decidia quando podia acontecer.

## Modos de jogo (Fase 21, tudo medido por sonda)

**O modo é um nome, não um bool.** `VoxelConfig.h` traz `VOXEL_MODE_SURVIVAL/CREATIVE/SPECTATOR` com
`voxelGameModeName`/`voxelGameModeFromName`; a cena tem a linha `Game Mode` (`survival` / `creative` /
`spectator`), e `VOXEL_MODE` no ambiente manda no processo inteiro, porque um run headless não tem
inspector para mudar nada. Um nome que esta build não conhece — `adventure`, caixa errada, linha vazia
— é **sobrevivência**, o mais estrito dos três: um erro de digitação custa um jogo, não entrega um
passe livre. As duas afirmações (ida e volta pela tabela, e o desconhecido que vira o mais duro) rodam
antes de qualquer guard de streaming, porque não dependem do mundo.

**O controlador possui o modo; o módulo de vida é informado.** O host chama `setVoxelMode` todo frame,
e `updateVoxelLife`, `damageVoxelLife`, `eatVoxelLife` e `drinkVoxelLife` se recusam fora de sobrevivência
por conta própria. A razão é a regra do projeto, escrita no código: uma regra que só vale enquanto cada
chamador lembra de perguntar não é uma regra — a mordida de um mob da fase 22, o `G` de comer e a sonda
são três chamadores que não estavam previstos.

**`isAirborne()` é a resposta do modo, não o toggle.** Sobrevivência nunca está no ar — o flag é
**ignorado, não limpo**, então quem volta do criativo mantém o `F` que deixou ligado e não ganha o ar;
criativo está no ar quando o toggle diz; espectador está sempre. O controlador publica isso como
`isFlying()`, e é por essa porta que o host preenche `VoxelBody.flying` — o campo com que a vida precifica
uma queda. Um toggle esquecido no criativo, lido cru, cancelaria dano de queda em sobrevivência com o
corpo no chão.

**Quem escreve no mundo é gateado dentro da função que escreve.** `canEdit()` abre `updateMining`,
`tryPlace` e `pickTarget`, não só a leitura dos botões: a sonda, o realce de entidade de uma fase futura
e qualquer chamador novo herdam a recusa. Medido: no espectador o voxel continua o mesmo depois de
mineração de um segundo, de um place e de um pick, o progresso fica em `0.00` e a pilha da mão não muda
de `stone x3`. O voo do espectador é o olhar como volante, normalizado em três dimensões (apontar para o
chão e segurar `W` entra no chão na mesma velocidade de um voo reto — é para isso que a moda tem pitch),
`4,4×` a velocidade, sem gravidade, sem chão e sem arrasto de água ou lava: um corpo que atravessa um
bloco não está dentro dele.

**Trocar de moda não apaga as barras.** Elas são **guardadas**, não zeradas: `setVoxelMode` re-ancora o
pico da queda só numa mudança real (a queda em progresso quando o criativo assumiu não é cobrada na
volta) e levanta um corpo morto ao sair de sobrevivência sem contar uma segunda morte; o host reconcilia
o `lifeAwaitingRespawn` com `voxelAlive()` porque um respawn pendente de um corpo que a nova moda já não
tem não é uma coisa esperando acontecer. Medido: `9,950 / 9,920 / 9,000 / 10,00` voltam exatamente como
foram deixados, e `deaths` não muda.

**A placa F3 é escrita pelo que foi compilado e pela moda em vigor.** `[G] eat [H] drink` só aparece em
sobrevivência (fora dela o módulo não aceita comida nem água), `[M] mode` desaparece quando `VOXEL_MODE`
pinou o processo (uma tecla que o ambiente overruleia é uma tecla que não funciona), e a linha `held:`
vira `spectator` com `[space] up [ctrl] down`. A hotbar e o crosshair somem no espectador por **alpha,
não por teardown**: o frame que sai da moda repõe os mesmos widgets com os mesmos rótulos. Corrigido
junto: a placa anunciava `[R] new world` mesmo com o controlador dono do `R`, que é o tier da ferramenta
— agora cada um dos dois caminhos do `#ifdef` anuncia a tecla que realmente trata.

**O que a sonda não prova:** `[M]` nunca foi apertado por uma mão — a ciclagem é provada escrevendo o
nome na linha da cena e relendo as regras que dele decorrem, não pelo gesto. O HUD em si é a única parte
da entrega que só uma janela veria, e por isso ela foi capturada, não apenas afirmada: `voxel_mode.png`
(sobrevivência: 9 dígitos, célula âmbar selecionada, `+` no centro) contra `voxel_spectator.png` (mesmo
terreno, mesmo enquadramento: nenhum dígito, nenhuma moldura, nenhum `+`). Um run pinado em
`VOXEL_MODE=spectator` fecha em **131 passed / 0 failed** com `PROBE SKIP player` dizendo que a moda não
deu mãos ao módulo — o skip existe para que uma pinagem não vire doze falhas falsas.

## Modelos do player e dos mobs (Fase 22, tudo medido por sonda e fotografado)

**O corpo são seis cubos e nenhuma junta.** `VoxelModel.{h,cpp}` monta cabeça, torso, dois braços e duas
pernas como **seis entidades independentes** — sem hierarquia de transform e sem skinning. O motivo é de
custo: o pool de mobs já pagava uma escrita de transform por mob por frame; seis são a mesma ordem de
grandeza, e nada aqui assume que o renderer compõe transform de pai. Nenhum vertex buffer é tocado depois
de o rig ser criado, então as seis malhas continuam alocações **IMMUTABLE** como as dos chunks — a
animação é aritmética (`voxelModelPartPose`) escrita em transform. É por isso que os 20 checks do módulo
rodam sobre números, não pixels: um rig sem renderizador ainda tem juntas, e junta é o que é afirmável.

**A skin é o layout, não a arte.** `VOXEL_SKIN_PX` é 64 e as áreas de mapa são as do formato: face da
cabeça em **(8,8)**, coroa em (8,0), nuca em (24,8), torso em **(20,20)**, braço direito em **(44,20)**.
Duas afirmações fazem o layout servir em vez de ser decoração: **nenhuma parte divide um texel com
outra** (uma skin pinta seis corpos ao mesmo tempo) e a área de cada face tem **o tamanho da própria
face** — um cubo 8×8×8 não pode ocupar um retângulo 12×4. Os pixels vêm de
`scripts/generate_voxel_skins.py`, que desenha um 64×64 de paleta plana (cabelo 52,37,24 · camisa
0,123,123 · pele 200,153,105 · calça 48,58,112, resto transparente) em `textures/skins/default.png`. A
amostragem é **NEAREST** com `CLAMP_TO_EDGE`, como o atlas de blocos: a face 8×8 *é* o visual, e filtrar
uma skin até virar mingau a distância é como um jogador para de parecer o desenho que alguém fez.

**Escala é altura, não redesenho.** Uma skin é autorada para um corpo de **2 unidades** (pernas 0,75 +
torso 0,75 + cabeça 0,5), então `voxelModelScaleForHeight(h) = h / 2`: um jogador de 1,8 bloco é 0,9 e um
mob de 1,4 é 0,7 — o mob não é uma skin menor, é a mesma skin mais baixa. A afirmação que une modelo e
colisor é a medida: **a coroa do modelo é o topo da hitbox** (pescoço 65,350, escala 0,900, cabeça 0,500
→ coroa 65,800 contra um colisor que para em 65,800). Um dedo a mais é uma cabeça que atravessa o teto
sob o qual o corpo passou; um a menos é um jogador em pé sobre os próprios pés.

**O erro que um rig de cubos sem esqueleto comete** é construir o membro **centrado** na junta: isso é um
corpo em T-pose para sempre, e nenhum balanço esconde. A geometria local de braço e perna nasce
**pendurada** abaixo da junta, e a sonda cobra o número — cotovelo em **60,750** contra a linha do quadril
em **60,750**, e 0,375 para o lado. O passo é fase adiantada pela **distância realmente andada**
(`voxelModelWalkPhase`, 1,6 bloco por ciclo tanto para o jogador quanto para o mob), não por tempo: frame
rate que muda não troca o passo nem arrasta o pé, e uma volta inteira de fase devolve o corpo onde estava
(0 partes moveram depois de 2π). Braço direito e perna esquerda andam em antífase medida (−17,1 / +17,1),
a amplitude cresce com a velocidade (0 / 17,1 / 38,7) e **para em 55°** — um braço que passa do ângulo
reto atravessa o torso. A cabeça gira **relativa** ao corpo (corpo 30°, cabeça 75°) e o pitch é só da
cabeça.

**Os dois sinais que se confundem** — e que a sonda agora mede nos quaterniões que a engine está
segurando, em vez de repetir a álgebra deste arquivo: a câmera olha pelo seu **−Z** enquanto a face de um
cubo é o seu **+Z**, e um pitch de câmera positivo olha **para cima** enquanto um pitch de cabeça positivo
inclina a face **para baixo**. Daí `yaw do corpo = yaw da câmera + 180°` e `headPitch = −pitch`. Provado em
yaw 37 / pitch −14: `look (-0.584, -0.242, -0.775)` e `face (-0.584, -0.242, -0.775)` — **dot 1.00000**. A
cabeça que o jogador veste olha exatamente para onde a câmera olha; do lado do mob, 8 de 8 rumos têm a
face +Z apontando na direção em que o sim anda.

**Terceira pessoa não move o olho.** Neste projeto `bodyEntity == camEntity`, então puxar a câmera para trás
da cabeça é mover a entidade da câmera — e o `posX/posY/posZ` **publicado** continua sendo a cabeça. É
essa a razão da separação: o alcance do raycast, o afogamento e o início de uma queda leem o olho
publicado, e um ângulo de câmera que arrastasse o olho para trás mudaria a jogada por fora. Medido: olho
34,50 65,62 34,50 contra pés 34,50 64,00 34,50, câmera a 3,20 e −3,20 ao longo do olhar. E o recuo é
**stopado por parede**, não enterrado: a sonda escreve pedra no voxel onde a câmera ficaria, e ela vem para
35,66,36 (segurando ar) a **2,22** do olho em vez de 3,20. `[V]` liga tudo isso; `VOXEL_THIRD=1` faz o
mesmo num run sem teclado, porque a única foto que este projeto pode tirar é uma captura.

**Do lado do mob, o rig pende do corpo do sim, não do ponto onde nasceu.** `createVoxelModelRig` é chamado
uma vez por slot (com a tinta do slot multiplicando a skin: as mesmas quatro cores da fase 11 — grama,
terra, pedra e a rosa fora do padrão), e `poseMobModel` escreve a pose com o `m.y` **daquele** frame. A nova
afirmação (`entity 14 → 15`) confere quadril e pescoço contra as alturas que o módulo promete para a escala
daquele corpo — `4 mobs posed, 0 with a joint off its height` —, o que também diz que o rig não ficou
posado no spawn. O cull esconde as **seis** partes junto com a âncora (esconder só a âncora deixaria cinco
cubos desenhando no lugar do mob), e um teardown não deixa entidade viva atrás: os buffers são alocações da
GPU, e a GPU as recolhe junto com a entidade.

**Sem o módulo, o mob continua existindo.** `VoxelModel.h` é opcional (`__has_include`) e o caminho de
fallback desenha o mob como **uma** malha de dois cubos na tinta do slot — exatamente o que a fase 11
provava, e o controle A/B de qualquer captura. As duas configurações compilam sem aviso no MSVC e no GCC
`-Wall -Wextra`; é isso que permite a fase 22 entrar sem quebrar um build onde a skin não está. O avatar do
jogador também é discreto: `thirdPerson` e o rig só existem dentro do `#ifdef`, e em primeira pessoa o
corpo **não é desenhado** (`pose.visible = thirdPerson && !isSpectator()`), então a fase 22 não custou um
triângulo no jogo em primeira pessoa.

**O que a sonda não prova:** o pixel. Ela prova texel, junta, ângulo, altura e visibilidade; **que** o corpo
texturizado aparece na cena foi visto em três capturas — `voxel_third.png` (o mundo em terceira pessoa com
12 mobs), `crop_mobs.png` (um mob legível como humanoide texturizado: cabelo escuro, rosto de pele, torso
azul-esverdeado, pernas) e `voxel_mine3rd.png` + `crop_mine.png` (o próprio corpo do jogador enquanto o
harness de mineração roda). O que a **rede** ainda não veste: um avatar remoto recebe posição, não corpo —
a pose é uma estrutura só, então vestir um jogador remoto é ligar o mesmo `poseVoxelModelRig` ao que já
chega pelo `POS` — está listado como tarefa aberta. Numa máquina nova, o material do modelo pede as
variantes `mesh:Uv1,Puc,Shw,Nor,Vc4,Fog,Sao,Aop` e `...,Dlo` já compiladas em
`%LOCALAPPDATA%/quantum/shaders/v40`: sem elas o processo avisa e sai 1, porque **a runtime não compila
GLSL**.

## Drops de item (corte de 24, tudo medido por sonda e fotografado)

Um bloco minerado **cai no mundo** antes de cair na mão: `scripts/VoxelDrop.{h,cpp}` é um pool de
`MAX_DROPS = 96` cubos de `0.30` bloco, cada um desenhado com o **tile do próprio bloco no atlas**,
girando `90°/s` e flutuando `0.05` acima da sombra. O cubo e não um ícone porque o projeto ainda não
tem atlas de itens — e o tile do bloco já diz o que a coisa é.

Não há `Body3D` aqui, como em nenhuma entidade do projeto: a grade voxel é o colisor e o item anda por
ela com o mesmo `moveBoxVoxel()` do jogador e dos mobs, então um item entra no canto, sobe o degrau e
flutua na água **pelas mesmas razões** que um corpo. A gravidade é `24` (mais leve que os `28` do
jogador: um item parece arremessado), o empuxo para cima é `6`, e o passo é limitado a `0.1 s` para um
travamento não atravessar um assoalho.

A animação é **função da idade, não acumulador** (`dropSpinDeg(age)` / `dropBobY(age)`): um drop que
nunca foi desenhado continua girando, e a sonda mede o turno sem cena.

**A costura é um ponteiro de arquivo, duas metades separadas de propósito:**

- `itemDropSpawner` — o controlador chama quando um bloco vira item (`VoxelPlayer.cpp:477`) e quando
  `[J]` arremessa o slot selecionado 2,2 blocos à frente. Sem o módulo, o mesmo `VoxelPlayer.cpp:485` devolve o
  item direto para a mão: o jogo inteiro funciona sem drops.
- `dropCollectFn(type, count, x, y, z)` — o drop **pergunta**; quem responde é a mão
  (`addToHotbar`, `VoxelPlayer.cpp:426`: enche pilha existente, depois slot vazio, depois o próximo
  vazio — um tipo nunca se espalha por nove slots enquanto uma pilha dele está em 64). Resposta
  `false` deixa o item onde estava. É todo o motivo da separação: **"a hotbar está cheia" vira uma
  coisa que se olha**, e não um bloco que desapareceu no instante em que foi minerado. O que ainda
  desaparece é o item do **outro** caminho: a mineração (`VoxelPlayer.cpp:545`) ignora a resposta — `#223`.

O ímã fecha o último passo sem buscar pela tela inteira: `DROP_MAGNET_RADIUS = 1.8` puxa,
`DROP_PICKUP_RADIUS = 0.85` toma, `DROP_DELAY = 0.6 s` impede que um drop recém-nascido volte para a
mão de quem o jogou, e a recusa pergunta de novo a cada `0.25 s` em vez de todo frame. `DROP_MERGE_*`
junta duas pilhas vizinhas do mesmo bloco em uma de dois, e `DROP_LIFETIME = 180 s` faz item
abandonado expirar em vez de comer o pool.

**10 checks na sonda** (`drops 10`): nasce no mundo e não na mão (`born 1 slot 0 live 1`), é o bloco
que caiu e começa acima do assoalho (`type 1 amount 1 y 40.90 floor 40.00`), a gravidade para no bloco
de baixo (`y 40.150 grounded 1`), gira `45.0°` em `0.5 s`, bob de `0.050` de `0.050` com `0.056` de
deslocamento total, duas pilhas viram uma pilha de dois (`merged 0 -> 1, live 1 -> 1`), **um corpo que
não pode receber deixa o item no mundo** (`live 1, 2 asks refused`), o ímã sobe o item até o peito
(`40.150 -> 40.774`, peito `40.720`), liberar um slot toma a pilha inteira (`picked 0->1 last 1 x2`) e
a sonda não deixa nada no mundo.

Um capture headless não tem dedo, então `VOXEL_DROP=<n>` arremessa `n` blocos da paleta na frente do
olho no frame 90 (`seedDrops`). Foi ele que achou o defeito de verdade: `lookDirection()` devolvia
`+worldDirection`, que é o **trás** da câmera — os itens eram puestos atrás das costas do jogador e o
PNG mostrava chão vazio. Corrigido para `-worldDirection` e a linha mira agora o bloco do raycast.
Fotografado em `voxel_drops_g.png` / `crop_drops_g.png`: cinco cubos apoiados à frente, cada um
virado para uma face diferente (228° no frame do obturador).

**O que ainda não está aqui:** o som do item caindo, o sprite de item (fase 24/26, `#204`) e o
`loot_table` real (fase 26, `#210` — hoje o drop é o próprio bloco minerado).

## UI com os sprites do pack (corte de 25, medido por sonda e fotografado)

`VoxelUi.{h,cpp}` é a camada que veste o HUD e os modais com a arte do pack, e nada dela é obrigatório
para o jogo rodar: sem o pack no disco cada função devolve `false`/`0` e as placas procedurais de
`buildHud()` continuam exatamente como foram escritas.

- **A tabela.** 19 linhas em `kSprites[]` (`nome`, caminho relativo a
  `assets/minecraft/textures/gui/sprites/`, largura, altura, borda). O caminho é resolvido por
  `FileData::getSystemPath` porque o VFS não serve para um arquivo que o jogo apenas **nomeia**. Cada
  sprite carrega **uma vez** por run; os pixels ficam no `Loaded` do processo e o widget recebe uma
  view.
- **PNG cinza vira vermelho.** Um PNG de 1 canal chega do loader como `ColorFormat::RED`, e o shader
  que amostra R8 lê `(v, 0, 0, 1)`: todo sprite grayscale do pack pintaria **vermelho** na tela. A
  camada expande para RGBA numa cópia sua (o quarto byte é opaco porque o arquivo não tem alpha) e
  recusa o que não for RED nem RGBA — com uma linha de log, não com um crash. É o que o check
  `grayscale sprites reach the widget as RGBA, not RED` trava.
- **A view não é dona de nada.** `TextureData(w, h, size, fmt, channels, void* data)` pede `void*`
  **não-const** (um `const unsigned char*` não compila: "conversão perde qualificadores") e a cópia é
  shallow, com dtor que só libera quando `dataOwned`. Uma `TextureData` dona dos mesmos pixels seria
  liberada por nós **e** pelo pool que a copia; a view que nunca se libera é a forma segura.
- **Nine-slice lido do disco.** `Image::setPatchMargin(border)` e a borda vem do `.mcmeta` do próprio
  pack (`gui.border`), não da tabela: botão 3, painel 6, tooltip 10. A sonda compara o que leu com a
  tabela e conta `border mismatch` — uma tabela que diverge do pack é um bug de arte, não de código.
- **Um sprite = um arquivo.** `UISystem::createImagePatches` mapeia os UVs pela textura **inteira**,
  então uma célula de atlas não é possível no `Image` — é o contrário do atlas de blocos, que o mesher
  recorta por `voxelTileRect()`.
- **`setColor` multiplica.** As nove células da hotbar nasceram como placa preta translúcida
  (`0,0,0,0.42`) e, re-bindadas a `container/slot.png`, continuaram quase pretas: a tinta é um multiply
  e preto vezes qualquer coisa é preto. Quem re-bind pinta de branco e o slot selecionado passa a ser
  destacado pelo **brilho do próprio sprite** (`uiHotbarBound()`), não pela tinta de antes.
- **Modal sem z-order.** A UI não tem z-order: o hit test anda pelo array de layouts e o **último** rect
  visível ganha. Um modal por cima do mundo é `moveToTop()` + `Scene::setEnableUIEvents(...)`, e toda
  placa decorativa leva `ignoreEvents` — senão ela come o clique do botão que está embaixo dela.
- **Botão tem quatro caras.** O `ButtonComponent` carrega `textureNormal`/`textureHovered`/
  `texturePressed`/`textureDisabled`; a skin preenche as quatro com os sprites do pack e liga
  `needUpdateButton`. `uiClickButton("nome")` dispara o callback **headless**, que é o que permite
  provar um modal sem dedo: abrir, bloquear input, clicar, fechar, e a resposta do `ask` chega pela
  mesma porta.
- **A fonte do pack não serve.** A tipografia do Minecraft é um **atlas bitmap** (`font/ascii.png` +
  um `.json` de métricas) e o `Text` da engine quer um TTF — não existe caminho para apontar para ela.
  `uiSetFontPath` aceita um caminho real e o **checa em disco** antes de guardar, porque o loader só
  loga uma vez por frame e um widget apontando para um arquivo ausente é um jogo rodando sem texto e
  sem erro. Sem fonte, a default da engine.
- **Sede não tem sprite.** Em `hud/` não há arte de sede: o único `air*` do pack é a coluna de ar de
  baixo d'água. Pintar a fome de água com ela diria "está sem fôlego" a quem só está com sede, então a
  sede continua sendo número na placa de diagnóstico.
- **O que a foto achou.** Coração e comida foram construídos na **mesma linha e no mesmo x**: o
  argumento `mirrored` invertia a ordem de preenchimento dos ícones mas nunca movia as caixas, e o
  `voxel_ui_hud.png` saiu com dez tambores desenhados por cima de dez corações. Agora a comida e o ar
  são ancorados na **borda direita** (`anchorPointLeft == 1.0`), que é também o layout do HUD original —
  os dois lados drenam para o centro. O check novo lê a âncora de volta do `UILayoutComponent` em vez
  de confiar no argumento de quem construiu, porque foi exatamente o argumento que mentiu.

Fotografado em `voxel_ui_hud2.png` com `crop_hearts_left.png` (dez corações cheios sobre os
contêineres), `crop_food_right.png` (dez tambores sobre os vazios) e a hotbar com as molduras do
`container/slot.png` e o slot 1 aceso. **Sonda: `ui 18`, total 216 / 0 falhas.**

**O que ainda não está aqui:** o inventário e o menu ESC abertos por tecla (`#198`), os ícones de item
dentro dos slots (`#204`) e a tela de criação de mundo — os três são chamadores desta camada, que já
responde modal, botão, request de texto, confirmação, rótulo e nove-slice.

## Água que se vê (corte de 26, medida por pixel)

`#199` abriu com "não vejo água" e se fechou em **três** defeitos empilhados, cada um invisível por
cima do anterior. A régua foi sempre a mesma: dois runs da mesma seed, um com `VOXEL_NOWATER=1` e outro
sem, e o diff de pixel contra pixel responde quem pintou o quê — nenhuma sonda de estado diria nada
dos três, porque em todos eles a água **estava** no mundo, no simulador e no HUD.

1. **Geometria degenerada.** `BlockDef` de água e lava tem `shapeCount = 0` e `{ }`, e o mesher lia
   `shapes[0]` como a caixa de desenho: toda quad líquida nascia com área zero. Consertado com a caixa
   do líquido autorada pelo próprio mesher (`b0 = 0`, `b1[0] = b1[2] = 1`,
   `b1[1] = 0.125 + 0.8125 × nível/15`) e com um guarda permanente na sonda —
   `every quad the mesher emits has area` devolve **`0 flat quads, 8153 of them liquid`**, e a segunda
   linha (`the liquid is meshed, not only counted`) afirma as mesmas 8 153 desde o boot.
2. **Uma folha só.** O tile do atlas carrega alpha 180/255 = 0,706, e uma quad translúcida sobre areia
   clara é exatamente o buraco vazio que o olho lê. O modelo trocou: a coluna agora é absorção
   acumulada por bloco — `n` camadas de um tile que transmite `t` deixam `tⁿ` do leito visível, então
   `alpha = 1 − tⁿ` com `t = 0,294` e teto 1,42 (o vertex color é multiplicado no alpha **antes** do
   blend, que é o único jeito na engine de pedir mais opacidade do que a textura carrega). A cor vai
   junto: água absorve vermelho primeiro, depois verde, quase nada de azul — `0,80 / 0,90 / 0,97` por
   bloco, com a coluna contada até **10** porque além disso o mar já está opaco e um expoente mais
   fundo só o converte em tinta. A rampa de cor é só de `BLOCK_WATER`: os mesmos fatores dariam a uma
   coluna de lava um marrom-azulado, o oposto do que rocha incandescente faz quando aprofunda.
3. **A lavagem da luz.** Medida, não suposta: com `receiveLights = true`, a folha pintada sobre a
   areia saía `rgb(145,153,172)` — o mesmo cinza-azulado pálido da nuvem atrás dela. É o espelho de
   Fresnel do céu: num mar plano a câmera só o vê em ângulo rasante, e em ângulo rasante um dielétrico
   de `roughness` baixa **é** um espelho. O líquido agora é unlit (`mesh.receiveLights = !liquid`),
   porque toda sombra que ele tem já está assada no vertex color — shade de face, AO, skylight e a
   rampa de profundidade. Os sólidos continuam iluminados, então o sol ainda modela o terreno.

**Armadilha nova, e ela morde qualquer mudança de material:** mexer em `receiveLights`/`alphaMode`
muda a **variante** de shader pedida, e a runtime não compila GLSL — ela aborta listando o que falta e
a linha de comando exata. Aqui foi um único variante:

```bash
"$QUANTUM_BUILD_DIR/Debug/quantum-resource-compiler.exe" shaders \
    --out "$LOCALAPPDATA/quantum/shaders/v40" --backend opengl --shader "mesh:Ult,Uv1,Vc4,Fog"
# [BUILD] Shader mesh:Ult,Uv1,Vc4,Fog (glsl410) generated successfully  (4 260 bytes)
```

**O enquadramento também era um defeito, e ele tinha duas metades.** A primeira é o ranking:
`findShoreColumn` parava no primeiro anel Chebyshev que tivesse **qualquer** costa, e uma enseada
fechada por morro passa no teste de entrada (cinco dos oito vizinhos submersos) do mesmo jeito que um
oceano — o spawn caía num cercado e o mar ficava atrás do morro. Agora a entrada é o anel d'água e o
ranking é o **run**: `score = run×10000 − freeboard×300 + anel×10 + profundidade`, com `shoreRun`
medindo até 24 blocos na direção que a própria coluna olha, e a varredura de anéis só para quando
encontra 12 blocos de água aberta na frente (`kOpenRun`) — o melhor score até ali vence de qualquer
forma, e o empate é recusado. O `run×10000` domina a soma dos outros termos (um platô a exatamente
`SEA_LEVEL`, com anel de água de um bloco, faz 119 contra 9 100 de uma costa com uma coluna alagada na
frente), então não existe categoria separada de "poça": ela só vence quando o raio inteiro não tem
nada além dela.

A segunda metade é a que fazia a captura mudar de um run para o outro: **o ponto de partida** da busca
era a coluna onde a câmera salva do mundo deixou o corpo, e isso não é função da seed. Agora a busca
parte de `(0,0)` do mundo, a mesma célula em toda rodada de uma seed, e `applyShoreSpawn` imprime a
resposta no log —
`VoxelShore: seed 1337 coast -10,76, aim (-1.00,1.00), yaw 135.0`. Medido com o binário entregue: duas
rodadas de 300 frames, mesma seed, mesma célula `-9.5 44.6 76.5 biome Beach` nas duas, e o mesmo quadro
nas duas (255 147 e 255 468 pixels azul-dominantes, 27,7 % em ambas). A sonda amarra as duas pontas com
uma afirmação nova — `the run stands on the coast its seed names` lê a coluna em que o corpo foi posto
e exige que seja a que a busca da origem devolve — então um spawn que volta a depender da câmera não
passa mais verde.

Medido no binário entregue, mesma coluna, `VOXEL_NOWATER` ligado e desligado: **243 298 pixels
mudam (26,40 % do frame)** — 99,4 % deles azul-dominantes — e nesses pixels a média vai de
`rgb(122,105,69)` — areia e chão — para `rgb(3,60,150)` — mar. No frame inteiro, azul-dominante passou
de **0,5 %** (antes da série) para
**27,7 %**, com 26,1 % fortemente azuis e média do frame `rgb(11,67,153)`. A sonda completa, na receita
de cima (com o diretório frio de salvamento da sonda, então os 15 checks de save entram), é
**225 passed, 1 failed**; com `VOXEL_SAVEDIR=""` são 210 e 1, pelos mesmos motivos de sempre.
A falha é a afirmação pré-existente do vidro (`a glass roof dims the room
under it by exactly its filter`: luz chega na célula de topo da sala e não no chão), aberta como tarefa
própria.

**Três knobs de captura que mordem**, e agora está escrito onde nascem: `VOXEL_AERIAL=1` e os dois
últimos números de `VOXEL_SHOTCAM` são ignorados, porque `VoxelPlayer.cpp` reescreve o `Transform` da
câmera a partir do próprio `pitch/yaw` em todo update, e esses membros são privados sem setter. Já a
**posição** do `VOXEL_SHOTCAM` vence sim — ela é escrita no frame da foto, depois do último update — e
foi exatamente assim que uma captura voltou com 0,1 % de azul: um pin para uma célula de fundo oceânico
a `SEA_LEVEL` olhando para baixo apaga o mar da foto sem mudar nada no mundo. O pin serve para A/B de
dois binários na mesma célula, não para escolher o enquadramento; o enquadramento é o spawn.

```bash
VOXEL_SHOT=/tmp/vox_ocean.png VOXEL_FRAMES=300 VOXEL_SAVEDIR='' \
    ./.quantum/build-standalone/VoxelWorld.exe | grep -E "VoxelShore|^pos "
# VoxelShore: seed 1337 coast -10,76, aim (-1.00,1.00), yaw 135.0
# pos -9.5 44.6 76.5   biome Beach
```

**O que ainda não está aqui:** a tira animada `water_still.png` / `water_flow.png` com `frametime` 2 e
o overlay de `#207`, que é onde o vermelho fundo de 2 vira azul-ciano de jogo; o tile costurado hoje é
a média dos frames, e ela é escura. E o `#208` (shadow map sobre a malha voxel) continua sem tocar o
líquido, que por decisão medida não recebe luz nenhuma.

## O que a reclamação de 2/11 mediu (corte de 27)

Cinco queixas, e cada uma foi medida antes de virar task. Nenhuma delas é o que o README dizia.

**As texturas não estão erradas — e a sonda não tinha como saber.** A captura do usuário mostra o atlas
do pack, e isso foi provado na entrega: a sonda imprime `chosen textures/voxel/atlas_minecraft.png` e
`material path textures/voxel/atlas_minecraft.png`, 256×256, `0 of 78 shared rows differ` entre os dois
atlas. Recortado tile por tile, o `grass_top` traz a faixa de terra do `grass_side`, o `dirt` traz as
manchas escuras (que são a textura real, não um defeito), o `log_birch_side` é branco com casca preta e
o `leaves_oak` é verde com furos. O filtro é `NEAREST` + `CLAMP_TO_EDGE` (`VoxelMesh.cpp:188-191`) e
`voxelTileRect` já entra meio texel (`VoxelTiles.h:104-115`), então não há sangramento do vizinho. O
alpha das folhas é **binário** (172 px de 255 e 84 px de 0, RGB `(0,0,0)` por baixo) e o material é
`MASK` com cutoff `0,5` (`VoxelMesh.cpp:184-185`): o "tracejado branco" que se vê são os furos deixando
o céu passar — o modo *fancy* do Minecraft, não um tile errado. O que **falta** são três coisas, todas
em `#227`: tinta por bioma em runtime (o tint é só de build — `scripts/build_minecraft_atlas.py`, linhas
46, 65-66, 102, 126-127, 146, 154 e 198-203 — e o tint por vértice usa `def.r/g/b`, que vale
`255,255,255` para grass/dirt/leaves, então o mundo inteiro veste o único ponto temperate do colormap);
nenhum log dizendo qual atlas foi escolhido (só a sonda sabe, então uma máquina sem o pack parece ter
textura errada e não diz);
e um `probeVoxelAtlas` (`VoxelMesh.cpp:536-615`) que afirma identidade e layout mas **nunca um pixel** —
as sete checks passam com o quadro completamente errado.

**A água não escorre para o vizinho porque o ramo está morto.** Em `VoxelWater.cpp`, o espalhamento
lateral (`:349-367`) é inalcançável para quase toda célula: o ramo de queda (`:309-333`) testa
`isFree(world.get(x, y-1, z))` e **retorna `STEP_KEEP` em `:332`** antes de chegar ao laço lateral — e
`isFree` (`:70-76`) conta água como livre, porque `BUILD_LIQUID` inclui `FLAG_REPLACEABLE`
(`VoxelBlocks.cpp:20`). Logo: só a célula que pousa em sólido espalha; nada sobe (`:556`); e o mar
gerado é `makeState(BLOCK_WATER, 15)` (`VoxelGen.cpp:423`), isto é, toda célula é fonte, enquanto
`scanEdits` (`:214-232`) só agenda o que foi **editado** — um oceano intacto tem zero células ativas até
alguém quebrar um bloco ao lado dele. A sonda da água (`:522-622`) só testa água apoiada no chão, e por
isso os `water 8` passam verdes com o fluxo quebrado. É a task `#220`, e ela corrige a linha 7 da tabela
de Estado: "esconde por gravidade/lateral" estava prometendo um lado que não acontece.

**Debaixo d'água não aparece nada porque não há nada para aparecer.** `inWater`/`eyeInWater` nascem em
`VoxelPlayer.cpp:832-833` e são consumidos **só** por bolha do HUD (`VoxelGame.cpp:694`), duck e leito
sonoro (`:1062` → `VoxelAudio.cpp:511,571,574`), splash (`:1084-1086`), dreno de vida (`:1149-1150` via
`VoxelLife.cpp:186,203`) e o beber (`:1225`). Não existe tint de tela, overlay, post-process nem textura
de bolha em lugar nenhum do projeto. O fog é `FogType::LINEAR` criado em `VoxelGame.cpp:276-278` e
tocado apenas pelo ciclo dia/noite (`:827-841`) — nunca por `eyeInWater` — e `Main Scene.scene` nem tem
nó de fog. É `#221`, e ela é a metade visual do `#207`, que hoje só cobre texturas animadas.

**A velocidade por ferramenta já existe e está certa; a ferramenta é que é invisível.** `miningSeconds`
(`VoxelPlayer.cpp:386-397`) faz `seconds = hardness/(20*speed) + 0,05` com `speed = rightTool ?
(1 + 1,5*nível) : 0,35`, penalidade de tier `*= 0,3` e água `*2`/`*1,5`, sobre a tabela de
`hardness`/`tool`/`minLevel` (`VoxelBlocks.cpp:23-82`, campos em `VoxelBlocks.h:43-45`). Medido na mão:
dirt **0,11 s**, stone **2,19 s**, obsidian **23,9 s**; com picareta de madeira stone **0,35 s** e
obsidian **3,38 s**, este último sem drop nenhum (ferramenta errada não derruba — `VoxelPlayer.cpp:535`).
Dois defeitos, ambos em `#222`: o construtor põe `heldTool = 1, heldToolLevel = 1`
(`VoxelPlayer.h:146-147`), então o jogador nasce com uma picareta de madeira **invisível** e a velocidade
de mão nuca é alcançável em jogo; e o HUD imprime o nome do bloco (`VoxelGame.cpp:598-600`) e nunca a
ferramenta nem o tier, tornando o ciclo `[Q]`/`[R]` (`VoxelPlayer.cpp:1078,1081`, `cycleTool` `:408-417`)
uma tecla sem resposta na tela.

**Inventário e crafting: um é nove slots, o outro é zero.** O storage é `struct Slot { uint16_t type;
int count; }` (`VoxelPlayer.h:113-116`) com `hotbar[9]` (`:126`) e `HOTBAR_MAX_STACK = 64` (`:111`);
`addToHotbar` (`:426-453`) completa pilha e depois usa o primeiro vazio, devolve `false` na barra cheia
— e o drop fica no mundo pelo `dropCollectFn` (`VoxelGame.cpp:346-349`), mas o caminho da **mineração**
(`VoxelPlayer.cpp:545`) descarta o item em silêncio. Não há grid de 36 nem UI de inventário (`#223`), e
não há crafting de espécie alguma: `BLOCK_CRAFTING_TABLE` (`VoxelBlocks.cpp:61`) é um bloco que se
coloca e nada mais (`#224`). Os ícones faltam por um motivo concreto: a hotbar desenha **dígitos**
(`VoxelGame.cpp:479,656-658`), e o resolutor de sprites do pack é uma tabela fixa (`PACK_ROOT`
`VoxelUi.cpp:32`, `kSprites` `:42-62`, `spriteFullPath` `:94-97`) sem **nenhuma** fileira para
`textures/item/` nas constantes `UI_SPRITE_*` (`VoxelUi.h:42-61`) — `#225`.

**A placa cinza do canto é um bug, e é pequeno.** `overlayBg` é criado incondicionalmente em
`VoxelGame.cpp:428-431` (`panel.png` × `setColor(0,0,0,0.45)`, 540×276) e é o único fundo do bloco de
debug; o texto é gateado por `showDebug` (`:1361`), mas o `F3` (`:863-866`) só faz
`overlay->setAlpha(...)` e nunca esconde a placa. Com debug desligado sobra um retângulo cinza de 536×272
sem nada escrito. `#226`.

## Licença dos assets

O atlas é **gerado por script** (`scripts/generate_voxel_atlas.py` na raiz do repositório) a partir
de paletas planas e padrões procedurais. Essa continua sendo a **arte embarcada** do projeto.

Texturas, skins e modelos do Minecraft original são propriedade da Mojang/Microsoft. O usuário tem um
dump local deles — `templates/voxelworld-main/minecraft-assets-26.3/`, 28 111 arquivos — e a fronteira
é esta, escrita uma vez:

1. **O pack é ignorado pelo git** (`.gitignore` do projeto) e nada **costurado dele** é commitado
   também: um atlas montado a partir de PNGs da Mojang carrega o mesmo copyright dos originais. Estão
   no `.gitignore` `textures/voxel/atlas_minecraft.*`, `textures/gui/` e `textures/entity/`.
2. O pack é **entrada de build e de runtime lida do disco**: as ferramentas (`#202`) e o jogo leem, o
   repositório não guarda. Rodar sem ele é o caminho normal — tudo cai de volta nos gerados por script.
3. O que **não é arte** e portanto não viola nada: o *layout de arquivo* de 64×64 da skin, o formato
   `.mcmeta` de animação, os nomes de chaves de `data/minecraft/recipe` e as tabelas de
   `loot_table`/`tags`. Isso é especificação; os pixels e os `.ogg` não são.

Censo do pack, armadilhas de nome (`oak_log` vs `oak_log_top`, `short_grass`, folha cinza sem tinta,
`grass_block_top` **cinza** multiplicada pelo `colormap/grass.png`) e o que ele **não** dá (geometria
de mob, cor de água, lightmap, shadow map) estão na skill `quantum-voxel-assets`.

O mesmo vale para o som: os 35 WAV de `sounds/voxel/` são **sintetizados** por
`scripts/generate_voxel_sounds.py` (ruído em camadas, envelopes e ruído determinístico por arquivo),
não amostrados de lugar nenhum. Nenhuma gravação nem áudio de jogo entra no repositório. Trocar por
um pack próprio é escrever os arquivos no mesmo layout de nome — a sonda reclama de qualquer
divergência antes de o primeiro frame rodar. Os 5 698 `.ogg` do pack (`#212`) entram por esse mesmo
caminho de runtime, lidos do disco do usuário, nunca commitados.

E o mesmo vale para a **skin**: `textures/skins/default.png` é **desenhado por script**
(`scripts/generate_voxel_skins.py`). O que o projeto compartilha com o Minecraft é o **layout de arquivo**
de 64×64 — especificação de formato, não arte — e a geometria é original. Uma skin **do usuário** funciona
sem nada mudar no repositório: `PLAYER_SKIN_PATH` e `MOB_SKIN_PATH` apontam hoje para
`textures/skins/default.png`, e a fase 23 é o que os fará aceitar um PNG próprio em
`textures/skins/<nome>.png`. As 9 skins oficiais em `textures/entity/player/{slim,wide}` são exatamente
esse caso: carregam do caminho local do usuário em runtime e não são commitadas.

## Tarefas abertas

Todo módulo abaixo está **escrito, compilado com MSVC e rodando** dentro de `VoxelWorld.exe`. O que
falta em cada um é a prova de *comportamento* — o número que ele tem que devolver, não o frame.

- [x] Fase 5: geração — `VoxelGen.cpp` (height fBm por biome, cavernas 3D, veios de minério por
      altitude, árvores por biome, gelo/neve). *Medido pelo próprio módulo: 9 biomas, 28,4 % de
      coluna submersa, 4 % de caverna, byte-identico entre runs.*
- [x] Fase 6: iluminação — `VoxelLight.cpp` (skylight desce, blocklight BFS 15→0). *Costura 7×7 sem
      divergência e 12 checks de comportamento na sonda.*
- [x] Fase 7: água — `VoxelWater.cpp` (nível por bloco, coluna vertical, lava, `WaterBlock` fontes).
      *8 checks* — mas **só do que cai**: o espalhamento lateral existe como código e nunca roda, e o
      mar gerado não tem nenhuma célula agendada. Ver `#220` abaixo e "O que a reclamação de 2/11 mediu".
- [x] Fase 9/10: jogador — `VoxelPlayer.cpp` (colisão por shape de item, DDA, quebrar/construir).
      *13 checks de colisão + 14 do controlador (7 na entrega, mais os 7 do portão de modo).*
- [x] Fase 8: save — `VoxelSave.cpp` (arquivo por chunk, LRU, `ThreadPoolManager` para I/O)
      *15 checks de comportamento na sonda, inclusive os quatro modos de arquivo danificado recusados.*
- [x] Fase 11: entidades — `VoxelEntity.cpp` (mobs com gravidade voxel, `updateFallingBlocks`)
      *14 checks de comportamento na sonda: spawn apoiado, teto de pool, andar sem deriva entre sim e
      malha, queda no buraco aberto sob os pés, natação, cull + slot livre, e areia/cascalho célula a
      célula pelo caminho alto.*
- [x] Fase 12: multiplayer — `VoxelNet.cpp` (host autoritativo, protocolo, diff de blocos).
      *Round-trip provado entre dois processos: 15 checks, os dois blocos visíveis nos dois PNGs.*
- [x] Fase 13/14: `VoxelGame.cpp` (instala hooks, HUD lido do `VoxelPlayerScript`, sol orbital,
      fog, overlay)
- [x] Fase 16: build headless — `generate` → `configure` → `build` → `VoxelWorld.exe` roda e captura
      PNG. O `quantum-build-generator.exe` de 26/10 não lia `scriptDirs` e só conhecia o extinto
      `ChunkGenerator.cpp`; reconstruído, e o caminho do atlas no README estava errado (`assets/`
      não existe — é `textures/voxel/`).
- [x] Fase 18: rachadura — `VoxelBreak.cpp` (cubo recortado no bloco alvo, 10 estágios por
      `textureRect`, lasca por dureza, cancelamento ao trocar de alvo). *10 checks na sonda +
      `vb15_stage.png`, e os três achados de render em "Rachadura no bloco".*
- [x] Fase 19: som — `VoxelAudio.cpp` + 35 WAV sintetizados por `scripts/generate_voxel_sounds.py`
      (anel fixo de 12 vozes 3D, três camas em loop, passo por distância, crossfade por iluminação,
      linha própria no F3). *20 checks na sonda, inclusive o pack aberto arquivo por arquivo pela
      camada de arquivos da engine, o roubo de voz contado contra a precedência medida, o passo
      cobrado por passada e negado no ar, e a linha do overlay relida de volta. Nada foi ouvido —
      ver "Som: o pack e as vozes".*
- [x] Fase 20: vida — `VoxelLife.cpp` + o host `updateLifeFx` (barras de 20 com decaimento por
      atividade, queda medida de pico, ar de 10 s, lava/cacto/afogamento/fome/sede, cura portada e
      paga, morte com uma causa e respawn re-tentado coluna a coluna, `G`/`H` na hotbar, linha própria
      no F3). *28 checks na sonda na entrega (37 hoje, com os 9 do portão de modo da fase 21),
      inclusive a integrabilidade (10 s num passo = 10 s em 500 frames),
      o "corpo morto não decai", e a linha do overlay relida de volta. Nenhuma afirmação planta um
      cacto ao lado do jogador nem aperta `G` — ver "Vida, fome e sede".*
- [x] Fase 21: modos — `VoxelConfig.h` (vocabulário), `VoxelPlayer.cpp` (dono: colisão, gravidade,
      edição, voo, resgate do vazio e o noclip 3D do espectador) + o host informa `VoxelLife.cpp` a
      cada frame, que se recusa a decair, machucar, matar, comer e beber fora de sobrevivência. *16
      checks novos (jogador 14, vida 37), inclusive o espectador que não muda um voxel em três cliques
      e o toggle de voo que sobrevivência ignora sem limpar. `[M]` nunca foi apertado por uma mão, e o
      HUD foi capturado, não afirmado — ver "Modos de jogo".*
- [x] Fase 22: modelos — `VoxelModel.{h,cpp}` (seis cubos com UVs no layout de skin 64×64, pose
      aritmética escrita em transform, um rig por corpo no pool, cull e teardown das seis partes) + o
      avatar do jogador em `VoxelPlayer.cpp` (`[V]`, recuo de câmera stopado por parede, `VOXEL_THIRD`) +
      `scripts/generate_voxel_skins.py` com `textures/skins/default.png`. *20 checks novos (`model 20`) e
      +5 nos módulos que já existiam (jogador 14 → 18, entidades 14 → 15), total **168 / 0 falhas** em 12
      corridas seguidas, MSVC sem aviso nas duas configurações (com e sem `VoxelModel.h`). O que uma
      função não prova foi fotografado: `voxel_third.png`, `crop_mobs.png` e `crop_mine.png` — ver
      "Modelos do player e dos mobs".*
- [x] Fase 24 (corte): drops — `VoxelDrop.{h,cpp}` (cubo do tile do bloco, 90°/s, bob, gravidade 24
      com empuxo 6, merge de pilha, ímã de 1,8 que para antes de buscar de volta o que `[J]` jogou,
      recusa do coletor que **deixa o item no mundo**, expiração em 180 s, pool de 96 com despawn por
      distância) + a costura `itemDropSpawner` / `dropCollectFn` em `VoxelGame.cpp:328-345` e
      `addToHotbar` (`VoxelPlayer.cpp:426`, pilha primeiro, depois slot vazio, um tipo nunca espalhado
      por nove slots). *10 checks (`drops 10`) e `VOXEL_DROP=<n>` para a captura. O achado real foi de
      render: `lookDirection()` devolvia `+worldDirection` — o **trás** da câmera — e os itens do
      capture nasciam atrás das costas do jogador. Ver "Drops de item".*
- [x] Olhar com o ponteiro capturado: **o warp apagava o movimento** (`#214`). `applyLook()` lia o
      delta contra a posição do frame anterior enquanto `Engine::setMousePosition` reescrevia a posição
      reportada de volta ao centro — a conta era a diferença entre duas viagens consecutivas, então um
      varrimento constante (girar para os lados) se cancelava e a tela não virava. A baseline agora é o
      ponto do warp, o `grabMouse` semeia nele, e três checks (`player 24 → 27`) travam o
      comportamento: varrimento de 12 px/frame vira os 7,680° de yaw prometidos, a mão que para não
      puxa a vista de volta, e a mesma viagem vertical dá 5,760° de pitch. **Não verificado na janela**
      — headless não há mouse, e a máquina deste shell não entrega `SendInput` (lastError 87); quem
      prova é o usuário, e `VOXEL_LOOK=30` diz o que a camada de ponteiro entregou se ainda falhar.
- [x] Cursor voltando ao centro **com a janela sem foco** (`#218`). Eram duas camadas negando a mesma
      coisa. Na engine, `WindowWin::setMousePosition` agora pergunta `hasFocus()` (novo virtual em
      `System`, `SystemWin`, `WindowWin`, exposto como `Engine.hasFocus` para Lua) e **recusa o warp
      físico** quando o processo não é a janela em primeiro plano; um backend que não rastreia foco
      devolve `true`, então quem não passa pelo portão mantém o comportamento de antes. No jogo,
      `applyFocusPolicy()` decide a posse do ponteiro: perder o foco solta a captura (e desliga o modo
      que esconde e clipa a seta), retomá-la readquire — o que o backend Linux já fazia com
      FocusIn/FocusOut (`QuantumLinux.cpp:271-279`) — mas um `Esc` apertado pelo jogador é pedido, não
      acidente: foco nenhum o re-toma, e um launch nascido atrás de outra janela espera o primeiro
      plano. *Cinco checks novos (`player 27 → 32`), com `VOXEL_FOCUS=0/1` fixando a resposta de foco
      que um run headless não pode arranjar. O que a sonda **não** vê é o meio do caminho: os deltas
      crus não chegam sem foco porque o dispositivo alvo é `hwndTarget = gWindow` sem
      `RIDEV_INPUTSINK` — ou seja, a recusa do warp é a outra metade do conserto e quem prova essa
      metade é o usuário, arrastando o mouse com o jogo atrás de outra janela.*
- [x] UI do jogo com os sprites **originais** do pack (`#215`/`#216`). `VoxelUi.{h,cpp}`: tabela de 19
      sprites resolvida em runtime, cada um carregado **uma** vez e entregue ao widget como view
      não-dona; PNG grayscale expandido para RGBA (o `ColorFormat::RED` do loader pinta **vermelho**
      qualquer sprite cinza); nine-slice com a borda lida do `.mcmeta` do pack — botão 3, painel 6,
      tooltip 10; modal com `moveToTop()` + `setEnableUIEvents` porque a UI não tem z-order nenhum e o
      último rect visível fica com o clique; botão vestindo as quatro texturas do `ButtonComponent`;
      request de texto e confirmação com callback disparável **headless**; rótulo com fonte opcional
      checada em disco, porque a tipografia do pack é atlas bitmap e o `Text` da engine quer TTF. No
      HUD: corações à esquerda, comida e ar ancorados à direita, e as nove células da hotbar
      re-bindadas a `container/slot.png` — repintadas de branco, já que `setColor` multiplica e a placa
      preta apagaria a moldura. *18 checks (`ui 18`) e `VOXEL_SHOT`: a foto achou o defeito que a sonda
      não via — as duas linhas nasciam no mesmo x e os tambores saíram **por cima** dos corações; o
      check agora lê a âncora de volta do componente em vez de confiar no argumento de quem construiu.
      Ver "UI com os sprites do pack". A sede ficou de fora de propósito: não há sprite dela no pack.*
- [x] Skill do pack — `quantum-voxel-assets` nas quatro árvores (censo medido dos 28 111 arquivos, os
      nomes que não são o que se supõe, como o projeto consome o pack e o que ele **não** dá).
- [x] Sonda compartilhada — `VoxelProbe.h` + um `probeX()` por módulo, todos rodando num
      `VOXEL_PROBE=1`: **226 checks, 1 falha** (mesh 2, atlas 7, colisão 13, luz 12, água 8, jogador 33,
      vida 43, rachadura 10, som 20, entidades 15, drops 10, modelo 20, ui 18, save 15). A falha é a do
      vidro, listada como item aberto abaixo. A sonda agora roda sobre a
      **seed** e não sobre o mundo do run anterior: com `VOXEL_PROBE` o salvamento vai para um
      diretório frio em `<temp>` — ver "A sonda de comportamento". Cada veredito é seguido de
      `fflush(stdout)`: `Log::print` cai num `printf` com buffer e um processo que morre em `abort`
      nunca o descarrega, então o crash de um módulo apagava a lista inteira do anterior — foi assim
      que um `System.h` com slots de vtable renumerados ficou invisível por uma sessão inteira.
- [x] Provar **comportamento** do que ainda só roda: que um cliente TCP receba a edição do host e o
      host receba a do cliente (12, dois processos). *Fechado por `scripts/voxel-net-roundtrip.sh`.*
- [x] `#199` Água que se vê — três defeitos empilhados, cada um invisível por cima do anterior: a caixa
      de desenho do líquido era `shapes[0]` de um bloco com `shapeCount = 0` (toda quad líquida nascia
      com **área zero**), o mar era uma folha de alpha 0,706 sobre areia clara, e a folha iluminada
      saía `rgb(145,153,172)` — o mesmo pálido do céu atrás, porque um dielétrico de `roughness` baixa
      em ângulo rasante **é** um espelho. Agora a coluna é absorção acumulada (`alpha = 1 − tⁿ`, rampa
      `0,80 / 0,90 / 0,97` por bloco, contada até 10) e o líquido é **unlit**: toda sombra dele já está
      assada no vertex color. Medido por A/B com `VOXEL_NOWATER` na mesma coluna: **243 298 px
      (26,40 % do frame, 99,4 % deles azul-dominantes)** mudam de `rgb(122,105,69)` para `rgb(3,60,150)`,
      e o azul-dominante do frame
      foi de 0,5 % a **27,7 %**. A busca de praia passou a ranquear pelo **run** de água aberta e a
      varrer anéis até achar 12 blocos, porque uma enseada murada passava no teste de entrada e ganhava
      sempre — e passou a partir de `(0,0)` do mundo em vez da coluna onde a câmera salva deixou o
      corpo, porque era isso que fazia duas rodadas da mesma seed pararem em praias diferentes. Medido:
      duas rodadas de 300 frames, mesma célula `-9.5 44.6 76.5 biome Beach` e 27,7 % azul nas duas.
      *Sonda: 225 / 1 falha (a do vidro, acima), com uma afirmação nova amarrando o spawn à seed —
      `the run stands on the coast its seed names`. Ver "Água que se vê", incluindo a armadilha de que
      mudar um material do mesher pede um `.sdat` novo, porque a runtime não compila GLSL.*
- [ ] Luz: o vidro não acende o **chão** da sala (`light 11/1` — `a glass roof dims the room under it
      by exactly its filter`: a célula de topo recebe o céu filtrado, a do chão recebe 0). A passada por
      coluna cobra o filtro ao **sair** do bloco e o flood fill ao **entrar**; num quarto selado de ar
      acima do vidro nada cobra ao entrar, então a luz para no teto. É a única falha da suíte e ela é
      anterior a este corte.
- [ ] Otimização seguinte: buffer dinâmico persistente por chunk em vez de recriar a entidade
- [ ] Rede: o `CHUNK` vai cru — `Chunk::serialize()` são ~96 KB por coluna, e `pushChunks` libera
      `CHUNKS_PER_TICK = 2` por cliente por tick. Um run-length nas colunas de ar é a próxima
      economia; sem pedido sob demanda do cliente, o host só acompanha o `POS` que recebe.
- [ ] Rede: nada de UDP nem de interpolação de posição — `tickVoxelNet` manda o `POS` no frame em
      que está, para cada cliente, e o receptor aplica cru; o avatar remoto salta com o jitter.
- [ ] Rede: um avatar remoto ainda **não veste o rig** — `VoxelNet` e `VoxelGame` não chamam nada de
      `VoxelModel`: o `POS` que chega move um ponto, não seis cubos. A pose é uma estrutura só
      (`VoxelModelPose`) e o pool de mobs já demonstra o caminho, então vestir um jogador remoto é ligar
      `poseVoxelModelRig` ao que já chega pela rede — nada novo no módulo, só o chamador.
- [ ] Mundo: estruturas nomeadas (vila, poço, mina abandonada) — a geração hoje faz bioma, altura,
      caverna, veio, árvore e linha de neve (`h>=88` neva), mas nenhuma construção.
- [ ] `#223` Inventário: sair dos **9** slots da hotbar. O storage é `struct Slot { uint16_t type; int
      count; }` (`VoxelPlayer.h:113-116`) com `hotbar[9]` (`:126`) e `HOTBAR_MAX_STACK = 64` (`:111`);
      `addToHotbar` (`:426-453`) completa pilha e depois usa o primeiro vazio e devolve `false` na barra
      cheia — o `dropCollectFn` do mundo (`VoxelGame.cpp:346-349`) deixa o item onde está, mas o caminho
      da **mineração** (`VoxelPlayer.cpp:545`) descarta em silêncio. Não há grid de 27+9 nem UI. Provar
      headless: encher os 9, minerar mais, afirmar que nenhum item desapareceu e que a pilha parou em 64.
- [ ] `#224` Crafting: **existe zero**. `BLOCK_CRAFTING_TABLE` (`VoxelBlocks.cpp:61`) é um bloco que se
      coloca, sem UI, sem receita, sem resultado. O pack responde a pergunta —
      `data/minecraft/recipe/` com 1 766 arquivos (`crafting_table.json`, `copper_pickaxe.json`,
      `acacia_planks.json`) — mas o pack é da Mojang: a saída é uma tabela gerada em build e **commitada
      como dado nosso**, nunca um byte do dump. Depende de `#223`.
- [ ] `#222` Ferramentas: o jogador nasce com uma picareta de madeira **invisível**. `heldTool = 1,
      heldToolLevel = 1` são o default do construtor (`VoxelPlayer.h:146-147`), então a velocidade de mão
      nuca é alcançável em jogo — e o HUD imprime o nome do **bloco** (`VoxelGame.cpp:598-600`), nunca a
      ferramenta, então `[Q]`/`[R]` (`VoxelPlayer.cpp:1078,1081`) não têm resposta na tela. A conta já
      está certa e medida (`miningSeconds` `:386-397`): na mão dirt 0,11 s, stone 2,19 s, obsidian 23,9 s;
      com picareta de madeira stone 0,35 s e obsidian 3,38 s, este sem drop (ferramenta errada não
      derruba — `:535`). Começar desarmado e mostrar ferramenta + tier + tempo no HUD.
- [ ] `#220` Água: o espalhamento **horizontal nunca roda**. O ramo de queda (`VoxelWater.cpp:309-333`)
      retorna `STEP_KEEP` em `:332` antes do laço lateral (`:349-367`), e `isFree` (`:70-76`) conta água
      como livre porque `BUILD_LIQUID` inclui `FLAG_REPLACEABLE` (`VoxelBlocks.cpp:20`) — só a célula que
      pousa em sólido espalha, e nada sobe (`:556`). Somado: o mar é `makeState(BLOCK_WATER, 15)`
      (`VoxelGen.cpp:423`) e `scanEdits` (`:214-232`) só agenda edições, então um oceano intacto tem
      **zero células ativas**. Os `water 8` passam verdes porque todos testam água apoiada no chão.
- [ ] `#221` Água: o estado de submerso não tem **um pixel** próprio. `eyeInWater` (`VoxelPlayer.cpp:832`)
      alimenta bolha, som, splash, dreno de vida e beber — e nada mais; fog é `LINEAR`
      (`VoxelGame.cpp:276-278`) tocado só pelo dia/noite (`:827-841`). Cor + densidade sob a câmera, tint
      nos sólidos vistos através da água, e um PNG de `VOXEL_SHOTCAM` submerso.
- [ ] `#226` HUD: o `F3` apaga o texto (`VoxelGame.cpp:863-866` faz só `overlay->setAlpha`) e **deixa a
      placa** — `overlayBg` é criado incondicionalmente em `:428-431` e é o único fundo do bloco de debug,
      então com debug desligado sobra um retângulo cinza de 536×272 no canto. É a placa da captura do
      usuário. Ligar a visibilidade dela à mesma flag do texto.
- [ ] `#227` Texturas: o atlas do pack **é** o desenhado e **é** fiel (a sonda imprime
      `chosen textures/voxel/atlas_minecraft.png`, `0 of 78 shared rows differ`, e os tiles recortados são
      os do Minecraft), mas faltam três coisas: tinta por bioma em runtime (o tint é só de build, e o
      tint por vértice usa `def.r/g/b` = `255,255,255` em grass/dirt/leaves, então o mundo inteiro veste
      o único ponto temperate do colormap); um log de qual atlas foi escolhido (`voxelAtlasPath()` não
      diz, só a sonda sabe); e uma asserção de **pixel** em `probeVoxelAtlas` (`VoxelMesh.cpp:536-615`),
      que hoje afirma identidade e layout e passaria verde com o quadro errado.
- [ ] Player: sem pausa e sem menu; um mundo por pasta de save (`Save Folder` é uma propriedade da
      cena, não um seletor). Seed, dia, mobs, porta e endereço já são editáveis no inspector.

**A frente 26 — o pack local da Mojang** (`#202`-`#212`; o censo e as armadilhas estão em
`quantum-voxel-assets`; nada disso entra no git, ver "Licença dos assets"):

- [x] `#202` Costurador: `scripts/build_minecraft_atlas.py` lê `textures/block/*.png` do pack, multiplica
      os **cinza** pela `colormap/grass.png` (256×256, temperatura × umidade), resolve `.mcmeta` de
      frame 0, e escreve `textures/voxel/atlas_minecraft.png` **+ `.txt` na mesma ordem de slots de
      `VoxelTiles.h`**. Append-only: um mundo salvo guarda números de slot, não nomes. A sonda dá
      `skip` quando o pack não está no disco — rodar sem ele é o caminho normal. Medido: `atlas 7`
      (dos quais um afirma que o atlas **veio do pack**, não do gerador) e o `.txt` lido de volta pelo
      mesmo `voxelTileRect()`.
- [x] `#203` Ligar o atlas costurado ao mesher e ao HUD pelo **mesmo** `voxelTileRect()`, com o gerado
      por script como fallback quando o slot não tem tile no pack. `voxelAtlasPath()` resolve por
      `FileData::getSystemPath` (o VFS não serve para um arquivo que o jogo apenas **nomeia**), e a
      escolha é um `static` de função: enquanto o processo não reiniciar, o atlas é o mesmo. A sonda
      (`atlas 7`) afirma que o material do chunk carrega **o** atlas escolhido, que a escolha acontece
      exatamente quando o costurado está no disco, que os dois `.txt` põem todo tile no mesmo slot
      (compare linha a linha, `drift == 0`) e que a grade é 16×16 normalizada. As **cinco** primeiras
      valem sem pack (identidade do atlas, fallback, regra da troca, material do chunk, líquido unlit);
      com o pack fora do disco as duas últimas viram um único `skip` — rodar sem ele é o caminho normal.
      O que a sonda **não** afirma é pixel: nenhuma das sete olha um texel, e foi por isso que a queixa
      de textura precisou de recorte manual do atlas para ser respondida — ver `#227`.
- [ ] `#204` / `#225` Ícones reais de item (832 `textures/item/*.png`) na hotbar, no inventário e no drop.
      Medido: a hotbar desenha **dígitos** (`VoxelGame.cpp:479,656-658`) e o resolutor do pack é uma
      tabela fixa (`kSprites` `VoxelUi.cpp:42-62`, `spriteFullPath` `:94-97`) sem **nenhuma** fileira para
      `textures/item/` em `UI_SPRITE_*` (`VoxelUi.h:42-61`) — por isso não há imagem, e o drop desenha o
      tile do bloco.
- [ ] `#205` Skins reais de mob no rig de cubos (`textures/entity/{creeper,cow,chicken,pig,zombie}`) e
      as 9 skins oficiais de `textures/entity/player/{slim,wide}` para `#196`.
- [ ] `#206` Sprites reais de HUD: corações, fome, ar e moldura de slot de `gui/sprites/` (24
      categorias de arquivos pequenos) no lugar das barras desenhadas.
- [ ] `#207` / `#221` Água: `water_still.png` / `water_flow.png` são **tiras verticais** animadas por
      `.mcmeta` (`frametime` 2) — levar o overlay animado e a cor por profundidade. A outra metade, que o
      usuário chamou de "não parece que estou debaixo d'água", é o **estado de submerso**: hoje ele não
      tem um pixel próprio, só bolha no HUD, som abafado e dreno de vida. Ver `#221`.
- [ ] `#208` Sombra: shadow map do sol sobre a malha voxel + a sombra do item dropado usando
      `textures/misc/shadow.png` (64×64, alpha-only) em vez do elipse desenhado.
- [ ] `#209` / `#224` Crafting: as receitas reais de `data/minecraft/recipe` na mesa 3×3. Hoje é **zero**,
      não "faltam as receitas": `BLOCK_CRAFTING_TABLE` é um bloco colocável sem UI nem resultado.
- [ ] `#210` Dados: drop por `data/minecraft/loot_table/blocks` (1 203 tabelas) e nome exibido de
      `lang/en_us.json` (144 locales no pack) no HUD.
- [ ] `#211` Shaders: `lightmap` 16×16, *face shading* e nuvens **a partir da referência** dos 96
      arquivos de `shaders/` — que são especificação GLSL, não código: **a runtime não compila GLSL**,
      então o que vale é a port para o shader embutido + `.sdat` já compilado.
- [ ] `#212` Som: os 5 698 `.ogg` + `sounds.json` lidos do caminho local em runtime, com o pack
      sintetizado como fallback.
