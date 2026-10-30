#pragma once

#include "sim/world.hpp"

#include <cstdint>
#include <functional>

namespace ts2::sim {

// Push Interaction byte 3 bit 7: the pushed interaction's link number, which the script keeps to
// link a later push with it. One without a number gets a new one, which it also drops.
std::uint16_t ownLinkId(World& world, QueuedInteraction& pushed);

// Push Interaction byte 3 bit 6: links `pushed` with the interaction whose link number is `with`,
// running or waiting on any Sim. `pushed` takes over what that one dropped and gets a new number,
// which that one now drops. Nothing happens when no interaction has the number.
void linkInteraction(World& world, QueuedInteraction& pushed, std::int16_t with);

// What the game does when `ended` ends, is cancelled or is skipped: it follows the chain of
// interactions `ended` drops, across all Sims but `skip` (null for none), each link once. A
// waiting one leaves its queue, after `skipped` (may be empty) runs its object's Queue Skipped
// function. A running one stays, but its Sim's person data 0x6E, "Interactions Linked Deleted",
// becomes 1 for its scripts to see.
void dropLinkedInteractions(World& world, QueuedInteraction& ended, const SimObject* skip,
                            const std::function<void(SimObject& sim, const QueuedInteraction& removed)>& skipped);

} // namespace ts2::sim
