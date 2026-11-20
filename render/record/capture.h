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
//   RC_FRAME_RECORD_REF=<n>|hash|off  exact mode: a stored reference picture every n-th cycle (default 1), only its
//                                   hash, or neither
//   RC_FRAME_RECORD_STRICT=1        stop the run when exact mode is not possible
//
// Exact mode (batch 2; tools/frame_record.sh) is on when the GS is the bare software rasteriser with VU1 and the GIF
// on the EE thread (ps2x::capture::exactCapable) and RC_TRACK_SHADER is not 1: then each KICK carries the GS register
// file before it and the hash of the GIF bytes it produced, each END the hash of the picture at DISPFB2 (and REFI the
// picture), and keyframes exact GS memory. Every keyframe carries EMUS (ps2x::savestate::saveGraphics) in both modes.
// Side tags (batch 3; render/gfx/tags.h): while installed, the capture is the tag channel's consumer (RC_FRAME_RECORD_TAGS=0
// leaves the channel detached). Tags go into the dump as TAG records in emission order; an anchored tag is resolved at
// the first kick whose stream reaches its anchor (KICK spans), kick spans open at a kick become its labels; anchored tags
// no kick consumed are dropped two cycle closes after they were made; each keyframe carries the ones still waiting
// (TAGP), and the file the name table (TNAM).
// No ring, key or state-load handling yet (batch 4).
// (Step 7 batch 5) The capture is also installed, with no recording, when a live consumer is added (addLiveConsumer:
// RC_RENDERER=vk RC_VK_PICTURE=native's frame reader); live consumers see the events whether a dump is recorded or not.

namespace rc::rec
{
    struct Consumer;

    // True when RC_FRAME_RECORD installed the hooks.
    bool captureInstalled();

    // (step 7 batch 5) Installs the hooks if RC_FRAME_RECORD has not, and adds a live consumer: from boot on, every hw,
    // crt and kick event (guest RAM in place) and every cycle begin and end on the EE thread, recording or not, and after
    // a save-state load a keyframe of the loaded machine (kKeyAfterStateLoad). Must be called before main (a static
    // initializer). The consumer lives for the rest of the run.
    void addLiveConsumer(Consumer *consumer);
}
