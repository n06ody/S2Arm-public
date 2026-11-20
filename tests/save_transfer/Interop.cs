using System.Runtime.CompilerServices;
using System.Text.Json;
using LibHac;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.Fs.Shim;
using LibHac.FsSrv;
using LibHac.FsSystem;
using Path = System.IO.Path;

Ryujinx.HLE.FileSystem.S2AHostFileSystem.Transaction = new S2A.Apple.SaveCommit.AppleHostDirectoryTransaction();

if (args.Length is 1 or 2 && args[0] == "--describe")   // 审计 P2-11：失败原因转人话的映射，interop.py 按手算期望逐条比；第二参数 = 请求里的 language（#316）
{
    S2A.Services.SaveTransfer.UseLanguage(args.Length == 2 ? JsonSerializer.Serialize(new { language = args[1] }) : "{}");
    foreach (var ex in new Exception[] { new IOException("disk", 28), new IOException("quota", 69), new UnauthorizedAccessException("no"), new IOException("perm", 13),
                                         new InvalidDataException("bad"), new JsonException("bad"), new IOException("other", 5), new ArgumentException("arg"), new InvalidOperationException("state") })
    {
        var (code, message) = S2A.Services.SaveTransfer.Describe(ex);
        Console.WriteLine(JsonSerializer.Serialize(new { type = ex.GetType().Name, code, message }));
    }
    return;
}
if (args.Length == 3 && args[0] == "--copytree")   // 审计 P2-24：恢复用的数据根复制（跳过存档三处、其余克隆），interop.py 核内容与耗时
{
    var watch = System.Diagnostics.Stopwatch.StartNew();
    S2A.Services.SaveTransfer.CopyTree(args[1], args[2], skipSaves: true);
    Console.WriteLine(JsonSerializer.Serialize(new { ms = watch.ElapsedMilliseconds }));
    return;
}
if (args.Length is 2 or 3 && args[0] == "--request")
{
    if (args.Length == 3 && (args[2] != "--require-aot" || RuntimeFeature.IsDynamicCodeSupported || JsonSerializer.IsReflectionEnabledByDefault))
        throw new InvalidOperationException("Save inspection requires NativeAOT without reflection serialization.");
    Console.WriteLine(S2A.Services.SaveTransfer.Execute(File.ReadAllText(args[1])));
    return;
}
if (args.Length != 4 || args[0] != "--create") throw new ArgumentException("--create root repository user-id[,user-id…]");
// 用户号可给多个（逗号分隔，#623 本地玩家）：第一个照旧建账号存档 + 设备存档，其余每人再建一份账号存档（同一款游戏几个玩家）
string root = args[1], repository = args[2];
string[] userTexts = args[3].Split(',');
string userText = userTexts[0];
Directory.CreateDirectory(root);
var horizon = new Horizon(new HorizonConfiguration());
using var serverClient = horizon.CreatePrivilegedHorizonClient();
var server = new FileSystemServer(serverClient);
using var baseFs = new LocalFileSystem(root);
var keys = KeySet.CreateDefaultKeySet();
RandomDataGenerator random = Random.Shared.NextBytes;
var objects = DefaultFsServerObjects.GetDefaultEmulatedCreators(baseFs, keys, server, random);
FileSystemServerInitializer.InitializeWithConfig(serverClient, server, new FileSystemServerConfig
{
    ExternalKeySet = keys.ExternalKeySet,
    FsCreators = objects.FsCreators,
    StorageDeviceManagerFactory = objects.StorageDeviceManagerFactory,
    RandomGenerator = random,
});
using var client = horizon.CreatePrivilegedHorizonClient();
var application = new LibHac.Ncm.ApplicationId(0x0100000000000000);
var user = new UserId(Convert.ToUInt64(userText[..16], 16), Convert.ToUInt64(userText[16..], 16));
Check(client.Fs.CreateSaveData(application, user, application.Value, 0x100000, 0x100000, SaveDataFlags.None));
Check(client.Fs.CreateDeviceSaveData(application, application.Value, 0x100000, 0x100000, SaveDataFlags.None));
Check(client.Fs.MountSaveData("account"u8, application, user));
Check(client.Fs.MountDeviceSaveData("device"u8, application));
Write("account:/contributing.md"u8, File.ReadAllBytes(Path.Combine(repository, "CONTRIBUTING.md")));
Check(client.Fs.CreateDirectory("account:/empty"u8));
Check(client.Fs.CreateDirectory("account:/nested"u8));
Write("account:/nested/readme.md"u8, File.ReadAllBytes(Path.Combine(repository, "README.md")));
Write("device:/contributing.md"u8, File.ReadAllBytes(Path.Combine(repository, "CONTRIBUTING.md")));
Check(client.Fs.Commit("account"u8));
Check(client.Fs.Commit("device"u8));
client.Fs.Unmount("account"u8);
client.Fs.Unmount("device"u8);
foreach (string more in userTexts.Skip(1))
{
    var other = new UserId(Convert.ToUInt64(more[..16], 16), Convert.ToUInt64(more[16..], 16));
    Check(client.Fs.CreateSaveData(application, other, application.Value, 0x100000, 0x100000, SaveDataFlags.None));
    Check(client.Fs.MountSaveData("player"u8, application, other));
    Write("player:/readme.md"u8, File.ReadAllBytes(Path.Combine(repository, "README.md")));
    Check(client.Fs.Commit("player"u8));
    client.Fs.Unmount("player"u8);
}
Directory.CreateDirectory(Path.Combine(root, "system"));
File.WriteAllText(Path.Combine(root, "system/Profiles.json"), JsonSerializer.Serialize(new
{
    last_opened = userText,
    profiles = userTexts.Select((u, i) => new { user_id = u, name = i == 0 ? "Public-file interop account" : "Public-file interop player " + (i + 1) }).ToArray(),
}));
Console.WriteLine("Created and committed account/device saves through LibHac; payloads are repository CONTRIBUTING.md and README.md.");

void Check(Result result) { if (result.IsFailure()) throw new InvalidOperationException("LibHac: " + result); }
void Write(LibHac.Common.U8Span path, byte[] bytes)
{
    Check(client.Fs.CreateFile(path, bytes.Length));
    Check(client.Fs.OpenFile(out FileHandle handle, path, OpenMode.Write));
    try { Check(client.Fs.WriteFile(handle, 0, bytes, WriteOption.Flush)); }
    finally { client.Fs.CloseFile(handle); }
}

namespace S2A.Services
{
    public static partial class SaveTransfer
    {
        // 审计 P2-177：interop.py 设 SAVE_TRANSFER_FAIL_AT=<阶段> 让恢复在该检查点抛错，核失败后不留目录
        static partial void Checkpoint(string phase)
        {
            if (Environment.GetEnvironmentVariable("SAVE_TRANSFER_FAIL_AT") == phase) throw new IOException("injected failure at " + phase);
        }
    }
}
