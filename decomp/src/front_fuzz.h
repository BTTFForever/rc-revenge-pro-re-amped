// Inputs for the random test of FRONT.c, CAROUSEL.c and CREDITS.c (front_fuzz.cpp). Each has the signature RC_DECOMP_FN_SETUP expects.
#pragma once

#include "rc_decomp.h"

namespace rc::fuzz {

void frontAnimGroup(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontBanner(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCameraDestination(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarMenu(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarousel(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarouselInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarouselSetUp(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarToTag(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCarType(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCheatKeys(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCheatResults(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontChopItem(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontConfirm(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCredits(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontCupAvailability(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontDirectionMenu(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontDraw(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontEffects(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontEndInit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontEndProcess(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontEndTerm(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontItemDisabler(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontKeyDefinitions(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontLevelAvailability(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontMemoryCard(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontMenuScan(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontParse(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontRemapDestTags(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontRewardMessage(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontRewardTable(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontSelectionTest(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontSetRewards(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontSetupClass(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontSetUpVehicle(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontSfx(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontStatsDaemon(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontStatsBar(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontStubbed(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontTag(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontTask(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontTimeTrialExit(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontTimeTrialTime(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontVehicleStats(R5900Context *ctx, uint64_t &state, uint32_t scratch);
void frontWorld(R5900Context *ctx, uint64_t &state, uint32_t scratch);

} // namespace rc::fuzz
