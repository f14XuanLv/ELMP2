// Regenerates the item catalog, icons and fonts from the game paks (dev tool; outputs are committed and shipped).
//   data/catalog/items.json  spirits / passives: localized name + short explanation (5 cultures), spirit type,
//                            passive slot cost and bAvailableInGame, icon file
//   data/icons/<kind>_<id>.png, ui_mouse_cursor.png  (PaperSprite crops of the icon atlases)
//   data/fonts/*.otf|ttf     the game's composite-font faces (FOT-PearlStd-L + chinese-traditional / korean / russian)
// Usage: dotnet run -- --paks <Content\Paks> --mappings <.usmap> --repo <repo root> [--size 128]
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using CUE4Parse.FileProvider;
using CUE4Parse.MappingsProvider.Usmap;
using CUE4Parse.UE4.Assets.Objects;
using CUE4Parse.UE4.Assets.Exports.Engine;
using CUE4Parse.UE4.Assets.Exports.Internationalization;
using CUE4Parse.UE4.Assets.Exports.Texture;
using CUE4Parse.UE4.Localization;
using CUE4Parse.UE4.Objects.Core.i18N;
using CUE4Parse.UE4.Objects.Core.Math;
using CUE4Parse.UE4.Objects.UObject;
using CUE4Parse.UE4.Versions;
using CUE4Parse_Conversion.Textures;
using SkiaSharp;

var options = ParseArgs(args);
var paks = options.GetValueOrDefault("paks") ?? throw new ArgumentException("--paks is required");
var mappings = options.GetValueOrDefault("mappings") ?? throw new ArgumentException("--mappings is required");
var repo = options.GetValueOrDefault("repo") ?? throw new ArgumentException("--repo is required");
var size = int.Parse(options.GetValueOrDefault("size") ?? "128");
var pathOf = new Dictionary<string, string>();

var provider = new DefaultFileProvider(paks, SearchOption.TopDirectoryOnly, true, new VersionContainer(EGame.GAME_UE4_26));
provider.MappingsContainer = new FileUsmapTypeMappingsProvider(mappings);
provider.Initialize();
provider.Mount();

// --triggers: list every blueprint class deriving from /Script/Zenith.Trigger (input for data/gates.json) and exit.
if (options.ContainsKey("triggers"))
{
    Console.OutputEncoding = System.Text.Encoding.UTF8;
    var parents = new Dictionary<string, string>();
    foreach (var path in provider.Files.Keys.Where(k => k.EndsWith(".uasset") && k.Contains("/_Zenith/")).ToList())
    {
        try
        {
            var pkg = provider.LoadPackage(path);
            foreach (var export in pkg.GetExports())
            {
                if (export is not CUE4Parse.UE4.Objects.Engine.UBlueprintGeneratedClass cls) continue;
                var super = cls.SuperStruct?.ResolvedObject;
                parents[cls.Name] = super?.Name.Text ?? "";
                if (super != null && super.Class?.Name.Text == "Class") parents.TryAdd(super.Name.Text, "<native:" + (super.Outer?.Name.Text ?? "") + ">");
                if (parents.Count % 500 == 0) Console.Error.Write(".");
                pathOf[cls.Name] = path;
            }
        }
        catch { }
    }
    bool IsTrigger(string name, int depth = 0)
    {
        if (depth > 20 || !parents.TryGetValue(name, out var p)) return false;
        if (p == "Trigger") return true;
        return IsTrigger(p, depth + 1);
    }
    foreach (var name in parents.Keys.Where(n => n.EndsWith("_C") && IsTrigger(n)).OrderBy(n => n, StringComparer.Ordinal))
        Console.WriteLine($"{name}\t{parents[name]}\t{pathOf.GetValueOrDefault(name)}");
    return 0;
}

// --imports <name>: list every package importing an object with this exact name (e.g. a UFunction such as
// PlayEventAttached), for research — which blueprints call a native entry.
if (options.TryGetValue("imports", out var importName))
{
    Console.OutputEncoding = System.Text.Encoding.UTF8;
    var n = 0;
    foreach (var path in provider.Files.Keys.Where(k => k.EndsWith(".uasset") || k.EndsWith(".umap")).ToList())
    {
        try
        {
            var pkg = provider.LoadPackage(path);
            var hit = false;
            if (pkg is CUE4Parse.UE4.Assets.IoPackage io)
                foreach (var idx in io.ImportMap)
                    hit |= io.ResolveObjectIndex(idx)?.Name.Text == importName;
            else if (pkg is CUE4Parse.UE4.Assets.Package legacy)
                foreach (var imp in legacy.ImportMap)
                    hit |= imp.ObjectName.Text == importName;
            if (hit) Console.WriteLine(path);
            if (++n % 1000 == 0) Console.Error.Write(".");
        }
        catch { }
    }
    return 0;
}

// --dump <file name> --out <dir>: write every matching package (exports + blueprint bytecode) as JSON, for research.
if (options.TryGetValue("dump", out var dumpName))
{
    var outDir = options.GetValueOrDefault("out") ?? throw new ArgumentException("--out is required with --dump");
    Directory.CreateDirectory(outDir);
    provider.ReadScriptData = true;
    foreach (var path in provider.Files.Keys.Where(k => k.EndsWith(".uasset") && Path.GetFileNameWithoutExtension(k).Equals(dumpName, StringComparison.OrdinalIgnoreCase)))
    {
        var dumped = Newtonsoft.Json.JsonConvert.SerializeObject(provider.LoadPackage(path).GetExports(), Newtonsoft.Json.Formatting.Indented);
        var file = Path.Combine(outDir, Path.GetFileNameWithoutExtension(path) + ".json");
        File.WriteAllText(file, dumped);
        Console.WriteLine($"{path} -> {file}");
    }
    return 0;
}

var iconDir = Path.Combine(repo, "data", "icons");
var fontDir = Path.Combine(repo, "data", "fonts");
var catalogDir = Path.Combine(repo, "data", "catalog");
foreach (var d in new[] { iconDir, fontDir, catalogDir }) Directory.CreateDirectory(d);
var failures = new List<string>();
var atlases = new Dictionary<string, SKBitmap>();

string[] cultures = ["en-US", "zh-Hans", "zh-Hant", "ja-JP", "ko-KR"];
var localized = new Dictionary<string, Dictionary<(string, string), string>>();
foreach (var culture in cultures)
{
    // The paks declare no CulturesToStage, so CUE4Parse's culture switch is unavailable: read the locres directly.
    var path = $"EnderLilies/Content/Localization/StringTables/{culture}/StringTables.locres";
    if (!provider.Files.TryGetValue(path, out var file)) throw new FileNotFoundException(path);
    var locres = new FTextLocalizationResource(file.CreateReader());
    var map = new Dictionary<(string, string), string>();
    foreach (var (ns, entries) in locres.Entries)
        foreach (var (key, entry) in entries) map[(ns.Str, key.Str)] = entry.LocalizedString;
    localized[culture] = map;
}

JsonObject Text(FStructFallback row, string prop)
{
    var o = new JsonObject();
    if (!row.TryGetValue(out FText text, prop)) return o;
    if (text.TextHistory is FTextHistory.StringTableEntry e && provider.TryLoadPackageObject<UStringTable>(e.TableId.Text, out var st))
    {
        var ns = st.StringTable.TableNamespace;
        foreach (var c in cultures) o[c] = localized[c].TryGetValue((ns, e.Key), out var v) ? v : e.SourceString ?? "";
    }
    else foreach (var c in cultures) o[c] = text.Text;
    return o;
}

var tables = new[] {
    ("spirits", "EnderLilies/Content/_Zenith/Gameplay/Data/DT_Items_Spirits.DT_Items_Spirits"),
    ("passives", "EnderLilies/Content/_Zenith/Gameplay/Data/DT_Items_Passives.DT_Items_Passives"),
};
var root = new JsonObject { ["schema"] = 1, ["generator"] = "tools/catalog", ["cultures"] = new JsonArray(cultures.Select(c => (JsonNode)c).ToArray()) };
foreach (var (kind, table) in tables)
{
    var rows = new JsonObject();
    foreach (var (rowName, row) in provider.LoadPackageObject<UDataTable>(table).RowMap.OrderBy(r => r.Key.Text, StringComparer.Ordinal))
    {
        var id = rowName.Text;
        var e = new JsonObject { ["name"] = Text(row, "Name"), ["desc"] = Text(row, "ShortExplanation") };
        if (kind == "spirits")
        {
            e["type"] = row.TryGetValue(out FName t, "ItemSpiritType") ? t.Text : "";
            e["boss"] = row.TryGetValue(out bool boss, "bBossSpirit") && boss;
        }
        else
        {
            e["slots"] = row.TryGetValue(out int slots, "SlotCount") ? slots : 0;
            e["available"] = row.TryGetValue(out bool available, "bAvailableInGame") && available;
        }
        if (Icon(row, $"{kind}_{id}") is { } icon) e["icon"] = icon;
        rows[id] = e;
    }
    root[kind] = rows;
    Console.WriteLine($"{kind}: {rows.Count} rows");
}
// UI sprites at native size (the game's software mouse cursor, drawn again above ELMP windows).
UiSprite("mouse_cursor", "/Game/_Zenith/UI/Textures/Atlas-Common/T_MouseCursor.T_MouseCursor");

var json = root.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
File.WriteAllText(Path.Combine(catalogDir, "items.json"), json.Replace("\r\n", "\n") + "\n", new System.Text.UTF8Encoding(false));

foreach (var face in new[] { "FOT-PearlStd-L", "chinese-traditional", "korean", "russian" })
{
    var path = $"EnderLilies/Content/_Zenith/Fonts/{face}.ufont";
    if (!provider.Files.TryGetValue(path, out var file)) { failures.Add($"font {face}: not found"); continue; }
    var bytes = file.Read();
    var otf = bytes.Length >= 4 && System.Text.Encoding.ASCII.GetString(bytes, 0, 4) == "OTTO";
    File.WriteAllBytes(Path.Combine(fontDir, $"{face}.{(otf ? "otf" : "ttf")}"), bytes);
    Console.WriteLine($"font {face}: {bytes.Length} bytes");
}
foreach (var f in failures) Console.WriteLine($"skipped {f}");
return failures.Count == 0 ? 0 : 2;

string? Icon(FStructFallback row, string file)
{
    if (!row.TryGetValue(out FSoftObjectPath icon, "Icon") || icon.AssetPathName.IsNone) { failures.Add($"{file}: no icon"); return null; }
    try
    {
        using var crop = Crop(icon.AssetPathName.Text);
        var scale = Math.Min(1.0, (double)size / Math.Max(crop.Width, crop.Height));
        using var scaled = crop.Resize(new SKImageInfo(Math.Max(1, (int)Math.Round(crop.Width * scale)), Math.Max(1, (int)Math.Round(crop.Height * scale))), SKFilterQuality.High);
        Save(scaled, file);
        return file;
    }
    catch (Exception e) { failures.Add($"{file}: {e.Message}"); return null; }
}

void UiSprite(string name, string sprite)
{
    try { using var crop = Crop(sprite); Save(crop, $"ui_{name}"); }
    catch (Exception e) { failures.Add($"ui_{name}: {e.Message}"); }
}

// Icons are PaperSprites: BakedSourceTexture (atlas) + BakedSourceUV / BakedSourceDimension (pixel rect).
SKBitmap Crop(string spritePath)
{
    var sprite = provider.LoadPackageObject(spritePath);
    var atlasRef = sprite.GetOrDefault<FPackageIndex>("BakedSourceTexture") ?? throw new InvalidOperationException("sprite has no atlas");
    var uv = sprite.GetOrDefault<FVector2D>("BakedSourceUV");
    var dim = sprite.GetOrDefault<FVector2D>("BakedSourceDimension");
    var key = atlasRef.ToString();
    if (!atlases.TryGetValue(key, out var atlas))
    {
        var texture = atlasRef.Load<UTexture2D>() ?? throw new InvalidOperationException("atlas texture not found");
        atlas = (texture.Decode() ?? throw new InvalidOperationException("atlas decode failed")).ToSkBitmap();
        atlases[key] = atlas;
    }
    var rect = SKRectI.Create((int)uv.X, (int)uv.Y, (int)dim.X, (int)dim.Y);
    var crop = new SKBitmap(rect.Width, rect.Height);
    if (!atlas.ExtractSubset(crop, rect)) throw new InvalidOperationException($"bad sprite rect {rect}");
    return crop;
}

void Save(SKBitmap bitmap, string name)
{
    using var image = SKImage.FromBitmap(bitmap);
    using var data = image.Encode(SKEncodedImageFormat.Png, 100);
    File.WriteAllBytes(Path.Combine(iconDir, name + ".png"), data.ToArray());
}

static Dictionary<string, string> ParseArgs(string[] args)
{
    var result = new Dictionary<string, string>();
    for (var i = 0; i + 1 < args.Length; i += 2)
    {
        if (!args[i].StartsWith("--")) throw new ArgumentException($"unexpected argument {args[i]}");
        result[args[i][2..]] = args[i + 1];
    }
    return result;
}
