using Ryujinx.Graphics.Shader.IntermediateRepresentation;
using Ryujinx.Graphics.Shader.StructuredIr;
using System;
using System.Collections.Generic;
using System.Numerics;

namespace Ryujinx.Graphics.Shader.Translation
{
    class AttributeUsage
    {
        public bool NextUsesFixedFuncAttributes { get; private set; }
        public int UsedInputAttributes { get; private set; }
        public int UsedOutputAttributes { get; private set; }
        public HashSet<int> UsedInputAttributesPerPatch { get; }
        public HashSet<int> UsedOutputAttributesPerPatch { get; }
        public HashSet<int> NextUsedInputAttributesPerPatch { get; private set; }
        public int PassthroughAttributes { get; private set; }
        private int _nextUsedInputAttributes;
        private int _thisUsedInputAttributes;
        private Dictionary<int, int> _perPatchAttributeLocations;
        private readonly IGpuAccessor _gpuAccessor;
        private LinkedLocations _linkedInputs;
        private LinkedLocations _linkedOutputs;

        // 配对两端共用逻辑位置集合；IR / 插值 / XFB 仍使用客体位置，只重编号宿主 Location。
        // 解码只提供消费需求；生产者以最终 IR 声明收口，不把恒假写等解码提示当成实际输出。
        // 压缩编号第一次发射后不可改变；保留逻辑编号时新增声明不会改变已经发射的位置。
        private sealed class LinkedLocations
        {
            private uint _mask;
            private bool _emitted;
            private int _locationCount = 32;
            private string _identityOnlyReason;

            public LinkedLocations(int mask, string identityOnlyReason, int locationCount)
            {
                _mask = (uint)mask;
                _locationCount = locationCount;
                _identityOnlyReason = identityOnlyReason;
            }

            public void Include(int mask)
            {
                uint added = (uint)mask & ~_mask;
                if (_emitted && _identityOnlyReason == null && added != 0)
                {
                    throw new InvalidOperationException("Varying interface changed after shader emission.");
                }

                _mask |= (uint)mask;
            }

            public void LimitLocations(int count) => _locationCount = Math.Min(_locationCount, count);

            public void RequireIdentity(string reason)
            {
                if (reason != null && _identityOnlyReason == null)
                {
                    if (_emitted)
                    {
                        throw new InvalidOperationException("Varying interface requires logical locations after compact shader emission.");
                    }

                    _identityOnlyReason = reason;
                }
            }

            public int GetLocation(int logicalLocation)
            {
                if ((uint)logicalLocation >= 32 || (_mask & (1u << logicalLocation)) == 0)
                {
                    throw new InvalidOperationException($"Unlinked varying location {logicalLocation}.");
                }

                if (logicalLocation >= _locationCount)
                {
                    throw new NotSupportedException($"Varying location {logicalLocation} exceeds indexed interface domain {_locationCount}.");
                }

                _emitted = true;
                if (_identityOnlyReason != null) return logicalLocation;
                return BitOperations.PopCount(_mask & ((1u << logicalLocation) - 1));
            }
        }

        public bool RequiresLinkedVaryingInterface { get; private set; }
        public int FinalOutputAttributes { get; private set; }

        public void SetTargetLanguage(TargetLanguage language)
        {
            RequiresLinkedVaryingInterface = language == TargetLanguage.Spirv && _gpuAccessor.QueryHostRequiresLinkedVaryingInterface();
        }

        public int MaximumVertexOutputComponents => _gpuAccessor.QueryHostMaximumVertexOutputComponents();
        public int MaximumFragmentInputComponents => _gpuAccessor.QueryHostMaximumFragmentInputComponents();

        // 每次代码生成独立记账，不能存入 AttributeUsage：VTG 会共用它先发射计算再发射顶点。
        public sealed class InterfaceBudget
        {
            private readonly bool _enabled;
            private readonly bool _output;
            private readonly int _limit;
            private int _userEnd;
            private int _builtIns;

            public InterfaceBudget(AttributeUsage usage, ShaderStage stage)
            {
                _enabled = usage.RequiresLinkedVaryingInterface && (stage == ShaderStage.Vertex || stage == ShaderStage.Fragment);
                _output = stage == ShaderStage.Vertex;
                _limit = _output ? usage.MaximumVertexOutputComponents : usage.MaximumFragmentInputComponents;
            }

            public void Record(bool output, int location, int component, int count, bool builtIn = false)
            {
                if (!_enabled || output != _output) return;
                if (builtIn) _builtIns = checked(_builtIns + count);
                else _userEnd = Math.Max(_userEnd, checked(location * 4 + component + count));
            }

            public void Validate()
            {
                if (_enabled && _limit > 0 && _userEnd + _builtIns > _limit)
                {
                    throw new NotSupportedException($"{(_output ? "Vertex output" : "Fragment input")} interface requires {_userEnd} user endpoint + {_builtIns} built-in components; host limit {_limit}.");
                }
            }

            public static int Components(AggregateType type, int arrayLength = 1)
            {
                int count = type.GetSizeInBytes() / 4;
                if (count == 0) throw new InvalidOperationException($"Unknown interface type {type}.");
                return checked(count * ((type & AggregateType.Array) != 0 ? arrayLength : 1));
            }
        }

        public UInt128 NextInputAttributesComponents { get; private set; }
        public UInt128 ThisInputAttributesComponents { get; private set; }

        public AttributeUsage(IGpuAccessor gpuAccessor)
        {
            _gpuAccessor = gpuAccessor;

            UsedInputAttributesPerPatch = [];
            UsedOutputAttributesPerPatch = [];
        }

        public void SetInputUserAttribute(int index, int component)
        {
            int mask = 1 << index;

            UsedInputAttributes |= mask;
            _thisUsedInputAttributes |= mask;
            ThisInputAttributesComponents |= UInt128.One << (index * 4 + component);
        }

        public void SetInputUserAttributePerPatch(int index)
        {
            UsedInputAttributesPerPatch.Add(index);
        }

        public void SetOutputUserAttribute(int index)
        {
            UsedOutputAttributes |= 1 << index;
        }

        public void SetOutputUserAttributePerPatch(int index)
        {
            UsedOutputAttributesPerPatch.Add(index);
        }

        public void LinkVaryingLocations(AttributeUsage nextStage, string identityOnlyReason, int locationCount)
        {
            _linkedOutputs = new LinkedLocations(nextStage.UsedInputAttributes, identityOnlyReason, locationCount);
            nextStage._linkedInputs = _linkedOutputs;
        }

        public void LinkPassthroughInputs(AttributeUsage previousStage, string identityOnlyReason, int locationCount)
        {
            if (_linkedOutputs != null)
            {
                _linkedOutputs.RequireIdentity(identityOnlyReason);
                _linkedOutputs.LimitLocations(locationCount);
                // 原生 GS passthrough 的输入也直接成为输出；前一阶段必须共用同一编号。
                _linkedInputs = _linkedOutputs;
                previousStage._linkedOutputs = _linkedOutputs;
            }
        }

        public void InheritLinkedOutputs(AttributeUsage source, bool passthroughInputs = false)
        {
            RequiresLinkedVaryingInterface = source.RequiresLinkedVaryingInterface;
            _linkedOutputs = source._linkedOutputs;
            if (passthroughInputs) _linkedInputs = _linkedOutputs;
        }

        public void ObserveFinalInterface(IEnumerable<IoDefinition> definitions, ShaderStage stage)
        {
            if (!RequiresLinkedVaryingInterface || stage == ShaderStage.Compute) return;

            int inputMask = 0;
            int outputMask = 0;
            foreach (IoDefinition definition in definitions)
            {
                if (definition.IoVariable != IoVariable.UserDefined) continue;
                if (definition.StorageKind == StorageKind.Input) inputMask |= 1 << definition.Location;
                if (definition.StorageKind == StorageKind.Output) outputMask |= 1 << definition.Location;
            }

            _linkedInputs?.Include(inputMask);
            _linkedOutputs?.Include(outputMask);
            FinalOutputAttributes = outputMask;
        }

        public int GetVaryingLocation(int logicalLocation, bool isOutput)
        {
            LinkedLocations locations = isOutput ? _linkedOutputs : _linkedInputs;
            int hostLocation = locations?.GetLocation(logicalLocation) ?? logicalLocation;
            (isOutput ? _emittedOutputLocations : _emittedInputLocations)[logicalLocation] = hostLocation;
            return hostLocation;
        }

        // S2A：本阶段实际发射的「客体逻辑 location → 宿主 Location」，只记不改（诊断读，翻译结果不受影响）。
        private readonly SortedDictionary<int, int> _emittedInputLocations = new();
        private readonly SortedDictionary<int, int> _emittedOutputLocations = new();

        public IReadOnlyDictionary<int, int> S2AEmittedVaryingLocations(bool isOutput) =>
            isOutput ? _emittedOutputLocations : _emittedInputLocations;

        public void MergeFromtNextStage(bool gpPassthrough, bool nextUsesFixedFunctionAttributes, AttributeUsage nextStage)
        {
            NextInputAttributesComponents = nextStage.ThisInputAttributesComponents;
            NextUsedInputAttributesPerPatch = nextStage.UsedInputAttributesPerPatch;
            NextUsesFixedFuncAttributes = nextUsesFixedFunctionAttributes;
            MergeOutputUserAttributes(gpPassthrough, nextStage.UsedInputAttributes, nextStage.UsedInputAttributesPerPatch);

            if (UsedOutputAttributesPerPatch.Count != 0)
            {
                // Regular and per-patch input/output locations can't overlap,
                // so we must assign on our location using unused regular input/output locations.

                Dictionary<int, int> locationsMap = new();

                int freeMask = ~UsedOutputAttributes;

                foreach (int attr in UsedOutputAttributesPerPatch)
                {
                    int location = BitOperations.TrailingZeroCount(freeMask);
                    if (location == 32)
                    {
                        _gpuAccessor.Log($"No enough free locations for patch input/output 0x{attr:X}.");
                        break;
                    }

                    locationsMap.Add(attr, location);
                    freeMask &= ~(1 << location);
                }

                // Both stages must agree on the locations, so use the same "map" for both.
                _perPatchAttributeLocations = locationsMap;
                nextStage._perPatchAttributeLocations = locationsMap;
            }
        }

        /// <summary>S2A（10-19 出包线，阶段独立翻译）：当作下一阶段（片元）读全部 32 个通用输入的全部分量——顶点没写的输出补默认值（与原「片元读、顶点没写」同一处理），
        /// 顶点翻译结果不再随配对的片元变化，可单独翻、单独进离线库；片元用固定功能属性时调用方仍走原合并</summary>
        public void MergeAllGenericInputsForFragment(bool gpPassthrough)
        {
            NextInputAttributesComponents = UInt128.MaxValue;
            NextUsedInputAttributesPerPatch = new HashSet<int>();
            NextUsesFixedFuncAttributes = false;
            MergeOutputUserAttributes(gpPassthrough, -1, Array.Empty<int>());
        }

        private void MergeOutputUserAttributes(bool gpPassthrough, int mask, IEnumerable<int> perPatch)
        {
            _nextUsedInputAttributes = mask;

            if (gpPassthrough)
            {
                PassthroughAttributes = mask & ~UsedOutputAttributes;
            }
            else
            {
                UsedOutputAttributes |= mask;
                UsedOutputAttributesPerPatch.UnionWith(perPatch);
            }
        }

        public int GetPerPatchAttributeLocation(int index)
        {
            if (_perPatchAttributeLocations == null || !_perPatchAttributeLocations.TryGetValue(index, out int location))
            {
                return index;
            }

            return location;
        }

        public bool IsUsedOutputAttribute(int attr)
        {
            // The check for fixed function attributes on the next stage is conservative,
            // returning false if the output is just not used by the next stage is also valid.
            if (NextUsesFixedFuncAttributes &&
                attr >= AttributeConsts.UserAttributeBase &&
                attr < AttributeConsts.UserAttributeEnd)
            {
                int index = (attr - AttributeConsts.UserAttributeBase) >> 4;
                return (_nextUsedInputAttributes & (1 << index)) != 0;
            }

            return true;
        }

        public int GetFreeUserAttribute(bool isOutput, int index)
        {
            int useMask = isOutput ? _nextUsedInputAttributes : _thisUsedInputAttributes;
            int bit = -1;

            while (useMask != -1)
            {
                bit = BitOperations.TrailingZeroCount(~useMask);

                if (bit == 32)
                {
                    bit = -1;
                    break;
                }
                else if (index < 1)
                {
                    break;
                }

                useMask |= 1 << bit;
                index--;
            }

            if (bit >= 0)
            {
                // 固定功能与 Layer 共用此分配器；合成属性与客体用户属性保持同一逻辑编号域。
                LinkedLocations locations = isOutput ? _linkedOutputs : _linkedInputs;
                locations?.RequireIdentity("synthesized user attributes");
            }

            return bit;
        }

        public void SetAllInputUserAttributes()
        {
            UsedInputAttributes |= Constants.AllAttributesMask;
            ThisInputAttributesComponents |= ~UInt128.Zero >> (128 - Constants.MaxAttributes * 4);
        }

        public void SetAllOutputUserAttributes()
        {
            UsedOutputAttributes |= Constants.AllAttributesMask;
        }
    }
}
