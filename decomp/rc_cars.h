// More cars in a race (NOTES.md "Car limits"): up to kMaxCars instead of the PS2's 8 (single player) and 6 (two players).
//
// The game's own Game Setup page already has "Single race Number of Cars" (2-8) and "Multi-player Number of Cars" (2-6);
// their ranges go up to kMaxCars here. The chosen counts are kept in settings.ini (cars_single, cars_multi; RC_CARS_SINGLE,
// RC_CARS_MULTI for one run). While a count has never been set there (0), the memory card's value is used, as on the PS2;
// the memory card itself only ever gets values the PS2 game accepts (at most 8 and 6), so a save stays loadable there.
//
// The tables the game sized for 8 cars live in this extension block of guest RAM instead (the top of the gap between the
// executable and the game's heap, which the runtime's own allocations no longer reach): the race order (aiVarG.raceOrder),
// the cup's car list (mainVarG.cupCarList) and the championship points kept between races (mainVarG.champPoints), the
// start grid with rows added behind the level's 8 places, and the results and cup ranking menus' extra lines.
#pragma once

#include <cstdint>

namespace rc::cars {

inline constexpr int kMaxCars = 12;       // the tables below (sized for 12; races use at most kMaxRace)
// The largest race offered (2026-11-25: back to 8 for single player, as on the PS2; two players raised from 6 to 8).
// The 9-12 car code (added grid rows, places 9-12, texture banks) stays but is not reached while this is 8.
inline constexpr int kMaxRace = 8;
inline constexpr int kPs2Single = 8, kPs2Multi = 6; // the PS2's ranges' tops (what the memory card keeps at most)

// The extension block: [kGuestBase, kGuestEnd) of guest RAM, below the game's heap at 0x500000.
inline constexpr uint32_t kGuestBase = 0x004F8000u, kGuestEnd = 0x00500000u;
inline constexpr uint32_t kRaceOrder = kGuestBase;                       // _sRaceOrder[kMaxCars], 12 bytes each
inline constexpr uint32_t kRaceOrderStride = 12u;
inline constexpr int kCupListSize = kMaxCars + 4;    // the computer's cars are taken from it skipping the players' (two)
inline constexpr uint32_t kCupCarList = kRaceOrder + 12u * kMaxCars;     // eObjectType[kCupListSize]
inline constexpr uint32_t kChampCars = kCupCarList + 4u * kCupListSize;  // eObjectType[kMaxCars] (champPoints.carList)
inline constexpr uint32_t kChampPoints = kChampCars + 4u * kMaxCars;     // uint8[kMaxCars] (champPoints.carPoints)
inline constexpr uint32_t kStartGrid = kChampPoints + kMaxCars;          // SVECTOR[kMaxCars]: the level's 8, then the added rows
inline constexpr uint32_t kMenuLines = (kStartGrid + 8u * kMaxCars + 0xfu) & ~0xfu; // the menus' added lines (rc_cars.cpp)
inline constexpr uint32_t kRaceOrderEntry(int i) { return kRaceOrder + kRaceOrderStride * static_cast<uint32_t>(i); }

// The counts a race starts with (the settings', else the memory card's value).
int singleCount(int cardValue);
int multiCount(int cardValue);
// A championship's cars: the cup's 8, or the single race count when that is more.
int cupCount();

// Game Setup: on entering the page, the two items show the counts and their range reaches kMaxCars; on OK, the values
// chosen are kept (settings.ini) and the memory card's copy gets them clamped to the PS2's ranges.
void entrySettings(uint32_t menuItem);   // an item of the page (AMenuEntrySettingsMVAR), by its selection tag
void chosen(int single, int multi, int32_t &cardSingle, int32_t &cardMulti);

// The start grid for count cars: the level's table when count <= 8, else a copy with rows added behind it along the
// AI's route (called when the cars are made).
uint32_t startGrid(int count);
// The heading (ObjectCreateCars' units) of an added place (8 and on) of the last startGrid; false for the level's own.
bool addedPlaceHeading(int place, int16_t &heading);

// Points for a finishing place (the game's table has 8: 9, 7, ..., 1); 0 from 9th.
uint8_t racePoints(int place);

// The results and cup ranking menus: lines added for places 9 to the race's count (each race's level file brings 8).
void extendResultMenus();

// RC_PLACE_DUMP=<dir> (rc_place_glyphs.cpp): the level's place sprites as images, once a level (HudTopRight).
void dumpPlaceSprites(uint32_t res);

// The position sprite's texture for a place 0..7 (the level's), or 0 from 9th (none: callers draw text instead).
uint32_t placeSprite(uint32_t res, int place);

// GS texture memory (the game's allocator, nextTexAddr up to 0x400000; the cars' models load last): whether car `place`
// (CarListConstructDaemon's order) fits; always for the first 8 and a type already loaded. Else the race goes without it
// and the cars after it (dropCars: noAI and aiVarG.noAIPlayers lowered, logged); restoreCount, at each race's start,
// gives the next level the chosen count again, until the front end or the Track Editor sets one (forgetCount).
bool roomForCar(int place);
uint32_t textureCost(int place); // the GS memory car `place`'s type is expected to take (measured, else a reserve)
void dropCars(int keep);
void restoreCount();
void forgetCount();

// Texture banks (rc_car_textures.cpp): with roomForCar's limit lifted, the car types past the 8th that do not fit share
// one region of GS memory, each loaded into it in turn and kept by the host; the GS frontend copies a car's set back
// into the region before anything drawn with it (a marker in the DMA chain). RC_CAR_TEXTURE_BANKS=0: as before (cars
// that do not fit are left out).
inline constexpr int kMaxBanks = kMaxCars; // past the 8th in a race; all of them with RC_CAR_BANK_TEST
inline constexpr uint32_t kBankPackets = 0x004FF000u; // the marker packets, 2 x 0x90 bytes per bank
enum class CarTextures { Resident, Banked, Drop };
CarTextures carTextures(int place, int total); // before car `place` (CarListConstructDaemon's order) is loaded
bool carLoaded(int place);          // after it: false if a banked car's textures did not fit after all (it is left out)
void texturesReset();               // GS texture memory starts again (GfxTex_Init)
bool bankUpload(uint32_t address, uint32_t size); // GfxTex_ManageTexture: false if the upload would pass the end
void bankTexture(uint32_t texture); // a texture placed while a bank loads
void bankShape(uint32_t shape);     // Rdr_FixVUShape: a shape fixed while a bank loads belongs to it
int shapeBank(uint32_t shape);      // its bank, or -1
void useBank(int bank);             // before drawing with a bank's textures: the marker, unless it was the last asked
void texturesFrameStart();          // Rdr_StartFrame
// The sorted transparent triangles (RdrAlphaAddTriangles, RdrAlphaAddToDmaChain): each of a banked shape is noted with
// its bank; the chain asks for the bank when it changes (alphaMarker: the packet to call); alphaDone after each chain.
void noteAlpha(uint32_t triangle, int bank);
int alphaBank(uint32_t triangle);
uint32_t alphaMarker(int bank);
int lastBank();
void alphaDone();

// The cars the race needs beyond the cup's 8 (MainSetCars 3): further different cars.
void fillCupList(int count);

} // namespace rc::cars
