using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace S2A.Android.Aot
{
    /// <summary>
    /// Forwards selected diagnostic lines to logcat in addition to the session log. The game
    /// session binds fd 1/2 to a private file, so without a debuggable data directory the
    /// only host-readable evidence is logcat. Only the shader/pipeline cache prefixes are
    /// forwarded, so a game log cannot flood the log buffer.
    /// </summary>
    internal sealed class LogcatTee : TextWriter
    {
        private const int InfoPriority = 4;
        private const int ErrorPriority = 6;
        private const int MaximumChunkBytes = 900;

        private static readonly string[] Forwarded = ["[s2a]", "[vkpb]", "[vkpipe]", "【红】"];

        private readonly TextWriter _inner;
        private readonly StringBuilder _line = new();
        private readonly object _gate = new();
        private readonly System.Diagnostics.Stopwatch _uptime = System.Diagnostics.Stopwatch.StartNew();

        public LogcatTee(TextWriter inner)
        {
            _inner = inner ?? throw new ArgumentNullException(nameof(inner));
        }

        public override Encoding Encoding => _inner.Encoding;

        public override void Write(char value) => Append(value.ToString());

        public override void Write(string value)
        {
            if (value != null) Append(value);
        }

        public override void WriteLine(string value) => Write(value + Environment.NewLine);

        public override void WriteLine() => Write(Environment.NewLine);

        public override void Flush()
        {
            lock (_gate)
            {
                _inner.Flush();
            }
        }

        private void Append(string text)
        {
            lock (_gate)
            {
                _inner.Write(text);
                foreach (char c in text)
                {
                    if (c == '\n') Emit();
                    else if (c != '\r' && _line.Length < 8192) _line.Append(c);
                }
            }
        }

        private void Emit()
        {
            string line = _line.ToString();
            _line.Clear();
            if (!Forward(line)) return;

            int priority = line.Contains("【红】", StringComparison.Ordinal) ? ErrorPriority : InfoPriority;
            for (int offset = 0; offset < line.Length; offset += MaximumChunkBytes)
            {
                int length = Math.Min(MaximumChunkBytes, line.Length - offset);
                AndroidLog(priority, line.Substring(offset, length));
            }
        }

        private bool Forward(string line)
        {
            // Startup failures (in particular a service that never reaches the GPU stage)
            // must be visible on a device without a readable private data directory.
            if (_uptime.Elapsed < TimeSpan.FromSeconds(10)) return true;
            foreach (string prefix in Forwarded)
                if (line.Contains(prefix, StringComparison.Ordinal)) return true;
            return line.Contains("Android session failed", StringComparison.Ordinal);
        }

        internal static void WriteError(string message) => AndroidLog(ErrorPriority, message);

        private static void AndroidLog(int priority, string message)
        {
            try
            {
                __android_log_print(priority, "S2Arm", "%s", message);
            }
            catch
            {
                // Diagnostics must never break the session; the private log still has the line.
            }
        }

        [DllImport("liblog", EntryPoint = "__android_log_print")]
        private static extern int __android_log_print(int priority, string tag, string format, string message);
    }
}
