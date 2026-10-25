/*
 * GameStateSnapshot.h, part of VCMI engine
 *
 * Authors: listed in file AUTHORS in main folder
 *
 * License: GNU General Public License v2.0 or later
 * Full text of license available in license.txt file, in main folder
 *
 */
#pragma once

#include <string>

/// Text description of what the current human player can see.
/// Call only on the GUI thread, while holding the interface mutex.
std::string buildMcpGameState();

/// Adventure-map camera FOV: terrain, roads, rivers, visitable objects. GUI thread only.
std::string buildMcpMapState();
