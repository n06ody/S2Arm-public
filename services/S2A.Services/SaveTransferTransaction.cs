using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json.Nodes;

namespace S2A.Services;

public static partial class SaveTransfer
{
    // 恢复时要删的「上次读写摘要」文件名（审计 P1-178）：备份里没有的玩家各删「savedata-state-<用户号>.txt」；其中对得上第一个玩家的，
    //   他的摘要没有后缀（savedata-state.txt），也删。first = null（认不出第一个玩家是谁）就不碰无后缀那份——删错了会让刚恢复进来的玩家
    //   下一局拿存档位置里恢复前的旧档盖掉新档。纯函数，S2A.Services.Tests 测
    internal static System.Collections.Generic.List<string> StateFilesToDrop(System.Collections.Generic.IEnumerable<string> dropped, string first)
    {
        string stem = Path.GetFileNameWithoutExtension(SaveFolder.StateFile);
        var names = new System.Collections.Generic.List<string>();
        foreach (string u in dropped)
        {
            names.Add(stem + "-" + u + ".txt");
            if (first != null && string.Equals(u, first, StringComparison.OrdinalIgnoreCase)) names.Add(SaveFolder.StateFile);
        }
        return names;
    }
    // 第一个玩家的用户号：同起局时的选法（LocalPlayer.Select → SaveFolder.SelectUser）——第一个玩家的存档位置里记了用户号就是它，没记就是缺省用户。
    //   第一个玩家的存档位置：设了 S2A_SAVE_FOLDER 且当前是第一个玩家就是它，否则是本款文档根下的 SaveData（ios/settings.m）。文档根不明回 null（不碰无后缀摘要）
    private static string FirstPlayerUser(JsonNode manifest)
    {
        string folder = Environment.GetEnvironmentVariable("S2A_SAVE_FOLDER");
        if (string.IsNullOrEmpty(folder) || LocalPlayer.FileSuffix() != "")
        {
            string docs = Environment.GetEnvironmentVariable("S2A_DOCS");
            if (string.IsNullOrEmpty(docs)) { Console.Error.WriteLine("S2A 存档恢复：认不出第一个玩家的存档位置（没有 S2A_DOCS），不动他的摘要"); return null; }
            folder = Path.Combine(docs, "SaveData");
        }
        ulong pid = 0;
        foreach (var e in manifest["saveEntries"]?.AsArray() ?? new JsonArray())
            if (ulong.TryParse(Str(e, "programId"), System.Globalization.NumberStyles.HexNumber, null, out pid) && pid != 0) break;
        return (SaveFolder.FolderUser(folder, pid) ?? Ryujinx.HLE.HOS.Services.Account.Acc.AccountManager.DefaultUserId.ToString()).ToLowerInvariant();
    }
    private static string Previous(Context c)
    {
        if (!Directory.Exists(c.Backups)) return "";
        CheckLinks(c.Backups);
        var paths = Directory.EnumerateDirectories(c.Backups).Select(d => Path.Combine(d, "before-restore.s2asave")).ToArray();
        foreach (string path in paths) CheckLinks(path);
        return paths.Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault() ?? "";
    }
    private static JsonObject Status(Context c, bool recovered)
    {
        var scan = Scan(c.Root);
        bool hasSave = scan.Files.Keys.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal));
        JsonObject manifest = hasSave ? MakeManifest(c, c.Root, out _) : null;
        return new() { ["hasSave"] = hasSave, ["fileCount"] = hasSave ? scan.Files.Count : 0,
            ["totalBytes"] = hasSave ? scan.Files.Sum(p => p.Value) : 0, ["users"] = Copy(manifest?["users"]),
            ["previousBackup"] = Previous(c), ["recovered"] = recovered };
    }
    private static void Atomic(string path, JsonNode value)
    {
        CheckLinks(path);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".partial";
        try
        {
            using (var file = NewFile(temporary))
            {
                file.Write(Encoding.UTF8.GetBytes(value.ToJsonString()));
                file.Flush(flushToDisk: true);
            }
            File.Move(temporary, path, overwrite: true);
            SyncDirectory(Path.GetDirectoryName(path));
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
    // 审计 P2-24：恢复时原先把整个数据根（着色器缓存等非存档文件常有几百 MB 到数 GB）逐字节流式拷进事务目录，存档三处随后又删掉重填。
    //   现在 skipSaves 跳过存档三处（反正要从备份包填回）；其余文件用 File.Copy——苹果 APFS 上它是写时复制克隆（本机实测 1 GB：克隆 0 ms、流式 1979 ms），
    //   不占额外空间、之后两边各写各的。不用硬链接：旧数据根会整棵挪进备份区保留，硬链接下新根原地写会连带改坏备份。
    //   仍先经平台接口打开源文件（拒链接 / 非普通文件），拷完核长度
    internal static void CopyTree(string source, string destination, bool skipSaves = false)
    {
        var scan = Scan(source);
        Directory.CreateDirectory(destination);
        foreach (string name in scan.Directories) if (!(skipSaves && Allowed(name, directory: true))) Directory.CreateDirectory(Path.Combine(destination, name));
        foreach (var file in scan.Files)
        {
            if (skipSaves && Allowed(file.Key)) continue;
            string sourcePath = Path.Combine(source, file.Key), target = Path.Combine(destination, file.Key);
            Directory.CreateDirectory(Path.GetDirectoryName(target));
            using (var src = ReadFile(sourcePath)) Require(src.Length == file.Value, T("复制期间目标存档发生变化。", "The target save changed during copying."), "save_changed");
            File.Copy(sourcePath, target);
            using var dst = new FileStream(target, FileMode.Open, FileAccess.ReadWrite);
            Require(dst.Length == file.Value, T("复制期间目标存档发生变化。", "The target save changed during copying."), "save_changed");
            dst.Flush(flushToDisk: true);
        }
    }
    private static void SyncTree(string root)
    {
        var scan = Scan(root);
        foreach (string dir in scan.Directories.OrderByDescending(d => d.Count(c => c == '/'))) SyncDirectory(Path.Combine(root, dir));
        SyncDirectory(root);
    }
    private static void SyncParents(string path, string boundary)
    {
        string current = path;
        while (current != null)
        {
            SyncDirectory(current);
            if (current == boundary) return;
            current = Path.GetDirectoryName(current);
        }
    }
    private static void DurableDirectory(string path)
    {
        CheckLinks(path);
        var missing = new System.Collections.Generic.Stack<string>();
        string current = path;
        while (!Directory.Exists(current))
        {
            Require(!File.Exists(current), T("目录路径已被普通文件占用。", "A regular file is in the way of a folder path."));
            missing.Push(current);
            current = Path.GetDirectoryName(current) ?? throw new Failure("save_invalid", T("找不到可访问的父目录。", "No accessible parent folder was found."));
        }
        while (missing.Count != 0)
        {
            current = missing.Pop();
            Directory.CreateDirectory(current, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            SyncDirectory(current);
            SyncDirectory(Path.GetDirectoryName(current));
        }
    }
    private static bool Recover(Context c)
    {
        CheckLinks(c.Journal);
        if (!File.Exists(c.Journal)) return false;
        try
        {
            var record = Parse(SmallFile(c.Journal));
            string token = Str(record, "transaction");
            string phase = Str(record, "phase");
            bool hadOriginal = record["hadOriginal"].GetValue<bool>();
            Require(record["schema"].GetValue<int>() == 1 && Str(record, "gameId") == Str(c.Game, "id") &&
                Str(record, "dataRoot") == c.Root && Hex(token, 32) && phase is "prepared" or "committed", T("恢复记录无效。", "Invalid recovery record."));
            string transaction = Path.Combine(c.Transactions, token);
            string previous = Path.Combine(c.Backups, token);
            string old = Path.Combine(previous, "data");
            string next = Path.Combine(transaction, "new");
            CheckLinks(old); CheckLinks(next);
            var expectedOwner = new JsonObject { ["schema"] = 1, ["gameId"] = Copy(c.Game["id"]), ["dataRoot"] = c.Root, ["transaction"] = token };
            Require(JsonNode.DeepEquals(Parse(SmallFile(Path.Combine(transaction, "owner.json"))), expectedOwner), T("事务归属记录不符。", "The transaction ownership record doesn’t match."));
            Require(!File.Exists(old), T("原存档副本不是目录。", "The original save copy is not a folder."));
            if (phase == "committed") Require(Directory.Exists(c.Root), T("已提交存档根缺失。", "The committed save root is missing."));
            else if (Directory.Exists(old))
            {
                if (Directory.Exists(c.Root))
                {
                    Require(!Directory.Exists(next) && !File.Exists(next), T("恢复状态不明确。", "The recovery state is unclear."));
                    Directory.Move(c.Root, next);
                    SyncDirectory(Path.GetDirectoryName(c.Root)); SyncDirectory(transaction);
                }
                DurableDirectory(Path.GetDirectoryName(c.Root));
                Directory.Move(old, c.Root);
                SyncDirectory(Path.GetDirectoryName(c.Root)); SyncDirectory(previous);
            }
            else if (hadOriginal)
                Require(Directory.Exists(c.Root) && Directory.Exists(next), T("原存档副本缺失。", "The original save copy is missing."));
            else if (Directory.Exists(c.Root))
            {
                Require(!Directory.Exists(next), T("恢复时出现未知存档根。", "An unknown save root appeared during recovery."));
                Directory.Move(c.Root, next);
                SyncDirectory(Path.GetDirectoryName(c.Root)); SyncDirectory(transaction);
            }
            File.Delete(c.Journal);
            SyncDirectory(c.Transactions);
            Directory.Delete(transaction, recursive: true);
            SyncDirectory(c.Transactions);
            return phase != "committed";
        }
        catch (Exception ex) when (ex is Failure or IOException or UnauthorizedAccessException or ArgumentException or InvalidOperationException or System.Text.Json.JsonException)
        {
            throw new Failure("save_recovery", T("未完成的存档事务无法安全恢复，已阻止后续操作。请保留原副本和事务记录。", "An unfinished save transaction can’t be recovered safely, so further operations are blocked. Keep the original copy and transaction records."));
        }
    }
    private static JsonObject Restore(Context c)
    {
        using var archive = new Archive(Source(c));
        Identity(c, archive.Manifest);   // 对不上的提示检查时已给玩家看过（确认即同意）；sync 时照旧拒绝
        string fingerprint = Fingerprint(c.Root);
        // 维护者 11-17：检查之后备份或目标存档又变了（玩家刚又存了一次）不再拒绝——上面刚按现在的内容重新校验过，恢复前副本也在这次执行时现拍，
        //   刚存的进度就在副本里（stderr 记一句）。iCloud 自动同步（sync）照旧拒绝
        bool sync = c.Request["sync"]?.GetValue<bool>() == true;
        bool requireUnchanged = sync || c.Request["unattendedRestore"]?.GetValue<bool>() == true;
        bool changed = c.Request["archiveSha256"]?.GetValue<string>() != archive.Sha || c.Request["targetFingerprint"]?.GetValue<string>() != fingerprint;
        Require(!(requireUnchanged && changed), T("备份或目标已变化，请重新检查后确认。", "The backup or target has changed. Check again, then confirm."), "save_changed");
        if (changed) Console.Error.WriteLine("S2A 存档恢复：检查之后备份或当前存档又变了，按现在的内容恢复（恢复前副本在这次执行时现拍）");
        long total = Scan(c.Root).Files.Sum(p => p.Value) + archive.Manifest["files"].AsArray().Sum(f => f["size"].GetValue<long>());
        Space(c.Work, checked(2 * total + (64 << 20)));
        string token = Guid.NewGuid().ToString("N");
        string transaction = Path.Combine(c.Transactions, token);
        string previous = Path.Combine(c.Backups, token);
        string next = Path.Combine(transaction, "new");
        string old = Path.Combine(previous, "data");
        bool hadOriginal = Directory.Exists(c.Root);
        string priorArchive = "";
        try
        {
            DurableDirectory(transaction);
            DurableDirectory(previous);
            Atomic(Path.Combine(transaction, "owner.json"), new JsonObject { ["schema"] = 1, ["gameId"] = Copy(c.Game["id"]), ["dataRoot"] = c.Root, ["transaction"] = token });
            SyncParents(transaction, c.Work); SyncParents(previous, c.Work);
            Checkpoint("dirsCreated");
            if (Scan(c.Root).Files.Keys.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal)))
            {
                priorArchive = Path.Combine(previous, "before-restore.s2asave");
                WriteArchive(c, c.Root, priorArchive);
            }
            CopyTree(c.Root, next, skipSaves: true);
            foreach (string name in Trees.Append(Profile))
            {
                string path = Path.Combine(next, name);
                if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
                else if (File.Exists(path)) File.Delete(path);
            }
            // 审计 P1-178：备份里没有的本地玩家，删掉他们外部存档文件夹的「上次读写摘要」（SaveFolder.StatePath 的按玩家文件名），
            //   下一局按已有的 SeedFrom 把他文件夹里的进度读回来；恢复进来的玩家的摘要不动（不然他文件夹里恢复前的旧档会盖掉刚恢复的）
            //   第一个玩家的摘要没有后缀（savedata-state.txt，LocalPlayer.FileSuffix）：拿掉的用户号对得上第一个玩家时一并删（审计 P1-178 余项）
            string first = FirstPlayerUser(archive.Manifest);
            foreach (string name in StateFilesToDrop(UsersNotInBackup(c, archive.Manifest), first))
            {
                string state = Path.Combine(next, name);
                if (File.Exists(state)) { File.Delete(state); Console.Error.WriteLine($"S2A 存档恢复：{name} 对应的玩家不在这份备份里，下一局从他的存档位置重新读入"); }
            }
            foreach (var dir in archive.Manifest["directories"].AsArray()) Directory.CreateDirectory(Path.Combine(next, dir.GetValue<string>()));
            var damaged = (archive.Manifest["damaged"] as JsonArray)?.Select(d => d.GetValue<string>()).ToHashSet(StringComparer.Ordinal) ?? new System.Collections.Generic.HashSet<string>(StringComparer.Ordinal);
            foreach (var item in archive.Manifest["files"].AsArray())
            {
                string path = Path.Combine(next, Str(item, "path"));
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                if (damaged.Contains(Str(item, "path")))   // 校验对不上的：不写坏的，目标原来有就留原来的那份
                {
                    string kept = Path.Combine(c.Root, Str(item, "path"));
                    if (File.Exists(kept) && !new FileInfo(kept).Attributes.HasFlag(FileAttributes.ReparsePoint)) File.Copy(kept, path);
                    continue;
                }
                var entry = archive.Zip.GetEntry("payload/" + Str(item, "path"));
                using var src = entry.Open();
                using var dst = NewFile(path);
                CopyEntry(src, dst, item["size"].GetValue<long>(), Str(item, "sha256"), entry.Crc32);
                dst.Flush(flushToDisk: true);
            }
            archive.Source.Position = 0;
            // 恢复途中目标又被写了：自动同步照旧拒绝；手动恢复再拍一份恢复前副本（刚写的进度在里面）照常恢复。备份文件在途中变了：逐文件已按清单 SHA 核过，照常
            if (Fingerprint(c.Root) != fingerprint)
            {
                Require(!requireUnchanged, T("确认后备份或目标发生变化。", "The backup or target changed after confirmation."), "save_changed");
                if (Scan(c.Root).Files.Keys.Any(n => n.StartsWith("bis/user/save/", StringComparison.Ordinal)))
                    WriteArchive(c, c.Root, Path.Combine(previous, "before-restore-2.s2asave"));
                Console.Error.WriteLine("S2A 存档恢复：恢复途中当前存档又变了，已另拍一份恢复前副本");
            }
            Require(!requireUnchanged || Hash(archive.Source) == archive.Sha, T("确认后备份或目标发生变化。", "The backup or target changed after confirmation."), "save_changed");
            SyncTree(next);
            var record = new JsonObject { ["schema"] = 1, ["gameId"] = Copy(c.Game["id"]), ["dataRoot"] = c.Root,
                ["transaction"] = token, ["hadOriginal"] = hadOriginal, ["phase"] = "prepared" };
            Atomic(c.Journal, record);
            Checkpoint("prepared");
            DurableDirectory(Path.GetDirectoryName(c.Root));
            SyncDirectory(Path.GetDirectoryName(c.Root));
            if (hadOriginal)
            {
                Directory.Move(c.Root, old);
                SyncDirectory(previous); SyncDirectory(Path.GetDirectoryName(c.Root));
            }
            Checkpoint("oldMoved");
            Directory.Move(next, c.Root);
            SyncDirectory(transaction); SyncDirectory(Path.GetDirectoryName(c.Root));
            Checkpoint("newMoved");
            record["phase"] = "committed";
            Atomic(c.Journal, record);
            Checkpoint("committed");
            // Keep the committed journal until the next prelaunch/status operation.
            // This makes the commit point unambiguous even if cleanup is interrupted.
            return new() { ["committed"] = true, ["hadOriginal"] = hadOriginal, ["outputPath"] = c.Root,
                ["previousBackup"] = priorArchive, ["cleanupPending"] = true };
        }
        catch
        {
            if (File.Exists(c.Journal)) Recover(c);
            else Abandon(transaction, previous);
            throw;
        }
    }
    // 审计 P2-177：恢复记录写出之前失败，活存档根还没动，本次新建的事务目录与备份目录（含恢复前备份）一并删掉；
    //   清理失败不盖住原失败（调用方照抛原异常）
    private static void Abandon(params string[] paths)
    {
        foreach (string path in paths)
        {
            try { if (Directory.Exists(path)) Directory.Delete(path, recursive: true); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
        }
    }
    // No implementation is linked into production. The process-exit tests pause
    // after real durable filesystem boundaries and kill the child from its parent.
    static partial void Checkpoint(string phase);
}
