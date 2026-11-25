# Guia de Integração — AETHERIUS - CORREÇÃO INVENTARIO

Passo a passo para instalar o mod de correção no Mod Organizer 2 **sem alterar diretamente nenhum arquivo existente** e para incorporar o patch cirúrgico na linha oficial do `aetherius-client` / `AetheriusUI_Core`.

---

## 1. Instalação direta no Mod Organizer 2 (modo mod isolado)

1. Copie o conteúdo de `mo2-mod/` para uma pasta de mod separada no MO2 chamada **`AETHERIUS - CORREÇÃO INVENTARIO`** (ex.: `C:\modOrganizer\mods\AETHERIUS - CORREÇÃO INVENTARIO`).
2. Ative o mod no perfil desejado e posicione-o **abaixo (maior prioridade)** de `AETHERIUS - UI CORE` e `AETHERIUS - MERIDIAN - COMPATIBILIDADE`.
3. **Não** substitua arquivos dentro de `AETHERIUS - UI CORE` nem dentro dos mods de meshes/texturas (`SMIM`, `Particle Patch`, `Embers XD`, `Awesome Potions`, `Simple Wearable Lanterns`).
   - Apenas `MeridianUI/MeridianUI.dll` é sobreposta pelo VFS do MO2; os assets web sob `MeridianUI/aetheriusui/` permanecem intactos no `AETHERIUS - UI CORE` (sem conflito com o `ModSchemeHandler`).
4. Inicie o jogo normalmente pelo SKSE no MO2.

> **Rollback:** basta desmarcar `AETHERIUS - CORREÇÃO INVENTARIO` no MO2 para restaurar instantaneamente o comportamento anterior.

---

## 2. Integração oficial no repositório (`aetherius-client` / `AetheriusUI_Core`)

1. Parta de um checkout limpo do `heathbrownkeyworks/MeridianUI` no commit `5707877322c85a1bd1c2e3309487f266d0647ca9` já com `client/ui-core/meridian/changes.patch` aplicado.
2. Valide e aplique o patch cirúrgico:
   ```powershell
   git apply --check patch/meridian-inventory-preview-fix.patch
   git apply patch/meridian-inventory-preview-fix.patch
   ```
3. Recompile o MeridianUI em Release (MSVC 14.44 / VS 2022 Build Tools, Windows SDK 10.0.26100.0 `fxc.exe`, triplet `x64-windows-meridian`), conforme `AetheriusUI_Core/docs/DLL_BUILD_GUIDE.md`.
4. Execute os testes de regressão:
   ```powershell
   ctest --test-dir <buildDir> -C Release --output-on-failure -R "^(NifPreviewGpuTests|NifPreviewGpuUnsupportedSharingTests|NifLightingTests|PackedGeometryDecoderTests|NifTexturePathTests|NifPreviewRendererArchitectureTests|NifCameraMathTests|NifSceneCompositionTests|NifWeightInterpolationTests)$"
   ```
5. Atualize `client/ui-core/meridian/changes.patch` no `aetherius-client` (e `integrations/runtime-changes/MeridianUI/` no `AetheriusUI_Core`) e distribua o novo `MeridianUI.dll`.

---

## 3. Publicação da Documentação no Acervo Aetherius

Os arquivos prontos no padrão obrigatório do `AETHERIUS-ACERVO-DE-INFORMACOES` (somente `.md`, UTF-8 LF, `kebab-case`, frontmatter válido) estão em `docs/acervo/`:

1. Copie `docs/acervo/02-sistemas/correcao-inventario/` → `02-sistemas/correcao-inventario/`
2. Copie `docs/acervo/03-decisoes/ADR-0038-correcao-renderizacao-preview-inventario.md` → `03-decisoes/` *(confira se `0038` segue sendo o próximo número livre antes do commit)*
3. Copie `docs/acervo/07-guia-da-staff/itens/previa-3d-inventario.md` → `07-guia-da-staff/itens/`
4. Aplique as atualizações de índice descritas em `docs/ATUALIZACOES-ACERVO.md`.

---

## 4. Checklist de validação em jogo

- [ ] **Ovas de Salmão:** exibe os ovinhos sem o bloco retangular branco gigante.
- [ ] **Ovos de Chaurus:** exibe o ovo com brilho ciano suave, sem o disco branco gigante do `ppLight` e com enquadramento próximo correto.
- [ ] **Libélula Azul:** asas exibidas com transparência recortada (sem planos retangulares brancos).
- [ ] **Larva de Pescador:** corpo translúcido/halo suave sem a esfera branca sólida.
- [ ] **Poeira Lucente:** tigela e pó visíveis com efeito suave sem o cone branco sólido.
- [ ] **Tocha:** cabo e chama aditiva sem topo branco opaco e sem plano quadrado de `HeatRefraction`.
- [ ] **Lanterna de Ferro:** exibe textura metálica/vidro normalmente (não fica branca), inclusive após equipar/desequipar.
- [ ] **Poções (*Awesome Potions Simplified*):** rolha/corda marrom-douradas (sem branco lavado), líquido colorido e **animado em movimento contínuo**, vidro cristalino com contorno Fresnel e reflexo vertical sem ficar leitoso nem fumê preto.
- [ ] **Itens escuros (Adaga de Ferro, Elmo Daédrico, Hot Cabbage Potato Soup):** detalhes de metal, relevos, brilho cubemap e tigela de madeira claramente visíveis (não viram vulto preto).
- [ ] **Estabilidade visual:** olhar fixamente para um item por 15+ segundos sem pisca-pisca durante sincronizações do inventário.
- [ ] **Regressão geral:** conferir armaduras, armas, livros, UI CEF (radial TAB, HUD, criação de personagem, bancada de Alquimia) e tela de carregamento.
