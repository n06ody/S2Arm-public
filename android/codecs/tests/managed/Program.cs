using Ryujinx.Common.Logging;
using Ryujinx.Graphics.Nvdec;
using Ryujinx.Graphics.Nvdec.Android;
using Ryujinx.Graphics.Nvdec.FFmpeg.H264;
using Ryujinx.Graphics.Video;
using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;

static class Program
{
    static void Check(bool value, string message) { if (!value) throw new Exception(message); }
    static H264PictureInfo Info(JsonElement m)
    {
        var p = m.GetProperty("picture_info");
        uint U(string key) => p.TryGetProperty(key, out var v) ? v.GetUInt32() : 0;
        bool B(string key) => U(key) != 0;
        int I(string key) => p.TryGetProperty(key, out var v) ? v.GetInt32() : 0;
        return new()
        {
            ChromaFormatIdc = (ushort)U("chroma_format_idc"), FrameMbsOnlyFlag = B("frame_mbs_only_flag"),
            Direct8x8InferenceFlag = B("direct_8x8_inference_flag"), EntropyCodingModeFlag = B("entropy_coding_mode_flag"),
            PicOrderPresentFlag = B("bottom_field_pic_order_in_frame_present_flag"),
            Log2MaxFrameNumMinus4 = U("log2_max_frame_num_minus4"), PicOrderCntType = U("pic_order_cnt_type"),
            Log2MaxPicOrderCntLsbMinus4 = U("log2_max_pic_order_cnt_lsb_minus4"),
            NumRefIdxL0ActiveMinus1 = U("num_ref_idx_l0_default_active_minus1"), NumRefIdxL1ActiveMinus1 = U("num_ref_idx_l1_default_active_minus1"),
            WeightedPredFlag = B("weighted_pred_flag"), WeightedBipredIdc = U("weighted_bipred_idc"),
            PicInitQpMinus26 = I("pic_init_qp_minus26"), ChromaQpIndexOffset = I("chroma_qp_index_offset"), SecondChromaQpIndexOffset = I("second_chroma_qp_index_offset"),
            DeblockingFilterControlPresentFlag = B("deblocking_filter_control_present_flag"), ConstrainedIntraPredFlag = B("constrained_intra_pred_flag"),
            RedundantPicCntPresentFlag = B("redundant_pic_cnt_present_flag"), Transform8x8ModeFlag = B("transform_8x8_mode_flag"),
            PicWidthInMbsMinus1 = (uint)(m.GetProperty("width").GetInt32() / 16 - 1), PicHeightInMapUnitsMinus1 = (uint)(m.GetProperty("height").GetInt32() / 16 - 1), NumRefFrames = 16,
        };
    }
    static byte[] StripHeaders(ReadOnlySpan<byte> source)
    {
        using var result = new MemoryStream();
        int start = -1, prefix = 0;
        for (int i = 0; i < source.Length; i++)
        {
            int size = i + 3 < source.Length && source[i] == 0 && source[i + 1] == 0 && source[i + 2] == 0 && source[i + 3] == 1 ? 4 :
                i + 2 < source.Length && source[i] == 0 && source[i + 1] == 0 && source[i + 2] == 1 ? 3 : 0;
            if (size == 0) continue;
            if (start >= 0 && (source[start + prefix] & 31) is not 7 and not 8) result.Write(source[start..i]);
            start = i; prefix = size; i += size - 1;
        }
        if (start >= 0 && (source[start + prefix] & 31) is not 7 and not 8) result.Write(source[start..]);
        return result.ToArray();
    }
    static unsafe string Hash(Plane plane, int width, int height, int stride)
    {
        using var sha = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        for (int row = 0; row < height; row++) sha.AppendData(new ReadOnlySpan<byte>((void*)(plane.Pointer + row * stride), width));
        return Convert.ToHexStringLower(sha.GetHashAndReset());
    }
    static H264PictureInfo Basic() => new() { ChromaFormatIdc=1, FrameMbsOnlyFlag=true, PicWidthInMbsMinus1=19, PicHeightInMapUnitsMinus1=14 };
    static void Contracts()
    {
        var info=Basic();
        using IH264Decoder unavailable = new UnavailableH264Decoder();
        using var surface = unavailable.CreateSurface(320,240);
        byte[] guest = Enumerable.Repeat((byte)0xa5,4096).ToArray(); int writes=0;
        for (int i=0;i<3;i++) if (unavailable.Decode(ref info,surface,new byte[]{0,0,1,0x65})) { writes++; guest[0]=0; }
        Check(writes==0 && guest.All(x=>x==0xa5),"unavailable write guard");
        using var vp8 = new AndroidVp8Decoder();
        var vp8Info = new Vp8PictureInfo(); using var vp8Surface=vp8.CreateSurface(320,240);
        Check(!vp8.Decode(ref vp8Info,vp8Surface,Array.Empty<byte>()),"Android VP8 loader boundary");
        using var loader = new AvcH264Decoder();
        using var output=loader.CreateSurface(320,240);
        Check(!loader.Decode(ref info,output,new byte[]{0,0,1,0x65}),"missing native backend");
        loader.Dispose(); loader.Dispose();
        Check(output.YPlane.Pointer==0,"dispose detaches view");
        info.PicOrderCntType=1; Check(!AvcH264Decoder.Supported(ref info),"POC type1 rejected");
        info=Basic(); info.FieldPicFlag=true; Check(!AvcH264Decoder.Supported(ref info),"field rejected");
        info=Basic(); info.ChromaFormatIdc=2; Check(!AvcH264Decoder.Supported(ref info),"422 rejected");
        CallPath.Run(new UnavailableH264Decoder(),Basic(),new byte[]{0,0,1,0x65},false);
        Console.WriteLine("PASS contracts; SENTINEL process continues after red+false");
    }
    static void Vectors(string directory, string emit)
    {
        foreach (string path in Directory.GetFiles(directory,"*.json").Order())
        {
            using var manifest=JsonDocument.Parse(File.ReadAllText(path)); var m=manifest.RootElement;
            var info=CallPath.Metadata(Info(m)).Convert();
            if(emit.Length==0)
            {
                byte[] work=new byte[512];
                string hash=Convert.ToHexStringLower(SHA256.HashData(SpsAndPpsReconstruction.Reconstruct(ref info,work)));
                Check(hash==m.GetProperty("reconstructed_headers_sha256").GetString(),"production header oracle identity");
            }
            int width=m.GetProperty("width").GetInt32(),height=m.GetProperty("height").GetInt32();
            byte[] bytes=File.ReadAllBytes(Path.Combine(directory,m.GetProperty("file").GetString()!));
            using var decoder=new AvcH264Decoder(); using var surface=decoder.CreateSurface(width,height);
            if(emit.Length!=0) { byte[] work=new byte[512]; File.WriteAllBytes(Path.Combine(emit,Path.GetFileNameWithoutExtension(path)+".headers"),SpsAndPpsReconstruction.Reconstruct(ref info,work).ToArray()); info.ChromaQpIndexOffset++; info.SecondChromaQpIndexOffset++; File.WriteAllBytes(Path.Combine(emit,Path.GetFileNameWithoutExtension(path)+".pps-change"),SpsAndPpsReconstruction.Reconstruct(ref info,work).ToArray()); continue; }
            using(var rawDecoder=new AvcH264Decoder())
            using(var rawSurface=rawDecoder.CreateSurface(width,height))
            {
                var first=m.GetProperty("frames")[0];
                Check(rawDecoder.Decode(ref info,rawSurface,bytes.AsSpan(first.GetProperty("offset").GetInt32(),first.GetProperty("length").GetInt32())),"inband equivalent SPS/PPS");
            }
            int index=0;
            byte[][] aus=m.GetProperty("frames").EnumerateArray().Select(frame=>StripHeaders(bytes.AsSpan(frame.GetProperty("offset").GetInt32(),frame.GetProperty("length").GetInt32()))).ToArray();
            if(width==320 && height==240)
            {
                var first=m.GetProperty("frames")[0];
                CallPath.Run(new AvcH264Decoder(),info,StripHeaders(bytes.AsSpan(first.GetProperty("offset").GetInt32(),first.GetProperty("length").GetInt32())),true);
            }
            foreach(var frame in m.GetProperty("frames").EnumerateArray())
            {
                byte[] au=StripHeaders(bytes.AsSpan(frame.GetProperty("offset").GetInt32(),frame.GetProperty("length").GetInt32()));
                Check(decoder.Decode(ref info,surface,au),Path.GetFileName(path)+" frame "+index);
                string[] actual={Hash(surface.YPlane,width,height,surface.Stride),Hash(surface.UPlane,width/2,height/2,surface.UvStride),Hash(surface.VPlane,width/2,height/2,surface.UvStride)};
                Check(actual.SequenceEqual(frame.GetProperty("reconstructed_planes_sha256").EnumerateArray().Select(x=>x.GetString()!)),"SHA "+path+" frame "+index);
                index++;
            }
            // Warm the real path (including native DPB); measure only synchronous
            // Decode over a second complete trajectory, without hash/test allocations.
            foreach(byte[] au in aus) Check(decoder.Decode(ref info,surface,au),"warm decode");
            long before=GC.GetAllocatedBytesForCurrentThread(); bool all=true;
            foreach(byte[] au in aus) all &= decoder.Decode(ref info,surface,au);
            long allocated=GC.GetAllocatedBytesForCurrentThread()-before;
            Check(all && allocated==0,"steady Decode allocations="+allocated);
            Console.WriteLine("PASS steady managed allocation=0 bytes for "+index+" calls");
            decoder.Dispose(); Check(surface.YPlane.Pointer==0,"lifetime invalidation");
            Console.WriteLine("PASS production reconstruction "+Path.GetFileName(path)+" frames="+index);
            if(m.GetProperty("frames")[0].TryGetProperty("pps_changed_planes_sha256",out _))
            {
                using var ppsDecoder=new AvcH264Decoder(); using var ppsSurface=ppsDecoder.CreateSurface(width,height);
                int i=0; var changing=info;
                foreach(var frame in m.GetProperty("frames").EnumerateArray())
                {
                    if(i==4) { changing.ChromaQpIndexOffset++; changing.SecondChromaQpIndexOffset++; }
                    Check(ppsDecoder.Decode(ref changing,ppsSurface,aus[i]),"PPS-only update");
                    string[] hashes={Hash(ppsSurface.YPlane,width,height,ppsSurface.Stride),Hash(ppsSurface.UPlane,width/2,height/2,ppsSurface.UvStride),Hash(ppsSurface.VPlane,width/2,height/2,ppsSurface.UvStride)};
                    Check(hashes.SequenceEqual(frame.GetProperty("pps_changed_planes_sha256").EnumerateArray().Select(x=>x.GetString()!)),"PPS changed oracle frame "+i);i++;
                }
                Console.WriteLine("PASS PPS-only change preserves DPB "+Path.GetFileName(path));
            }
        }
    }
    static unsafe void Vp9Mc()
    {
        // S2A_MC_FAKE_MODE 驱动 fixture dylib 的演出路径；验的是 IVp9NativeBackend 契约下
        // McVp9Backend 的裁决（gated/回退/记账），不验像素（fixture 不是解码器）。
        string mode=Environment.GetEnvironmentVariable("S2A_MC_FAKE_MODE") ?? "ok";
        byte[] probs;
        {
            Vp9EntropyProbs fc = Ryujinx.Graphics.Nvdec.Vp9.Decoder.DefaultProbs();
            probs = MemoryMarshal.AsBytes(new[] { fc }.AsSpan()).ToArray();
            Check(probs.Length == McVp9Backend.ProbsSize, "default probs blob is 3087 bytes");
            Check(probs[0] != 0, "default probs blob is populated");
        }
        byte[] au={0x12,0x34,0x56,0x78};
        byte[] mvs=Enumerable.Repeat((byte)0xAA,128).ToArray();
        byte[] counts=Enumerable.Repeat((byte)0xAA,McVp9Backend.CountsSize).ToArray();
        var key=new Vp9NativeParams{IntraFrame=1,ShowFrame=1,Width=32,Height=16};
        using McVp9Backend backend=McVp9Backend.TryCreate();
        if(mode=="noprobe")
        {
            Check(backend==null,"noprobe: TryCreate null → factory falls to vpx");
            Console.WriteLine("PASS vp9 selection boundary mode="+mode);return;
        }
        Check(backend!=null,"probe ok: backend created");
        Vp9NativeResult RP(ReadOnlySpan<byte> pb,Vp9NativeParams p,out Vp9NativePicture pic,out string err)
        {
            Array.Fill(mvs,(byte)0xAA); Array.Fill(counts,(byte)0xAA);
            return backend.Decode(au,pb,in p,ReadOnlySpan<byte>.Empty,mvs,counts,out pic,out err);
        }
        Vp9NativeResult R(Vp9NativeParams p,out Vp9NativePicture pic,out string err) => RP(probs,p,out pic,out err);
        if(mode=="ok")
        {
            Check(R(key,out var pic,out var err)==Vp9NativeResult.Decoded,$"keyframe decodes: {err}");
            Check(pic.Y!=0&&pic.Width==32&&pic.Height==16,"picture view populated");
            Check(mvs.All(x=>x==0xAA),"mvsOut untouched — hardware produces none, nothing fabricated");
            Check(counts.All(x=>x==0xAA),"countsOut untouched — hardware produces none, nothing fabricated");
            var inter=key; inter.IntraFrame=0;
            Check(R(inter,out _,out _)==Vp9NativeResult.Decoded,"inter frame decodes");
            // 隐藏帧 / 供给表≠默认表 / 越界尺寸：如实回退，不产出图。
            var hidden=key; hidden.ShowFrame=0;
            Check(R(hidden,out _,out _)==Vp9NativeResult.NotRepresentable,"hidden job not representable");
            byte[] bad=(byte[])probs.Clone(); bad[2000]^=0xFF;
            Check(RP(bad,key,out _,out _)==Vp9NativeResult.NotRepresentable,"non-default probs rejected");
            // seg 字段不进解码：供给表仅 seg 段不同仍应通过（负对照）。
            int segTree=(int)Marshal.OffsetOf<Vp9EntropyProbs>(nameof(Vp9EntropyProbs.SegTreeProb));
            bad=(byte[])probs.Clone(); bad[segTree]^=0xFF; bad[segTree+8]^=0xFF;
            Check(RP(bad,key,out _,out _)==Vp9NativeResult.Decoded,"seg fields excluded from compare");
            // 占位压缩头（1 字节 0x00）强解出 tx_mode=ONLY_4X4、reference_mode=SINGLE、
            // use_prev_frame_mvs=0：真实字段超出占位语义就回退，不然位流失步花屏。
            var txsel=inter; txsel.TxMode=4;
            Check(R(txsel,out _,out _)==Vp9NativeResult.NotRepresentable,"tx_mode_select not representable");
            var txsel2=inter; txsel2.TxMode=1;
            Check(R(txsel2,out _,out _)==Vp9NativeResult.NotRepresentable,"allow_8x8 not representable");
            var refsel=inter; refsel.ReferenceMode=2;
            Check(R(refsel,out _,out _)==Vp9NativeResult.NotRepresentable,"reference_mode_select not representable");
            var refcmp=inter; refcmp.ReferenceMode=1;
            Check(R(refcmp,out _,out _)==Vp9NativeResult.NotRepresentable,"compound ref not representable");
            var keytx=key; keytx.TxMode=4; keytx.ReferenceMode=2; // intra 规范强制两值，不算违例
            Check(R(keytx,out _,out _)==Vp9NativeResult.Decoded,"intra tx/ref fields spec-forced, still decodes");
            var prevmv=inter; prevmv.UsePrevFrameMvs=1;
            Check(R(prevmv,out _,out _)==Vp9NativeResult.NeedsKeyFrame,"prev-mv job after decoded needs keyframe");
            byte[] prevMvBuf=new byte[16];
            Check(backend.Decode(au,probs,in prevmv,prevMvBuf,mvs,counts,out _,out _)==Vp9NativeResult.NeedsKeyFrame,"non-empty prevMvs same rule");
            // 尺寸变化的非关键帧 → NeedsKeyFrame；关键帧则重建后照常解。
            var resize=key; resize.IntraFrame=0; resize.Width=64; resize.Height=32;
            Check(R(resize,out _,out _)==Vp9NativeResult.NeedsKeyFrame,"non-key resize needs keyframe");
            resize.IntraFrame=1;
            Check(R(resize,out var pic2,out _)==Vp9NativeResult.Decoded,"keyframe resize reconfigures");
            Check(pic2.Width==64&&pic2.Height==32,"picture follows new size");
        }
        if(mode=="pending")
        {
            // decode 不出图、wait 排空补齐：同一作业内 Decoded，不回退。
            Check(R(key,out var pic,out var err)==Vp9NativeResult.Decoded,$"pending resolved via wait: {err}");
            Check(pic.Y!=0,"pending resolved picture");
        }
        if(mode=="starve")
        {
            // 预算内补不齐：如实 NotRepresentable；连续三次升级 Error。
            Check(R(key,out _,out _)==Vp9NativeResult.NotRepresentable,"starved job falls back");
            Check(R(key,out _,out _)==Vp9NativeResult.NotRepresentable,"starved again");
            Check(R(key,out _,out var err)==Vp9NativeResult.Error,"consecutive starvation escalates to Error");
            Check(R(key,out _,out _)==Vp9NativeResult.Error,"starved backend stays sticky-failed");
        }
        if(mode=="createfail"||mode=="decodefail")
        {
            Check(R(key,out _,out var err)==Vp9NativeResult.Error,$"{mode} is Error: {err}");
        }
        if(mode=="latefail")
        {
            Check(R(key,out _,out var err)==Vp9NativeResult.Decoded,$"first frame ok: {err}");
            Check(R(key,out _,out err)==Vp9NativeResult.Error,"mid-stream failure is Error");
            Check(R(key,out _,out _)==Vp9NativeResult.Error,"failure is sticky");
        }
        Console.WriteLine("PASS vp9 selection boundary mode="+mode);
    }
    static void Reconfiguration(string directory)
    {
        using var small=JsonDocument.Parse(File.ReadAllText(Path.Combine(directory,"b-refs4.json")));
        using var large=JsonDocument.Parse(File.ReadAllText(Path.Combine(directory,"b-720.json")));
        var a=CallPath.Metadata(Info(small.RootElement)).Convert();var b=CallPath.Metadata(Info(large.RootElement)).Convert();
        byte[] Input(JsonElement m,int i) { byte[] data=File.ReadAllBytes(Path.Combine(directory,m.GetProperty("file").GetString()!));var f=m.GetProperty("frames")[i];return StripHeaders(data.AsSpan(f.GetProperty("offset").GetInt32(),f.GetProperty("length").GetInt32())); }
        using(var decoder=new AvcH264Decoder())
        using(var old=decoder.CreateSurface(320,240))
        using(var next=decoder.CreateSurface(1280,720))
        {
            Check(decoder.Decode(ref a,old,Input(small.RootElement,0)),"initial sequence");
            Check(decoder.Decode(ref b,next,Input(large.RootElement,0)),"IDR resolution switch");
            Check(old.YPlane.Pointer==0,"old view detached across IDR resize");
            Check(!decoder.Decode(ref a,old,Input(small.RootElement,1)),"non-IDR resolution switch rejected");
            Check(next.YPlane.Pointer==0,"failure detached previous view");
        }
        Console.WriteLine("PASS IDR resize, non-IDR change red+false, old view invalidated");
    }
    static int Main(string[] args)
    {
        try
        {
            string library=Environment.GetEnvironmentVariable("S2A_AVC_LIBRARY") ?? "";
            string mclibrary=Environment.GetEnvironmentVariable("S2A_MC_LIBRARY") ?? "";
            NativeLibrary.SetDllImportResolver(typeof(AvcH264Decoder).Assembly,(name,assembly,path)=>
                name=="s2arm_android" ? (mclibrary.Length!=0 ? mclibrary : library).Length!=0 ? NativeLibrary.Load(mclibrary.Length!=0 ? mclibrary : library) : throw new DllNotFoundException("fixture: absent native backend") : 0);
            if(args.Length>0 && args[0]=="baseline-ffmpeg")
            {
                using var baseline=new Ryujinx.Graphics.Nvdec.FFmpeg.H264.Decoder();
                throw new Exception("baseline unexpectedly found FFmpeg");
            }
            if(args.Length>0 && args[0]=="vp9") Vp9Mc();
            else if(args.Length==0) Contracts(); else { Vectors(args[0],args.Length>1?args[1]:""); if(args.Length==1) Reconfiguration(args[0]); }
            return 0;
        }
        catch(Exception error) { Console.Error.WriteLine(error); return 1; }
        finally { Logger.Shutdown(); }
    }
}
