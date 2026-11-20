$code = @"
using System;
using System.Threading;
using System.Runtime.InteropServices;

public class LauncherDaemon {
    [StructLayout(LayoutKind.Sequential)]
    public struct STARTUPINFO {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PROCESS_INFORMATION {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr OpenDesktop(string lpszDesktop, uint dwFlags, bool fInherit, uint dwDesiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool SetThreadDesktop(IntPtr hDesktop);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool CloseDesktop(IntPtr hDesktop);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    public static extern bool CreateProcess(
        string lpApplicationName,
        string lpCommandLine,
        IntPtr lpProcessAttributes,
        IntPtr lpThreadAttributes,
        bool bInheritHandles,
        uint dwCreationFlags,
        IntPtr lpEnvironment,
        string lpCurrentDirectory,
        ref STARTUPINFO lpStartupInfo,
        out PROCESS_INFORMATION lpProcessInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr hObject);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    public static void Run() {
        Thread worker = new Thread(() => {
            IntPtr hDesk = OpenDesktop("Default", 0, false, 0x1FF);
            if (hDesk == IntPtr.Zero) {
                Console.WriteLine("OpenDesktop failed: " + Marshal.GetLastWin32Error());
                return;
            }
            if (!SetThreadDesktop(hDesk)) {
                Console.WriteLine("SetThreadDesktop failed: " + Marshal.GetLastWin32Error());
                CloseDesktop(hDesk);
                return;
            }

            STARTUPINFO si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(si);
            si.lpDesktop = null;
            PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
            string app = @"C:\Users\Recko\Documents\github\kyty\_Build\windows\install\kyty_launcher.exe";
            string cmd = "\"" + app + "\"";
            string dir = @"C:\Users\Recko\Documents\github\kyty\_Build\windows\install";
            bool ok = CreateProcess(app, cmd, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, dir, ref si, out pi);
            if (!ok) {
                Console.WriteLine("CreateProcess failed: " + Marshal.GetLastWin32Error());
                CloseDesktop(hDesk);
                return;
            }

            Console.WriteLine("KytyPS5 Launcher started on Default desktop! PID: " + pi.dwProcessId);

            // Bring to front after brief initialization
            for (int i = 0; i < 30; i++) {
                Thread.Sleep(200);
                IntPtr hWnd = FindWindow(null, "KytyPS5 Launcher");
                if (hWnd != IntPtr.Zero) {
                    ShowWindow(hWnd, 9); // SW_RESTORE
                    SetForegroundWindow(hWnd);
                    Console.WriteLine("Window brought to foreground: " + hWnd);
                    break;
                }
            }

            // Wait indefinitely until the user closes the launcher
            WaitForSingleObject(pi.hProcess, 0xFFFFFFFF);
            Console.WriteLine("Launcher closed by user.");

            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseDesktop(hDesk);
        });

        worker.SetApartmentState(ApartmentState.STA);
        worker.Start();
        worker.Join();
    }
}
"@

Add-Type -TypeDefinition $code -Language CSharp
[LauncherDaemon]::Run()
