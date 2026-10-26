#include "interiors.h"

#include <windows.h>

#include <initializer_list>
#include <utility>
#include <set>
#include <string>
#include <vector>

#include "MinHook.h"
#include "log.h"

namespace coop::interiors {

namespace {

// ---- interiors built inside the world room ----
// What a room is made of (decompiled/tools/DumpRooms.csx on data.win), in room coordinates.
struct Background {
    int depth;
    int sprite;  // -1: just the colour
    int colour;  // GML colour (0xBBGGRR): the fill, or the sprite's blend
    bool htiled, vtiled;
};
struct Picture {
    int depth;
    int sprite;
    double x, y;
};
struct Tiles {
    int depth;
    int tileset;
    int cols, rows;  // 16 px cells
    const uint16_t* cells;  // row by row; 0 = empty
};
struct Placed {
    int object;
    double x, y, xscale, yscale;
    int roomId;              // its id in the room (what other instances' creation code refers to it by)
    uint32_t creationCode;   // its instance creation code (gml_RoomCC_*), 0 if none
};
struct Plan {
    int room;
    double width, height;
    double spawnX, spawnY;  // where the room puts the player
    std::vector<Background> backgrounds;
    std::vector<Picture> pictures;
    std::vector<Tiles> tiles;
    std::vector<Placed> instances;
};

#include "interior_plans.inc"

constexpr int kRoomGame = yyc::kRoomGame;
// Where an interior is built: past the world room's right edge (rmGame is 1344 wide), so the camera inside shows
// nothing of the world.
constexpr double kOriginX = 1700, kOriginY = 300;
constexpr int kSlotSpawnPlayerX = 0x7E6, kSlotSpawnPlayerY = 0x7E7;  // globals: where the world puts the player
constexpr int kSlotOwner = yyc::slot::owner;
// Globals with the camera's bounds, as mobs.cpp reads them (decompiled/code/var_slots.tsv).
constexpr int kSlotCameraBottom = 0xE0, kSlotCameraLeft = 0xE1, kSlotCameraRight = 0xE2, kSlotCameraTop = 0xE3;
constexpr uint32_t kScriptWaterCollision = yyc::script::WaterCollision;

bool (*g_inSession)() = nullptr;
const Plan* g_inside = nullptr;  // the interior the local player is in, or nullptr
std::vector<int> g_built;        // its instances
std::vector<double> g_layers;    // its layers (backgrounds, pictures, tiles)
bool g_building = false;
int g_playerId = 0;  // the local player (room_goto is called by the transition object, not by the player)
yyc::ScriptFn g_origWater = nullptr;


constexpr int kFirstInterior = 22;  // rmWizardTowerInterior, rmDruidTreeInterior, rmMuseum (decompiled/data/rooms.tsv)
constexpr int kLastInterior = 24;
constexpr unsigned kDumpAfterSteps = 120;  // layers and instances are all made by then
constexpr double kAll = -3;               // GML `all`
// layer_get_element_type (GMS 2.2)
constexpr int kElementBackground = 1, kElementInstance = 2, kElementSprite = 4, kElementTilemap = 5;

yyc::CInstance* g_self = nullptr;

// A builtin's result as an RValue (the caller frees it).
yyc::RValue Call(const char* name, std::initializer_list<double> args) {
    std::vector<yyc::RValue> values;
    for (const double a : args) values.push_back(yyc::Real(a));
    yyc::RValue result{};
    yyc::CallBuiltin(name, g_self, static_cast<int>(values.size()), values.data(), &result);
    return result;
}

double Real(const char* name, std::initializer_list<double> args) {
    yyc::RValue r = Call(name, args);
    const double v = yyc::ToReal(r);
    yyc::Free(&r);
    return v;
}

std::string Text(const char* name, std::initializer_list<double> args) {
    yyc::RValue r = Call(name, args);
    const std::string v = yyc::ToString(r);
    yyc::Free(&r);
    return v;
}

// The numbers in a GML array result (layer ids, element ids).
std::vector<double> Array(const char* name, std::initializer_list<double> args) {
    yyc::RValue r = Call(name, args);
    std::vector<double> out;
    if ((r.kind & yyc::kKindMask) == yyc::kArray) {
        yyc::RValue length{};
        yyc::CallBuiltin("array_length_1d", g_self, 1, &r, &length);
        const int n = static_cast<int>(yyc::ToReal(length));
        yyc::Free(&length);
        for (int i = 0; i < n; ++i) out.push_back(yyc::ToReal(yyc::ArrayElement(&r, i)));
    }
    yyc::Free(&r);
    return out;
}

void DumpInstances() {
    const int count = static_cast<int>(Real("instance_number", {kAll}));
    Log("interiors: %d instances", count);
    for (int i = 0; i < count; ++i) {
        const double id = Real("instance_find", {kAll, double(i)});
        yyc::CInstance* instance = yyc::FindInstance(static_cast<int>(id));
        if (instance == nullptr) continue;
        const double object = yyc::GetBuiltinReal(instance, yyc::va::YYVar_object_index);
        Log("interiors:   %s (%g) at (%g, %g) depth %g sprite %g", Text("object_get_name", {object}).c_str(), object,
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_x), yyc::GetBuiltinReal(instance, yyc::va::YYVar_y),
            yyc::GetBuiltinReal(instance, yyc::va::YYVar_depth), yyc::GetBuiltinReal(instance, yyc::va::YYVar_sprite_index));
    }
}

void DumpTilemap(double tilemap) {
    const int width = static_cast<int>(Real("tilemap_get_width", {tilemap}));
    const int height = static_cast<int>(Real("tilemap_get_height", {tilemap}));
    Log("interiors:     tilemap %g: tileset %g, %d x %d cells of %g x %g at (%g, %g)", tilemap,
        Real("tilemap_get_tileset", {tilemap}), width, height, Real("tilemap_get_tile_width", {tilemap}),
        Real("tilemap_get_tile_height", {tilemap}), Real("tilemap_get_x", {tilemap}), Real("tilemap_get_y", {tilemap}));
    for (int y = 0; y < height && y < 64; ++y) {
        std::string row;
        for (int x = 0; x < width && x < 64; ++x) {
            const double data = Real("tilemap_get", {tilemap, double(x), double(y)});
            row += std::to_string(static_cast<int>(Real("tile_get_index", {data}))) + " ";
        }
        Log("interiors:       row %2d: %s", y, row.c_str());
    }
}

void DumpLayers() {
    const std::vector<double> layers = Array("layer_get_all", {});
    Log("interiors: %zu layers", layers.size());
    for (const double layer : layers) {
        const std::vector<double> elements = Array("layer_get_all_elements", {layer});
        Log("interiors:   layer %g '%s' depth %g visible %g at (%g, %g), %zu elements", layer,
            Text("layer_get_name", {layer}).c_str(), Real("layer_get_depth", {layer}), Real("layer_get_visible", {layer}),
            Real("layer_get_x", {layer}), Real("layer_get_y", {layer}), elements.size());
        for (const double element : elements) {
            const int type = static_cast<int>(Real("layer_get_element_type", {element}));
            if (type == kElementBackground) {
                Log("interiors:     background: sprite %g colour %g alpha %g tiled %g/%g stretch %g", Real("layer_background_get_sprite", {element}),
                    Real("layer_background_get_blend", {element}), Real("layer_background_get_alpha", {element}),
                    Real("layer_background_get_htiled", {element}), Real("layer_background_get_vtiled", {element}),
                    Real("layer_background_get_stretch", {element}));
            } else if (type == kElementTilemap) {
                DumpTilemap(element);
            } else if (type == kElementSprite) {
                Log("interiors:     sprite %g at (%g, %g)", Real("layer_sprite_get_sprite", {element}),
                    Real("layer_sprite_get_x", {element}), Real("layer_sprite_get_y", {element}));
            } else if (type != kElementInstance) {
                Log("interiors:     element of type %d", type);
            }
        }
    }
}

void DumpView() {
    const double camera = Real("view_get_camera", {0});
    Log("interiors: view 0 visible %g, camera at (%g, %g) size %g x %g; room %g x %g", Real("view_get_visible", {0}),
        Real("camera_get_view_x", {camera}), Real("camera_get_view_y", {camera}), Real("camera_get_view_width", {camera}),
        Real("camera_get_view_height", {camera}), Real("room_get_width", {0}), Real("room_get_height", {0}));
}

const Plan* PlanFor(int room) {
    for (const Plan* plan : kPlans) {
        if (plan->room == room) return plan;
    }
    return nullptr;
}

double GlobalReal(int slot) {
    const yyc::RValue* value = yyc::GlobalVar(slot);
    return value != nullptr ? yyc::ToReal(*value) : 0;
}

// The room runs each instance's creation code right after its Create: the museum's bundles get their number, name
// and items there, the showcases the item and bundle (`owner`) they show - without it every shelf stood empty
// (playtest 6). An `owner` set there is the room's id of its bundle, so it is turned into the id the bundle has here.
void RunCreationCode(const std::vector<std::pair<const Placed*, int>>& coded) {
    for (const auto& [placed, id] : coded) {
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance != nullptr) yyc::At<yyc::EventFn>(placed->creationCode)(instance, instance);
    }
    for (const auto& [placed, id] : coded) {
        yyc::RValue owner{};
        if (!yyc::GetVar(id, kSlotOwner, &owner) || !yyc::IsNumber(owner)) continue;
        const int roomId = static_cast<int>(yyc::ToReal(owner));
        for (const auto& [other, otherId] : coded) {
            if (other->roomId != roomId) continue;
            yyc::RValue here = yyc::Real(otherId);
            yyc::SetVar(id, kSlotOwner, &here);
            break;
        }
    }
}

// The interior `plan` past the world's edge, and the player in it: the world room stays, so the world goes on.
// The interior's layers and instances past the world's edge.
void Build(yyc::CInstance* self, const Plan& plan) {
    g_self = self;
    for (const Background& b : plan.backgrounds) {
        const double layer = Real("layer_create", {double(b.depth)});
        g_layers.push_back(layer);
        const double background = Real("layer_background_create", {layer, double(b.sprite)});
        Real("layer_background_blend", {background, double(b.colour)});
        Real("layer_background_htiled", {background, b.htiled ? 1.0 : 0.0});
        Real("layer_background_vtiled", {background, b.vtiled ? 1.0 : 0.0});
    }
    for (const Picture& picture : plan.pictures) {
        const double layer = Real("layer_create", {double(picture.depth)});
        g_layers.push_back(layer);
        Real("layer_sprite_create", {layer, kOriginX + picture.x, kOriginY + picture.y, double(picture.sprite)});
    }
    for (const Tiles& t : plan.tiles) {
        const double layer = Real("layer_create", {double(t.depth)});
        g_layers.push_back(layer);
        const double map = Real("layer_tilemap_create", {layer, kOriginX, kOriginY, double(t.tileset), double(t.cols), double(t.rows)});
        for (int y = 0; y < t.rows; ++y) {
            for (int x = 0; x < t.cols; ++x) {
                const uint16_t cell = t.cells[y * t.cols + x];
                if (cell != 0) Real("tilemap_set", {map, double(cell), double(x), double(y)});
            }
        }
    }
    g_building = true;
    std::vector<std::pair<const Placed*, int>> coded;  // instances with creation code, and their ids here
    for (const Placed& p : plan.instances) {
        const int id = static_cast<int>(Real("instance_create_depth", {kOriginX + p.x, kOriginY + p.y, 0, double(p.object)}));
        yyc::CInstance* instance = yyc::FindInstance(id);
        if (instance == nullptr) continue;
        yyc::SetBuiltinReal(instance, yyc::va::YYVar_image_xscale, p.xscale);
        yyc::SetBuiltinReal(instance, yyc::va::YYVar_image_yscale, p.yscale);
        g_built.push_back(id);
        if (p.creationCode != 0) coded.emplace_back(&p, id);
    }
    RunCreationCode(coded);
    g_building = false;
}

// Its instances (without their destroy events: a chest would drop its loot) and layers go.
void Teardown(yyc::CInstance* self) {
    g_self = self;
    for (const int id : g_built) {
        if (yyc::FindInstance(id) != nullptr) Real("instance_destroy", {double(id), 0});
    }
    g_built.clear();
    for (const double layer : g_layers) Real("layer_destroy", {layer});
    g_layers.clear();
}

void Enter(yyc::CInstance* player, const Plan& plan) {
    Build(player, plan);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_x, kOriginX + plan.spawnX);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_y, kOriginY + plan.spawnY);
    g_inside = &plan;
    Log("interiors: in room %d inside the world room (%zu objects)", plan.room, g_built.size());
}

// Back out at the door the world keeps for the player (spawnPlayerX/Y, set when it went in); `move`: false when
// the player is already out (died: respawned on the start island).
void Leave(yyc::CInstance* player, bool move = true) {
    Teardown(player);
    if (move) {
        yyc::SetBuiltinReal(player, yyc::va::YYVar_x, GlobalReal(kSlotSpawnPlayerX));
        yyc::SetBuiltinReal(player, yyc::va::YYVar_y, GlobalReal(kSlotSpawnPlayerY));
    }
    Log("interiors: left room %d for the world at (%.0f, %.0f)", g_inside != nullptr ? g_inside->room : -1,
        GlobalReal(kSlotSpawnPlayerX), GlobalReal(kSlotSpawnPlayerY));
    g_inside = nullptr;
}

bool InsideRect(double x, double y) {
    return g_inside != nullptr && x >= kOriginX && y >= kOriginY && x <= kOriginX + g_inside->width &&
           y <= kOriginY + g_inside->height;
}

// Inside, nothing is water (past the world's edge everything is).
yyc::RValue* __cdecl WaterHook(yyc::CInstance* self, yyc::CInstance* other, yyc::RValue* result, int argc,
                               yyc::RValue** args) {
    if (g_inside != nullptr && argc >= 2 && InsideRect(yyc::ToReal(*args[0]), yyc::ToReal(*args[1]))) {
        yyc::Free(result);
        *result = yyc::Real(0);
        return result;
    }
    return g_origWater(self, other, result, argc, args);
}

}  // namespace

bool Enabled() {
    // In a session interiors are built in the world room; COOP_ROOM_INTERIORS=1 goes back to the game's own rooms.
    static const bool enabled = GetEnvironmentVariableA("COOP_ROOM_INTERIORS", nullptr, 0) == 0;
    return enabled;
}

bool InstallHooks(bool (*inSession)()) {
    g_inSession = inSession;
    const bool ok = MH_CreateHook(yyc::At<void*>(kScriptWaterCollision), reinterpret_cast<void*>(&WaterHook),
                                  reinterpret_cast<void**>(&g_origWater)) == MH_OK;
    if (!ok) Log("interiors: hooks failed");
    return ok;
}

bool InterceptRoomGoto(yyc::CInstance* self, int room) {
    if (!Enabled() || g_inSession == nullptr || !g_inSession()) return false;
    yyc::CInstance* player = g_playerId != 0 ? yyc::FindInstance(g_playerId) : nullptr;
    if (player == nullptr) return false;
    const int current = static_cast<int>(yyc::GetBuiltinReal(player, yyc::va::YYVar_room));
    if (current != kRoomGame) return false;
    if (g_inside == nullptr) {
        const Plan* plan = PlanFor(room);
        if (plan == nullptr) return false;
        Enter(player, *plan);
        return true;
    }
    if (room == kRoomGame) {
        Leave(player);
        return true;
    }
    return false;
}

void AfterWorldEndStep(yyc::CInstance* self) {
    if (g_inside == nullptr || self == nullptr) return;
    yyc::CInstance* player = g_playerId != 0 ? yyc::FindInstance(g_playerId) : nullptr;
    if (player == nullptr) return;
    g_self = self;
    if (!InsideRect(yyc::GetBuiltinReal(player, yyc::va::YYVar_x), yyc::GetBuiltinReal(player, yyc::va::YYVar_y))) {
        Leave(player, false);  // out without the door (died and respawned): the interior goes
        return;
    }
    const double camera = Real("view_get_camera", {0});
    const double width = Real("camera_get_view_width", {camera});
    const double height = Real("camera_get_view_height", {camera});
    const double left = yyc::GetBuiltinReal(player, yyc::va::YYVar_x) - width / 2;
    const double top = yyc::GetBuiltinReal(player, yyc::va::YYVar_y) - height / 2;
    Real("camera_set_view_pos", {camera, left, top});
    // What the game culls by (CullObject): else the interior's objects would be put to sleep as off-screen.
    const std::pair<int, double> bounds[] = {
        {kSlotCameraLeft, left}, {kSlotCameraRight, left + width}, {kSlotCameraTop, top}, {kSlotCameraBottom, top + height}};
    for (const auto& [slot, value] : bounds) {
        if (yyc::RValue* global = yyc::GlobalVar(slot)) {
            yyc::Free(global);
            *global = yyc::Real(value);
        }
    }
}

// A world save while inside: for its time the interior is taken down and the player stands at the door, so the save
// has the world as if the player had not gone in; then the interior is put back.
double g_savedX = 0, g_savedY = 0;

void BeforeSave() {
    yyc::CInstance* player = g_inside != nullptr && g_playerId != 0 ? yyc::FindInstance(g_playerId) : nullptr;
    if (player == nullptr) return;
    g_savedX = yyc::GetBuiltinReal(player, yyc::va::YYVar_x);
    g_savedY = yyc::GetBuiltinReal(player, yyc::va::YYVar_y);
    Teardown(player);  // the save takes layer sprites and instances: none of the interior's may be there
    yyc::SetBuiltinReal(player, yyc::va::YYVar_x, GlobalReal(kSlotSpawnPlayerX));
    yyc::SetBuiltinReal(player, yyc::va::YYVar_y, GlobalReal(kSlotSpawnPlayerY));
}

void AfterSave() {
    yyc::CInstance* player = g_inside != nullptr && g_playerId != 0 ? yyc::FindInstance(g_playerId) : nullptr;
    if (player == nullptr) return;
    Build(player, *g_inside);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_x, g_savedX);
    yyc::SetBuiltinReal(player, yyc::va::YYVar_y, g_savedY);
    Log("interiors: the world saved without the interior, as if the player were at the door");
}

bool IsBuilding() { return g_building; }

bool IsInside() { return g_inside != nullptr; }

bool IsInterior(int id) {
    for (const int built : g_built) {
        if (built == id) return true;
    }
    return false;
}

void Reset() {
    g_inside = nullptr;
    g_built.clear();
    g_layers.clear();
}

void DebugTick(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_DUMP_ROOMS", nullptr, 0) > 0;
    static std::set<int> dumped;
    static int lastRoom = -1;
    static unsigned stepsInRoom = 0;
    if (localPlayer != nullptr) g_playerId = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_id));
    if (!enabled || localPlayer == nullptr) return;
    const int room = static_cast<int>(yyc::GetBuiltinReal(localPlayer, yyc::va::YYVar_room));
    stepsInRoom = room == lastRoom ? stepsInRoom + 1 : 0;
    lastRoom = room;
    if (room < kFirstInterior || room > kLastInterior || stepsInRoom != kDumpAfterSteps || dumped.count(room) > 0) {
        return;
    }
    dumped.insert(room);
    g_self = localPlayer;
    Log("interiors: === room %d ===", room);
    DumpView();
    DumpInstances();
    DumpLayers();
    Log("interiors: === end of room %d ===", room);
}

}  // namespace coop::interiors
