# Plano — Rebrand Quantum: marca, créditos, logos e documentação

Trocar **toda a superfície pública** do projeto — nome exibido, créditos, autor,
logos, ícones, links e o repositório de documentação — para:

| Campo | Hoje | Novo |
|---|---|---|
| Nome do produto | `Quantum` / `Quantum Engine` | **Quantum** / **Quantum Engine** |
| Crédito de autoria | `SR VINIX (and contributors)` | **SR VINIX** |
| GitHub | `github.com/quantumengine/quantum` | **`github.com/alguemqualquer123/quantum`** (upstream vira remote `upstream`, D10) |
| Docs | `github.com/alguemqualquer123/quantum-docs` → `docs.quantum.org` | **`github.com/alguemqualquer123/quantum-docs`** → `alguemqualquer123.github.io/quantum-docs/` (local: `C:\quantum-docs`) |
| Logo | arte do `quantum-site` + `.ico`/`.icns`/`.png` atuais | **orbital + wordmark `QUANTUM`** (D7), gerado em `brand/` |

Este plano **não** é o rename de código (`namespace quantum` → `quantum`, alvos
CMake, macros). Esse trabalho já está planejado em
[QUANTUM_REBRAND_PLAN.md](file:///C:/quantum2/QUANTUM_REBRAND_PLAN.md), em 8 fases,
e continua válido. Aqui o escopo é o que o usuário **vê e lê**. A Fase 6 de lá
("docs and agents") é expandida por este documento.

---

## 0. Fronteira legal — o que NÃO pode ser removido

MIT tem exatamente uma condição: manter o aviso de autoria. Trocar a marca é
lícito; apagar a atribuição original não é. Então:

- **Intocável**: [LICENSE](file:///C:/quantum2/LICENSE) (`Copyright (c) 2026 SR VINIX and contributors`),
  as ~1384 linhas `// (c) SR VINIX` + `SPDX-License-Identifier: MIT` no topo
  de cada arquivo-fonte, e todos os avisos de terceiros em `engine/libs/` e `libs/`.
  Nota: [QUANTUM_REBRAND_PLAN.md:17](file:///C:/quantum2/QUANTUM_REBRAND_PLAN.md#L17)
  cita o LICENSE como "SR VINIX Lima" — o texto real é "SR VINIX and
  contributors". Corrigir essa linha no plano antigo.
- **Substituível com atribuição mantida**: todo o resto — nome exibido, títulos de
  janela, About, splash, logos, ícones, `CompanyName`/`ProductName` do VERSIONINFO
  Windows, URLs, README, site de docs, metainfo AppImage.
- **Onde `SR VINIX` hoje é *identidade* e não *aviso*** (metainfo
  `<developer name>`, `mkdocs.yml site_author`/`copyright`, About "Developed by",
  `docs/about/license.md` "created and maintained by", `plugins/hello/plugin.json`,
  `.github/FUNDING.yml`): **reescrever como dono novo + uma linha de upstream
  explícita**, nunca apagar silenciosamente. Exemplo de forma final:
  `Quantum é mantido por SR VINIX. Fork do engine Quantum, de SR VINIX (MIT).`

**Aceite do dono (2026-11-04)**: `LegalCopyright` do `.rc` e `copyright:` do
`mkdocs.yml` passam a creditar SR VINIX, mantendo a frase "fork of Quantum Engine,
(c) SR VINIX — MIT" visível no LICENSE, no NOTICE e na página de licença.

---

## 1. Identidade nova (decisões)

- **D1 — marca.** `Quantum` (display), `QUANTUM_` (macros), `quantum` (lowercase
  em ids de pacote). Sem colisão: `grep -ri vinix` dá zero nos dois repos;
  `quantum` só aparece no plano antigo e em `miniaudio.h` (vendored, intocável).
- **D2 — autoria nas artes.** Creditada como **SR VINIX**, sempre com a linha de
  upstream ao lado.
- **D3 — ids técnicos: trocar tudo** (fechado em D13). Linux `org.quantum.editor` →
  `org.srvinix.quantum.editor`; Android `org.quantumengine.quantum` →
  `org.srvinix.quantum`; macOS bundle id `org.quantum.quantumengine` →
  `org.srvinix.quantum`. Isso é binário-compatível com o AppImage/`.desktop` novos,
  mas **quebra atualização por cima** de installs antigos — os dois convivem, e a
  migração de dados (D4) roda no primeiro launch.
- **D4 — config do usuário migra, não desaparece.** `%APPDATA%/Quantum` →
  `%APPDATA%/Quantum` (e equivalentes macOS/Linux), copiando `settings.yaml`,
  `editor_layout.ini` e `editor.log` se o destino não existir.
- **D5 — links de comunidade.** O **Discord passa a ser o do dono**
  (`https://discord.gg/gSHUVHXNwP`, D11). YouTube, as demos
  `quantumengine.github.io/*` e o site `quantum.org` **saem** da superfície pública.
  Docs e downloads viram GitHub Pages + releases do fork (D8/D9). Nada de link morto.
- **D6 — logo é gerado, não editado.** Toda arte nova nasce de um arquivo-fonte
  (SVG/PNG mestro) em `brand/`, com um script que deriva `.ico`, `.icns`, os PNGs
  hicolor e os dois headers C++ embutidos. Trocar logo vira um passo reproduzível.

---

## 2. Mapa completo de edição (inventário verificado)

Cada linha abaixo foi conferida em `path:line`. Contagem total: **~118 pontos** no
repo do engine + **~20 no docs repo** + ~180 páginas de docs com o nome.

### 2.1 Créditos / autoria

| Local | Atual |
|---|---|
| [LICENSE:2](file:///C:/quantum2/LICENSE#L2) | `Copyright (c) 2026 SR VINIX and contributors` — **NÃO MEXER** |
| `NOTICE.md`, `AUTHORS` | **não existem** — criar (Fase 0) |
| [AGENTS.md:3](file:///C:/quantum2/AGENTS.md#L3), [:4](file:///C:/quantum2/AGENTS.md#L4), [:13](file:///C:/quantum2/AGENTS.md#L13) | "MIT, by SR VINIX" + regra do header |
| [editor/resources/icons/quantum-editor.rc.in:32](file:///C:/quantum2/editor/resources/icons/quantum-editor.rc.in#L32) | `CompanyName "Quantum Engine Community"` |
| [.rc.in:36](file:///C:/quantum2/editor/resources/icons/quantum-editor.rc.in#L36) | `LegalCopyright "Copyright (C) 2026 SR VINIX and contributors. MIT License."` |
| [.rc.in:38](file:///C:/quantum2/editor/resources/icons/quantum-editor.rc.in#L38) | `ProductName "Quantum Engine"` |
| [editor/resources/macos/Info.plist.in:28](file:///C:/quantum2/editor/resources/macos/Info.plist.in#L28) | `Copyright (C) 2026 SR VINIX and contributors. MIT License.` |
| [editor/resources/linux/org.quantum.editor.metainfo.xml:15](file:///C:/quantum2/editor/resources/linux/org.quantum.editor.metainfo.xml#L15) | `<name>SR VINIX Lima</name>` |
| [plugins/hello/plugin.json:5](file:///C:/quantum2/plugins/hello/plugin.json#L5) | `"author": "SR VINIX"` |
| [.github/FUNDING.yml:1](file:///C:/quantum2/.github/FUNDING.yml#L1) | `github: [eduardodoria]` |
| [editor/window/dialog/AboutWindow.cpp:74](file:///C:/quantum2/editor/window/dialog/AboutWindow.cpp#L74) | `centeredText("Developed by SR VINIX and contributors")` |

### 2.2 Nome / metadados de build

| Local | Atual |
|---|---|
| [CMakeLists.txt:2](file:///C:/quantum2/CMakeLists.txt#L2) | `project(quantum-editor)` |
| [CMakeLists.txt:5-33](file:///C:/quantum2/CMakeLists.txt#L5-L33) | `QUANTUMEDITOR_VERSION`, `QUANTUM_VERSION_NUMERIC` |
| [CMakeLists.txt:34](file:///C:/quantum2/CMakeLists.txt#L34) | `message(STATUS "Quantum version is ...")` |
| [CMakeLists.txt:209-212](file:///C:/quantum2/CMakeLists.txt#L209-L212) | `quantum_add_windows_resource(... "quantum-editor.ico" ... "Quantum Engine Editor")` |
| [CMakeLists.txt:1043-1046](file:///C:/quantum2/CMakeLists.txt#L1043-L1046) | `"...-cmd.ico" ... "Quantum Engine Editor (command line)"` |
| [CMakeLists.txt:947-994](file:///C:/quantum2/CMakeLists.txt#L947-L994) | `QUANTUM_BUNDLE_ICON_NAME "quantum.icns"` + iconset |
| [CMakeLists.txt:218-230](file:///C:/quantum2/CMakeLists.txt#L218-L230), [:1099-1112](file:///C:/quantum2/CMakeLists.txt#L1099-L1112) | install de `.desktop`/metainfo/hicolor |
| [editor/EditorVersion.h.in:4](file:///C:/quantum2/editor/EditorVersion.h.in#L4) | `#define QUANTUM_EDITOR_VERSION` |
| [engine/CMakeLists.txt:491](file:///C:/quantum2/engine/CMakeLists.txt#L491), [:598](file:///C:/quantum2/engine/CMakeLists.txt#L598) | `APP_BUNDLE_IDENTIFIER "org.quantum.quantumengine"` |

### 2.3 Nome visível em runtime

| Local | Atual |
|---|---|
| [editor/backend/BackendWin.cpp:515](file:///C:/quantum2/editor/backend/BackendWin.cpp#L515) | `config.title = "Quantum Engine"` |
| [BackendWin.cpp:57](file:///C:/quantum2/editor/backend/BackendWin.cpp#L57) | `WINDOW_CLASS_NAME = L"QuantumEditorNativeWindow"` |
| [BackendWin.cpp:522](file:///C:/quantum2/editor/backend/BackendWin.cpp#L522) | `LoadImageW` do ícone (GLFW_ICON) |
| [editor/backend/BackendLinux.cpp:997](file:///C:/quantum2/editor/backend/BackendLinux.cpp#L997) | `classHint.res_class = "QuantumEditor"` |
| [BackendLinux.cpp:2302](file:///C:/quantum2/editor/backend/BackendLinux.cpp#L2302), [:2751](file:///C:/quantum2/editor/backend/BackendLinux.cpp#L2751) | `setWindowTitle(..., "Quantum Engine")` |
| [engine/platform/win/QuantumWin.cpp:42](file:///C:/quantum2/engine/platform/win/QuantumWin.cpp#L42) | `#define QUANTUM_WINDOW_TITLE "Quantum"` |
| [QuantumWin.cpp:54](file:///C:/quantum2/engine/platform/win/QuantumWin.cpp#L54) | `L"QuantumGameWindow"` |
| [engine/platform/common/VulkanContext.cpp:104](file:///C:/quantum2/engine/platform/common/VulkanContext.cpp#L104), [:106](file:///C:/quantum2/engine/platform/common/VulkanContext.cpp#L106) | `pApplicationName` / `pEngineName = "Quantum"` |
| [editor/App.cpp:461](file:///C:/quantum2/editor/App.cpp#L461), [:736](file:///C:/quantum2/editor/App.cpp#L736) | `"Help: About Quantum"` / `"About Quantum"` |
| [editor/App.cpp:2736](file:///C:/quantum2/editor/App.cpp#L2736) | `updateWindowTitle("Quantum Hub")` |
| [AboutWindow.cpp:42](file:///C:/quantum2/editor/window/dialog/AboutWindow.cpp#L42), [:53](file:///C:/quantum2/editor/window/dialog/AboutWindow.cpp#L53) | `"About Quantum##AboutModal"` |
| [AboutWindow.cpp:73](file:///C:/quantum2/editor/window/dialog/AboutWindow.cpp#L73) | `"Version: " QUANTUM_EDITOR_VERSION` |
| [editor/Project.cpp:3028](file:///C:/quantum2/editor/Project.cpp#L3028) | `return "Quantum";` (fallback do nome do app) |
| [editor/AppSettings.cpp:19-30](file:///C:/quantum2/editor/AppSettings.cpp#L19-L30) | `%APPDATA%/Quantum`, `Application Support/Quantum`, `xdg/quantum`, `.config/quantum` |
| [editor/Out.cpp:115](file:///C:/quantum2/editor/Out.cpp#L115) | `=== Quantum editor <versão> started at` |
| [editor/resources/linux/quantum-editor.desktop:5,6,10,15](file:///C:/quantum2/editor/resources/linux/quantum-editor.desktop#L5-L15) | `Name=Quantum`, `Exec`, `Icon`, `StartupWMClass=QuantumEditor` |
| [metainfo.xml:2,5,14,42-45,50-58](file:///C:/quantum2/editor/resources/linux/org.quantum.editor.metainfo.xml#L2-L58) | `<id>org.quantum.editor</id>`, `developer id="org.quantum"`, URLs, screenshots `quantum.org/*.png` |

### 2.4 Serviços do editor (identidade que vaza para fora)

| Local | Atual |
|---|---|
| [editor/ai/McpServer.cpp:168](file:///C:/quantum2/editor/ai/McpServer.cpp#L168) | `{"name","quantum-editor"},{"title","Quantum Editor"}` |
| [editor/ai/AiProvider.cpp:393](file:///C:/quantum2/editor/ai/AiProvider.cpp#L393), [:416](file:///C:/quantum2/editor/ai/AiProvider.cpp#L416) | header `X-Title: Quantum Editor` |
| [editor/ai/AiService.cpp:773](file:///C:/quantum2/editor/ai/AiService.cpp#L773) | "You are Quantum Editor Assistant …" |
| [editor/App.cpp:1091](file:///C:/quantum2/editor/App.cpp#L1091) | `https://docs.quantum.org/` ou issues `quantumengine/quantum` |

### 2.5 Geração de projeto / export (marca que entra no jogo do usuário)

| Local | Atual |
|---|---|
| [editor/Generator.cpp:1039](file:///C:/quantum2/editor/Generator.cpp#L1039) | URL de source no `README` gerado |
| [editor/Generator.cpp:1314](file:///C:/quantum2/editor/Generator.cpp#L1314) | "Full engine **and editor** source (upstream): …" |
| [editor/Exporter.cpp:2249](file:///C:/quantum2/editor/Exporter.cpp#L2249) | `<script src="https://www.youtube.com/game_api/v1">` no export web |
| [editor/Exporter.cpp:2332](file:///C:/quantum2/editor/Exporter.cpp#L2332) | `replaceAll(... "org.quantum.quantumengine" ...)` |
| [editor/Exporter.cpp:17](file:///C:/quantum2/editor/Exporter.cpp#L17), [:2231](file:///C:/quantum2/editor/Exporter.cpp#L2231) | favicon embutido escrito no projeto exportado |

### 2.6 Android (pacote e JNI — rename caro)

| Local | Atual |
|---|---|
| `engine/platform/android/java/org/quantumengine/quantum/{MainActivity,ActivityListener,UserSettings}.java:1` | `package org.quantumengine.quantum;` (+ `MainActivity.java:75-76`) |
| `engine/platform/android/java-{admob,billing}/org/quantumengine/quantum/*Wrapper.java:1` | mesmo pacote |
| [engine/platform/android/NativeEngine.cpp:351](file:///C:/quantum2/engine/platform/android/NativeEngine.cpp#L351), [:582](file:///C:/quantum2/engine/platform/android/NativeEngine.cpp#L582) | `Java_org_quantumengine_quantum_*`, `"org/quantumengine/quantum/UserSettings"` |
| [engine/workspaces/androidstudio/app/build.gradle:66](file:///C:/quantum2/engine/workspaces/androidstudio/app/build.gradle#L66) | `namespace = 'org.quantumengine.quantum'` |

### 2.7 CI

| Local | Atual |
|---|---|
| [.github/workflows/cmake.yml:28](file:///C:/quantum2/.github/workflows/cmake.yml#L28) | `SIGNPATH_PROJECT_SLUG: quantum` |
| [cmake.yml:343](file:///C:/quantum2/.github/workflows/cmake.yml#L343) | zsync slug `gh-releases-zsync\|quantumengine\|quantum\|latest\|Quantum-*-x86_64.AppImage.zsync` |
| workflows em geral | badge URLs `github.com/quantumengine/quantum/actions/...` |

### 2.8 README do engine (página pública)

| Local | Atual |
|---|---|
| [README.md:3-5](file:///C:/quantum2/README.md#L3-L5) | `<picture>` com logos `quantumengine/quantum-site/main/logo/*` |
| [README.md:18-21](file:///C:/quantum2/README.md#L18-L21) | Download / Docs / Releases / Discord |
| [README.md:25](file:///C:/quantum2/README.md#L25) | thumbnail do YouTube |
| [README.md:31-36](file:///C:/quantum2/README.md#L31-L36) | 5 badges de CI + **badge Discord com id de servidor** (`img.shields.io/discord/1356958061880934480`) |
| [README.md:69-78](file:///C:/quantum2/README.md#L69-L78) | 6 screenshots hospedados em `quantum-site` |
| [README.md:106](file:///C:/quantum2/README.md#L106), [:122](file:///C:/quantum2/README.md#L122) | "Quantum website" + `git clone` do upstream |
| [README.md:227-228](file:///C:/quantum2/README.md#L227-L228) | demos `quantumengine.github.io/*` |
| [README.md:239-244](file:///C:/quantum2/README.md#L239-L244) | bloco Links (site, docs, issues, Discord) |
| `README.md` "Why Quantum?" | headings e copy que nomeiam o produto |

### 2.9 Logos e ícones atuais (a substituir)

| Asset | Consumo |
|---|---|
| [editor/resources/icons/quantum-editor.ico](file:///C:/quantum2/editor/resources/icons/quantum-editor.ico) (46 KB) | `CMakeLists.txt:209-212` |
| [quantum-editor-cmd.ico](file:///C:/quantum2/editor/resources/icons/quantum-editor-cmd.ico) (32 KB) | `CMakeLists.txt:1043-1046` |
| [quantum-editor.png](file:///C:/quantum2/editor/resources/icons/quantum-editor.png) (188 KB) | `.desktop` / AppImage |
| `icons/linux/quantum-editor-{16,32,48,64,128,256,512}.png` | install hicolor (`CMakeLists.txt:218-230`) |
| [quantum-logo_png.h](file:///C:/quantum2/editor/resources/icons/quantum-logo_png.h) (`inline unsigned char quantum_logo_png[]`, 6780 bytes de PNG em `:573`) | `AboutWindow.cpp:9,32-33` e `LoadingWindow.cpp:7,56-57,96` |
| [quantum-favicon_png.h](file:///C:/quantum2/editor/resources/icons/quantum-favicon_png.h) | `Exporter.cpp:17,2231` |
| `quantum.icns` (gerado do iconset) | `CMakeLists.txt:947-994` |
| `C:\quantum-docs\docs\assets\logo\{logo_part1,logo_part2,logo_part3,quantum_logo_dark,quantum_logo_image,quantum_logo_transparent}.png` | `mkdocs.yml:18-19`, `theme/partials/header.html:8-9` |

Os dois `_png.h` são PNG serializado como array C — **não editar à mão**: gerar a
partir do PNG mestre (Fase 1). Os demais `_png.h` da pasta (camera, lua, scene…)
são ícones de UI, não marca — ficam.

### 2.10 Docs repo `C:\quantum-docs` (remoto ainda é `quantum-docs`)

| Local | Atual |
|---|---|
| git remote `origin` | `https://github.com/alguemqualquer123/quantum-docs.git` → **`quantum-docs`** |
| [mkdocs.yml:1](file:///C:/quantum-docs/mkdocs.yml#L1) | `site_name: Quantum Engine Docs` |
| [mkdocs.yml:2](file:///C:/quantum-docs/mkdocs.yml#L2) | `site_url: 'https://docs.quantum.org'` |
| [mkdocs.yml:3](file:///C:/quantum-docs/mkdocs.yml#L3) | `site_author: 'SR VINIX'` |
| [mkdocs.yml:4](file:///C:/quantum-docs/mkdocs.yml#L4) | `site_description` com o nome |
| [mkdocs.yml:6-8](file:///C:/quantum-docs/mkdocs.yml#L6-L8) | `repo_name` / `repo_url` / `edit_uri` |
| [mkdocs.yml:10](file:///C:/quantum-docs/mkdocs.yml#L10) | `copyright: 'Copyright &copy; 2026 SR VINIX. Documentation under CC-BY 4.0.'` |
| [mkdocs.yml:18-19](file:///C:/quantum-docs/mkdocs.yml#L18-L19) | `theme.logo` / `theme.favicon` → arte nova |
| [mkdocs.yml:35-40](file:///C:/quantum-docs/mkdocs.yml#L35-L40) | `social` GitHub/Discord, `homepage: quantum.org`, `analytics: G-D2G3S99SDC` |
| [mkdocs.yml:213](file:///C:/quantum-docs/mkdocs.yml#L213) | nav `License & Credits → about/license.md` |
| [theme/partials/header.html:7](file:///C:/quantum-docs/theme/partials/header.html#L7), [:8-9](file:///C:/quantum-docs/theme/partials/header.html#L8-L9) | `default('https://quantum.org')`, `alt="Quantum"` |
| [theme/partials/header.html:32](file:///C:/quantum-docs/theme/partials/header.html#L32) | link Discord do Quantum no header do site (não estava no inventário original) |
| [theme/partials/page-footer.html:9](file:///C:/quantum-docs/theme/partials/page-footer.html#L9) | `{{ config.copyright }}` |
| [theme/css/quantum.css:2-3](file:///C:/quantum-docs/theme/css/quantum.css#L2-L3) | cabeçalho do arquivo (renomar para `quantum.css` + `<theme>/static` refs) |
| [theme/css/highlight.css:2-3](file:///C:/quantum-docs/theme/css/highlight.css#L2-L3) | "Quantum docs / Quantum accent palette" |
| [theme/js/quantum.js:2](file:///C:/quantum-docs/theme/js/quantum.js#L2) | cabeçalho do arquivo |
| [docs/about/license.md:16](file:///C:/quantum-docs/docs/about/license.md#L16) | "Quantum Engine is created and maintained by **SR VINIX**" |
| [docs/about/license.md:33-36](file:///C:/quantum-docs/docs/about/license.md#L33-L36) | Website / GitHub / Discord / `github.com/sponsors/eduardodoria` |
| [docs/index.md:3,12,18,26,35,40,60,103-112](file:///C:/quantum-docs/docs/index.md#L3-L112) | hero, CTA de download, nota Supernova |
| [README.md:1-3](file:///C:/quantum-docs/README.md#L1-L3) | título e link do repo |
| [scripts/build-versions.sh:25](file:///C:/quantum-docs/scripts/build-versions.sh#L25) | `DOCS_SITE_URL_BASE:-https://docs.quantum.org` |
| [docs/contributing/documentation-guide.md:84-86](file:///C:/quantum-docs/docs/contributing/documentation-guide.md#L84-L86) | URLs `docs.quantum.org/{,0.7/,unstable/}` |
| [docs/about/faq.md:128-129](file:///C:/quantum-docs/docs/about/faq.md#L128-L129) | `%APPDATA%\Quantum\editor.log`, `~/Library/Application Support/Quantum/` |
| [docs/about/faq.md:142](file:///C:/quantum-docs/docs/about/faq.md#L142) | linha "Discord — discord.gg/yXXDyJf3gT for questions and …" |

Menções do nome por pasta de `docs/` (arquivos / ocorrências): `about` 2/18 ·
`building` 8/29 · `contributing` 1/2 · `editor` 17/40 · `getting-started` 4/22 ·
`manual` 24/71 · `reference` 25/30 · `tutorials` 4/6 — **85 arquivos**. Troca é
mecânica (`sed`) **exceto** onde "Quantum" é identificador técnico (caminhos de
config, nomes de arquivo, API), que seguem a Fase 4 deste plano e o rename de
código do plano antigo.

---

## 3. Logos novos — especificação

Conceito **aprovado (D7)**: símbolo de **núcleo com anéis de orbital** + wordmark
`QUANTUM` em caixa alta, sans serif geométrica; acento ciano→violeta, mais neutro
escuro/claro para as duas variantes do `<picture>` de preferência de tema. Duas
formas por arte: mark sozinho (ícones 16→512 px, favicon) e mark + wordmark
(README, splash, header dos docs). Legibilidade obrigatória em 16 px — se o orbital
virar manchinha no tamanho pequeno, o mark simplifica antes de entrar no repo.

Artefatos a produzir, todos derivados de um mestre `brand/quantum-logo.svg` (+
`brand/quantum-mark.svg` só o símbolo):

| Saída | Uso |
|---|---|
| `brand/quantum-logo_{dark,light,transparent}.png` (2048 px) | README `<picture>` + `quantum-site` substituto |
| `brand/quantum-mark-512.png` | base de `.icns`/`.ico` |
| `quantum-editor.ico` + `quantum-editor-cmd.ico` (16→256) | Windows, `CMakeLists.txt:209-212,1043-1046` |
| `icons/linux/quantum-editor-{16,32,48,64,128,256,512}.png` | hicolor |
| `quantum.icns` | macOS bundle |
| `quantum-logo_png.h` / `quantum-favicon_png.h` | About + Loading splash + favicon exportado |
| `docs/assets/logo/{logo_mark,logo_wordmark,logo_dark,logo_light}.png` | `mkdocs.yml` logo/favicon + header |

Ferramenta: `scripts/quantum-brand.sh` (Fase 1) — PNG mestre → `.ico` (ImageMagick
ou `iconutil` no macOS), → `_png.h` no mesmo formato `inline unsigned char` com a
linha `// (c) SR VINIX` preservada no topo (o header é arquivo-fonte do repo
e cai na regra do LICENSE; o *conteúdo* é arte nova), e → docs assets. Determinístico e
verificável com `--report`.

---

## 4. Fases

Cada fase termina com **build MSVC ok + exe rodando headless + probe próprio**, e
com commits separados. Nunca `git add -A` (regra do repo).

**Fase 0 — terreno e checkpoint.**
`git status` limpo ou checkpoint; confirmar que nenhum agente está editando;
corrigir a linha do LICENSE no plano antigo; criar `NOTICE.md` + `AUTHORS`
(fork de Quantum, commit-base do upstream `d363862c`-ancestral real, © SR VINIX nas
modificações); gravar este arquivo. *Probe:* `cmake --build build --target quantum-editor`
ainda verde **antes** de qualquer mudança; `git log -1` mostra o checkpoint.

**Fase 1 — ferramental de marca.**
`scripts/quantum-brand.sh` (derivadas de arte + `_png.h`) e `scripts/brand-rename.sh`
com denylist explícita (`LICENSE`, linhas 1-2 de todo fonte, `engine/libs/`, `libs/`,
`build/`, `templates/*/.quantum/`, `miniaudio.h`). *Probe:* `--report` bate com a
tabela da seção 2; aplicar num clone temporário dá `git diff` revisável.

**Fase 2 — artes novas.**
Gerar/aceitar o conceito, produzir todos os artefatos da seção 3, plugar os dois
`_png.h`, `.ico`s, PNGs linux, `.icns`, e as imagens do docs. *Probe:* build;
`grep` dos consumos em `AboutWindow.cpp:9,32-33`, `LoadingWindow.cpp:7,56-57,96`,
`BackendWin.cpp:522`, `Exporter.cpp:2231` apontando para os nomes novos. **Nota:**
a troca de logo só é *vista* no About/splash da GUI — verificar com screenshot
capturado do framebuffer do próprio editor, ou declarar visualmente não verificado
e pedir ao dono que olhe (regra 1 do repo: nunca abrir `quantum-editor.exe` por conta).

**Fase 3 — strings de marca e créditos.**
Seções 2.1→2.4 e 2.7: About (`Developed by` → `Developed by SR VINIX` + linha de
upstream), títulos de janela, menus, `VERSIONINFO`, `Info.plist`, `.desktop`,
metainfo, slug SignPath e zsync, MCP/`X-Title`/prompt do assistente, `Out.cpp:115`,
`pApplicationName/pEngineName` Vulkan, `Project.cpp:3028`. Aqui também o corte de
links de terceiros que saem (D11): `App.cpp:1091` → Pages dos docs,
`Exporter.cpp:2249` (o `<script>` de YouTube some do export web), e os URLs de
download/docs que `editor/` injeta. *Probe:* build;
`grep -ri "quantum engine" editor/ engine/` sem hits fora de LICENSE/headers/attribution.

**Fase 4 — ids técnicos e dados do usuário.**
Ids definidos em D13: `org.quantum.editor` → `org.srvinix.quantum.editor`, bundle id
macOS `org.srvinix.quantum`, pacote Android `org.srvinix.quantum` +
símbolos JNI + `build.gradle`, `%APPDATA%`/xdg → `Quantum` com a migração
de D4, e o `.quantum/` por projeto. *Probe:* rodar `--project` numa cópia de template
com `.quantum/` antigo e confirmar reabertura + config no diretório novo;
Android: `assembleDebug` (se o SDK estiver na máquina, senão declarar não verificado).

**Fase 5 — README, docs repo e links.**
Reescrever `README.md` (seção 2.8): artes locais de `brand/`, Documentation →
`https://alguemqualquer123.github.io/quantum-docs/`, Downloads → releases do fork,
Discord → convite do dono (`gSHUVHXNwP`), **sem** YouTube e **sem** as demos do org
antigo (D11), screenshots substituídos por capturas nossas
geradas com `--screenshot`, e uma linha "forked from Quantum Engine" com o link do
upstream. Nos docs: atualizar os 20 pontos da seção 2.10 conforme a tabela 7.1
(`site_url`, `repo_*`, `edit_uri`, `copyright`, `social`, `homepage`, remover
`analytics`, trocar logo/favicon) + `sed` das 85 páginas.
**Git (ação em sistema compartilhado — confirmar com o dono antes de cada push):**
criar `alguemqualquer123/quantum-docs` (renomear o repo `quantum-docs` atual no
GitHub é o caminho mais curto e preserva stars/issues; é ação no repo **do dono**,
não do terceiro), `git remote set-url origin` em `C:\quantum-docs`; no engine,
`origin` → `alguemqualquer123/quantum` e `upstream` → `quantumengine/quantum` (D10).
Habilitar Pages para `quantum-docs`. *Probe:* `mkdocs build --strict` verde; grep por
`yXXDyJf3gT|1356958061880934480|eduardodoria` (o que é marca de comunidade antiga) e
por `quantum.org|docs.quantum|quantumengine|youtube` sobre `README.md`,
`C:/quantum-docs/mkdocs.yml`, `C:/quantum-docs/theme/` e `C:/quantum-docs/docs/` — os
dois devem retornar zero, exceto as linhas de atribuição ao upstream.

**Fase 6 — agents e skills.**
`AGENTS.md` (linhas 3, 4, 13 e a tabela de skills), `CLAUDE.md`, e as skills
espelhadas byte-idênticas em `.qoder/`, `.opencode/`, `.agents/`, `.claude/`
rename `quantum-*` → `quantum-*` (id = nome da pasta). *Probe:* os quatro trees
batem byte a byte; `grep -ri quantum AGENTS.md` só retorna a regra de header/attribution.

**Fase 7 — sweep final.**
`git grep -in quantum` e classificar o que sobrou em 4 baldes aceitos: LICENSE,
headers `// (c) SR VINIX`, notices de terceiros, e linhas explícitas de
"forked from Quantum". Qualquer hit fora desses baldes é bug. *Probe:* os dois
binários rodam headless, `--screenshot` de um template, e os 48 checks de
`tests/worldcreator/probe` continuam passando.

---

## 5. Verificação por fatia

```bash
cmake --build "$QUANTUM_BUILD_DIR" --target quantum-editor --config Debug       # MSVC é autoritativo
"$QUANTUM_BUILD_DIR/Debug/quantum-engine.exe" --project <dir> --window 1280x720 \
    --screenshot /tmp/quantum_frame.png --capture-frame 120                    # frame real
cd C:/quantum-docs && pip install -r requirements.txt && mkdocs build --strict
```

Sem unit tests no repo (`ctest` vazio): nada está pronto sem build + exe rodando.
Mudança de GUI/logo **não** se verifica abrindo o editor; usar `--screenshot` ou o
capture do framebuffer, ou declarar visualmente não verificado.

---

## 6. Riscos e não-metas

- **Fork duro.** A partir das Fases 3-4, commits do upstream conflitam em quase
  tudo que tem marca. Merge futuro = cherry-pick manual. Registrar em `NOTICE.md`.
- **Ids de pacote (D13) quebram upgrade no lugar** em Linux/Android/macOS —
  decidido aceitar: os dois instaladores convivem, e quem tinha o antigo reinstala
  do zero. Mitigação obrigatória: migração de dados (D4) no primeiro launch, para o
  usuário não perder config/layout mesmo reinstalando.
- **Android JNI é frágil**: `NativeEngine.cpp:351,582` precisa bater caractere a
  caractere com o novo `package`; um erro aqui só aparece em device.
- **`_png.h` é fonte, não gerado por build** — sem target CMake que os recrie,
  então a arte nova precisa ser commitada, não ignorada.
- **Não metas**: apagar ou reescrever `LICENSE`/headers de copyright; tocar código
  de terceiros; mudar formatos binários (a mágica é `DXPK1` e os esquemas VFS são
  `asset:// builtin:// data:// lua:// shader://` — nenhum carrega o nome, nada a
  migrar); trocar `namespace quantum` (isso é o plano antigo).
- **Concorrência**: pausar outros agentes nas Fases 1-3; um reescritor de árvore
  inteira contra arquivos em edição gera conflito ilegível.

---

## 7. Decisões fechadas (2026-11-04)

As perguntas em aberto viraram estas decisões. Nenhuma bloqueia mais nenhuma fase.

- **D7 — conceito do logo: orbital + wordmark.** Símbolo = núcleo com anéis de
  orbital, wordmark `QUANTUM` em caixa alta, sans serif geométrica. Duas variantes
  obrigatórias por uso (mark sozinho, mark + wordmark). Escala testada de 16 px
  (favicon) a 2048 px (README). Paleta: um acento primário novo (ciano→violeta é o
  default, já que o acento atual do site é de marca do Quantum) + neutro escuro e
  claro para o `<picture>` de preferência de tema.
- **D8 — URL pública dos docs: GitHub Pages.**
  `site_url: https://alguemqualquer123.github.io/quantum-docs/`,
  `homepage:` idem (era `quantum.org`),
  `edit_uri: https://github.com/alguemqualquer123/quantum-docs/edit/main/docs/`,
  `repo_name/repo_url: alguemqualquer123/quantum-docs` → **`quantum-docs`**, e
  `scripts/build-versions.sh:25` passa a default `DOCS_SITE_URL_BASE` para o Pages.
  Sem domínio próprio por ora; se vier um depois, é uma mudança em 5 chaves de
  `mkdocs.yml` + os URLs do `.metainfo.xml`.
- **D9 — sem site/landing próprio.** O que era `quantum.org/#download` vira a página
  de releases do fork: `https://github.com/alguemqualquer123/quantum/releases`.
  Assume-se que o repo do engine passa a chamar-se **`quantum`** no GitHub do dono
  (ver D10); se o nome final for outro, só o README e os badges mudam.
- **D10 — remote do engine: fork com upstream ao lado, nada de transfer.**
  Hoje `origin` = `github.com/quantumengine/quantum`. Forma final:
  `origin` → `https://github.com/alguemqualquer123/quantum` (repo novo do dono,
  push da branch atual) e `upstream` → `quantumengine/quantum` (fetch read-only, para
  cherry-pick futuro, que é o único caminho depois do fork duro da seção 6).
  **Não renomear/transferir o repo do Quantum** — isso é ação em propriedade de
  terceiro e fica fora de escopo. Feito por comando na Fase 5, com confirmação do
  dono antes do primeiro `git push`.
- **D11 — comunidade: Discord próprio, resto sai.**
  **Discord do projeto = `https://discord.gg/gSHUVHXNwP`** (do dono, SR VINIX), e
  entra nos 7 pontos que hoje apontam para o convite do Quantum
  (`yXXDyJf3gT`): [README.md:21](file:///C:/quantum2/README.md#L21),
  [README.md:36](file:///C:/quantum2/README.md#L36),
  [README.md:244](file:///C:/quantum2/README.md#L244),
  `C:/quantum-docs/mkdocs.yml:37`, `C:/quantum-docs/docs/about/license.md:35`,
  `C:/quantum-docs/docs/about/faq.md:142` e
  `C:/quantum-docs/theme/partials/header.html:32`. O badge de membros em
  `README.md:36` usa o **id numérico do servidor** (`img.shields.io/discord/1356958061880934480`)
  e esse id não está no convite: ou o dono fornece o id do servidor dele, ou o badge
  vira um link simples com `?label=Discord&logo=discord` sem contagem.
  Continuam **removidos**, sem substituto: o thumbnail/`<script>` do YouTube
  (`README.md:25`, `Exporter.cpp:2249`), as 2 demos `quantumengine.github.io/*`
  (`README.md:227-228`) e os screenshots do `quantum-site` (`README.md:3-5,69-78`,
  `metainfo.xml:50-58`). As imagens passam a ser arquivos locais (`brand/` no
  engine, `docs/assets/` nos docs) e os screenshots de README viram capturas
  nossas, geradas com `--screenshot`.
- **D12 — analytics removido.** `mkdocs.yml:40` `G-D2G3S99SDC` é propriedade do
  Quantum: deletar a chave `analytics` inteira. Não colocar id novo até o dono criar
  a propriedade dele.
- **D13 — ids técnicos: trocar tudo** (era a pergunta 6). Linux
  `org.srvinix.quantum.editor` + `developer id="org.srvinix"`, macOS
  `org.srvinix.quantum`, Android `org.srvinix.quantum` com os símbolos JNI e
  `build.gradle namespace` acompanhando. Consequência aceita: installs antigos não
  atualizam por cima — as duas instalações convivem. A migração de dados de usuário
  (D4) continua obrigatória e roda no primeiro launch.
- **D14 — `LICENSE` e os headers `// (c) SR VINIX` permanecem byte-idênticos**
  (seção 0). SR VINIX entra como autor das modificações em `NOTICE.md`/`AUTHORS`,
  About, `.rc` `LegalCopyright`, `Info.plist`, metainfo `<developer name>` e na
  página de licença dos docs — sempre com a linha de upstream ao lado.

## 7.1 Efeito imediato nas tabelas da seção 2

| Onde | Valor final |
|---|---|
| `mkdocs.yml:2` | `site_url: 'https://alguemqualquer123.github.io/quantum-docs/'` |
| `mkdocs.yml:6-8` | `repo_name: 'alguemqualquer123/quantum-docs'`, `repo_url` idem, `edit_uri` com `quantum-docs` |
| `mkdocs.yml:10` | `copyright: 'Copyright &copy; 2026 SR VINIX. Quantum é fork do engine Quantum, (c) SR VINIX — MIT. Documentação CC-BY 4.0.'` |
| `mkdocs.yml:35-40` | `social`: GitHub + **Discord `gSHUVHXNwP`**; `homepage` = Pages; `analytics` fora |
| `theme/partials/header.html:32` | href do Discord → `https://discord.gg/gSHUVHXNwP` |
| `docs/about/faq.md:142` | texto e href do Discord → convite novo |
| `README.md:21,244` | Documentation → Pages; Discord → `https://discord.gg/gSHUVHXNwP` |
| `README.md:36` | badge: href → convite novo; a **contagem** exige o id numérico do servidor (ou badge sem número) |
| `README.md:25,227-228` | removidos (YouTube + demos do org antigo, D11) |
| `README.md:3-5,69-78` | artes locais `brand/*` + screenshots capturados por nós |
| `docs/about/license.md:16` | "Quantum é mantido por **SR VINIX**. Fork do engine Quantum, criado por SR VINIX (MIT)." |
| `docs/about/license.md:33-36` | Website/Discord/sponsors removidos; GitHub → `alguemqualquer123/quantum` |
| `metainfo.xml:5,14,42-45,50-58` | `org.srvinix.quantum.editor`, vcs-browser `alguemqualquer123/quantum`, screenshots locais |
| `editor/App.cpp:1091` | `https://alguemqualquer123.github.io/quantum-docs/` (docs) / issues do fork |
| `editor/Generator.cpp:1039,1314` | URL do fork do dono no `README` gerado do projeto |
| `editor/Exporter.cpp:2249` | `<script>` de YouTube removido |
| `CMakeLists.txt:491,598` (engine) / `build.gradle:66` | `org.srvinix.quantum` |
| git remotos | engine: `origin`→`alguemqualquer123/quantum`, `upstream`→`quantumengine/quantum`; docs: `origin`→`alguemqualquer123/quantum-docs` |

---

## 8. Checklist

- [x] F0: checkpoint + `NOTICE.md`/`AUTHORS` + correção da linha do LICENSE no plano antigo
- [x] F1: derivador de marca (`scripts/quantum_brand.py`, um script só em vez dos dois
      `.sh` previstos) + artes-mestre em `brand/`
- [x] F2: artes novas no conceito D7 (orbital + wordmark; `.ico`, `.icns`, PNGs
      linux, 2 `_png.h`, logo/favicon dos docs); splash/About verificados por build
      headless, não por janela aberta
- [x] F3: strings de marca/créditos (2.1-2.4, 2.7), About com "SR VINIX" + upstream,
      e cortes de terceiros de D11 (YouTube no export, docs link em `App.cpp:1091`)
      e os 7 pontos de Discord → `gSHUVHXNwP`
- [x] F4: ids de pacote de D13 (`org.srvinix.*`, Android + JNI) + `%APPDATA%`/`.quantum` → `Quantum` com migração
- [x] F5: README novo sem links mortos; docs em `C:\quantum-docs` reescritos para a
      marca nova. `mkdocs build --strict` **não** rodou (mkdocs não instalado na
      máquina); links verificados por inspeção. Pages e o repo `quantum-docs` no
      GitHub dependem de F5b.
- [ ] F5b: remotos — engine `origin`→`alguemqualquer123/quantum` + `upstream`→Quantum,
      docs `origin`→`quantum-docs`; **cada push com confirmação explícita do dono**
- [x] F6: `AGENTS.md`/`CLAUDE.md` + 4 trees de skills renomeadas e byte-idênticas
- [x] F7: sweep — ver §9; os hits restantes são identifiers de código, não marca
- [x] Nenhum `git add -A`, nenhuma execução do editor pela minha mão, um build tree só

---

## 9. Desvios registrados na execução (2026-11-05)

O que ficou com o nome antigo **de propósito**, e por quê. Todos são escopo do
`QUANTUM_REBRAND_PLAN.md` (renomeação de código), não da marca.

| O que ficou | Por que não foi trocado aqui |
|---|---|
| Alvos e executáveis `quantum`, `quantum-editor`, `quantum-engine`, `quantum-reflector`, `quantum-resource-compiler`, `quantum-build-generator`, `quantum-editor-cmd`; `namespace quantum`, `QUANTUM_*`, `Quantum.h`, `.quantum/` | Identificadores de código: trocar um deles sem trocar todos quebra build e scripts de projeto. É a Fase 2-4 do plano de rebrand de código. |
| Nomes de scheme Xcode (`Quantum iOS`, `Quantum macOS`) em `editor/Exporter.cpp` e `.github/workflows/engine-xcode.yaml` | Os schemes vivem no template checkado `engine/workspaces/xcode/Quantum.xcodeproj`. `Exporter::writeXcodeBuildSettings` exige que o nome bata com o template; trocar só o lado do editor faz todo export Apple falhar com "Apple export template has no ... target". |
| `org.quantum.quantumengine` como bundle id default | Aparece duas vezes em `engine/CMakeLists.txt`, quatro em `project.pbxproj` e é a string que `Exporter.cpp` usa para reconhecer o default. Nunca chega a um bundle publicado (o projeto sempre traz o seu), e `project.pbxproj` não é verificável neste ambiente. |
| `set(APP_NAME quantum-project)` em `engine/CMakeLists.txt:27`, o matcher `Exporter.cpp:2119` e o fallback `Exporter::getAppName()` | Os três formam um par chave/valor: o editor só substitui a linha default se ela bater literalmente. Trocar um sem os outros silenciosamente desliga a substituição. |
| `SIGNPATH_PROJECT_SLUG: quantum` + `SIGNPATH_ORGANIZATION_ID` em `cmake.yml` | Slug de projeto dentro da conta SignPath do upstream; não existe no nosso até alguém criar. Renomear a string do CI produz um erro de assinatura, não uma marca nova. Decisão do dono. |
| `libs/uilab` e seus `quantum-patches/` | Biblioteca vendida pristine-upstream + patches; só se mexe nela por `scripts/sync-uilab.sh regen`. |
| `scripts/quantum-build-dir.bat`, `tests/quantum_tests.cmake`, `engine/ui_demo/quantum_ui_demo.cmake`, `flake.nix` (`quantum`), atributos CMake/nix | Nomes de arquivo/atributo de build. São a Fase 3 do plano de código. |

Ganhos além do inventário da seção 2 (encontrados no sweep):

- `tools/reflector/generate.cpp:1085` emitia `# quantum Lua API` no título do
  `build/generated/lua_api.md`; agora `# Quantum Lua API`, e o doc foi regenerado.
- `BillingWrapper.java` tinha `TAG = "Quantum"` (visível no logcat) → `"Quantum"`.
- `%LOCALAPPDATA%/quantum/{shaders,material-previews}` estavam documentados nas skills
  como cache do editor depois de a rota ter virado `quantum/`; as skills corriam atrás
  de um caminho que não existe mais.
- A CI do AppImage copiava `linux/quantum-editor-512.png` para `AppDir/quantum-editor.png`
  e o `.desktop` velho, enquanto o `Icon=` do novo `quantum-editor.desktop` pide
  `quantum-editor` — o ícone do AppImage ficava quebrado. Agora os dois nomes batem.
- Os ícones dos tools (`resource-server`, `build-generator`, `resource-compiler`) ainda
  apontavam para os `.ico` antigos; agora usam `quantum-editor*.ico`.
- Skills: as descrições e títulos `Quantum ...` viraram `Quantum ...`; `AGENTS.md` ganhou
  a linha de fork + SR VINIX e a tabela de skills renomeada.

Pendência de verificação (2026-11-05): `build/Debug/quantum-editor.exe` ainda embute o
literal velho `QuantumProjects` porque `quantum-editor.dir/Debug/HubWindow.obj` tinha o
mesmo minuto do meu edit e o MSBuild o considerou atualizado. A fonte e o `.obj`
recompilado (21:22) já trazem `QuantumProjects`, e o `quantum-editor-cmd.exe` linkado
depois disso está correto; só falta o link do editor, que não rodei porque outro agente
segurava a árvore de build (o relink falhou com `C1090`/PDB). Um `cmake --build build
--target quantum-editor --config Debug` resolve.


