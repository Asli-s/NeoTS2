// The internals of the SimAntics virtual machine, shared by vm.cpp and the vm_*.cpp files that
// implement its primitives by group. The VM's interface is in vm.hpp.
#pragma once

#include "sim/vm.hpp"
#include "content/animation_lists.hpp"
#include "diag/log.hpp"
#include "sim/advertisement.hpp"
#include "sim/clock.hpp"
#include "sim/find_best_object.hpp"
#include "sim/in_view.hpp"
#include "sim/lua_runtime.hpp"
#include "sim/object_functions.hpp"
#include "sim/motive_update.hpp"
#include "sim/object_reset.hpp"
#include "sim/pie_menu.hpp"
#include "sim/route_goals.hpp"
#include "sim/find_location.hpp"
#include "sim/snap.hpp"
#include "sim/walk_sweep.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace ts2::sim::vm_detail {


constexpr std::uint16_t kSleepOpcode = 0x0000;
constexpr std::uint16_t kGenericSimsCallOpcode = 0x0001;
constexpr std::uint16_t kRandomNumberOpcode = 0x0008;
constexpr std::uint16_t kBreakPointOpcode = 0x000F;
constexpr std::uint16_t kFindLocationForOpcode = 0x0010;
constexpr std::uint16_t kSnapOpcode = 0x002E;
constexpr std::uint16_t kSnapIntoOpcode = 0x0071;
constexpr std::uint16_t kReachPutOpcode = 0x0074;
// Animation list 0x80 names a clip by the Sim's age (see specialAnimationName).
constexpr std::uint8_t kSpecialAnimationList = 0x80;
constexpr std::uint16_t kSetToNextOpcode = 0x001F;
constexpr std::uint16_t kSetMotiveChangeOpcode = 0x001D;
constexpr std::uint16_t kWantSatisfyOpcode = 0x007C;
constexpr std::uint16_t kOnTimerOpcode = 0x007A;
constexpr std::uint16_t kMessageOpcode = 0x0077;
constexpr std::uint16_t kGetDirectionToOpcode = 0x000C;
constexpr std::uint16_t kTurnBodyTowardsOpcode = 0x0016;
constexpr std::uint16_t kGoToRoutingSlotOpcode = 0x002D;
constexpr std::uint16_t kGoToRelativePositionOpcode = 0x001B;
constexpr std::size_t kRouteResultPersonData = 0x3E;
// What a planned route leaves in the Sim's person data: the slot's owner, the slot as named (1000 and
// up for a global slot), and the joint of the slot whose spot was reached (-1 for none).
constexpr std::size_t kRouteOwnerPersonData = 0x28, kRouteSlotPersonData = 0x25, kRouteJointPersonData = 0x26;
// The route result when the post-route tree returns false.
constexpr std::int16_t kPostRouteFailed = 12;
// Route stretches shorter than this don't turn a walking Sim.
constexpr float kWalkSliver = 0.01f;
// Facings closer than this (radians) need no turn. Route points closer than this (tiles) are one.
constexpr float kSameHeading = 1e-4f;
constexpr float kSamePoint = 1e-4f;
constexpr std::size_t kGenderPersonData = 0x41;
constexpr std::uint16_t kGetDistanceToOpcode = 0x000B;
constexpr std::uint16_t kPushInteractionOpcode = 0x000D;
constexpr std::uint16_t kAlterBudgetOpcode = 0x0019;
constexpr std::uint16_t kIdleForInputOpcode = 0x0011;
constexpr std::uint16_t kNotifyOutOfIdleOpcode = 0x0031;
constexpr std::uint16_t kActionStringOpcode = 0x0032;
constexpr std::uint16_t kManageInventoryOpcode = 0x0033;
constexpr std::uint16_t kLuaScriptOpcode = 0x007E;
constexpr std::uint16_t kAnimateObjectOpcode = 0x0069;
constexpr std::uint16_t kAnimateSimOpcode = 0x006A;
constexpr std::uint16_t kAnimateOverlayOpcode = 0x006B;
constexpr std::uint16_t kAnimateStopOpcode = 0x006C;
constexpr std::uint16_t kLookAtOpcode = 0x006E;
constexpr std::uint16_t kStopAllSoundsOpcode = 0x0030;
constexpr std::uint16_t kAssignLocomotionOpcode = 0x0072;
constexpr std::uint16_t kDebugOpcode = 0x0073;
constexpr std::uint16_t kEffectOpcode = 0x0070;
constexpr std::uint16_t kCinematicOpcode = 0x007B;
constexpr std::uint8_t kCinematicIndexInVariable = 0x10;
constexpr std::uint16_t kChangeLightOpcode = 0x006F;
constexpr std::uint16_t kLightNameTable = 0x8E;
constexpr std::uint16_t kFindBestObjectOpcode = 0x000E;
constexpr std::uint16_t kFindBestInteractionOpcode = 0x0003;
constexpr std::uint16_t kGosubFoundActionOpcode = 0x001E;
constexpr std::uint16_t kRunFunctionalTreeOpcode = 0x0014;
constexpr std::uint16_t kActionStringTable = 0x12E;
constexpr std::size_t kDirectionField = 0x01;
// A placed object takes diagonal facings only with this definition field 0 and object data under 2.
constexpr std::uint16_t kNoDiagonalFacingField = 0x0A;
constexpr std::size_t kFacingRuleField = 0x17;
// Object data whose bit 0x40 Idle for Input sets while interactions are waiting.
constexpr std::size_t kIdleFlagsField = 0x08;
constexpr std::int16_t kInteractionsWaitingBit = 0x40;
// Where Push Interaction reports why it failed (object data "primitive result").
constexpr std::size_t kPrimitiveResultField = 0x34;
constexpr std::size_t kPriorityPersonData = 0x21;
constexpr std::size_t kInteractionAutonomousPersonData = 0xA6;
// Holds back interruptions (see interruptPending). Starting an interaction clears it, and what
// sets it is not known yet.
constexpr std::size_t kHoldInterruptsPersonData = 0x47;
// "Interactions Linked Deleted": 1 once an interaction linked with the running one has ended (see
// dropLinkedInteractions), 0 again when an interaction starts.
constexpr std::size_t kLinkedDeletedPersonData = 0x6E;
// Autonomy's choice: the object (person data) and the interaction number (object data).
constexpr std::size_t kBestActionObjectPersonData = 0x35;
constexpr std::size_t kTreeTableEntryField = 0x14;
// How many interactions and sub-interactions may wait for Push Interaction to add another.
constexpr std::size_t kMaxQueuedInteractions = 7;
constexpr std::size_t kMaxQueuedSubInteractions = 5;
// The Sim's semi-attribute 0x22, "Carry Flags": not 0 while it carries something.
constexpr std::size_t kCarryFlagsSemiAttribute = 0x22;
// Continuations skip the game's limits. A safety limit of ours keeps a pushing loop from
// growing the queue without end.
constexpr std::size_t kMaxQueuedWithContinuations = 64;
// Interaction table line flag: the queue shows the interaction nested.
constexpr std::uint32_t kNestedInteractionFlag = 0x8000;
// Interaction table line flag: it runs at once instead of waiting in the queue.
constexpr std::uint32_t kRunAtOnceInteractionFlag = 0x4;
// The first waiting interaction, as the game shows it to scripts ("cn: Next Queued Interaction ...").
constexpr std::size_t kNextQueuedIndexPersonData = 0x2D;
constexpr std::size_t kNextQueuedContinuationPersonData = 0x48;
constexpr std::size_t kNextQueuedObjectPersonData = 0x4F;
// The interaction running on my stack ("ci: Current ..."), and the "Running" pair, which names the
// running sub-interaction while there is one (see noteSubQueue).
constexpr std::size_t kCurrentIndexPersonData = 0x15;
constexpr std::size_t kCurrentObjectPersonData = 0x50;
constexpr std::size_t kRunningIndexPersonData = 0x66;
constexpr std::size_t kRunningObjectPersonData = 0x67;
// The object of the running sub-interaction ("ci: Sub Queue ..."), 0 once it ends.
constexpr std::size_t kSubQueueObjectPersonData = 0x9D;
// The sub-queue as scripts see it (see noteSubQueue).
constexpr std::size_t kSubQueueCountPersonData = 0x1A;
constexpr std::size_t kSubQueueMasterObjectPersonData = 0x4B;
constexpr std::size_t kSubQueueMasterIndexPersonData = 0x4C;
constexpr std::size_t kNextSubIndexPersonData = 0x4D;
constexpr std::size_t kNextSubObjectPersonData = 0x4E;
// Global 0x23: parental control is on.
constexpr std::size_t kParentalControlGlobal = 0x23;
// A level counts as this many tiles in distances.
constexpr float kLevelHeight = 10.0f;
// OBJD field a category want compares with an object's.
constexpr std::size_t kWantCategoryField = 0x2F;
// Set Motive Change rates are per Sim hour. The game keeps them per tick (1,800 ticks an hour).
constexpr float kTicksPerSimHour = 1800.0f;
constexpr std::uint16_t kCreateObjectOpcode = 0x002A;
constexpr std::uint16_t kRunTreeByNameOpcode = 0x001C;
constexpr std::uint16_t kRelationshipOpcode = 0x001A;
constexpr std::uint32_t kTestOwnTypeGuid = 0x4C7CAB2B;
constexpr std::uint16_t kRemoveObjectOpcode = 0x0012;
constexpr std::uint16_t kMakeNewCharacterOpcode = 0x0013;
constexpr int kYoungAdultAgeFlag = 0x40;
constexpr std::size_t kNewAgePersonData = 0x3A, kYoungAdultPersonData = 0xAF;
// Make New Character flags (byte 9).
constexpr std::uint8_t kNewCharacterChild = 0x01;         // a child of two parents
constexpr std::uint8_t kNewCharacterParent1Neighbor = 0x02;  // parent 1 is a neighbor id, else an object
constexpr std::uint8_t kNewCharacterParent2Neighbor = 0x04;
constexpr std::uint8_t kNewCharacterTemplateInTemps = 0x08;
constexpr std::uint8_t kNewCharacterTemplateFromToken = 0x10;
constexpr std::uint8_t kRemoveReturnImmediately = 0x01;
// Objects the game refuses to remove (a fixed list).
constexpr std::uint32_t kProtectedGuids[] = {
    0xCC7B08F8, 0x6C7B0905, 0xCC203477, 0x865A6812, 0x81E6BEF9, 0xD20C0AEE, 0x39CCF441,
};
constexpr std::uint8_t kTestOriginalGuid = 0x01;
constexpr std::uint8_t kTestNeighborId = 0x02;
constexpr std::size_t kOriginalGuidLow = 0x46;
constexpr std::size_t kOriginalGuidHigh = 0x47;
constexpr std::uint8_t kTestGuidExists = 0x04;
constexpr std::uint8_t kRelationshipFailIfMissing = 0x01;
constexpr std::uint8_t kRelationshipNeighborIds = 0x02;
constexpr std::uint8_t kRelationshipWrite = 0x04;
constexpr std::uint8_t kRelationshipOnlyA = 0x08;
constexpr std::uint16_t kRoomOutside = 1;
constexpr std::uint16_t kRoomLevel = 2;
constexpr std::uint16_t kRoomArea = 3;
constexpr std::uint16_t kRoomPool = 4;
constexpr std::uint16_t kRoomLayer1 = 7;
constexpr std::uint8_t kRunInMyStack = 0x00;
constexpr std::uint8_t kRunInStackObjectsStack = 0x01;
constexpr std::uint8_t kPushOntoMyStack = 0x02;
constexpr std::uint8_t kRunTypeTreeFromTemps = 0x03;
constexpr std::uint16_t kTreeNameTable = 0x12F;

constexpr std::uint8_t kPlaceOnTopOfMe = 0x01;
constexpr std::uint8_t kPlaceInMyHand = 0x02;
constexpr std::uint8_t kPlaceInFrontOfStackObject = 0x03;
constexpr std::uint8_t kPlaceInStackObjectSlot = 0x04;
constexpr std::uint8_t kPlaceInSlotOfObjectInTemp0 = 0x0A;
constexpr std::uint8_t kPlaceUnderneathMe = 0x05;
constexpr std::uint8_t kPlaceOutOfWorld = 0x06;
constexpr std::uint8_t kPlaceBesideMe = 0x09;
constexpr std::uint8_t kPlaceBelowObjectInParam0 = 0x07;
constexpr std::uint8_t kPlaceBelowObjectInLocal = 0x08;
constexpr std::uint8_t kCreateDoNotDuplicate = 0x01;
constexpr std::uint8_t kCreatePassObjectIds = 0x02;
constexpr std::uint8_t kCreateFromNeighbor = 0x04;
constexpr std::uint8_t kCreateFailIfTileOccupied = 0x08;
constexpr std::uint8_t kCreatePassTemp0 = 0x10;
constexpr std::uint8_t kCreateFromInventoryToken = 0x40;
constexpr std::uint8_t kCreateGuidFromTemps = 0x80;
constexpr std::uint8_t kCreateCopyMaterials = 0x02;
constexpr std::uint16_t kTemplateObjectType = 0x10;
constexpr std::uint16_t kNotCreatableObjectType = 0x11;
constexpr std::uint16_t kTestObjectTypeOpcode = 0x0020;
constexpr std::uint16_t kArrayOperationOpcode = 0x0076;
constexpr std::uint16_t kRefreshOpcode = 0x0007;
constexpr std::uint16_t kFindWorstMotivesOpcode = 0x0021;
constexpr std::uint16_t kTestSimInteractingWithOpcode = 0x0025;
constexpr std::uint16_t kFollowSimOpcode = 0x007D;
constexpr std::uint16_t kChangeMaterialOpcode = 0x006D;
constexpr std::uint16_t kSoundEventOpcode = 0x0017;
constexpr std::uint16_t kRayTraceOpcode = 0x0078;
constexpr std::uint16_t kChangeOutfitOpcode = 0x0079;
constexpr std::uint16_t kDialogOpcode = 0x0024;
constexpr std::uint16_t kCameraControlOpcode = 0x0023;
// Dialog types (byte 5).
constexpr std::uint8_t kTextNotification = 0x08, kStopNotification = 0x09, kModifyNotification = 0x0A,
                       kAppendNotification = 0x1C, kTutorialTasks = 0x12, kTutorialNext = 0x16, kTutorialNextModal = 0x19,
                       kHint = 0x39, kSetBabyName = 0x17;
// Pickers, which list the stack object's object arrays.
constexpr std::uint8_t kPickerDialogs[] = {0x05, 0x06, 0x07, 0x0B, 0x0C, 0x0D, 0x0F, 0x21, 0x33, 0x34, 0x46};
constexpr std::size_t kContainerData = 2;    // object data: the object it is held by
constexpr int kWindowObjectType = 9;          // OBJD type
constexpr std::uint16_t kSoundNameTable = 0x4132;
constexpr std::uint8_t kSoundFromStackObject = 0x02;  // byte 4
constexpr std::uint8_t kSoundStop = 0x40;
constexpr std::size_t kHiddenFlags = 0x22;            // object data, bit 0x02 silences the object
constexpr std::uint16_t kMaterialNameTable = 0x88;
constexpr std::uint16_t kMeshGroupNameTable = 0x87;
// Change Material flags (byte 2).
constexpr std::uint8_t kMeshGroupFromSource = 0x01;
constexpr std::uint8_t kMaterialGlobal = 0x02;
constexpr std::uint8_t kMaterialSemiGlobal = 0x04;
constexpr std::uint8_t kMaterialFromSource = 0x08;
constexpr std::uint8_t kMaterialIndexInTemp0 = 0x10;
constexpr std::uint8_t kMeshGroupIndexInTemp1 = 0x20;
constexpr std::uint8_t kMeshGroupGlobal = 0x40;
constexpr std::uint8_t kMeshGroupSemiGlobal = 0x80;
constexpr std::uint8_t kMaterialIsTvChannel = 0x01;  // byte 13
// The motives Find 5 Worst Motives sorts, whatever its search type: energy, comfort, hunger, hygiene,
// bladder, environment, social, fun.
constexpr std::array<std::uint8_t, 8> kWorstMotiveCandidates{5, 6, 7, 8, 9, 0x0D, 0x0E, 0x0F};
constexpr std::size_t kWorstMotiveCount = 5;
// Array Operation operations (byte 1). Sorting: 0x0C puts the highest first, 0x0D the lowest first.
constexpr std::uint8_t kArrayClear = 0;
constexpr std::uint8_t kArraySize = 1;
constexpr std::uint8_t kArrayResize = 2;
constexpr std::uint8_t kArrayFill = 3;
constexpr std::uint8_t kArrayInsertFront = 4;
constexpr std::uint8_t kArrayInsertBack = 5;
constexpr std::uint8_t kArrayInsertAtIterator = 6;
constexpr std::uint8_t kArrayRemoveFront = 7;
constexpr std::uint8_t kArrayRemoveBack = 8;
constexpr std::uint8_t kArrayRemoveAtIterator = 9;
constexpr std::uint8_t kArrayFind = 10;
constexpr std::uint8_t kArraySwap = 11;
constexpr std::uint8_t kArraySortDescending = 12;
constexpr std::uint8_t kArraySortAscending = 13;

// Generic Sims Calls with no effect on simulation state: camera, UI, preloading and 3D sync. 0x1C
// floats the money a transaction moved (temp 0 x temp 2) over the object in temp 1 (UI message 0xD6).
constexpr std::uint8_t kNoOpGenericCalls[] = {
    0x01, 0x02, 0x03, 0x12, 0x18, 0x1C, 0x1D, 0x1E, 0x1F, 0x28, 0x29, 0x2A, 0x2C, 0x33,
};
// Generic call 0x21 marks the stack object's route footprint as changed. The route world updates
// it before it is next used. True.
constexpr std::uint8_t kUpdateFootprintGenericCall = 0x21;

// Generic call 0x41 starts the global "Init - Object" tree. The game sets the object's default
// data itself and returns true, skipping the tree's own (older) steps (see initObjectDefaults).
constexpr std::uint8_t kInitObjectGenericCall = 0x41;
constexpr std::uint8_t kOkToIdleHereGenericCall = 0x2B;
constexpr std::uint8_t kWallInFrontGenericCall = 0x34;
// Generic call 0x2F clears a Sim's hair override (what its hair shows under a hat). Always true.
constexpr std::uint8_t kClearHairOverrideGenericCall = 0x2F;
// Generic call 0x3C sets a Sim's skin: with temp 0 = 0 to the Sim's own, which it already wears, so
// nothing changes and it is true. Other skins are named by a text resource (not kept yet).
constexpr std::uint8_t kSetSkinGenericCall = 0x3C;
constexpr std::uint8_t kAddToFamilyGenericCall = 0x04;
// Generic calls 0x0A and 0x0B turn Build and Buy mode off and back on for the object running them.
// Always true.
constexpr std::uint8_t kDisableBuildBuyGenericCall = 0x0A;
constexpr std::uint8_t kEnableBuildBuyGenericCall = 0x0B;
// Generic call 0x0D "abort interactions": every Sim but the stack object forgets the stack object,
// as when it is deleted, and drops or resets what it was doing with it. Always true.
constexpr std::uint8_t kAbortInteractionsGenericCall = 0x0D;
// Generic call 0x30 "Get Remapped Neighbor Id": temp 1 becomes what the lot's neighbor id remap
// table maps the neighbor id in temp 0 to (its low 16 bits), or -1. Always true.
constexpr std::uint8_t kRemappedNeighborIdGenericCall = 0x30;
// Generic call 0x31: whether the stack object would still pass the placement test where it stands
// with footprint index (object data 9) temp 0. True when that is already its footprint index. Chairs
// ask it before a sectional changes footprint.
constexpr std::uint8_t kFitsWithFootprintGenericCall = 0x31;
// Generic call 0x20, "Get Outfits Information": temp 1 = 1 when the object in temp 3 has a whole
// outfit in the outfit category in temp 0, else 0. Always true.
constexpr std::uint8_t kOutfitsInformationGenericCall = 0x20;
constexpr std::size_t kFootprintIndexField = 0x09;
// Generic call 0x27 refreshes a Sim's wants and fears in the way temp 0 asks (0-9 mine, 10 and up those
// of the neighbor in temp 1). Always true. The refresh itself isn't kept yet.
constexpr std::uint8_t kRecalcWantsGenericCall = 0x27;
// Generic call 0x17 ends the family tie between the neighbors in temps 0 and 1 (a broken engagement
// or a divorce). Each must be a neighbor. Always true.
constexpr std::uint8_t kRemoveFamilyTieGenericCall = 0x17;
// Generic Sims Call 0x15 "Add Child to Family Relationship Array": the neighbor in temp 0 becomes the
// child of the neighbor in temp 1 (World::setFamilyTie). Always true.
constexpr std::uint8_t kAddChildGenericCall = 0x15;

// The first interaction waiting in the Sim's main queue, or null. The game keeps sub-interactions
// (nested) in a queue of their own.
inline const QueuedInteraction* firstMainQueued(const SimObject& sim) {
    const auto it = std::find_if(sim.queue.begin(), sim.queue.end(), [](const QueuedInteraction& q) { return !q.nested; });
    return it != sim.queue.end() ? &*it : nullptr;
}

// Called whenever a Sim's queue changes.
inline void noteNextQueued(SimObject& sim) {
    const QueuedInteraction* next = firstMainQueued(sim);
    sim.personData[kNextQueuedIndexPersonData] = next ? static_cast<std::int16_t>(next->index) : 0;
    sim.personData[kNextQueuedContinuationPersonData] = next && next->continuation ? 1 : 0;
    sim.personData[kNextQueuedObjectPersonData] = next ? next->object : 0;
}

// The sub-queue's person data, kept as the game keeps them when a sub-interaction is queued, starts
// or is taken out: 0x1A how many wait, 0x4D/0x4E the index and object of the first, 0x4B/0x4C the
// object and index of the running interaction, and 0x66/0x67 those of the running sub-interaction,
// else of the running interaction. Scripts wait on 0x4E to start a sub-interaction.
inline void noteSubQueue(SimObject& sim) {
    const auto nested = [](const QueuedInteraction& q) { return q.nested; };
    const auto first = std::find_if(sim.queue.begin(), sim.queue.end(), nested);
    auto& pd = sim.personData;
    pd[kSubQueueCountPersonData] = static_cast<std::int16_t>(std::count_if(sim.queue.begin(), sim.queue.end(), nested));
    pd[kNextSubIndexPersonData] = first != sim.queue.end() ? static_cast<std::int16_t>(first->index) : 0;
    pd[kNextSubObjectPersonData] = first != sim.queue.end() ? first->object : 0;
    pd[kSubQueueMasterObjectPersonData] = sim.running ? sim.running->object : 0;
    pd[kSubQueueMasterIndexPersonData] = sim.running ? static_cast<std::int16_t>(sim.running->index) : 0;
    const std::optional<QueuedInteraction>& shown = sim.runningSub ? sim.runningSub : sim.running;
    pd[kRunningIndexPersonData] = shown ? static_cast<std::int16_t>(shown->index) : 0;
    pd[kRunningObjectPersonData] = shown ? shown->object : 0;
}

// Whether something should interrupt what the Sim does: its running interaction or sub-interaction
// was cancelled, or the first one waiting in the main queue has a higher priority than the Sim's
// Priority (person data 0x21). Person data 0x47 set holds everything back.
inline bool interruptPending(const SimObject& sim) {
    if (sim.personData[kHoldInterruptsPersonData] != 0) {
        return false;
    }
    if ((sim.runningSub && sim.runningSub->cancelled) || (sim.running && sim.running->cancelled)) {
        return true;
    }
    const QueuedInteraction* next = firstMainQueued(sim);
    return next != nullptr && sim.personData[kPriorityPersonData] < next->priority;
}

// Called when an interaction starts (its index and object) and when it ends (0, 0).
inline void noteCurrentInteraction(SimObject& sim, std::int16_t index, std::int16_t object) {
    sim.personData[kCurrentIndexPersonData] = index;
    sim.personData[kCurrentObjectPersonData] = object;
    sim.personData[kRunningIndexPersonData] = index;
    sim.personData[kRunningObjectPersonData] = object;
}

// While an interaction (not a sub-interaction) runs, my "Priority" and "Interaction Autonomous"
// person data hold its priority and whether it is autonomous. Both are 0 once it is over.
inline void notePriority(SimObject& sim, const QueuedInteraction* running) {
    sim.personData[kPriorityPersonData] = running ? running->priority : std::int16_t{0};
    sim.personData[kInteractionAutonomousPersonData] = running && running->autonomous ? 1 : 0;
}

// Makes `interaction` the one the Sim runs, its name and icon the action string and icon.
inline void startRunning(SimObject& sim, const QueuedInteraction& interaction) {
    sim.running = interaction;
    sim.personData[kHoldInterruptsPersonData] = 0;
    sim.personData[kLinkedDeletedPersonData] = 0;
    sim.actionString = interaction.name;
    sim.actionIconObject = interaction.icon();
    sim.actionIconGuid = interaction.iconGuid;
    sim.actionIconIndex = interaction.iconIndex;
}

// What generic call 0x41 sets on the object running it.
inline void initObjectDefaults(SimObject& me) {
    auto& d = me.data;
    d[0x28] = static_cast<std::int16_t>(d[0x28] | 0x2020);  // flag field 2: flags 6 and 14
    d[0x2A] = static_cast<std::int16_t>(d[0x2A] | 0x0003);  // placement flags 1 and 2
    d[0x2B] = static_cast<std::int16_t>(d[0x2B] | 0x000A);  // movement flags 2 and 4
    d[0x04] = static_cast<std::int16_t>(d[0x04] | 0x0001);  // allowed height flags
    d[0x17] = 2;                                            // rotation notches
    d[0x1E] = 0;                                            // room placement
    d[0x31] = 1;                                            // trash units
    d[0x4E] = 5;                                            // look at score
}

constexpr std::uint8_t kSearchObject = 0x00;
constexpr std::uint8_t kSearchPerson = 0x01;
constexpr std::uint8_t kSearchNonPerson = 0x02;
constexpr std::uint8_t kSearchObjectOfType = 0x04;
constexpr std::uint8_t kVerifyStackObject = 0x1A;
constexpr std::uint8_t kVerifyNeighborInStackObject = 0x1D;
constexpr std::uint8_t kVerifySimInStackObject = 0x1E;
constexpr std::uint8_t kSearchAnySpecies = 0x28;
constexpr std::uint8_t kSearchNonHuman = 0x29;
// Search 0x0C: objects whose category (object data 0x3B) equals temp 0. Search 0x06 does the same with the
// running tree's param 0.
constexpr std::uint8_t kSearchCategoryInTemp0 = 0x0C;
constexpr std::uint8_t kSearchCategoryInParam0 = 0x06;
constexpr std::size_t kCategoryField = 0x3B;
// Search 0x19: objects in the room in temp 0 (object data 0x1D).
constexpr std::uint8_t kSearchObjectInRoomInTemp0 = 0x19;
// Search 0x21 "Hauntable Object": objects whose OBJD field 0x1C is nonzero.
constexpr std::uint8_t kSearchHauntable = 0x21;
constexpr std::uint8_t kSearchSpecies1Or2 = 0x2A;
constexpr std::uint8_t kSearchSpecies1 = 0x2B;
constexpr std::uint8_t kSearchSpecies2 = 0x2C;
constexpr std::uint8_t kSearchSpecies3 = 0x2D;

// Person data the species searches test (0 = human).
constexpr std::size_t kSpeciesPersonData = 0xBA;
// Searches skip objects with bit 0x1 or 0x4 set in object data 0x38, or with a non-zero object
// data 0x6B. Set to Next flag bits 0 and 2 turn these filters off.
constexpr std::size_t kSearchSkipFlagsField = 0x38;
constexpr std::size_t kSearchSkipField = 0x6B;
// Search 0x08: objects on the same tile. A target with bit 0x2000 set in object data 0x2A is an
// error.
constexpr std::uint8_t kSearchSameTile = 0x08;
constexpr std::size_t kSameTileRefusedField = 0x2A;
// Search 0x03: the parts of a multi-tile object, which all hold the lead object's id in object
// data 0x13.
constexpr std::uint8_t kSearchMultiTilePart = 0x03;
// Search 0x12: like 0x04, with the GUID in temps 0 (low) and 1 (high).
constexpr std::uint8_t kSearchObjectOfTypeInTemps = 0x12;
// Search 0x09: the objects next to the object in a local, in the same room (object data 0x1D).
constexpr std::uint8_t kSearchAdjacent = 0x09;
// Searches 0x15 parents, 0x16 siblings, 0x17 children and 0x1F spouse of the neighbor in temp 0, from
// the family ties record.
constexpr std::uint8_t kSearchParents = 0x15, kSearchSiblings = 0x16, kSearchChildren = 0x17, kSearchSpouse = 0x1F;
// Search 0x1C: neighbors the neighbor in temp 0 is family to (relationship variable 3).
constexpr std::uint8_t kSearchFamilyTie = 0x1C;
constexpr std::size_t kFamilyTieRelationship = 3;
// Search 0x24: neighbors toward whom the neighbor in temp 0's relationship variable 1 (its flags) has
// any bit of temp 1.
constexpr std::uint8_t kSearchRelationshipFlags = 0x24;
// Search 0x23: neighbors toward whom the neighbor in temp 0's relationship variable 0 (daily) is above
// temp 1.
constexpr std::uint8_t kSearchRelationshipAbove = 0x23;
constexpr std::uint8_t kSearchObjectTypeByFields = 0x26;
// Search 0x10, "vehicle": objects of OBJD type 0x0D or 0x12.
constexpr std::uint8_t kSearchVehicle = 0x10;
// Search 0x20, "Owner of Object": ids toward whom the object in temp 0 holds relationship variable
// 1 of at least temp 1.
constexpr std::uint8_t kSearchObjectOwner = 0x20;
// Search 0x0E: the job object of the Sim in the target, whose GUID is person data 0x5A (high) and
// 0x59 (low).
constexpr std::uint8_t kSearchJobObject = 0x0E;
constexpr std::size_t kJobGuidHighPersonData = 0x5A, kJobGuidLowPersonData = 0x59;
constexpr std::size_t kRelationshipFlagsVariable = 1;
constexpr std::size_t kRoomDataField = 0x1D;
// Search 0x18: the objects in my view (objectsInView), with my temp 0 as the range. Sims only.
constexpr std::uint8_t kSearchInView = 0x18;
// Searches that walk the neighborhood's neighbor ids instead of objects, by species.
constexpr std::uint8_t kSearchHumanNeighbor = 0x05;
constexpr std::uint8_t kSearchAnyNeighbor = 0x2E;
constexpr std::uint8_t kSearchNonHumanNeighbor = 0x2F;
constexpr std::uint8_t kSearchSpecies1Or2Neighbor = 0x30;
constexpr std::uint8_t kSearchSpecies1Neighbor = 0x31;
constexpr std::uint8_t kSearchSpecies2Neighbor = 0x32;
constexpr std::uint8_t kSearchSpecies3Neighbor = 0x33;
// Search 0x11: neighbors of the family (person data 0x3D) in temp 0.
constexpr std::uint8_t kSearchFamilyNeighbor = 0x11;
// Search 0x07: neighbors whose Sim is of a type (see nextNeighborOfType).
constexpr std::uint8_t kSearchNeighborOfType = 0x07;
constexpr std::size_t kFamilyPersonData = 0x3D;
constexpr std::size_t kLeadTileObjectField = 0x13;
constexpr std::int16_t kSameTileRefusedBit = 0x2000;

struct Stop {
    Outcome outcome;
    std::string reason;
    std::string detail;
};

inline std::string hex(std::uint32_t value, int digits) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%0*X", digits, value);
    return text;
}


template <typename Container>
inline auto& slotAt(Container& values, std::int64_t index, const char* what) {
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        throw Stop{Outcome::Error, std::string(what) + " index out of range", std::to_string(index)};
    }
    return values[static_cast<std::size_t>(index)];
}

// The bit of a 16-bit value that flag `flag` (1 to 16) names. The game rejects only flags above 16. For
// flag 0 or below its bit lies outside the value (the shift wraps to 32-bit bits), so it names none.
inline std::int16_t flagBit(std::int16_t flag) {
    if (flag > 16) {
        throw Stop{Outcome::Error, "flag number out of range", std::to_string(flag)};
    }
    return flag < 1 ? std::int16_t{0} : static_cast<std::int16_t>(1 << (flag - 1));
}

class Execution {
public:
    Execution(CodeSource& code, std::array<std::int16_t, kGlobalCount>& globals,
              ThreadState& thread, World* world, SimObject* me, SimRandom& rng)
        : code_(code), globals_(globals), thread_(thread), world_(world), me_(me), rng_(rng) {}

    // Runs a tree to the end at once on a scratch thread, as `who` with the given stack object
    // (the game runs check trees this way). Set by the VM.
    std::function<RunResult(SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args)>
        runNow;
    // Runs a tree to the end at once on top of my own stack, as me, on my temps, with the given code
    // and stack object (a message handler). Set by the VM.
    std::function<RunResult(std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject, std::span<const std::int16_t> args)>
        runOnMe;
    // Runs a tree to the end at once as `who`, on its temps, with the given code and stack object.
    // Set by the VM.
    std::function<RunResult(SimObject& who, std::uint32_t codeGroup, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args)>
        runOnObject;
    // Starts a newly created object (see Vm::startCreated). Set by the VM.
    std::function<void(SimObject& created, const ObjectType& type)> startCreated;
    // Tells every Sim an object goes away (see Vm::tellSimsObjectGoes). Set by the VM.
    std::function<void(std::int16_t object)> tellSimsObjectGoes;
    // Where Add/Change the Action String puts its lines and icon while a candidate's check tree runs.
    std::vector<OfferedAction>* offered = nullptr;
    InteractionIcon* candidateIcon = nullptr;
    // The copy of the interaction line last tested by a check tree, if any.
    ttab::Interaction* testedLine = nullptr;
    // Runs an interaction's check tree as the pie menu does (see Vm::runCheck). Set by the VM.
    // `changed` receives the line as the tree left it.
    // `icon`: the candidate's queue icon when the tree checks a candidate (see Vm::runCheck), and
    // `offered` (if set) receives the lines it offers then.
    std::function<RunResult(SimObject& who, std::uint32_t codeOwner, std::uint16_t tree, std::int16_t stackObject,
                            std::span<const std::int16_t> args, const ttab::Interaction& line, ttab::Interaction& changed,
                            InteractionIcon* icon, std::vector<OfferedAction>* offered)>
        runCheck;
    const AutonomyTunings* autonomyTunings = nullptr;
    CheckTreeKinds* checkTreeKinds = nullptr;

    // Private and semi-global code and constants come from the frame's code owner.
    std::uint32_t groupFor(std::uint16_t id, std::uint32_t codeOwner) {
        if (id < 0x1000) {
            return kGlobalGroup;
        }
        if (id < 0x2000) {
            return codeOwner;
        }
        return code_.semiGlobalGroup(codeOwner);
    }

    struct NamedCall {
        std::uint32_t group = 0;
        std::uint16_t instance = 0;
        std::shared_ptr<const bhav::Behavior> code;
        std::uint32_t codeOwner = 0;
        std::vector<std::int16_t> args = std::vector<std::int16_t>(4);
        std::optional<std::int16_t> stackObject;  // the new frame's, when set
        std::optional<std::int16_t> runningOn;    // the object the new frame runs on, when it isn't the caller's
        bool atOnce = false;                      // modes 0 and 1: runs to its end before the caller goes on
        bool onStackObject = false;               // mode 1: runs as the stack object, on its temps
        std::optional<QueuedInteraction> interaction;  // Gosub Found Action: the interaction it starts
    };

    // Where Create Object puts a new object on the lot.
    struct CreateSpot {
        std::int32_t x = 0, y = 0;
        std::int32_t level = 0;
        std::optional<Tile> tile;
        std::optional<std::int16_t> facing;
    };

    using TokenList = std::vector<std::shared_ptr<InventoryToken>>;

    struct WalkStep {
        enum class Kind { Done, Wait, PreRoute, PostRoute, Stair, GoalCheck, RouteTree } kind = Kind::Done;
        bool result = false;
        // For PreRoute and PostRoute: my pre- or post-route tree and what it is given, run on my stack
        // before the instruction runs again. For Stair: a stair's Portal tree, run the same way with the
        // stair's entry part as stack object and the stair's code. For GoalCheck: the goal check of
        // the object my walking tree runs on, with that tree's stack object and the object's code. For
        // RouteTree: one of the trees a blocked or failed route runs (see RouteBlock), the same way with
        // its own stack object.
        std::uint32_t group = 0;
        std::uint16_t instance = 0;
        std::shared_ptr<const bhav::Behavior> code;
        std::array<std::int16_t, 9> args{};
        std::int16_t stackObject = 0;
        std::uint32_t codeOwner = 0;
    };

    struct TurnStep {
        enum class Kind { Done, Wait, PreRoute, PostRoute } kind = Kind::Done;
        bool result = false;
        // For PreRoute and PostRoute: the Sim's pre- or post-route tree, run on its stack before the
        // instruction runs again.
        std::uint32_t group = 0;
        std::uint16_t instance = 0;
        std::shared_ptr<const bhav::Behavior> code;
    };

    // My pre- or post-route tree (OBJf entry 0x25 or 0x26), if I have one, and an object's goal
    // check (entry 0x27).
    static constexpr std::size_t kPreRouteEntry = 0x25, kPostRouteEntry = 0x26, kGoalCheckEntry = 0x27;

    struct IdleResult {
        enum class Kind { True, Wait, Start } kind = Kind::True;
        QueuedInteraction interaction;
        bool sub = false;  // started at once: the Sim's running sub-interaction
    };

    struct ReachStep {
        enum class Kind { Done, Wait } kind = Kind::Done;
        bool result = false;
    };

    struct FloorSpot {
        float x = 0, y = 0;  // tiles
        std::int16_t facing = 0;  // compass direction
    };

    // Run Tree by Name, the primitive dispatch and call arguments (vm.cpp).
    std::optional<NamedCall> namedTree(const bhav::Instruction& in, Frame& f);
    bool primitive(const bhav::Instruction& in, Frame& f);
    std::array<std::int16_t, 8> callArgs(const bhav::Instruction& in, Frame& f,
                                         std::size_t calleeArgCount);

    // Reading and writing operands, Expression, fields, arrays and constants (vm_values.cpp).
    std::int16_t read(std::uint8_t owner, std::uint16_t data, Frame& f);
    std::int16_t roomField(std::int16_t roomId, std::uint16_t field);
    ttab::MotiveEffect& testedAd(std::uint16_t data);
    static std::int16_t personalityType(std::int16_t type);
    std::int16_t hasFunction(const SimObject& object, std::uint16_t data);
    std::int16_t slotValue(SimObject& object, std::uint16_t data);
    std::int16_t& arrayValue(SimObject& object, std::uint16_t data, bool elementAtIterator);
    std::int16_t& arrayElement(SimObject& object, std::uint16_t data, std::int16_t element);
    std::int16_t neighborField(std::int16_t neighborId, std::uint16_t field);
    std::int16_t neighborDefinitionField(std::uint16_t data);
    std::int16_t& temporaryTokenField(std::uint16_t data);
    std::int16_t& neighborPersonData(std::uint16_t data);
    void write(std::uint8_t owner, std::uint16_t data, Frame& f, std::int16_t value);
    bool expression(const bhav::Instruction& in, Frame& f);
    void assign32(const bhav::ExpressionOperands& x, Frame& f);
    bool arrayOperation(const bhav::Instruction& in, Frame& f);

    // Generic Sims Calls, family, new characters, relationships, motives and wants (vm_sims.cpp).
    bool genericSimsCall(const bhav::Instruction& in);
    bool addToFamily();
    bool fitsWithFootprint();
    bool outfitsInformation();
    std::uint32_t madeCharacterType(const ObjectType& from, std::uint32_t top);
    std::vector<std::int16_t> wantSimList(const std::string& treeName, std::int16_t sim);
    std::optional<bool> makeNewCharacter(const bhav::Instruction& in, Frame& f);
    std::optional<bool> makeChildren(const bhav::Instruction& in, Frame& f, SimObject& holder, std::optional<std::uint32_t> templateGuid);
    std::int16_t speciesFlag(std::int16_t neighbor);
    std::optional<std::int16_t> makeChild(std::int16_t parent1, std::int16_t parent2, std::optional<std::uint32_t> templateGuid);
    // A neighbor's surname: the one it took while playing, else its character's (catalog string 2).
    std::string neighborSurname(std::int16_t neighbor);
    std::optional<Genome> parentGenes(std::int16_t neighbor);
    static std::int16_t newCharacterAge(int flag);
    bool relationship(const bhav::Instruction& in, Frame& f);
    bool findWorstMotives(const bhav::Instruction& in);
    bool changeOutfit(const bhav::Instruction& in, Frame& f);
    bool testSimInteractingWith();
    bool followSim();
    bool setMotiveChange(const bhav::Instruction& in, Frame& f);
    bool wantSatisfy(const bhav::Instruction& in, Frame& f);
    std::int16_t toNeighbor(std::int16_t id, bool isNeighborId);
    std::uint32_t objectGuid(std::int16_t id);
    bool wantMatches(const swaf::Want& want, const swaf::Want& request, content::WantOperator op) const;

    // Objects: create and remove, lights, materials, sound, dialogs, timers, messages, Lua (vm_objects.cpp).
    std::int32_t sleepTicks(const bhav::Instruction& in, Frame& f);
    bool removeObject(const bhav::Instruction& in);
    bool changeLight(const bhav::Instruction& in, Frame& f);
    bool changeMaterial(const bhav::Instruction& in, Frame& f);
    bool soundEvent(const bhav::Instruction& in, Frame& f);
    std::optional<bool> dialog(const bhav::Instruction& in, Frame& f);
    std::optional<bool> babyNameDialog(Frame& f);
    std::optional<bool> cameraControl(const bhav::Instruction& in, Frame& f);
    static void setMaterial(SimObject& object, MaterialChange change);
    std::optional<CreateSpot> tileBeside(const SimObject& sim, std::int16_t turn);
    bool onLotTile(std::int32_t level, int x, int y);
    bool tileRefuses(const CreateSpot& spot, std::uint8_t flags, std::uint32_t guid);
    std::optional<CreateSpot> tileInFront(const SimObject* ref);
    bool createObject(const bhav::Instruction& in, Frame& f);
    void passToMain(SimObject& created, std::uint8_t flags);
    bool breakPoint(const bhav::Instruction& in, Frame& f);
    bool randomNumber(const bhav::Instruction& in, Frame& f);
    bool refresh(const bhav::Instruction& in);
    bool luaScript(const bhav::Instruction& in, Frame& f);
    bool message(const bhav::Instruction& in, Frame& f);
    bool onTimer(const bhav::Instruction& in, Frame& f);
    bool cinematic(const bhav::Instruction& in, Frame& f);
    bool notifyOutOfIdle();
    bool testObjectType(const bhav::Instruction& in, Frame& f);

    // Manage Inventory (vm_inventory.cpp).
    bool manageInventory(const bhav::Instruction& in, Frame& f);
    static std::uint16_t word(const std::array<std::uint8_t, 16>& o, std::size_t at);
    static std::int16_t checkedResult(std::int32_t v);
    void copyToObject(SimObject& self, const InventoryToken& t, int kind, std::int16_t id, bool counted,
                      std::size_t at);
    static std::int16_t property(const InventoryToken& t, std::int16_t i);
    static void setProperty(InventoryToken& t, std::int16_t i, std::int16_t v);
    static std::optional<std::size_t> findToken(const TokenList& tokens, std::optional<std::uint32_t> guid,
                                                std::int16_t start, std::uint16_t value, std::uint16_t mask,
                                                bool backward);
    template <typename Index, typename Value, typename Result>
    bool singularTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                        Index index, Value value, Result result, Frame& f, int kind, std::int16_t id);
    template <typename Index, typename Value, typename Result>
    bool countedTokens(Inventory& inventory, std::uint8_t op, std::uint32_t guid, const std::array<std::uint8_t, 16>& o,
                       Index index, Value value, Result result, Frame& f, int kind, std::int16_t id);

    // Set To Next and its searches (vm_set_to_next.cpp).
    bool nextObjectTypeByFields(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool jobObject(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool setToNext(const bhav::Instruction& in, Frame& f);
    template <typename Filters>
    bool nextInView(const Filters& passesFilters, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextNeighborOfType(std::uint32_t guid, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    template <typename Filters>
    bool nextNeighbor(std::uint8_t searchType, const Filters& passesFilters, std::uint8_t targetOwner,
                      std::uint16_t targetData, Frame& f);
    template <typename Fits, typename Filters>
    bool nextNeighborWhere(const Fits& fits, const Filters& passesFilters, std::uint8_t targetOwner,
                           std::uint16_t targetData, Frame& f, std::uint8_t searchType);
    bool nextRelative(std::uint8_t searchType, std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextFamilyTie(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextWithRelationshipFlags(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextWithRelationshipAbove(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextAdjacent(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner, std::uint16_t targetData,
                      Frame& f);
    bool nextMultiTilePart(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextObjectOwner(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextVehicle(std::uint8_t targetOwner, std::uint16_t targetData, Frame& f);
    bool nextOnSameTile(const std::array<std::uint8_t, 16>& o, std::uint8_t targetOwner,
                        std::uint16_t targetData, Frame& f);

    // Find Location For, slots, Snap and Snap Into (vm_placement.cpp).
    bool findLocationFor(const bhav::Instruction& in, Frame& f);
    std::vector<std::optional<ModelBone>> slotJoints(const SimObject& owner, const slot::Entry& entry, std::uint32_t group);
    static std::optional<int> jointFacing(const SimObject& owner, const slot::Entry& entry,
                                          const std::vector<std::optional<ModelBone>>& joints, float x, float y);
    bool placeBySlot(const std::array<std::uint8_t, 16>& o, std::int16_t index, SimObject& placed, const SimObject& ref);
    bool snap(const bhav::Instruction& in, Frame& f);
    void leaveContainer(SimObject& object);
    std::optional<ModelBone> slotBone(const SimObject& object, std::uint32_t group, const slot::Entry& entry);
    std::optional<ModelBone> modelJoint(const SimObject& object, const std::string& name);
    bool snapInto(const bhav::Instruction& in, Frame& f);
    void takeOutOfSlot(SimObject& object);
    void putInSlot(SimObject& object, SimObject& container, std::int16_t slot);

    // Animate Sim, Animate Overlay, Animate Stop and Reach/Put (vm_animation.cpp).
    std::optional<std::string> animationEntry(const Frame& f, std::uint16_t list, std::uint16_t entry, bool orMissing = false,
                                              std::uint8_t* category = nullptr);
    std::string objectAnimationText(std::uint32_t owner, const SimObject* sim, std::uint16_t entry);
    std::optional<std::string> socialAnimation(const Frame& f, const SimObject& sim, std::uint16_t entry);
    std::optional<bool> animateSim(const bhav::Instruction& in, Frame& f);
    std::uint8_t entryCategory(const std::string& name, std::uint8_t category);
    std::optional<std::string> specialAnimationName(const SimObject& sim, std::uint16_t entry, std::uint8_t* category = nullptr);
    bool animateOverlay(const bhav::Instruction& in, Frame& f);
    void layOverlay(SimObject& sim, PlayingOverlay overlay);
    bool animateStop(const bhav::Instruction& in, Frame& f);
    bool animateObject(const bhav::Instruction& in, Frame& f);
    bool lookAt(const bhav::Instruction& in, Frame& f);
    ReachStep reachPut(const bhav::Instruction& in, Frame& f);
    std::string childClip(const SimObject& sim, const std::string& clip);
    void playReachStages(SimObject& sim, const SimObject& object, bool grab, bool flipped, std::uint16_t entry, float length,
                         float event, double now);
    static FloorSpot floorSpot(const SimObject& sim, const SimObject& object);
    bool reachTransfer(SimObject& sim, const Reaching& reach);

    // Routing, walking and turning, distances, directions and Ray Trace (vm_routing.cpp).
    bool rayTrace(const bhav::Instruction& in, Frame& f);
    WalkStep goToRoutingSlot(const bhav::Instruction& in, Frame& f, const slot::Entry* relative = nullptr);
    WalkStep climbStair(SimObject& sim, Walking& walk);
    WalkStep goToRelativePosition(const bhav::Instruction& in, Frame& f);
    bool walkTo(SimObject& sim, Walking& walk, double now, std::int16_t& blocker);
    // A blocked walk's next move (vm_route_blocked.cpp): a step to return, or a plan again from where
    // the Sim stands.
    struct BlockedStep {
        bool planAgain = false;
        WalkStep step;
    };
    BlockedStep routeBlocked(SimObject& sim, Walking& walk, std::int16_t blocker, int depth = 0);
    BlockedStep blockedTreeReturned(SimObject& sim, Walking& walk);
    BlockedStep blockedAnimation(SimObject& sim, Walking& walk);
    BlockedStep askToMove(SimObject& sim, Walking& walk, int depth);
    BlockedStep lastResort(SimObject& sim, Walking& walk);
    BlockedStep routeFailed(SimObject& sim, Walking& walk, std::int16_t reason, std::int16_t object);
    // A route that ends while it waits on a Sim it asked to move cancels the interaction it gave
    // that Sim, as the game's router does whenever it drops a route. dropWalk ends my walk and every
    // route in it. endAside ends a step-aside, its route below going on.
    void releaseWait(RouteBlock& block);
    void dropWalk(SimObject& sim);
    void endAside(Walking& walk);
    bool stepAside(SimObject& sim, Walking& walk, const SimObject& other);
    bool askedToMove(SimObject& sim, Walking& walk, SimObject& other);
    SimObject* makeDestination(SimObject& mover, const SimObject& other, int mode);
    std::optional<SlotRoute> stepAsideRoute(SimObject& sim, const Walking& walk);
    void rememberSims(Walking& walk);
    void routeEnded(const SimObject& sim);
    SweepPlace sweepPlace(const SimObject& who, double ms);
    std::int16_t sweepBlocker(SimObject& sim, Walking& walk, double now);
    std::optional<WalkStep> blockedTree(Walking& walk, RouteBlock::Step kind, std::uint32_t group, std::uint16_t tree,
                                        std::int16_t stackObject, std::span<const std::int16_t> args);
    bool turnOnTheSpot(SimObject& sim, Walking& walk, double now);
    std::optional<LocoClip> locoClip(const std::string& name);
    std::optional<LocoPair> locoPair(const std::optional<WalkTurnClips>& names);
    void planWalkFor(const SimObject& sim, Walking& walk);
    void followWalk(SimObject& sim, Walking& walk, float walked, double now);
    void endWalk(SimObject& sim, const Walking& walk, double now);
    void standAfterWalk(SimObject& sim, double now);
    void stopWalk(SimObject& sim, Walking& walk, double now);
    void interruptWalk(SimObject& sim, Walking& walk, double now);
    WalkStep routeInterrupted(SimObject& sim);
    bool settleAfterStop(SimObject& sim, Walking& walk, double now);
    TurnStep turnBodyTowards(const bhav::Instruction& in, Frame& f);
    static void faceHeading(SimObject& sim, float heading);
    std::optional<float> turnTarget(const bhav::Instruction& in, Frame& f, const SimObject& sim, float current,
                                    std::int16_t direction);
    std::optional<float> slotTurnTarget(const bhav::Instruction& in, Frame& f, const SimObject& sim);
    std::optional<TurnStep> routeTree(const SimObject& sim, std::size_t entry);
    std::optional<WalkStep> goalCheck(SimObject& sim, const Frame& f, float remaining);
    bool getDistanceTo(const bhav::Instruction& in, Frame& f);
    bool getDirectionTo(const bhav::Instruction& in, Frame& f);
    static std::int16_t octant(float dx, float dy);

    // Interactions: action strings, Find Best Object/Interaction, Idle for Input, Push Interaction
    // (vm_interactions.cpp).
    void offerAction(const std::string& name, std::uint16_t flags, const Frame& f);
    bool actionString(const bhav::Instruction& in, Frame& f);
    void setActionIcon(SimObject& self, std::uint16_t flags, std::int16_t object, std::uint32_t guid, std::uint32_t index);
    std::optional<NamedCall> functionalTree(const bhav::Instruction& in, const Frame& f);
    std::optional<NamedCall> foundAction(const bhav::Instruction& in);
    bool findBestObject(const bhav::Instruction& in, Frame& f);
    bool findBestInteraction(const bhav::Instruction& in);
    IdleResult idleForInput(const bhav::Instruction& in, Frame& f);
    bool passesStartCheck(SimObject& sim, const SimObject& object, const QueuedInteraction& waiting);
    void queueSkipped(const SimObject& sim, SimObject& object);
    // dropLinkedInteractions, with each dropped waiting interaction's Queue Skipped function run.
    void dropLinked(QueuedInteraction& ended, const SimObject* skip);
    bool alterBudget(const bhav::Instruction& in, Frame& f);
    bool pushInteraction(const bhav::Instruction& in, Frame& f);
    // Runs a pushed interaction whose line has flag 0x4 to its end now.
    void runPushedAtOnce(SimObject& sim, const SimObject& object, const QueuedInteraction& pushed);

private:
    // Reading and writing operands, Expression, fields, arrays and constants (vm_values.cpp).
    std::int16_t constant(std::uint16_t data, std::uint32_t codeOwner);
    std::int16_t constantByTemp(std::uint16_t data, std::uint32_t codeOwner);
    const std::vector<std::int16_t>& constantTable(std::uint32_t id, std::uint32_t rangeSize,
                                                   std::uint32_t codeOwner);

    // Each frame has its own stack object. Operands read the current frame's.
    std::int16_t& stackObject() { return thread_.frames.back().stackObjectId; }

    SimObject& me() {
        if (me_ == nullptr) {
            throw Stop{Outcome::Unsupported, "run has no 'me' object", ""};
        }
        return *me_;
    }

    World& world() {
        if (world_ == nullptr) {
            throw Stop{Outcome::Unsupported, "run has no world", ""};
        }
        return *world_;
    }

    static std::uint32_t readGuid(const std::array<std::uint8_t, 16>& o) {
        return static_cast<std::uint32_t>(o[0]) | static_cast<std::uint32_t>(o[1]) << 8 |
               static_cast<std::uint32_t>(o[2]) << 16 | static_cast<std::uint32_t>(o[3]) << 24;
    }

    std::int16_t tempValue(std::uint16_t tempIndex) {
        return slotAt(thread_.temps, tempIndex, "temp");
    }

    // Motives are floats in the game, clamped to -100..100 on every write.
    static float& motive(SimObject& object, std::int64_t index) {
        if (!object.isPerson()) {
            throw Stop{Outcome::Error, "motives on an object that is not a person",
                       std::to_string(object.id)};
        }
        if (index < 0 || static_cast<std::size_t>(index) >= kMotiveCount) {
            throw Stop{Outcome::Error, "motive number out of range", std::to_string(index)};
        }
        return object.motives[static_cast<std::size_t>(index)];
    }

    static void setMotive(SimObject& object, std::int64_t index, std::int16_t value) {
        motive(object, index) = std::clamp(static_cast<float>(value), kMotiveMin, kMotiveMax);
    }

    static std::int16_t& personData(SimObject& object, std::int64_t index) {
        if (!object.isPerson()) {
            throw Stop{Outcome::Error, "person data on an object that is not a person",
                       std::to_string(object.id)};
        }
        return slotAt(object.personData, index, "person data");
    }

    SimObject& stack() {
        SimObject* object = world().find(stackObject());
        if (object == nullptr) {
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        return *object;
    }

    // Reading and writing the stack object's data also works on an object removed this tick: the
    // game looks it up by id, which still finds objects waiting to be deleted.
    SimObject& stackData() {
        SimObject* object = world().findIncludingRemoved(stackObject());
        if (object == nullptr) {
            throw Stop{Outcome::Error, "stack object does not exist", std::to_string(stackObject())};
        }
        return *object;
    }

    // The stack object's temp [index] (data owner 0x0D). When I am my own stack object these are my
    // running temps.
    std::int16_t& stackTemp(std::uint16_t index) {
        SimObject& object = stackData();
        auto& temps = &object == &me() ? thread_.temps : object.thread.temps;
        return slotAt(temps, index, "temp");
    }

    // The definition data owner 0x33 reads: the master type's for a multi-tile object, else the
    // stack object's own.
    const objd::ObjectDefinition& stackMasterDefinition() {
        const SimObject& object = stackData();
        if (object.definition.field(objd::field::kMultiTileMasterId) == 0) {
            return object.definition;
        }
        const ObjectType* type = code_.objectType(object.definition.guid());
        const ObjectType* master = type != nullptr ? code_.multiTileMaster(*type) : nullptr;
        if (master == nullptr) {
            throw Stop{Outcome::Error, "multi-tile object without a master", hex(object.definition.guid(), 8)};
        }
        return master->definition;
    }

    CodeSource& code_;
    std::array<std::int16_t, kGlobalCount>& globals_;
    ThreadState& thread_;
    World* world_;
    SimObject* me_;
    SimRandom& rng_;
};

} // namespace ts2::sim::vm_detail
