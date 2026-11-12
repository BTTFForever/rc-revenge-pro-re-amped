# Delegated work log

Which model did what, how much it used, and how it went. Kept so Fable 5.2 usage stays in check (user, 2026-11-11).

| Date | Model | Task | Tokens | Tool calls | Time | Result |
|---|---|---|---|---|---|---|
| 2026-11-11 | Fable 5.2 | Collision units ACOL, CARCOL, TCOL (48 fn) | 414,763 reported for the resumed stint; the first stint (cut off by the spend limit) went unreported | 106 (resumed stint) | about 15 min, then 8 min | Good. Ported 45 of 48 functions plus 8 MEM/RES/TSO helpers it needed; 0 mismatches on 5 levels. Found a real bug in the random-input harness (inputs were silently skipped). Left 3 CARCOL functions recompiled for a sound reason. Independent re-run on level 17: 3,050 compared, 0 mismatches. Weakness: heavy token use. |
| 2026-11-11 | Sonnet 5.5 | Re-run random-input checks on the 95 earlier functions after the harness fix | 65,016 | 16 | 3 min | Good. Found 2 real bugs hidden by the old harness (ArcTan signed divide, PS2C_Vec16Normalise sign extension) with correct diagnoses; Opus fixed both, re-checked at 2000 inputs, 0 mismatches. About 1/6 of the tokens of the Fable task. |
| 2026-11-11 | Sonnet 5.5 | Re-run random tests on collision functions that used to be cut short, after the harness fixes | 40,045 | 6 | 4 min | Good. 26 functions, 0 mismatches, 0 cut short. Concise, correct report. |
| 2026-11-12 | Sonnet 5.5 | GPU experiment: track geometry in device-local buffers | 69,175 | 32 | 29 min | Negative result, well measured: device copies were slower (level 18 down about 7-8 fps), so PCIe reads are not the bottleneck. Kept as opt-in PS2X_GS_DEVICE_VERTS. Found that run.sh does not enable the HW renderer/track shader. |
| 2026-11-12 | Sonnet 5.5 | tools/asm_facts.py: per-function fact sheet from the instructions (addresses, gp globals, calls with constant args, frame, delay-slot stores) and whole-program warnings | 87,789 | 22 | 5.5 min | Good. Confirms every Ghidra label mix-up found by hand (_dma_1 = 5 addresses, _dma_48 and _dma_51 = 2 each); 194 warnings saved to decomp/generated/asm_warnings.txt. Heuristic label pairing, documented. |
