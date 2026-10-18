using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

// Controller for the GPL-3.0-or-later ModOrganizer2/usvfs component.
// It does not run ModOrganizer.exe or alter the user's global filesystem view.
record Mapping(string Source, string Destination, bool CreateTarget = false);
record LaunchPlan(string Runtime, string Exe, string[] Arguments, string WorkingDirectory,
    Mapping[] Directories, Mapping[] Files, int TimeoutSeconds, string LogPath, string[]? ForcedLibraries = null);

static class Program
{
    static void Absolute(string path)
    {
        if (!Path.IsPathFullyQualified(path)) throw new ArgumentException("Absolute path required: " + path);
    }
    static string Quote(string value)
    {
        var result = new StringBuilder("\""); int slashes = 0;
        foreach (char c in value)
        {
            if (c == '\\') { slashes++; continue; }
            result.Append('\\', c == '"' ? slashes * 2 + 1 : slashes).Append(c); slashes = 0;
        }
        return result.Append('\\', slashes * 2).Append('"').ToString();
    }
    static int Main(string[] args)
    {
        nint parameters = 0; bool connected = false;
        Native.ProcessInfo child = default;
        StreamWriter? log = null;
        try
        {
            if (args.Length != 1) throw new ArgumentException("Expected one launch-plan JSON path.");
            var plan = JsonSerializer.Deserialize<LaunchPlan>(File.ReadAllText(args[0]),
                new JsonSerializerOptions { PropertyNameCaseInsensitive = true }) ?? throw new Exception("Empty launch plan.");
            foreach (string path in new[] { plan.Runtime, plan.Exe, plan.WorkingDirectory, plan.LogPath }) Absolute(path);
            if (!File.Exists(plan.Exe) || !Directory.Exists(plan.WorkingDirectory)) throw new IOException("Executable or working directory missing.");
            foreach (var map in plan.Directories.Concat(plan.Files)) { Absolute(map.Source); Absolute(map.Destination); }
            Directory.CreateDirectory(Path.GetDirectoryName(plan.LogPath)!);
            log = new StreamWriter(plan.LogPath, append: false, Encoding.UTF8) { AutoFlush = true };
            Native.Load(plan.Runtime);
            string? version = Marshal.PtrToStringUTF8(Native.usvfsVersionString());
            Native.usvfsInitLogging(false);
            parameters = Native.usvfsCreateParameters();
            if (parameters == 0) throw new Exception("usvfsCreateParameters returned null.");
            Native.usvfsSetInstanceName(parameters, "vrcenter-" + Guid.NewGuid().ToString("N"));
            Native.usvfsSetDebugMode(parameters, false);
            Check(Native.usvfsCreateVFS(parameters), "create VFS"); connected = true;
            Native.usvfsClearVirtualMappings(); Native.usvfsClearSkipFileSuffixes();
            Native.usvfsAddSkipFileSuffix(".mohidden");
            Native.usvfsClearSkipDirectories(); Native.usvfsAddSkipDirectory(".git");
            foreach (var map in plan.Directories)
            {
                if (!Directory.Exists(map.Source)) throw new DirectoryNotFoundException(map.Source);
                Check(Native.usvfsVirtualLinkDirectoryStatic(map.Source, map.Destination,
                    8u | (map.CreateTarget ? 4u : 0u)), "map directory " + map.Source);
            }
            foreach (var map in plan.Files)
                Check(Native.usvfsVirtualLinkFile(map.Source, map.Destination, 0), "map file " + map.Source);
            Native.usvfsClearLibraryForceLoads();
            foreach (string library in plan.ForcedLibraries ?? [])
            { Absolute(library); Native.usvfsForceLoadLibrary(Path.GetFileName(plan.Exe), library); }
            Drain(log);
            var startup = new Native.StartupInfo { cb = Marshal.SizeOf<Native.StartupInfo>() };
            var command = new StringBuilder(string.Join(" ", new[] { plan.Exe }.Concat(plan.Arguments).Select(Quote)));
            Check(Native.usvfsCreateProcessHooked(plan.Exe, command, 0, 0, false, 0x08000000,
                0, plan.WorkingDirectory, ref startup, out child), "create hooked process");
            log.WriteLine("pid=" + child.processId + " usvfs=" + version);
            var timer = Stopwatch.StartNew();
            while (Native.WaitForSingleObject(child.process, 200) == 258)
            {
                Drain(log);
                if (plan.TimeoutSeconds > 0 && timer.Elapsed.TotalSeconds >= plan.TimeoutSeconds)
                {
                    // Only our own returned process handle, never a name-based kill.
                    Native.TerminateProcess(child.process, 124);
                    throw new TimeoutException("Hooked child exceeded launch-plan timeout.");
                }
            }
            Drain(log);
            Check(Native.GetExitCodeProcess(child.process, out uint exit), "read child exit");
            Console.WriteLine(JsonSerializer.Serialize(new { pid = child.processId, exitCode = exit, usvfs = version }));
            return checked((int)exit);
        }
        catch (Exception error)
        {
            if (log != null) { Drain(log); log.WriteLine(error); }
            Console.Error.WriteLine(error.Message); return 1;
        }
        finally
        {
            if (child.thread != 0) Native.CloseHandle(child.thread);
            if (child.process != 0) Native.CloseHandle(child.process);
            if (connected) Native.usvfsDisconnectVFS();
            if (parameters != 0) Native.usvfsFreeParameters(parameters);
            log?.Dispose();
        }
    }
    static void Check(bool success, string operation)
    {
        if (!success) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }
    static void Drain(StreamWriter output)
    {
        if (!Native.Loaded) return;
        var buffer = new StringBuilder(65536);
        for (int i = 0; i < 4096 && Native.usvfsGetLogMessages(buffer, (nuint)buffer.Capacity, false); i++)
        { output.WriteLine(buffer.ToString()); buffer.Clear(); }
    }
}

static class Native
{
    const string Dll = "usvfs";
    public static bool Loaded { get; private set; }
    public static void Load(string folder)
    {
        CheckDllDirectory(folder);
        nint library = NativeLibrary.Load(Path.Combine(folder, "usvfs_x64.dll"));
        NativeLibrary.SetDllImportResolver(typeof(Native).Assembly, (name, _, _) => name == Dll ? library : 0);
        Loaded = true;
    }
    static void CheckDllDirectory(string folder)
    {
        if (!SetDllDirectory(folder)) throw new Win32Exception(Marshal.GetLastWin32Error());
    }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct StartupInfo
    {
        public int cb; public nint reserved, desktop, title;
        public uint x, y, width, height, xChars, yChars, fillAttribute, flags;
        public ushort showWindow, reserved2Count; public nint reserved2, stdInput, stdOutput, stdError;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct ProcessInfo { public nint process, thread; public uint processId, threadId; }
    [DllImport("kernel32", CharSet = CharSet.Unicode, SetLastError = true)] static extern bool SetDllDirectory(string path);
    [DllImport("kernel32", SetLastError = true)] public static extern uint WaitForSingleObject(nint handle, uint milliseconds);
    [DllImport("kernel32", SetLastError = true)] public static extern bool GetExitCodeProcess(nint handle, out uint code);
    [DllImport("kernel32")] public static extern bool TerminateProcess(nint handle, uint code);
    [DllImport("kernel32")] public static extern bool CloseHandle(nint handle);
    [DllImport(Dll)] public static extern nint usvfsVersionString();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern nint usvfsCreateParameters();
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void usvfsFreeParameters(nint p);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)] public static extern void usvfsSetInstanceName(nint p, string name);
    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)] public static extern void usvfsSetDebugMode(nint p, bool debug);
    [DllImport(Dll, SetLastError = true)] public static extern bool usvfsCreateVFS(nint p);
    [DllImport(Dll)] public static extern void usvfsDisconnectVFS();
    [DllImport(Dll)] public static extern void usvfsClearVirtualMappings();
    [DllImport(Dll)] public static extern void usvfsClearSkipFileSuffixes();
    [DllImport(Dll)] public static extern void usvfsClearSkipDirectories();
    [DllImport(Dll)] public static extern void usvfsClearLibraryForceLoads();
    [DllImport(Dll, CharSet = CharSet.Unicode)] public static extern void usvfsForceLoadLibrary(string process, string library);
    [DllImport(Dll, CharSet = CharSet.Unicode)] public static extern void usvfsAddSkipFileSuffix(string suffix);
    [DllImport(Dll, CharSet = CharSet.Unicode)] public static extern void usvfsAddSkipDirectory(string directory);
    [DllImport(Dll, CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool usvfsVirtualLinkDirectoryStatic(string source, string destination, uint flags);
    [DllImport(Dll, CharSet = CharSet.Unicode, SetLastError = true)] public static extern bool usvfsVirtualLinkFile(string source, string destination, uint flags);
    [DllImport(Dll)] public static extern void usvfsInitLogging([MarshalAs(UnmanagedType.I1)] bool local);
    [DllImport(Dll, CharSet = CharSet.Ansi)]
    [return: MarshalAs(UnmanagedType.I1)] public static extern bool usvfsGetLogMessages(StringBuilder buffer, nuint size, [MarshalAs(UnmanagedType.I1)] bool blocking);
    [DllImport(Dll, CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool usvfsCreateProcessHooked(string application, StringBuilder command,
        nint processAttributes, nint threadAttributes, bool inheritHandles, uint flags,
        nint environment, string directory, ref StartupInfo startup, out ProcessInfo process);
}
