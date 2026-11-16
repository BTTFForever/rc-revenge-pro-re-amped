// Inputs for the random test of SFX.c and REVSFX.c. Each has the signature RC_DECOMP_FN_SETUP expects.
//
// Offline the IOP is replaced (quietSound in the .cpp): sceSifCallRpc, which sceSdRemote calls, is a fake that logs
// the sdrdrv command and a hash of the argument words the command uses (or of the effect attributes 0x8130 sends),
// and answers from a script seeded by the setup (the ENDX voice masks 0x8040 reads, the addresses 0x8060 reads, and
// failures of 0x8000 and the transfer 0x80d0), writing the reply as the IOP does: the answer and three zero words over
// the first four words of sbuff. sceSdRemote copies six argument registers into sbuff whatever the command; the words
// the command does not use hold stale registers (the original's temporaries, or its caller's), which the IOP never
// reads: the fake sets them to 0 so both runs leave the same buffer. sceSdTransToIOP is logged with a hash of the
// bytes it sends (the volume and pitch batches), and the IOP heap, the remote's initialisation, FlushCache and
// printf are logged fakes with chosen answers. Timer 0 is stopped at a chosen count.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

// SFX
void sfxInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a mode; the IOP heap's answer
void sfxIop(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // no arguments; the IOP's answers
void sfxReverb(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; the reverb flags
void sfxReverbDepth(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a depth; the reverb flags
void sfxTransEffect(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 a VAG, a1 an image, a2 an id
void sfxVoices(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // no arguments; voices and plays
void sfxPlay(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0-t2 a sample, volume, pans, pitch...
void sfxPlayId(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 a play id
void sfxPlayIdByte(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // a0 a play id, a1 a value
void sfxVagDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a handle to a VAG
void sfxVagTail(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a VAG, a1 its size
void sfxByte(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 a value; sound on or off
void sfxPause(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 flags
void sfxStoreAddress(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a count, a1 a type
void sfxClearSram(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 an effect
void sfxVoiceId(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a voice
void sfxLoopAddress(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 an effect, a1 a VAG
// REVSFX
void revPlay3D(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0-t1 a sound, volume, pitch, place...
void revStopAll(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 flags or a mode (whole RAM)
void revInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // no arguments (whole RAM)
void revAmbient(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 split; the ambient sounds
void revPauseFade(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 flags; the fade and the volumes
void revReverb(R5900Context *ctx, uint64_t &state, uint32_t scratch);        // a0 split; the level and reverb level
void revSfxDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);     // a0 a handle to an SFX resource
void revSetupClass(R5900Context *ctx, uint64_t &state, uint32_t scratch);    // no arguments; the level's SFX data
void revTrigger(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 a trigger; the game timer
void revSpecialTrigger(R5900Context *ctx, uint64_t &state, uint32_t scratch); // a0 an ambient sound, a1 a place, a2 a time
void revAnim(R5900Context *ctx, uint64_t &state, uint32_t scratch);          // a0 an animation head, a1 flags
void revTso(R5900Context *ctx, uint64_t &state, uint32_t scratch);           // a0 an ambient sound and flags
void revAmbientList(R5900Context *ctx, uint64_t &state, uint32_t scratch);   // a0 flags; the ambient list
void revModifyVolume(R5900Context *ctx, uint64_t &state, uint32_t scratch);  // a0 a flag; the pause timer
void revReadyGo(R5900Context *ctx, uint64_t &state, uint32_t scratch);       // a0 frames, a1 a percentage
void revDolby(R5900Context *ctx, uint64_t &state, uint32_t scratch);         // a0 a mode, a1 flags

} // namespace rc::fuzz
