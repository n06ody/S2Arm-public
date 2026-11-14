#!/usr/bin/env python3
# 门禁手动：辅助脚本（sim_tiers.sh 展开模拟档用），本身不是单测；随 sim_tiers.sh 在 CI .github/workflows/android-vk.yml 里跑
"""把手写的模拟档（android/tests/vk/profiles/s2a_sim_tiers.json）展开成 profiles 层能直接用的档。

profiles 层一旦模拟格式（SIMULATE_FORMATS_BIT），档里没列的格式就全按「不支持」报。所以手写档只列
「要藏掉的格式」，这里拿宿主自己的格式表（lavapipe 上 `vulkaninfo --json` 的输出）补全：
  - 宿主有的格式原样抄 v1 位，另补 VkFormatProperties3（v1 位改名 + 按宿主
    shaderStorageImage{Read,Write}WithoutFormat 补 *_WITHOUT_FORMAT 位；只给 v1 位时层会把这两位丢掉，
    validation 因此误报 VUID-*-OpTypeImage-07029/07030）；
  - 手写档里列出的格式，按手写的值（一般是全空 = 不支持）覆盖；
  - 手写成 "VkFormatPropertiesRemove" 的格式，从宿主的位里去掉列出的几位（只藏某个特性，比如线性过滤 / 混合，#1072）。
不碰特性 / 扩展；没有 formats 的档原样输出。

用法：python3 sim_profile.py 宿主.json 手写档.json 输出.json
"""
import json
import sys


def v3(bits, read_wo, write_wo):
    out = [b.replace("VK_FORMAT_FEATURE_", "VK_FORMAT_FEATURE_2_") for b in bits]
    if any(b in bits for b in ("VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT", "VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT")):
        if read_wo:
            out.append("VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT")
        if write_wo:
            out.append("VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT")
    return out


def entry(v1, read_wo, write_wo):
    kinds = ("linearTilingFeatures", "optimalTilingFeatures", "bufferFeatures")
    p1 = {k: list(v1.get(k, [])) for k in kinds}
    return {"VkFormatProperties": p1, "VkFormatProperties3": {k: v3(p1[k], read_wo, write_wo) for k in kinds}}


def main(argv):
    if len(argv) != 4:
        print(__doc__, file=sys.stderr)
        return 64
    host = json.load(open(argv[1], encoding="utf-8"))
    tiers = json.load(open(argv[2], encoding="utf-8"))
    hcap = next(iter(host["capabilities"].values()))
    hfeat = hcap.get("features", {}).get("VkPhysicalDeviceFeatures", {})
    read_wo = bool(hfeat.get("shaderStorageImageReadWithoutFormat"))
    write_wo = bool(hfeat.get("shaderStorageImageWriteWithoutFormat"))
    base = {f: entry(p.get("VkFormatProperties", {}), read_wo, write_wo) for f, p in hcap.get("formats", {}).items()}
    if not base:
        print("宿主档里没有格式表（vulkaninfo --json 输出不对？）", file=sys.stderr)
        return 1
    for cap in tiers["capabilities"].values():
        hide = cap.get("formats")
        if not hide:
            continue
        full = dict(base)
        for f, p in hide.items():
            if "VkFormatPropertiesRemove" in p:
                host_v1 = hcap.get("formats", {}).get(f, {}).get("VkFormatProperties", {})
                rm = p["VkFormatPropertiesRemove"]
                kept = {k: [b for b in host_v1.get(k, []) if b not in rm.get(k, [])] for k in host_v1}
                full[f] = entry(kept, read_wo, write_wo)
            else:
                full[f] = entry(p.get("VkFormatProperties", {}), read_wo, write_wo)
        cap["formats"] = full
    with open(argv[3], "w", encoding="utf-8") as fh:
        json.dump(tiers, fh, ensure_ascii=False, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
