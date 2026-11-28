// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace S2A.Services.Tests
{
    public static unsafe partial class Program
    {
        private static readonly List<(ulong Address, ulong Size, int Write)> _guestExports = new();

        [UnmanagedCallersOnly]
        private static void RecordHostAddress(ulong address, ulong size, int write)
        {
            _guestExports.Add((address, size, write));
        }

        [SoloTest("--guest-host-address")]
        private static int GuestHostAddressRoutes()
        {
            byte* bytes = (byte*)NativeMemory.AllocZeroed(64);
            var previous = GuestMemory.HostAddressHook;
            GuestMemory.HostAddressHook = &RecordHostAddress;
            try
            {
                GuestMemory memory = new();
                ulong address = (ulong)bytes;
                int passed = 0;
                int checks = 0;
                void Expect(ulong offset, ulong size, int write, string route)
                {
                    checks++;
                    bool same = _guestExports.Count == 1 && _guestExports[0] == (address + offset, size, write);
                    if (same) passed++;
                    else Console.Error.WriteLine($"FAIL {route}: expected one notification at +{offset}, size={size}, write={write}; got {_guestExports.Count}");
                    _guestExports.Clear();
                }

                _ = memory.GetSpan(address + 1, 2)[0];
                Expect(1, 2, 0, "GetSpan");
                _ = memory.GetReadOnlySequence(address + 3, 2).FirstSpan[0];
                Expect(3, 2, 0, "GetReadOnlySequence");
                _ = memory.TryReadUnsafe(address + 5, 2, out _);
                Expect(5, 2, 1, "TryReadUnsafe mutable span");
                memory.GetWritableRegion(address + 7, 2).Memory.Span[0] = 0x48;
                Expect(7, 2, 1, "GetWritableRegion without Dispose");
                memory.GetRef<byte>(address + 9) = 0x53;
                Expect(9, 1, 1, "GetRef");
                _ = memory.GetHostRegions(address + 10, 2);
                Expect(10, 2, 1, "GetHostRegions");
                _ = memory.Read<uint>(address + 12);
                Expect(12, 4, 0, "Read<T>");
                memory.Read(address + 16, stackalloc byte[3]);
                Expect(16, 3, 0, "Read bytes");
                memory.Write(address + 19, (byte)0x65);
                Expect(19, 1, 1, "Write<T>");
                Console.WriteLine($"guest host addresses {passed}/{checks}; bytes {bytes[7]:X2}/{bytes[9]:X2}/{bytes[19]:X2}");
                return passed == checks && bytes[7] == 0x48 && bytes[9] == 0x53 && bytes[19] == 0x65 ? 0 : 1;
            }
            finally
            {
                GuestMemory.HostAddressHook = previous;
                NativeMemory.Free(bytes);
            }
        }
    }
}
