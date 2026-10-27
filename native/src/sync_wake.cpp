// Waking instances the game has put to sleep (culled off-screen) the way its own ProcessCulls does, and actions
// that wait for the next cull.
#include "sync.h"
#include "sync_internal.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "log.h"
#include "mobs.h"
#include "mutant.h"

namespace coop::sync {

namespace {

using yyc::CallReal;

// Actions for the next CullObject (WakeThen, RunBeforeCull). They hold the player and other instances as they were:
// one that waited long (the player was in another room, where nothing culls) may point at instances gone since, so
// past kAfterWakeMs it is dropped; and the queue is capped.
struct AfterWake {
    DWORD at;
    std::function<void()> action;
};
std::vector<AfterWake> g_afterWake;
constexpr DWORD kAfterWakeMs = 2000;
constexpr size_t kMaxAfterWake = 256;

void QueueAfterWake(std::function<void()> action) {
    if (g_afterWake.size() >= kMaxAfterWake) {
        Log("sync: too many actions waiting for the next cull - the oldest dropped");
        g_afterWake.erase(g_afterWake.begin());
    }
    g_afterWake.push_back({GetTickCount(), std::move(action)});
}

// What ProcessCulls sets on an instance it wakes. `active` = 1 above all: the game's IsDeactivated reads that
// variable, not the runtime's flag. Left 0, the next TrueInstanceExists/Find on the kind took the awake instance for
// a sleeper - TemporaryActivate, then ClearTemporaryInstances put it to sleep on no list: no camera woke it again
// (playtest 6: trees, fish traps, the bank and vaults vanishing at the guest, reconcile adding an empty vault in the
// hidden one's place). And off the queue ClearTemporaryInstances puts back to sleep: one TemporaryActivate woke for
// a moment (still on the sleep lists) went back to sleep after we took it off them (the fairy queen in
// sleeplists.ps1).
void MarkAwake(yyc::CInstance* self, int id) {
    constexpr int kSlotActive = 0x4F, kSlotStashId = 0x823, kSlotStashList = 0x824, kSlotTempActive = 0x860;
    constexpr int kSlotTemporaryActivations = 0x863;  // global.temporaryInstanceActivations
    yyc::RValue active = yyc::Real(1);
    yyc::SetVar(id, kSlotActive, &active);
    yyc::RValue notTemporary = yyc::Real(0);
    yyc::SetVar(id, kSlotTempActive, &notTemporary);
    for (const int slot : {kSlotStashId, kSlotStashList}) {
        yyc::RValue none{};
        none.kind = yyc::kUndefined;
        yyc::SetVar(id, slot, &none);
    }
    const yyc::RValue* temporary = yyc::GlobalVar(kSlotTemporaryActivations);
    if (temporary == nullptr || !yyc::IsNumber(*temporary)) return;
    const double list = yyc::ToReal(*temporary);
    for (double at; (at = CallReal(self, "ds_list_find_index", {list, double(id)})) >= 0;) {
        CallReal(self, "ds_list_delete", {list, at});
    }
}

// Waking as ProcessCulls does: off global.deactivatedInstances and off the cell's list it was culled into
// (`stashList`), then activated and marked awake. A bare instance_activate_* left the id in those lists: when the
// instance moved on and died, ProcessCulls later met it in the old cell - "Unable to find any instance" (a GML
// error: the host's crash after a guest killed a woken boar) - and SaveGame met sleepers it did not know of
// (playtest 4, bug 10).
void WakeWhere(yyc::CInstance* self, const std::function<bool(yyc::CInstance*)>& pick) {
    constexpr int kSlotDeactivated = 0x16C;  // decompiled/code/var_slots.tsv
    const yyc::RValue* listValue = yyc::GlobalVar(kSlotDeactivated);
    if (listValue == nullptr || !yyc::IsNumber(*listValue)) return;
    const double list = yyc::ToReal(*listValue);
    std::vector<int> woken;
    const bool bare = Mutant("wake");  // the old way: activated, left on the lists
    for (int i = static_cast<int>(CallReal(self, "ds_list_size", {list})) - 1; i >= 0; --i) {
        const int id = static_cast<int>(CallReal(self, "ds_list_find_value", {list, double(i)}));
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance != nullptr && !pick(instance)) continue;
        if (!bare) CallReal(self, "ds_list_delete", {list, double(i)});  // gone ones go too: nothing can wake them
        if (instance == nullptr) continue;
        if (std::find(woken.begin(), woken.end(), id) == woken.end()) woken.push_back(id);
    }
    for (const int id : woken) {
        // The cell lists hold [id, box] arrays: looking the id up there as a number (until 2026-10-25) found nothing.
        if (!bare) mobs::ForgetInstance(self, id);
        CallReal(self, "instance_activate_object", {double(id)});
        if (!bare && !Mutant("active")) MarkAwake(self, id);  // the mutant: `active` left 0, as until playtest 6
    }
}

}  // namespace

void WakeKind(yyc::CInstance* self, int object) {
    WakeWhere(self, [self, object](yyc::CInstance* instance) {
        const int kind = static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index));
        return kind == object || CallReal(self, "object_is_ancestor", {double(kind), double(object)}) != 0;
    });
}

void WakeRegion(yyc::CInstance* self, double left, double top, double width, double height) {
    WakeWhere(self, [=](yyc::CInstance* instance) {
        const double x = yyc::GetBuiltinReal(instance, yyc::va::YYVar_x);
        const double y = yyc::GetBuiltinReal(instance, yyc::va::YYVar_y);
        return x >= left && x <= left + width && y >= top && y <= top + height;
    });
}

void WakeInstance(yyc::CInstance* self, int id) {
    WakeWhere(self, [id](yyc::CInstance* instance) {
        return static_cast<int>(yyc::GetBuiltinReal(instance, yyc::va::YYVar_id)) == id;
    });
}

void WakeThen(yyc::CInstance* self, int object, std::function<void()> action) {
    WakeKind(self, object);
    QueueAfterWake(std::move(action));
}

void RunBeforeCull(std::function<void()> action) { QueueAfterWake(std::move(action)); }

void RunAfterWake() {
    if (g_afterWake.empty()) return;
    std::vector<AfterWake> actions;
    actions.swap(g_afterWake);
    const DWORD now = GetTickCount();
    size_t stale = 0;
    for (const AfterWake& waiting : actions) {
        if (now - waiting.at > kAfterWakeMs) {
            ++stale;
            continue;
        }
        waiting.action();
    }
    if (stale > 0) Log("sync: %zu actions waited too long for a cull (away from the world?) - dropped", stale);
}

void ForgetAfterWake() { g_afterWake.clear(); }

}  // namespace coop::sync
