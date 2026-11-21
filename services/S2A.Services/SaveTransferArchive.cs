using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.IO.Hashing;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;

namespace S2A.Services;

public static partial class SaveTransfer
{
    private sealed class Archive : IDisposable
    {
        internal readonly FileStream Source;
        internal readonly ZipArchive Zip;
        internal readonly JsonObject Manifest;
        internal readonly string Sha;
        internal Archive(string path)
        {
            Source = ReadFile(path);
            try
            {
                Require(Source.Length <= MaxBytes + (64 << 20), T("备份文件过大。", "The backup file is too large."));
                var directory = ZipBounds(Source);
                Sha = Hash(Source);
                Source.Position = 0;
                Zip = new ZipArchive(Source, ZipArchiveMode.Read, leaveOpen: true, entryNameEncoding: Encoding.UTF8);
                try
                {
                    Require(Zip.Entries.Count == directory.Count, T("ZIP 目录数量发生变化。", "The number of ZIP entries changed."));
                    for (int i = 0; i < directory.Count; i++)
                    {
                        var expected = directory[i];
                        var entry = Zip.Entries[i];
                        Require(entry.FullName == expected.Name && entry.Length == (long)expected.Size &&
                            entry.CompressedLength == (long)expected.Compressed && entry.Crc32 == expected.Crc &&
                            (uint)entry.ExternalAttributes == expected.Attributes, T("ZIP 公开目录解析与预校验不一致。", "The ZIP directory doesn’t match the pre-check."));
                    }
                    Manifest = Validate(this);
                    Source.Position = 0;
                    Require(Hash(Source) == Sha, T("检查期间备份发生变化。", "The backup changed while it was being checked."), "save_changed");
                }
                catch { Zip.Dispose(); throw; }
            }
            catch { Source.Dispose(); throw; }
        }
        public void Dispose() { Zip.Dispose(); Source.Dispose(); }
    }
    private static byte[] EntryBytes(Archive archive, string name)
    {
        var entry = archive.Zip.GetEntry(name);
        Require(entry != null && entry.Length <= MaxMetadata, T("备份元数据缺失或过大。", "Backup metadata is missing or too large."));
        using var stream = entry.Open();
        using var memory = new MemoryStream();
        CopyEntry(stream, memory, entry.Length, null, entry.Crc32);
        return memory.ToArray();
    }
    private static void CopyEntry(Stream source, Stream destination, long length, string expectedHash, uint expectedCrc)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var crc = new Crc32();
        byte[] buffer = new byte[1 << 20];
        long count = 0;
        int size;
        while ((size = source.Read(buffer)) != 0)
        {
            count = checked(count + size);
            Require(count <= length, T("备份内容超过声明大小。", "The backup content exceeds its declared size."));
            hash.AppendData(buffer, 0, size);
            crc.Append(buffer.AsSpan(0, size));
            destination.Write(buffer, 0, size);
        }
        Require(count == length && crc.GetCurrentHashAsUInt32() == expectedCrc &&
            (expectedHash == null || Convert.ToHexStringLower(hash.GetHashAndReset()) == expectedHash), T("备份文件 CRC 或 SHA-256 校验失败。", "The backup file failed the CRC or SHA-256 check."));
    }
    private static JsonObject Validate(Archive archive)
    {
        var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in archive.Zip.Entries)
        {
            Name(entry.FullName);
            int type = (entry.ExternalAttributes >> 16) & 0xf000;
            Require(names.Add(entry.FullName) && !entry.IsEncrypted && type is 0 or 0x8000 &&
                (entry.ExternalAttributes & (int)FileAttributes.Directory) == 0 && entry.Length >= 0, T("ZIP 含重复路径、链接、目录、加密或特殊条目。", "The ZIP contains duplicate paths, links, folders, encrypted or special entries."));
        }
        var node = Parse(EntryBytes(archive, "manifest.json"));
        Require(node is JsonObject && Str(node, "format") == "S2ArmSave" && node["schema"].GetValue<int>() == 1 &&
            Str(node, "layout") == "ryujinx-data-v1", T("不支持此存档格式或版本。", "This save format or version isn’t supported."));
        Require(node["files"] is JsonArray && node["directories"] is JsonArray, T("备份清单无效。", "Invalid backup manifest."));
        var files = node["files"].AsArray();
        var dirs = node["directories"].AsArray();
        Require(files.Count is > 0 and <= MaxFiles && dirs.Count <= MaxFiles, T("备份文件或目录数量无效。", "Invalid number of backup files or folders."));
        var paths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var fileNames = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var expected = new HashSet<string>(StringComparer.Ordinal) { "manifest.json" };
        long total = 0;
        foreach (var item in dirs)
        {
            string name = item.GetValue<string>();
            Name(name);
            Require(Allowed(name, true) && paths.Add(name), T("备份目录清单无效。", "Invalid backup folder list."));
        }
        foreach (var item in files)
        {
            string name = Str(item, "path");
            Name(name);
            long size = item["size"].GetValue<long>();
            string sha = Str(item, "sha256");
            Require(Allowed(name) && paths.Add(name) && size >= 0 && Hex(sha, 64), T("备份文件清单无效。", "Invalid backup file list."));
            fileNames.Add(name);
            expected.Add("payload/" + name);
            total = checked(total + size);
            var entry = archive.Zip.GetEntry("payload/" + name);
            Require(total <= MaxBytes && entry != null && entry.Length == size, T("备份大小或文件清单不符。", "The backup size or file list doesn’t match."));
        }
        foreach (string name in paths)
        {
            string parent = name;
            while (parent.Contains('/'))
            {
                parent = parent[..parent.LastIndexOf('/')];
                Require(!fileNames.Contains(parent), T("备份文件与目录冲突。", "A backup file conflicts with a folder."));
            }
        }
        Require(expected.SetEquals(archive.Zip.Entries.Select(e => e.FullName)) && fileNames.Contains(Profile) &&
            fileNames.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal)), T("备份缺少必要文件或含额外条目。", "The backup is missing required files or has extra entries."));
        // 维护者 11-17：个别文件 CRC / SHA 对不上不再整份拒绝——记进 damaged，恢复时不写它们、保留目标原来的那份，其余照常恢复。
        //   用户表与账号索引坏了没法知道哪份存档归谁，照旧拒绝
        var damaged = new JsonArray();
        foreach (var item in files)
        {
            string name = Str(item, "path");
            var entry = archive.Zip.GetEntry("payload/" + name);
            try
            {
                using var stream = entry.Open();
                CopyEntry(stream, Stream.Null, entry.Length, Str(item, "sha256"), entry.Crc32);
            }
            catch (Exception e) when ((e is Failure || e is InvalidDataException) && name != Profile && name != Index)
            {
                damaged.Add((JsonNode)JsonValue.Create(name));
            }
        }
        Profiles(EntryBytes(archive, "payload/" + Profile));
        var entries = Entries(name => EntryBytes(archive, "payload/" + name), files.Select(f => Str(f, "path")), dirs.Select(d => d.GetValue<string>()));
        if (node.AsObject().ContainsKey("saveEntries"))
            Require(JsonNode.DeepEquals(node["saveEntries"], entries) && JsonNode.DeepEquals(node["users"], Users(entries)), T("备份账号归属与 LibHac 索引不符。", "The backup’s account ownership doesn’t match the LibHac index."));
        node["saveEntries"] = entries;
        node["users"] = Users(entries);
        if (node.AsObject().ContainsKey("deviceSettings")) Require(node["deviceSettings"] is JsonObject, T("设备设置元信息无效。", "Invalid device settings metadata."));
        node["deviceSettings"] = Settings(node["deviceSettings"]);
        if (damaged.Count > 0) node["damaged"] = damaged;
        return (JsonObject)node;
    }
    // → 对不上但照常可恢复的几句提示（检查结果 warnings，确认框里给玩家看）。维护者 11-17：版本 / DLC 组合、账号归属对不上不再拒绝——
    //   玩家手动恢复（检查 → 看提示 → 确认）照常恢复，恢复前副本照留；请求带 sync（iCloud 自动同步：没人看提示、不经玩家确认）照旧拒绝，免得悄悄换掉进度。
    //   另一款游戏的备份照旧拒绝
    private static JsonArray Identity(Context c, JsonNode manifest)
    {
        bool sync = c.Request["sync"]?.GetValue<bool>() == true;
        bool unattended = sync || c.Request["unattendedRestore"]?.GetValue<bool>() == true;
        var warnings = new JsonArray();
        Require(string.Equals(Str(manifest["game"], "id"), Str(c.Game, "id"), StringComparison.OrdinalIgnoreCase), T("备份属于另一款游戏。", "The backup belongs to a different game."), "save_wrong_game");
        if (manifest["game"]["version"].GetValue<long>() != c.Game["version"].GetValue<long>() ||
            Str(manifest["game"], "contentIdentity") != Str(c.Game, "contentIdentity"))
        {
            Require(!sync, T("备份的游戏版本或 DLC 组合不符。", "The backup’s game version or DLC combination doesn’t match."), "save_version");
            warnings.Add((JsonNode)JsonValue.Create(c.Request["androidRevisionIdentity"]?.GetValue<bool>() == true
                ? T("无法用安卓资源版本核对备份的游戏版本与 DLC 组合。游戏可能读不到这份进度，恢复前的存档会自动保留一份副本。", "The Android resource revision can’t verify the backup’s game version and DLC. The game may not be able to read this progress. The save from before the restore is kept automatically.")
                : T("备份的游戏版本或 DLC 组合不符，仍可恢复；游戏可能读不到这份进度，恢复前的存档会自动保留一份副本。", "The backup’s game version or DLC combination doesn’t match. You can still restore it; the game may not be able to read this progress. The save from before the restore is kept automatically.")));
        }
        if (manifest["damaged"] is JsonArray damaged && damaged.Count > 0)
        {
            Require(!unattended, T("备份文件 CRC 或 SHA-256 校验失败。", "The backup file failed the CRC or SHA-256 check."), "save_damaged");
            string names = string.Join(", ", damaged.Take(5).Select(d => d.GetValue<string>())) + (damaged.Count > 5 ? " …" : "");
            warnings.Add((JsonNode)JsonValue.Create(string.Format(T("备份里有 {0} 个文件校验对不上（{1}），这几个不恢复、保留当前存档里原来的，其余照常恢复。", "{0} files in the backup failed the integrity check ({1}). Those files are not restored and the current save’s originals are kept; everything else is restored."), damaged.Count, names)));
        }
        var owners = OwnerSets(c, manifest);
        if (owners == null) return warnings;
        var (a, b) = owners.Value;
        // 本地玩家（#623）：一台设备上加了 / 删了玩家，两边的账号集合是包含关系（另一台设备还没建这个玩家、恢复建玩家之前的备份），照常恢复；
        //   两边各有对方没有的账号 = 不是同一套账号（原先要求完全相同，加了第二个玩家后 iCloud 另一端、旧备份都恢复不了）
        if (a.Keys.Intersect(b.Keys).Any(group => !(a[group].IsSubsetOf(b[group]) || b[group].IsSubsetOf(a[group]))))
        {
            Require(!unattended, T("备份与目标存档账号归属不同。", "The backup and the target save belong to different accounts."), "save_wrong_user");
            warnings.Add((JsonNode)JsonValue.Create(T("备份与目标存档账号归属不同，仍可恢复；恢复后当前账号的进度会换成备份里的，恢复前的存档会自动保留一份副本。", "The backup and the target save belong to different accounts. You can still restore it; the current account’s progress will be replaced with the backup’s. The save from before the restore is kept automatically.")));
        }
        return warnings;
    }
    // 目标（数据根现有）与备份两边的账号存档归属：组 = 程序号/类型，值 = 用户号集合。目标里没有存档 → null
    private static (Dictionary<string, HashSet<string>> target, Dictionary<string, HashSet<string>> backup)? OwnerSets(Context c, JsonNode manifest)
    {
        var scan = Scan(c.Root);
        if (!scan.Files.Keys.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal))) return null;
        var target = Entries(name => SmallFile(Path.Combine(c.Root, name)), scan.Files.Keys, scan.Directories);
        static Dictionary<string, HashSet<string>> Owners(JsonArray entries) => entries.GroupBy(e => Str(e, "programId") + "/" + Str(e, "type"))
            .ToDictionary(g => g.Key, g => g.Select(e => Str(e, "userId")).ToHashSet(StringComparer.Ordinal));
        return (Owners(target), Owners(manifest["saveEntries"].AsArray()));
    }
    // 审计 P1-178：恢复一份只含部分本地玩家的备份时，目标里有、备份里没有的账号用户号（全 0 的设备存档不算）。
    //   这些玩家的外部存档文件夹（SaveData-<玩家>）不在数据根里、恢复不动它；恢复时删掉他们的「上次读写摘要」，
    //   下一局摘要对不上，就按已有的 SeedFrom 把文件夹里他自己的进度读回来（不再看到新建的空档、退出也不会用空档盖掉文件夹）
    private static HashSet<string> UsersNotInBackup(Context c, JsonNode manifest)
    {
        var owners = OwnerSets(c, manifest);
        return owners == null ? new HashSet<string>(StringComparer.Ordinal) : DroppedUsers(owners.Value.target, owners.Value.backup);
    }
    // 纯集合：目标里有、备份同组里没有的用户号（全 0 = 设备存档，不算玩家）
    internal static HashSet<string> DroppedUsers(Dictionary<string, HashSet<string>> target, Dictionary<string, HashSet<string>> backup)
    {
        var dropped = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (group, users) in target)
            foreach (string u in users)
                if (u.Any(ch => ch != '0') && !(backup.TryGetValue(group, out var inBackup) && inBackup.Contains(u))) dropped.Add(u);
        return dropped;
    }
    private static string Source(Context c)
    {
        string path = Absolute(Str(c.Request, "path"));
        Require(!Within(path, c.Root) && (Str(c.Request, "operation") != "export" || !Within(path, c.Work)), T("请从数据与事务目录以外选择备份。", "Choose a backup outside the data and transaction folders."));
        return path;
    }
    private static JsonObject Inspect(Context c)
    {
        using var archive = new Archive(Source(c));
        var warnings = Identity(c, archive.Manifest);
        return new() { ["manifest"] = Copy(archive.Manifest), ["archiveSha256"] = archive.Sha, ["warnings"] = warnings,
            ["targetFingerprint"] = Fingerprint(c.Root), ["fileCount"] = archive.Manifest["files"].AsArray().Count,
            ["totalBytes"] = archive.Manifest["files"].AsArray().Sum(f => f["size"].GetValue<long>()) };
    }
    private static JsonObject MakeManifest(Context c, string root, out Snapshot scan)
    {
        scan = Scan(root);
        Require(scan.Files.Keys.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal)), T("此游戏尚无可备份存档。", "This game has no save to back up yet."), "save_empty");
        Profiles(SmallFile(Path.Combine(root, Profile)));
        var entries = Entries(name => SmallFile(Path.Combine(root, name)), scan.Files.Keys, scan.Directories);
        string platform = c.Request["androidRevisionIdentity"]?.GetValue<bool>() == true ? "android" : "ios";
        return new() { ["format"] = "S2ArmSave", ["schema"] = 1, ["layout"] = "ryujinx-data-v1", ["platform"] = platform,
            ["createdAt"] = DateTimeOffset.UtcNow.ToString("O"), ["game"] = Copy(c.Game), ["saveEntries"] = entries,
            ["users"] = Users(entries), ["deviceSettings"] = Settings(c.Request["deviceSettings"]),
            ["directories"] = new JsonArray(scan.Directories.Where(n => Exported(n, true)).Select(n => (JsonNode)JsonValue.Create(n)).ToArray()), ["files"] = new JsonArray() };
    }
    private static JsonObject WriteArchive(Context c, string root, string path)
    {
        CheckLinks(path);
        Require(!File.Exists(path) && !Directory.Exists(path), T("同名备份已存在。", "A backup with the same name already exists."), "save_exists");
        var manifest = MakeManifest(c, root, out var scan);
        long total = scan.Files.Where(p => Exported(p.Key)).Sum(p => p.Value);
        Space(Path.GetDirectoryName(path), total + (64 << 20));
        string before = Fingerprint(root);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            using (var output = NewFile(temporary))
            {
                using (var zip = new ZipArchive(output, ZipArchiveMode.Create, leaveOpen: true))
                {
                    foreach (var file in scan.Files.Where(p => Exported(p.Key)))
                    {
                        var entry = zip.CreateEntry("payload/" + file.Key, CompressionLevel.Fastest);
                        entry.ExternalAttributes = 0x180 << 16;
                        using var source = ReadFile(Path.Combine(root, file.Key));
                        Require(source.Length == file.Value, T("备份期间存档变化。", "The save changed during the backup."), "save_changed");
                        using var target = entry.Open();
                        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                        byte[] buffer = new byte[1 << 20];
                        int size; long copied = 0;
                        while ((size = source.Read(buffer)) != 0)
                        {
                            copied += size;
                            Require(copied <= file.Value, T("备份期间存档变化。", "The save changed during the backup."), "save_changed");
                            sha.AppendData(buffer, 0, size); target.Write(buffer, 0, size);
                        }
                        Require(copied == file.Value, T("备份期间存档变化。", "The save changed during the backup."), "save_changed");
                        manifest["files"].AsArray().Add((JsonNode)new JsonObject { ["path"] = file.Key, ["size"] = copied, ["sha256"] = Convert.ToHexStringLower(sha.GetHashAndReset()) });
                    }
                    using var metadata = zip.CreateEntry("manifest.json").Open();
                    metadata.Write(Encoding.UTF8.GetBytes(manifest.ToJsonString()));
                }
                output.Flush(flushToDisk: true);
            }
            Require(Fingerprint(root) == before, T("备份期间存档变化。", "The save changed during the backup."), "save_changed");
            using (var verified = new Archive(temporary)) Require(JsonNode.DeepEquals(manifest, verified.Manifest), T("导出后校验失败。", "Verification failed after export."));
            File.Move(temporary, path, overwrite: false);
            SyncDirectory(Path.GetDirectoryName(path));
            return manifest;
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    private static JsonObject Export(Context c)
    {
        string path = Source(c);
        Require(Directory.Exists(Path.GetDirectoryName(path)), T("备份目标目录不存在。", "The backup destination folder doesn’t exist."));
        var manifest = WriteArchive(c, c.Root, path);
        return new() { ["outputPath"] = path, ["fileCount"] = manifest["files"].AsArray().Count };
    }
    private static FileStream NewFile(string path) => new(path, new FileStreamOptions { Mode = FileMode.CreateNew,
        Access = FileAccess.Write, Share = FileShare.None, UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite });
    private static void Space(string path, long bytes)
    {
        long available;
        try
        {
            // Unix 的 AvailableFreeSpace 按原路径调用 statfs/statvfs。安卓应用可能无权枚举挂载点。
            available = new DriveInfo(path).AvailableFreeSpace;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException or OverflowException)
        {
            LogFailure("save_space_unknown", ex);
            throw new Failure("save_space_unknown", T("无法查询存档目录的可用空间，已停止操作。", "Could not determine available space in the save folder."));
        }
        Require(available >= bytes, T("磁盘空间不足，无法保留旧存档。", "Not enough disk space to keep the old save."), "save_space");
    }
}
