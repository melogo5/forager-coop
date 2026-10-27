// Testing aid: a query port into the running game. With COOP_DEBUG_PORT=1 the DLL listens on 127.0.0.1 (a free
// port, written to the log as "debugport: listening on 127.0.0.1:<port>"); a test sends one command per line and
// gets one line back, computed on the game thread. tools/gametest/query.ps1 is the client.
//
// Commands (numbers may be decimal or 0x hex; slots are decompiled/code/var_slots.tsv):
//   ping                         -> pong
//   player                       -> id x y room
//   count <object>               -> active instances of the object (or a parent)
//   list <object> [x y radius]   -> "id x y" per active instance (nearest first if a point is given), ';'-separated
//   at <object> <x> <y>          -> how many stand within 4 px of the point (doubles show as 2+)
//   var <id> <slot> [index]      -> an instance variable (an array's element with index)
//   set <id> <slot> [index] <n>  -> ok: sets an instance variable (an array's element with index) to a number
//   sets <id> <slot> <word>      -> ok: sets an instance variable to a string (one word, no spaces)
//   event <id> <type> <number>   -> ok: runs the instance's event with the local player as `other`
//                                   (7 11 = Other_11, what interacting with E does)
//   global <slot> [index]        -> a global variable
//   call <builtin> [numbers...]  -> a builtin's result, called as the local player
//   woken <object> <command>     -> the command, run once that kind is awake (far objects sleep: culled)
//   script <hex address> [n...]  -> a GML script's result (address from decompiled/code/index.tsv)
//   dump                         -> "N: object x y; ..." every world object, asleep ones too (world-diff.ps1)
//   obj <id>                     -> "object name at (x, y) [asleep]": what an instance id is
//   look <id>                    -> sprite, image, alpha, visible, xscale, depth: how it is drawn
//   sleepcheck                   -> "N: id; ..." instances asleep but on no sleep list (being woken)
//   cellcheck                    -> "N: id; ..." cell sleep-list entries whose instance is gone (ProcessCulls crash)
// It answers in the world and in the menus (there `player` is the menu object).
#pragma once

#include "yyc.h"

namespace coop::debugport {

// Starts the listener if COOP_DEBUG_PORT is set (once; later calls do nothing).
void Start();

// Game thread, every local player step: answers what has arrived.
void Tick(yyc::CInstance* localPlayer);

}  // namespace coop::debugport
