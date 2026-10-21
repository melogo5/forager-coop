#include "interiors.h"

#include <windows.h>

#include <initializer_list>
#include <set>
#include <string>
#include <vector>

#include "log.h"

namespace coop::interiors {

namespace {

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

}  // namespace

void DebugTick(yyc::CInstance* localPlayer) {
    static const bool enabled = GetEnvironmentVariableA("COOP_DEBUG_DUMP_ROOMS", nullptr, 0) > 0;
    static std::set<int> dumped;
    static int lastRoom = -1;
    static unsigned stepsInRoom = 0;
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
