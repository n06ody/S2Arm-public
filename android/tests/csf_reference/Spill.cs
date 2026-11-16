// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
using System;
using System.IO;
using S2A.DirectCompiler;
if (args.Length != 4) throw new ArgumentException("original.spv spill.spv set binding");
var result = DirectSharedSpill.Rewrite(File.ReadAllBytes(args[0]), 32768, int.Parse(args[2]), int.Parse(args[3]));
if (result == null) throw new InvalidOperationException("fixture did not exercise spill");
File.WriteAllBytes(args[1], result.Spirv);
Console.WriteLine($"bytesPerGroup={result.BytesPerGroup} remainingSharedBytes={result.RemainingSharedBytes}");
