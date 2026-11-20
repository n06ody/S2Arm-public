using Ryujinx.Graphics.Shader.StructuredIr;
using Ryujinx.Graphics.Shader.Translation;

namespace Ryujinx.Graphics.Shader.CodeGen
{
    readonly struct CodeGenParameters
    {
        public readonly AttributeUsage AttributeUsage;
        public readonly ShaderDefinitions Definitions;
        public readonly ShaderProperties Properties;
        public readonly HostCapabilities HostCapabilities;
        public readonly ILogger Logger;
        public readonly TargetApi TargetApi;
        public readonly ulong GpuAddress;   // S2A：被编译程序的 Maxwell 代码 GPU VA——报告要用程序身份（互锁省略计数的首个程序键），0 = 调用方没给

        public CodeGenParameters(
            AttributeUsage attributeUsage,
            ShaderDefinitions definitions,
            ShaderProperties properties,
            HostCapabilities hostCapabilities,
            ILogger logger,
            TargetApi targetApi,
            ulong gpuAddress = 0)
        {
            AttributeUsage = attributeUsage;
            Definitions = definitions;
            Properties = properties;
            HostCapabilities = hostCapabilities;
            Logger = logger;
            TargetApi = targetApi;
            GpuAddress = gpuAddress;
        }
    }
}
