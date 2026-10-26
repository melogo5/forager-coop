// Dumps rooms in full (layers with their tiles, backgrounds, instances, views) from data.win - the interiors
// (rooms 22-24) for building them inside the world room (docs/coop/INTERIORS.md).
// Output: $FORAGER_DUMP_DIR/rooms_full.txt. Rooms: $FORAGER_DUMP_ROOMS ("22,23,24").
using System.IO;
using System.Linq;
using System.Text;
using UndertaleModLib.Models;

string outDir = Environment.GetEnvironmentVariable("FORAGER_DUMP_DIR");
var wanted = (Environment.GetEnvironmentVariable("FORAGER_DUMP_ROOMS") ?? "22,23,24").Split(',').Select(int.Parse).ToList();
Directory.CreateDirectory(outDir);
var o = new StringBuilder();
foreach (int index in wanted) {
    var r = Data.Rooms[index];
    o.AppendLine($"room {index} {r.Name?.Content} {r.Width}x{r.Height} colour {r.BackgroundColor:X8} drawColour {r.DrawBackgroundColor} creation {r.CreationCodeId?.Name?.Content ?? "-"} flags {r.Flags}");
    for (int v = 0; v < r.Views.Count; v++) {
        var w = r.Views[v];
        if (w.Enabled) o.AppendLine($"  view {v}: at ({w.ViewX}, {w.ViewY}) {w.ViewWidth}x{w.ViewHeight} port {w.PortWidth}x{w.PortHeight} follow {w.ObjectId?.Name?.Content ?? "-"}");
    }
    foreach (var layer in r.Layers) {
        o.AppendLine($"  layer '{layer.LayerName?.Content}' depth {layer.LayerDepth} type {layer.LayerType} visible {layer.IsVisible} offset ({layer.XOffset}, {layer.YOffset}) speed ({layer.HSpeed}, {layer.VSpeed})");
        if (layer.TilesData != null) {
            var t = layer.TilesData;
            var ts = t.Background;
            o.AppendLine($"    tiles: tileset {Data.Backgrounds.IndexOf(ts)} {ts?.Name?.Content} tile {ts?.GMS2TileWidth}x{ts?.GMS2TileHeight} columns {ts?.GMS2TileColumns} border {ts?.GMS2OutputBorderX}; {t.TilesX}x{t.TilesY} cells");
            for (int y = 0; y < t.TilesY; y++) o.AppendLine("    row " + string.Join(" ", t.TileData[y].Select(c => c.ToString())));
        }
        if (layer.BackgroundData != null) {
            var b = layer.BackgroundData;
            o.AppendLine($"    background: sprite {b.Sprite?.Name?.Content ?? "-"} colour {b.Color:X8} tiled {b.TiledHorizontally}/{b.TiledVertically} stretch {b.Stretch} visible {b.Visible} foreground {b.Foreground}");
        }
        if (layer.AssetsData != null) {
            foreach (var s in layer.AssetsData.Sprites ?? new()) o.AppendLine($"    sprite {s.Sprite?.Name?.Content} at ({s.X}, {s.Y}) scale ({s.ScaleX}, {s.ScaleY}) frame {s.FrameIndex}");
            foreach (var lt in layer.AssetsData.LegacyTiles ?? new()) o.AppendLine($"    legacy tile {lt.BackgroundDefinition?.Name?.Content} at ({lt.X}, {lt.Y}) src ({lt.SourceX}, {lt.SourceY}) {lt.Width}x{lt.Height}");
        }
        if (layer.InstancesData != null) {
            foreach (var g in layer.InstancesData.Instances) o.AppendLine($"    instance {g.ObjectDefinition?.Name?.Content} at ({g.X}, {g.Y}) scale ({g.ScaleX}, {g.ScaleY}) rotation {g.Rotation} creation {g.CreationCode?.Name?.Content ?? "-"} pre {g.PreCreateCode?.Name?.Content ?? "-"} id {g.InstanceID}");
        }
    }
}
File.WriteAllText(Path.Combine(outDir, "rooms_full.txt"), o.ToString(), new UTF8Encoding(false));
