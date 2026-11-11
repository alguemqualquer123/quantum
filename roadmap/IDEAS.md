# Roadmap para Expansão da Engine de Jogos

A seguir estão sugestões de funcionalidades e sistemas que podem ser implementados para tornar a engine mais completa, aproximando‑se das capacidades da **Unreal Engine**.

---

## 1. Editor de Níveis
- **Editor WYSIWYG** (arrastar e soltar) para criar e posicionar objetos no mundo.
- **Gizmos de transformação** (transladar, rotacionar, escalar) com suporte a snapping.
- **Hierarquia de atores** e painel de propriedades editáveis.
- **Undo/Redo** robusto com suporte a múltiplas operações.

## 2. Sistema de Scripting / Blueprint
- **Visual scripting** (similar ao Blueprint) para permitir designers criar lógica sem código.
- **Binding de eventos** entre componentes e atores.
- **Exportação para C++/Lua** como fallback.

## 3. Renderização Avançada
- **Pipeline de renderização baseado em PBR** (Physically Based Rendering).
- **Suporte a iluminação dinâmica** (deferred shading, iluminação de cena).
- **Post‑process effects** (bloom, tone‑mapping, SSAO, motion blur).
- **Suporte a múltiplas plataformas** (DX12, Vulkan, OpenGL).

## 4. Sistema de Materiais
- **Editor de materiais node‑based**.
- **Biblioteca de shaders** (metallic/roughness, emissive, translucency).
- **Instancing de materiais** para reduzir draw calls.

## 5. Física
- **Integração com motor de física** (e.g., Bullet, PhysX, Havok).
- **Ragdoll**, colisões complexas, simulação de tecidos.
- **Detecção de colisão** em tempo real e callbacks de eventos.

## 6. Animação
- **Blend trees** e state machines para animações de personagens.
- **Skeletal mesh import/export** (FBX/GLTF).
- **Procedural animation** (IK, morph targets).

## 7. IA e Navegação
- **NavMesh generation** automática a partir dos níveis.
- **Behavior trees** para lógica de IA.
- **Sistema de percepção** (visão, audição).

## 8. Audio
- **Audio engine 3D** com spatialization.
- **Mixagem e efeitos** (reverb, doppler).
- **Sistema de cue e eventos**.

## 9. Sistema de Rede / Multiplayer
- **Sincronização de estado** usando replicação de atores.
- **Lobbies e matchmaking**.
- **Lag compensation** e interpolação.

## 10. Asset Pipeline
- **Importação automática** de modelos, texturas, áudio.
- **Gerenciamento de recursos** (streaming, LOD).
- **Build tool** para empacotar assets em pacotes (similar ao .pak).

## 11. Sistema de Partículas
- **Editor visual** para efeitos de partículas.
- **GPU‑accelerated particles**.
- **Suporte a shaders personalizados**.

## 12. Sistema de UI
- **Framework de UI** baseado em widgets, com suporte a escala e temas.
- **Blueprint‑compatible** para eventos de UI.

## 13. Ferramentas de Depuração
- **Visualizador de framerate** e profiling.
- **Log de eventos** e console interativo.
- **Hot‑reload** de código e assets.

## 14. Documentação e Exemplos
- **Tutorials passo‑a‑passo** mostrando como usar cada subsistema.
- **Projetos de exemplo** (primeiro‑pessoa shooter, plataforma, etc.).

---

> **Próximos passos**
> 1. Priorizar a implementação do **Editor de Níveis** e do **Sistema de Renderização PBR**.
> 2. Definir a arquitetura de plugins para que novos subsistemas (física, áudio, etc.) possam ser adicionados de forma modular.
> 3. Criar um repositório de **Blueprints** simples para validar o visual scripting.

Esta lista serve como ponto de partida; cada item pode ser detalhado em tickets individuais.
