$ErrorActionPreference='Stop'
if(-not ('VRCenter.ProfilePathChecks' -as [type])){
    Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text.RegularExpressions;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
using System.Collections;
using System.Collections.Generic;
using System.Management.Automation;
using System.Security.Cryptography;
using System.Globalization;
using System.Buffers;
namespace VRCenter {
    // Non-authoritative operation audit. Recovery continues to inspect the
    // immutable plan/manifests and actual filesystem hashes, not these records.
    public sealed class JournalProgressWriter : IDisposable {
        public const int MaximumRecordBytes = 1048576;
        private readonly string path;
        private readonly string[] immutablePaths;
        private readonly List<FileStream> immutableHandles = new List<FileStream>();
        private FileStream stream;
        private bool failed;
        public long Sequence { get; private set; }
        public string PathName { get { return path; } }
        public string ReceiptPath { get; private set; }
        public JournalProgressWriter(string path) : this(path, Array.Empty<string>(), null) { }
        public JournalProgressWriter(string path, string[] immutablePaths, string receiptPath) {
            this.path = Path.GetFullPath(path);
            this.immutablePaths = (string[])immutablePaths.Clone();
            ReceiptPath = receiptPath == null ? null : Path.GetFullPath(receiptPath);
            try {
                foreach (string part in this.immutablePaths) {
                    ProfilePathChecks.Plain(part);
                    immutableHandles.Add(new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.Read));
                }
                ProfilePathChecks.Plain(this.path);
                Directory.CreateDirectory(Path.GetDirectoryName(this.path));
                // A new transaction must never adopt an existing foreign log.
                // Retain this append-only handle; no seek/truncate API is exposed.
                stream = new FileStream(this.path, FileMode.CreateNew, FileAccess.Write, FileShare.Read, 65536);
                Guard(this.path);
            } catch { Dispose(); throw; }
        }
        private void Guard(string requestedPath) {
            if (stream == null || failed) throw new IOException("Journal progress writer is closed or failed.");
            string requested = Path.GetFullPath(requestedPath);
            ProfilePathChecks.Plain(requested);
            if (!String.Equals(requested, path, StringComparison.OrdinalIgnoreCase) ||
                !String.Equals(ProfilePathChecks.FinalPath(stream), path, StringComparison.OrdinalIgnoreCase))
                throw new IOException("Journal progress path identity changed.");
            for (int i = 0; i < immutablePaths.Length; i++) {
                string part = immutablePaths[i];
                // Fresh ancestry and live handle proof on every call, never a
                // cached filesystem identity. The held handles deny write/delete.
                ProfilePathChecks.Plain(part);
                if (!String.Equals(ProfilePathChecks.FinalPath(immutableHandles[i]), part, StringComparison.OrdinalIgnoreCase))
                    throw new IOException("Journal immutable part identity changed or is unavailable.");
            }
        }
        public void Validate() { Guard(path); }
        public void Append(string requestedPath, string json) {
            try {
                Guard(requestedPath);
                if (json.IndexOf('\n') >= 0 || json.IndexOf('\r') >= 0) throw new InvalidOperationException("Progress record must be one JSON line.");
                byte[] bytes = new UTF8Encoding(false, true).GetBytes(json + "\n");
                if (bytes.Length > MaximumRecordBytes) throw new InvalidOperationException("Journal progress record exceeds bound.");
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush();
                Sequence++;
            } catch { failed = true; throw; }
        }
        public void Dispose() {
            try { if (stream != null) { var held = stream; stream = null; held.Dispose(); } }
            finally { foreach (var part in immutableHandles) part.Dispose(); immutableHandles.Clear(); }
        }
        public sealed class Reader : IDisposable {
            private readonly IEnumerator<string> source;
            public Reader(string path) { source = ReadLines(path).GetEnumerator(); }
            public bool MoveNext() { return source.MoveNext(); }
            public string Current { get { return source.Current; } }
            public void Dispose() { source.Dispose(); }
        }
        public static IEnumerable<string> ReadLines(string path) {
            string full = Path.GetFullPath(path);
            ProfilePathChecks.Plain(full);
            using (var input = new FileStream(full, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 65536)) {
                if (!String.Equals(ProfilePathChecks.FinalPath(input), full, StringComparison.OrdinalIgnoreCase)) throw new IOException("Journal progress read identity changed.");
                using (var line = new MemoryStream()) {
                    int next;
                    while ((next = input.ReadByte()) >= 0) {
                        if (line.Length + 1 > MaximumRecordBytes) throw new InvalidOperationException("Journal progress record exceeds bound.");
                        if (next == 10) {
                            yield return new UTF8Encoding(false, true).GetString(line.GetBuffer(), 0, checked((int)line.Length));
                            line.SetLength(0);
                        } else line.WriteByte((byte)next);
                    }
                    // Only an incomplete final audit record may be ignored.
                }
            }
        }
    }
    public static class ProfilePathChecks {
        public static byte[] HashStream(Stream stream) {
            ArgumentNullException.ThrowIfNull(stream);
            byte[] buffer = ArrayPool<byte>.Shared.Rent(128 * 1024);
            try {
                using (var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256)) {
                    int read;
                    while ((read = stream.Read(buffer, 0, 128 * 1024)) != 0) hash.AppendData(buffer, 0, read);
                    return hash.GetHashAndReset();
                }
            } finally { ArrayPool<byte>.Shared.Return(buffer, clearArray: true); }
        }
        public static bool Utf8FileEquals(string path, string json) {
            return File.ReadAllBytes(path).AsSpan().SequenceEqual(Encoding.UTF8.GetBytes(json));
        }
        private static string Text(object value) { return LanguagePrimitives.ConvertTo<string>(value); }
        private static object Unwrap(object value) { var wrapper = value as PSObject; return wrapper == null ? value : wrapper.BaseObject; }
        private static bool Excluded(IDictionary scope, string relative) {
            var policy = (IDictionary)scope["exclusionPolicy"];
            var rootOnly = (HashSet<string>)policy["rootOnly"];
            if (rootOnly.Contains(relative)) return true;
            var exact = (HashSet<string>)policy["exact"];
            string cursor = relative;
            while (!String.IsNullOrEmpty(cursor)) {
                if (exact.Contains(cursor)) return true;
                int slash = cursor.LastIndexOf('/'); if (slash < 0) break;
                cursor = cursor.Substring(0, slash);
            }
            foreach (WildcardPattern pattern in (IEnumerable)policy["patterns"]) if (pattern.IsMatch(relative)) return true;
            return false;
        }
        private static bool Protected(IDictionary scope, string relative) {
            if (relative != "." && Excluded(scope, relative)) return true;
            var policy = (IDictionary)scope["exclusionPolicy"];
            return (relative == "." && (bool)policy["hasExclusions"]) || (bool)policy["protectedAllDirs"] || ((HashSet<string>)policy["protectedAncestors"]).Contains(relative);
        }
        public static string StorePath(string root, string relative) {
            Relative(relative, "directory");
            string path = Path.GetFullPath(Path.Combine(root, relative));
            if (!path.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Profile store path escaped its root.");
            Plain(path); return path;
        }
        public static string TargetPath(IDictionary scope, string relative) {
            if (scope == null) throw new InvalidOperationException("Unknown profile scope.");
            string root = Text(scope["root"]), kind = Text(scope["kind"]);
            Relative(relative, kind);
            if (Excluded(scope, relative)) throw new InvalidOperationException("Profile path is excluded by scope policy.");
            bool file = String.Equals(kind, "file", StringComparison.InvariantCultureIgnoreCase);
            string path = file ? root : Path.GetFullPath(Path.Combine(root, relative));
            if (!file && !path.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("Profile path escaped its scope.");
            Plain(path); return path;
        }
        public static string DirectoryPath(IDictionary scope, string relative) {
            if (scope == null || !String.Equals(Text(scope["kind"]), "directory", StringComparison.InvariantCultureIgnoreCase)) throw new InvalidOperationException("Invalid directory profile scope.");
            string path = relative == "." ? Text(scope["root"]) : TargetPath(scope, relative);
            Plain(path); return path;
        }
        public static object[] SortRows(object[] rows) {
            if (rows == null) return Array.Empty<object>();
            var output = (object[])rows.Clone();
            var culture = CultureInfo.CurrentCulture;
            Array.Sort(output, (left, right) => {
                var a = (IDictionary)Unwrap(left); var b = (IDictionary)Unwrap(right);
                int order = LanguagePrimitives.Compare(a["scope"], b["scope"], true, culture);
                return order != 0 ? order : LanguagePrimitives.Compare(a["path"], b["path"], true, culture);
            });
            return output;
        }
        public static object[] Inventory(IDictionary scopes, IDictionary skip) {
            var result = new List<object>();
            foreach (object value in scopes.Values) {
                var scope = (IDictionary)Unwrap(value); string root = Text(scope["root"]), kind = Text(scope["kind"]), id = Text(scope["id"]);
                bool fileScope = String.Equals(kind, "file", StringComparison.InvariantCultureIgnoreCase);
                Plain(root); FileAttributes rootAttributes;
                try { rootAttributes = File.GetAttributes(root); } catch (FileNotFoundException) { continue; } catch (DirectoryNotFoundException) { continue; }
                if (fileScope == ((rootAttributes & FileAttributes.Directory) != 0)) throw new InvalidOperationException("Scope root kind does not match the filesystem.");
                var pending = new Stack<string>(); pending.Push(root);
                while (pending.Count != 0) {
                    string path = pending.Pop(); FileAttributes attributes = File.GetAttributes(path);
                    if ((attributes & FileAttributes.ReparsePoint) != 0) throw new InvalidOperationException("Reparse entry in a profile scope.");
                    string relative = fileScope ? "." : (String.Equals(path, root, StringComparison.InvariantCultureIgnoreCase) ? "" : path.Substring(root.Length + 1).Replace('\\', '/'));
                    if (relative.Length != 0 && Excluded(scope, relative)) continue;
                    if ((attributes & FileAttributes.Directory) != 0) { foreach (string child in Directory.EnumerateFileSystemEntries(path)) pending.Push(child); continue; }
                    if (skip.Contains(id + "/" + relative)) continue;
                    Relative(relative, kind); long size = new FileInfo(path).Length;
                    Plain(path); string hash;
                    using (var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read)) hash = Convert.ToHexString(HashStream(stream));
                    result.Add(new Hashtable(StringComparer.OrdinalIgnoreCase) { {"scope",id}, {"path",relative}, {"sha256",hash}, {"size",size} });
                }
            }
            return result.ToArray();
        }
        public static object[] DirectoryInventory(IDictionary scopes) {
            var result = new List<object>();
            foreach (object value in scopes.Values) {
                var scope = (IDictionary)Unwrap(value); string root = Text(scope["root"]), id = Text(scope["id"]);
                if (!String.Equals(Text(scope["kind"]), "directory", StringComparison.InvariantCultureIgnoreCase) || !Directory.Exists(root)) continue;
                Plain(root); var pending = new Stack<string>(); pending.Push(root);
                while (pending.Count != 0) {
                    string path = pending.Pop(), relative = String.Equals(path, root, StringComparison.InvariantCultureIgnoreCase) ? "." : Path.GetRelativePath(root, path).Replace('\\', '/');
                    if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) throw new InvalidOperationException("Reparse directory in profile inventory.");
                    if (relative != "." && Excluded(scope, relative)) continue;
                    if (!Protected(scope, relative)) result.Add(new Hashtable(StringComparer.OrdinalIgnoreCase) { {"scope",id}, {"path",relative} });
                    foreach (string child in Directory.EnumerateDirectories(path)) pending.Push(child);
                }
            }
            return result.ToArray();
        }
        public static void ManifestDirectories(IDictionary scopes, object[] raw, object[] files, IDictionary output) {
            foreach (object item in raw) {
                if (!LanguagePrimitives.IsTrue(item)) continue;
                var entry = Unwrap(item) as IDictionary;
                if (entry == null) throw new InvalidOperationException("Invalid directory profile scope.");
                string id = Text(entry["scope"]), relative = Text(entry["path"]);
                var scope = scopes[id] as IDictionary;
                if (scope == null || !String.Equals(Text(scope["kind"]), "directory", StringComparison.InvariantCultureIgnoreCase)) throw new InvalidOperationException("Invalid directory profile scope.");
                DirectoryPath(scope, relative);
                if (!Protected(scope, relative)) output[id + "/" + relative] = new Hashtable(StringComparer.OrdinalIgnoreCase) { {"scope",entry["scope"]}, {"path",entry["path"]} };
            }
            foreach (object item in files) {
                var entry = (IDictionary)Unwrap(item);
                string id = Text(entry["scope"]), relative = Text(entry["path"]);
                var scope = (IDictionary)scopes[id];
                if (!String.Equals(Text(scope["kind"]), "directory", StringComparison.InvariantCultureIgnoreCase)) continue;
                while (relative != ".") {
                    int slash = relative.LastIndexOf('/'); relative = slash >= 0 ? relative.Substring(0, slash) : ".";
                    if (!Protected(scope, relative)) output[id + "/" + relative] = new Hashtable(StringComparer.OrdinalIgnoreCase) { {"scope",entry["scope"]}, {"path",relative} };
                }
            }
        }
        [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        private static extern uint GetFinalPathNameByHandleW(SafeFileHandle handle, StringBuilder path, uint length, uint flags);
        // Null is an unavailable proof, never permission to reuse cached bytes.
        public static string FinalPath(FileStream stream) {
            try {
                var buffer = new StringBuilder(512);
                uint length = GetFinalPathNameByHandleW(stream.SafeFileHandle, buffer, (uint)buffer.Capacity, 0);
                if (length == 0) return null;
                if (length >= buffer.Capacity) {
                    buffer = new StringBuilder(checked((int)length + 1));
                    length = GetFinalPathNameByHandleW(stream.SafeFileHandle, buffer, (uint)buffer.Capacity, 0);
                    if (length == 0 || length >= buffer.Capacity) return null;
                }
                string path = buffer.ToString();
                if (path.StartsWith(@"\\?\UNC\", StringComparison.OrdinalIgnoreCase)) return @"\\" + path.Substring(8);
                if (path.StartsWith(@"\\?\", StringComparison.Ordinal)) return path.Substring(4);
                return path;
            } catch (DllNotFoundException) { return null; }
              catch (EntryPointNotFoundException) { return null; }
              catch (PlatformNotSupportedException) { return null; }
        }
        private static readonly Regex InvalidCharacters = new Regex("[\\\\:*?\"<>|\\x00-\\x1f]", RegexOptions.Compiled);
        // Matches the existing PowerShell -match expression with current-culture
        // case folding. No filesystem identity or path result is cached here.
        private const string DeviceName = "^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\\..*)?$";
        public static void Plain(string path) {
            string cursor = Path.GetFullPath(path);
            while (!String.IsNullOrEmpty(cursor)) {
                FileAttributes? attributes = null;
                try { attributes = File.GetAttributes(cursor); }
                catch (FileNotFoundException) { }
                catch (DirectoryNotFoundException) { }
                if (attributes.HasValue && (attributes.Value & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidOperationException("Reparse path is not allowed: " + cursor);
                cursor = Path.GetDirectoryName(cursor);
            }
        }
        public static void Relative(string path, string kind) {
            if (String.Equals(kind, "file", StringComparison.InvariantCultureIgnoreCase)) {
                if (path != ".") throw new InvalidOperationException("A file scope accepts only the relative path dot.");
                return;
            }
            bool invalid = String.IsNullOrEmpty(path) || Path.IsPathRooted(path) || InvalidCharacters.IsMatch(path);
            if (!invalid) foreach (string segment in path.Split('/')) {
                if (segment == "" || segment == "." || segment == ".." || segment.EndsWith(".", StringComparison.Ordinal) || segment.EndsWith(" ", StringComparison.Ordinal) || Regex.IsMatch(segment, DeviceName)) { invalid = true; break; }
            }
            if (invalid) throw new InvalidOperationException("Invalid relative profile path: " + path);
        }
    }
}
'@
}
