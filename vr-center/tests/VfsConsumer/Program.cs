using System.Text.Json;
string game = args[0], report = args[1];
try
{
    var before = new {
        conflict = File.ReadAllText(Path.Combine(game, "conflict.txt")),
        stock = File.ReadAllText(Path.Combine(game, "stock.txt")),
        low = File.Exists(Path.Combine(game, "low-only.txt")) ? File.ReadAllText(Path.Combine(game, "low-only.txt")) : null,
        sharedArchive = File.Exists(Path.Combine(game, "archive/pc/mod/shared.archive")) ? File.ReadAllText(Path.Combine(game, "archive/pc/mod/shared.archive")) : null,
        overwrite = File.ReadAllText(Path.Combine(game, "existing.txt")),
        mutable = File.Exists(Path.Combine(game,"settings.txt")) ? File.ReadAllText(Path.Combine(game,"settings.txt")) : null,
        hidden = File.Exists(Path.Combine(game, "hidden.txt.mohidden")),
        nestedNative = File.Exists(Path.Combine(game, "bin/x64/plugins/mods/vr/reload/poses/native.lua")) ? File.ReadAllText(Path.Combine(game, "bin/x64/plugins/mods/vr/reload/poses/native.lua")) : null
    };
    Directory.CreateDirectory(Path.Combine(game, "generated"));
    File.WriteAllText(Path.Combine(game, "generated", "new.txt"), "new-profile-data");
    if(args.Length>2)File.WriteAllText(Path.Combine(game,"settings.txt"),args[2]);
    File.WriteAllText(report, JsonSerializer.Serialize(before));
    return 0;
}
catch(Exception e)
{
    File.WriteAllText(report, JsonSerializer.Serialize(new {error=e.ToString()}));
    return 1;
}
