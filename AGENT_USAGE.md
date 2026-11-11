# Delegated work log

Which model did what, how much it used, and how it went. Kept so Fable 5.2 usage stays in check (user, 2026-11-11).

| Date | Model | Task | Tokens | Tool calls | Time | Result |
|---|---|---|---|---|---|---|
| 2026-11-11 | Fable 5.2 | Collision units ACOL, CARCOL, TCOL (48 fn) | 414,763 reported for the resumed stint; the first stint (cut off by the spend limit) went unreported | 106 (resumed stint) | about 15 min, then 8 min | Good. Ported 45 of 48 functions plus 8 MEM/RES/TSO helpers it needed; 0 mismatches on 5 levels. Found a real bug in the random-input harness (inputs were silently skipped). Left 3 CARCOL functions recompiled for a sound reason. Independent re-run on level 17: 3,050 compared, 0 mismatches. Weakness: heavy token use. |
