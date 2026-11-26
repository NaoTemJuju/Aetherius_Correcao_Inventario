# Diagnóstico — Requisições Periódicas de Inventário (`snapshot`) e Impacto em Escala

> **Contexto:** Durante a validação da prévia 3D no inventário (`AETHERIUS - CORREÇÃO INVENTARIO`), os logs do servidor registraram rajadas periódicas de **7 requisições `request inventory snapshot`** a cada ~5 segundos mesmo com o jogador parado olhando o mesmo item. Este documento detalha a origem dessas chamadas, o custo real no servidor para 50–100 jogadores simultâneos e as correções recomendadas.

---

## 1. Para que servem essas 7 requisições?

O inventário completo do personagem não cabe em um único pacote customizado do SkyMP. Por isso, o servidor pagina a resposta em blocos limitados a **`PAGE_ITEMS = 120`** e **`PAGE_BYTES = 13000`** (~13 KB) em `aetherius-server-main/server/ts/systems/aetheriusInventoryService.ts`:

- Em um personagem de teste com **~243 itens**, cabem aproximadamente ~35 itens por pacote, resultando em **7 requisições `request inventory snapshot`** para concluir **1 leitura completa** do inventário.
- Em um jogador comum com 50–80 itens, cada leitura completa gera cerca de **2–3 requisições**.

---

## 2. Por que está disparando a cada ~5 segundos mesmo parado?

Foram identificados dois fatores combinados no ambiente atual:

1. **Gatilho periódico no cliente a cada ~5s:**
   - Em `aetherius-client-main/client/src/services/services/aetheriusUiService.ts`, a notificação `changed` para a UI deve ocorrer apenas em `onContainerChanged` ou `onEquipChanged` no jogador (`0x14`).
   - Disparar cravado a cada ~5 segundos com o jogador parado indica que algum mod/script ativo na load order está adicionando/removendo um item ou *token* invisível no inventário periodicamente, ou que a build do cliente em uso ainda utiliza polling antigo.
2. **Descompasso de protocolo entre o frontend do MO2 e o servidor local:**
   - O `inventory-module.js` instalado em `AETHERIUS - UI CORE` (`MeridianUI/aetheriusui/modules/inventory/inventory-module.js`) já envia `knownRevision` esperando que o servidor responda `notModified: true` em **1 único pacote leve** (e traz suporte a `pageOffsets` paralelos).
   - O `aetheriusInventoryService.ts` do servidor local ainda não trata `knownRevision` / `notModified`, então **toda atualização baixa novamente todas as 7 páginas completas**.

---

## 3. Isso causa lag com 50–100 jogadores? (Comparação com combate/dano)

**Sim.** Na implementação atual de `aetheriusInventoryService.ts`, o custo por leitura paginada é significativamente mais pesado do que eventos de combate/dano:

1. **Consulta SQL no PostgreSQL por página (`diademasBalance`):**
   - Cada chamada a `snapshot()` executa `this.model(actorId)`, que por sua vez executa uma query no PostgreSQL (`SELECT COALESCE(w.balance, 0) ... FROM characters c LEFT JOIN diademas_wallets w ...`).
   - Em um inventário de 7 páginas, são **7 queries SQL no PostgreSQL a cada ~5 segundos** para **1 único jogador** parado.
2. **Reconstrução completa do modelo + SHA-256 por página:**
   - `this.model(actorId)` percorre todos os itens, magias, armadura e favoritos e recalcula `JSON.stringify` + `createHash("sha256")` **7 vezes seguidas** dentro da mesma leitura paginada.
3. **Serialização `O(N²)` na montagem de cada página:**
   - O loop de paginação executa `Buffer.byteLength(JSON.stringify({ ...out, items: [...out.items, item] }))` a cada item inserido na página (~35 serializações crescentes por página × 7 páginas ≈ **~245 serializações** por atualização).

### Comparativo prático
- **Combate / Dano / Hit:** processa em memória sem `JOIN` no PostgreSQL por tick e sem serialização `O(N²)` de listas grandes.
- **Inventário (`snapshot`) sem otimização:** se **20 a 30 jogadores** estiverem com o inventário aberto simultaneamente (comum em cidades, tavernas ou pós-eventos), o servidor passa a executar **~15 a 40 reconstruções completas de inventário + queries SQL por segundo** na thread principal do Node.js, gerando risco real de engasgos e latência global.
- **Com as otimizações aplicadas:** um jogador parado olhando o inventário gera **0 requisições/segundo** (e qualquer verificação eventual retorna em 1 pacote `notModified: true` sem reenviar itens).

---

## 4. Plano de correção recomendado (Servidor e Client)

1. **Implementar `knownRevision` / `notModified` e `pageOffsets` em `aetheriusInventoryService.ts`:**
   - Alinhar o backend ao contrato já esperado pelo `inventory-module.js` do `AETHERIUS - UI CORE`.
2. **Cachear `model(actorId)` por revisão e `diademasBalance(characterId)` com TTL:**
   - Reaproveitar o modelo já montado entre páginas consecutivas da mesma revisão e cachear o saldo de diademas em memória (ex.: TTL de 15–30s, invalidado em transações de loja/carteira).
3. **Eliminar o loop `O(N²)` na paginação:**
   - Substituir a re-serialização do objeto inteiro por soma incremental de `Buffer.byteLength(JSON.stringify(item)) + 1`.
4. **Filtrar o gatilho de 5s no `aetheriusUiService.ts`:**
   - Identificar o item/token não-jogável que está disparando `onContainerChanged`/`onEquipChanged` a cada 5 segundos e ignorar tokens invisíveis ou mudanças que não alteram o inventário visível do jogador.
