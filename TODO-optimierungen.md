# TODO: Performance- & Speicher-Optimierungen

> **Status: alle Punkte umgesetzt (2026-06-17).** Build sauber unter den strikten
> Warnflags + ASan/UBSan; alle 35 C-Unit-Tests und 13 CLI-Suites grün. BLAKE3 ist
> per Known-Answer-Vektoren byte-identisch (Journal-Hash-Kette & HMAC-Seal
> unverändert), Replay weiterhin byte-identisch.
>
> | Punkt | Umsetzung | Verifikation |
> | --- | --- | --- |
> | **A1** Index-Cache | Generations-/Dirty-Flag + gerenderter Cache im `spg_mem_store`; `spg_mem_index` scannt nur noch bei Invalidierung (save/delete) | neuer Test `test_index_cache_invalidation` |
> | **A2** Save-Scans | ein `collect_by_recency`-Pass liefert Set+Count+Max; Index nach Write rein in-memory aufgebaut (Upsert), kein zweiter Scan | `test_mem_store`, manueller MEMORY.md-Check |
> | **B1** Render blockweise | `append_cstr`/`append_span` per `memcpy`, `append_char` nur Einzelzeichen | `test_context`, CLI-Suites |
> | **B2** Integer ohne snprintf | handgeschriebene Uint→Dezimal in `context.c` + `sexpr.c` | `test_context`/`test_sexpr` |
> | **B3** BLAKE3 words | LE-`memcpy` + Zero-Pad mit skalarem Fallback (`__BYTE_ORDER__`) | KAT-Vektoren `test_hash` |
> | **B4** BLAKE3 copies | Blockwörter direkt in `output`, redundante Pre-Loop-/Block-Kopien entfernt | KAT-Vektoren `test_hash`, `test_journal` |
> | **C1** Footprint/Reentranz | drei `static` Recency-Scratch-Arrays → ein dokumentierter geteilter Puffer; Nicht-Reentranz dokumentiert | `test_mem_store` |
> | **C2** strlen-Dedup | `strlen(body)` einmal in `spg_mem_save` | `test_mem_store` |
>
> Bewusst nicht umgesetzt: **B5** (SIMD-BLAKE3) — laut Vorgehen erst nach
> Profiling; ohne gemessenen Bedarf nicht angefasst (skalarer Pfad unverändert
> korrekt). Die physische In-Place-Permutation in BLAKE3 (Teil von B4) wurde
> zugunsten der klaren Temp+Copy-Form belassen (Korrektheit vor Mikro-Gewinn);
> der substanzielle Teil von B4 (Struct-Kopien) ist umgesetzt.

---

Analyse der Hot-Paths (Stand: 2026-06-17). Befunde sind nach **Wirkung × Aufwand**
priorisiert. Jeder Punkt nennt Datei/Zeile, Ursache und konkreten Lösungsweg.
Randbedingungen aus `.agent/AGENT.md` gelten: keine `malloc`/`assert`,
allokationsfreie Hot-Paths, caller-bereitgestellte Puffer, sauber unter
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow` + ASan/UBSan, deterministisch.

Legende: **Wirkung** (hoch/mittel/niedrig) · **Aufwand** (S/M/L)

---

## A. Algorithmisch / I/O — größter Hebel

### A1 — Mind-Palace: Index wird pro Tick komplett von Platte neu gebaut · Wirkung: hoch · Aufwand: M
- **Wo:** [src/run/orchestrator.c:178-183](src/run/orchestrator.c) ruft `spg_mem_index()`
  in **jedem** Tick auf → [collect_by_recency](src/memory/mem_store.c:190) macht
  `opendir` + öffnet & parst die Frontmatter **jeder** Memory-Datei.
- **Problem:** Kosten sind `O(Schritte × Dateien)` Datei-Öffnungen, auch wenn sich der
  Speicher zwischen den Schritten nicht ändert. Im `improve`-Loop (Suite läuft mehrfach:
  Baseline + pro Lektion + final) multipliziert sich das zusätzlich.
- **Fix:** Index im `spg_mem_store` (oder im Workspace) cachen und nur bei `save`/`delete`
  invalidieren. Generationszähler/Dirty-Flag im Store, das `spg_mem_save`/`spg_mem_delete`
  hochzählen; `spg_mem_index` rendert nur neu, wenn die Generation sich geändert hat —
  sonst gepufferten String zurückgeben. Determinismus bleibt erhalten (gleiche Eingabe → gleicher Index).

### A2 — Mind-Palace: `spg_mem_save` scannt das Verzeichnis 2–3× vollständig · Wirkung: hoch · Aufwand: M
- **Wo:** [spg_mem_save](src/memory/mem_store.c:299) ruft nacheinander:
  - [count_memories](src/memory/mem_store.c:259) — `opendir`-Scan (nur bei neuem Slug),
  - [max_updated](src/memory/mem_store.c:227) — `opendir` + öffnet & parst **jede** Datei,
  - [regenerate_index](src/memory/mem_store.c:276) → [collect_by_recency](src/memory/mem_store.c:190) — `opendir` + öffnet & parst **jede** Datei **erneut**.
- **Problem:** Ein einzelnes Speichern liest die Frontmatter **aller** N Dateien **zweimal**
  (`max_updated` + `collect_by_recency`) plus bis zu drei Verzeichnis-Scans.
- **Fix:** `collect_by_recency` liest `updated` bereits für jede Datei → das Maximum in genau
  diesem einen Durchlauf mitberechnen und an `regenerate_index` zurückgeben, statt `max_updated`
  separat laufen zu lassen. `count_memories` durch die Länge desselben Scans ersetzen. Damit
  von 2 vollständigen Parse-Durchläufen auf 1 reduzieren.

### A3 — `read_meta` öffnet/liest jede Datei einzeln zeilenweise · Wirkung: mittel · Aufwand: S
- **Wo:** [read_meta](src/memory/mem_store.c:89) — `fopen` + `fgets`-Schleife bis Frontmatter-Ende, pro Datei.
- **Problem:** Für sich genommen ok, aber multipliziert mit A1/A2 ist die Zahl der `fopen`-Aufrufe
  der eigentliche Kostentreiber.
- **Fix:** Primär durch A1/A2 (Caching, ein Durchlauf) entschärfen. Sekundär: Schleife nach dem
  schließenden `---` sofort verlassen (geschieht bereits via `break`) — sicherstellen, dass nicht
  über das Frontmatter hinaus gelesen wird (passt aktuell).

---

## B. Konstanter Faktor auf dem Tick-Hot-Path

### B1 — Kontext-Rendering Byte-für-Byte statt blockweise · Wirkung: mittel · Aufwand: S
- **Wo:** [append_char](src/context/context.c:362), [append_cstr](src/context/context.c:377),
  [append_bytes](src/context/context.c:370). Genutzt von `spg_context_render`, das **pro Tick**
  läuft und mehrere KB erzeugt.
- **Problem:** Jedes Zeichen einzeln mit Branch (`used+1 < capacity`) — viele Verzweigungen,
  keine Vektorisierung, dutzende Aufrufe pro Node/Fact/Event.
- **Fix:** `append_cstr` einmal `strlen` + Bounds-Check + `memcpy` des ganzen Laufs; `append_bytes`
  analog per `memcpy`. `required`-Buchführung beibehalten (Länge addieren). Deutlich weniger
  Branches, Compiler kann den Kopiervorgang vektorisieren.

### B2 — `snprintf` für jede Ganzzahl im Renderer · Wirkung: mittel · Aufwand: S
- **Wo:** [append_u64](src/context/context.c:386)/[append_u32](src/context/context.c:395) und
  [spg_sexpr_writer_append_u64](src/dsl/sexpr.c:433)/[append_size](src/dsl/sexpr.c:444).
- **Problem:** `snprintf` parst bei jedem Aufruf den Formatstring; pro Node/Fact/Event/Budget
  fallen viele Aufrufe an.
- **Fix:** Kleine handgeschriebene Uint→Dezimal-Routine in einen Stackpuffer (rückwärts füllen,
  kein Format-Parsing), dann `memcpy`. Eine gemeinsame Helper-Funktion für beide Stellen.

### B3 — BLAKE3 `words_from_block`: Wörter byteweise zusammengeschoben · Wirkung: mittel · Aufwand: M
- **Wo:** [words_from_block](src/core/hash.c:42) — `memset(64)` + 64 Iterationen Shift/Or pro Block.
- **Problem:** Liegt auf dem Journal-Hot-Path: **jeder** Record (Header + Payload) wird beim
  Anhängen gehasht, beim Replay/Verify erneut. Pro 64-Byte-Block 64 Shift/Or-Schritte.
- **Fix:** Auf Little-Endian-Zielen volle 64-Byte-Blöcke direkt per `memcpy` in `words` laden
  (Teilblock am Ende separat, null-gepaddet). Mit `__BYTE_ORDER__`/Endianness-Check absichern,
  skalaren Fallback behalten (AGENT.md: SIMD/Plattform-Kernels mit skalarem Fallback).

### B4 — BLAKE3 `permute` mit Temp-Array + Rückkopie pro Runde · Wirkung: niedrig · Aufwand: S
- **Wo:** [permute](src/core/hash.c:75) — legt `permuted[16]` an und `memcpy`t zurück, 6×/Compress.
  Zusätzlich kopiert [chunk_output](src/core/hash.c:135) wiederholt die ganze `blake3_output`-Struct.
- **Fix:** Permutation in-place (bekanntes BLAKE3-Schema mit Registern/lokalen Variablen statt
  Temp + Kopie), redundante Struct-`memcpy`s in `chunk_output` zusammenfassen.

### B5 — BLAKE3 Compress ohne SIMD · Wirkung: mittel · Aufwand: L
- **Wo:** [compress](src/core/hash.c:83)/[round_fn](src/core/hash.c:63)/[g](src/core/hash.c:50).
- **Problem:** Rein skalar; bei großen Payloads/häufigem Hashing (Journal + Replay) der dominante
  Rechenanteil.
- **Fix:** SSE2/AVX (x86) bzw. NEON (ARM) Compress-Kernel hinter klarer Kernel-Grenze, skalarer
  Fallback bleibt. Gegen die vorhandene Referenz-Implementierung verifizieren (Tests in `test/`).
  Nur angehen, wenn Profiling Hashing als relevant ausweist.

---

## C. Speicher / Footprint & Reentranz

### C1 — Große statische Scratch-Puffer im Mind-Palace · Wirkung: niedrig · Aufwand: M
- **Wo:** `static struct mem_entry entries[SPG_MEM_MAX_FILES]` in
  [regenerate_index](src/memory/mem_store.c:287) **und** [spg_mem_index](src/memory/mem_store.c:413);
  `static char all[SPG_MEM_MAX_FILES][SLUGBUF]` in [spg_mem_list](src/memory/mem_store.c:477).
- **Problem:** `mem_entry` ≈ 330 B × 128 ≈ 42 KB, zweimal vorhanden + Slug-Array → ~85 KB BSS.
  Außerdem machen die `static`-Puffer die Funktionen nicht-reentrant (per Design einthreadig, aber
  undokumentiert).
- **Fix:** Auf constrained Targets einen gemeinsamen Scratch teilen oder (AGENT.md-konform) als
  caller-bereitgestellten Workspace übergeben. Mindestens die Nicht-Reentranz dokumentieren.

### C2 — Mehrfache `strlen(body)` in `spg_mem_save` · Wirkung: niedrig · Aufwand: S
- **Wo:** [spg_mem_save](src/memory/mem_store.c:308) und [:333](src/memory/mem_store.c:333) berechnen
  `strlen(body)` mehrfach.
- **Fix:** Einmal in eine lokale Variable, wiederverwenden.

### C3 — Insertion-Sorts O(n²) in der Kontext-Auswahl · Wirkung: niedrig · Aufwand: — (nur beobachten)
- **Wo:** [insert_graph_ref](src/context/context.c:105)/[insert_memory_ref](src/context/context.c:173)/
  [insert_recent_journal_ref](src/context/context.c:207).
- **Status:** Durch die kleine Top-K-Kapazität begrenzt → aktuell akzeptabel. Nur reagieren, falls
  die Kapazitäten wachsen (dann Heap-basiertes Top-K).

---

## Vorgehen / Reihenfolge

1. **Erst messen:** Mikro-Benchmark/Profiling über `eval`/`improve` mit gesetztem `--memory-dir`
   (treibt A1/A2/A3) und über `replay`/`verify-journal` (treibt B3–B5). Ohne Messung keine
   SIMD-Arbeit (B5) starten.
2. **Quick Wins zuerst:** A2, B1, B2, C2 (alle Aufwand S/M, klarer Gewinn, geringes Risiko).
3. **Dann A1** (Index-Cache) — größter algorithmischer Hebel, braucht Invalidierungs-Logik + Tests.
4. **Hash-Pfad** B3/B4, danach optional B5.
5. **Footprint** C1, falls ein constrained Target konkret wird.

**Pflicht bei jeder Änderung:** bestehende Tests in `test/` grün halten, Determinismus &
Byte-identischer Replay unverändert, neue Boundary-/Negativtests gemäß AGENT.md ergänzen.
</content>
</invoke>
