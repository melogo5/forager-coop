// Dumps data.win metadata (no code — YYC build) into text files.
// Output dir is taken from the FORAGER_DUMP_DIR environment variable.
using System.IO;
using System.Linq;
using System.Text;
using UndertaleModLib.Models;

string outDir = Environment.GetEnvironmentVariable("FORAGER_DUMP_DIR");
Directory.CreateDirectory(outDir);

void Write(string file, IEnumerable<string> lines) =>
    File.WriteAllLines(Path.Combine(outDir, file), lines, new UTF8Encoding(false));

Write("objects.tsv", new[] { "index\tname\tparent\tsprite\tmask\tvisible\tsolid\tpersistent\tdepth\tevents" }
    .Concat(Data.GameObjects.Select((o, i) =>
    {
        var events = Enumerable.Range(0, o.Events.Count)
            .SelectMany(t => o.Events[t].Select(e => $"{(EventType)t}_{e.EventSubtype}"));
        return string.Join("\t", i, o.Name?.Content, o.ParentId?.Name?.Content ?? "-",
            o.Sprite?.Name?.Content ?? "-", o.TextureMaskId?.Name?.Content ?? "-",
            o.Visible, o.Solid, o.Persistent, o.Depth, string.Join(",", events));
    })));

Write("rooms.tsv", new[] { "index\tname\twidth\theight\tpersistent\tinstances" }
    .Concat(Data.Rooms.Select((r, i) =>
    {
        var counts = r.GameObjects.GroupBy(g => g.ObjectDefinition?.Name?.Content ?? "?")
            .Select(g => $"{g.Key}x{g.Count()}");
        return string.Join("\t", i, r.Name?.Content, r.Width, r.Height, r.Persistent, string.Join(",", counts));
    })));

Write("extensions.txt", Data.Extensions.SelectMany(e =>
    new[] { $"# {e.Name?.Content}" }.Concat(e.Files.SelectMany(f =>
        new[] { $"  file {f.Filename?.Content}" }.Concat(f.Functions.Select(fn =>
            $"    {fn.Name?.Content} -> {fn.ExtName?.Content} ({string.Join(",", fn.Arguments.Select(a => a.Type))}) : {fn.RetType}"))))));

Write("sprites.tsv", Data.Sprites.Select((s, i) => $"{i}\t{s.Name?.Content}\t{s.Width}x{s.Height}\t{s.Textures.Count}"));
Write("sounds.tsv", Data.Sounds.Select((s, i) => $"{i}\t{s.Name?.Content}\t{s.File?.Content}"));
Write("scripts.tsv", Data.Scripts.Select((s, i) => $"{i}\t{s.Name?.Content}"));
Write("strings.txt", Data.Strings.Select(s => s.Content.Replace("\r", "\\r").Replace("\n", "\\n")));
Write("general.txt", new[] {
    $"Name: {Data.GeneralInfo.Name?.Content}",
    $"DisplayName: {Data.GeneralInfo.DisplayName?.Content}",
    $"Version: {Data.GeneralInfo.Major}.{Data.GeneralInfo.Minor}.{Data.GeneralInfo.Release}.{Data.GeneralInfo.Build}",
    $"Config: {Data.GeneralInfo.Config?.Content}",
    $"Timestamp: {Data.GeneralInfo.Timestamp}",
    $"SteamAppID: {Data.GeneralInfo.SteamAppID}",
});
ScriptMessage("Dump complete: " + outDir);
