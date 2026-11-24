#pragma once
// Texture banks: several sets of textures time-sharing one region of GS memory (RC Revenge Pro's cars past the 8th,
// whose textures do not all fit in the 4 MB; NOTES.md "Car limits").
//
// The game loads each set into the shared region in turn and the host keeps a copy of the region after each load
// (GS::saveTextureBank). Before drawing with a set, the game's packet writes kGSTextureBankRegister (an unused GS
// register address) with the set's number; the frontend then puts that copy back into GS memory as an ordinary
// host-to-local transfer, in order with everything else, unless the region already holds that set. Any other transfer
// into the region forgets which set it holds.
#include <cstdint>

constexpr uint8_t kGSTextureBankRegister = 0x7Du; // 0x7E: gs_track_batch.h; 0x7F: the A+D no-op

// The register's value: the bank to load (0-based); bits 8 and up must be zero.
constexpr int kGSTextureBankCount = 32;

// The transfer that puts a bank back carries it in TRXPOS's source fields, which a host-to-local transfer does not use:
// SSAY kGSTextureBankTransferTag, SSAX the bank + 1. A backend that caches decoded textures can then keep each bank's
// apart (the HW backend keys the region's textures by bank) instead of decoding them again at every change.
constexpr uint16_t kGSTextureBankTransferTag = 0x7B5u;
