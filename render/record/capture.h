#pragma once

// Live capture (RENDERER_FRAME_RECORD.md sections 3 and 6): implements the runtime's capture hooks
// (third_party/ps2recomp/ps2xRuntime/include/ps2_capture_hooks.h), cuts the hardware traffic into display cycles,
// runs the page shadow and fans the events out to consumers (the file writer). Linked into the game only; it installs
// itself from a static initializer when RC_FRAME_RECORD is set, so an unset variable leaves the runtime untouched.
//
//   RC_FRAME_RECORD=<file>          write a dump (".rcfr"; "%d" is replaced by the dump's number, 0)
//   RC_FRAME_RECORD_AT=<trigger>    when (default boot+120; count in display cycles, default 120):
//       boot[+count]                from the first kick
//       cycle:<n>[+count]           display cycles since boot
//       game:<n>[+count]            game frames since boot (the GfxP2_SwapBuffers override's counter)
//       level:<n>[+count]           game frames since the last level start (as RC_RAMDUMP_FRAME counts; only after a
//                                   level has started)
//       scene:<hex>[+count]         the first cycle with that GS-survey scene id
//   RC_FRAME_RECORD_KEY=<n>         cycles per segment (a keyframe every n cycles; default 300)
//
// Batch 1 records in plain mode only (no reference pictures, register files or GIF hashes; batch 2) and has no
// ring, key or state-load handling (batch 4).

namespace rc::rec
{
    // True when RC_FRAME_RECORD installed the hooks.
    bool captureInstalled();
}
