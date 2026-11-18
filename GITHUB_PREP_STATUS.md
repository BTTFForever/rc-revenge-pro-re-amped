# Preparing the repository for a private GitHub repo: status

Interrupted on 2026-11-19 for a reboot. Nothing has been pushed; no remote has been added. Delete this file once
everything below is done (before or just after the first push).

## Done
1. Deleted: `run.sh`, `tools/pgo.sh`, `tools/measure.sh`, `tools/fps_run.sh`, `patches/upstream_attempt_archive.tar.gz`,
   `decomp/recordings/test1.pad`. Kept because `NOTES.md` refers to them: `decomp/recordings/flicker_*.pad`, `test.pad`,
   `session1.pad`, `session2.pad`, `session1_memcard/` (ignored, never committed). `playtest-1262.pad` committed.
   References fixed; `tools/play.sh` now also plays without recording, `tools/level_sweep.sh` runs through it.
2. Committed the `RC_DEBUG_DUMP_MS` / `tools/fps.py --ms` change.
3. Vendored PS2Recomp in `third_party/ps2recomp/` (4.3 MB: ps2xRecomp, ps2xRuntime without vita/, ps2xIOP,
   ps2xTest/tools/vu1_recomp.cpp, LICENSE, README; `VENDORED.md`). Two commits: upstream d06a4ab unchanged, then the
   local changes (identical to the old patch file, which is removed). CMake uses it; `~/PS2Recomp-sotc` is no longer read.
4. `LICENSE` (GPL-3.0); third-party licences listed in `README.md`.
5. `tools/setup.sh` + `tools/iso_extract.py`; disc files in `game/<version>/`; configs relative; no `/home/...` path
   in any tracked file. Locally, `game/usa` and `game/eu` are symlinks to `../disc`, `../disc_eu` and the ISOs.
6. `.gitignore` covers game/, output*/, vu1*/, build*/, mc0/, mc1/, decomp/generated/ (except three tracked text files).
7. History scan (`git rev-list --objects --all` with sizes): no disc data, game assets, dumps or recompiled code ever
   committed. Largest blobs are `third_party/port_engine/.../smaa/AreaTex.h` (1.1 MB, SMAA lookup table, MIT), old
   versions of the runtime patch file and `NOTES.md`. `decomp/src/zlib_corpus.inc` is synthetic (tools/gen_zlib_corpus.py).
8. `README.md` written.
9. Clean clone, USA: `tools/setup.sh --oracle` from the .cue succeeded; build matches the working tree byte for byte;
   game boots into a race from `playtest2` at 50-59 fps (details in `NOTES.md`, "Clean-clone check").

## For the user to decide before pushing
- Commit author identity: earlier commits carry the author name and personal e-mail address configured in git
  (and 35 commits a GitHub noreply address). Rewriting that would change history; left alone.
- `NOTES.md` and `AGENT_USAGE.md` mention `~/...` tool paths and the hardware (RTX 2060, Vega) in passing.

## Remaining, with commands (from the repository folder)
Run each in a fresh clone, so the check is real:
```sh
C=/tmp/rcclone; rm -rf $C && git clone -q "$PWD" $C && cd $C
```
1. Europe setup from the .iso (the symlink path) and a boot:
   ```sh
   tools/setup.sh --oracle -j 6 "<path>/RC Revenge Pro (Europe).iso"
   timeout -s INT 120 env RC_VERSION=eu tools/play.sh replay playtest2   # look at build-eu/play/session.log
   ```
   (`playtest2` was recorded on USA; on Europe it only needs to boot and reach the menus.)
2. Oracle in the clean clone (each version; the dumps are game data and stay in `<oracle build>/dumps`):
   ```sh
   CAPTURE_BINARY=$PWD/build-decomp/rc_revenge_pro tools/capture_dumps.sh && tools/oracle.sh --all
   RC_VERSION=eu CAPTURE_BINARY=$PWD/build-eu/rc_revenge_pro tools/capture_dumps.sh && RC_VERSION=eu tools/oracle.sh --all
   ```
   Expect 0 FAIL (last results in the working tree: USA 1629 ok, EU 1560 ok). If the USA clean clone is gone,
   re-run `tools/setup.sh --oracle` with the USA image first (about 20 minutes).
3. Delete the clone (`rm -rf $C`); note the results in `NOTES.md` ("Clean-clone check") and in `AGENT_USAGE.md`.
4. Only if 1-2 passed: retire the old branch. `git log decomp..main --oneline` was empty on 2026-11-19 (old `main`
   ends at c94065e, an ancestor of `decomp`), so nothing is lost:
   ```sh
   git log decomp..main --oneline        # must still print nothing
   git branch -D main && git branch -m decomp main
   ```
   then update `NOTES.md` "Branches" to say there is one branch, `main`.
5. Delete this file, commit, and ask the user before creating the GitHub repository or pushing.
