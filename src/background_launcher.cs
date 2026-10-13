// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
internal static class BackgroundHost {
    private static int Main() {
        string root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
        if (Path.GetFileName(root).Equals("outputs", StringComparison.OrdinalIgnoreCase)) root = Path.GetDirectoryName(root);
        string work = Path.Combine(root, "work");
        Directory.CreateDirectory(work);
        try {
            string script = Path.Combine(root, "scripts", "Run-MaintainedBridge.ps1");
            if (!File.Exists(script)) throw new FileNotFoundException("Background script is missing.");
            using (StreamWriter log = new StreamWriter(Path.Combine(work, "background-host.err"), false)) {
                log.AutoFlush = true;
                using (Process child = new Process()) {
                    child.StartInfo = new ProcessStartInfo {
                        FileName=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "WindowsPowerShell", "v1.0", "powershell.exe"),
                        Arguments="-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \""+script+"\"",
                        WorkingDirectory=root, UseShellExecute=false, CreateNoWindow=true, RedirectStandardError=true
                    };
                    child.ErrorDataReceived += delegate(object sender, DataReceivedEventArgs message) {
                        if (message.Data != null) {lock(log) {log.WriteLine(message.Data);}}
                    };
                    child.Start(); child.BeginErrorReadLine(); child.WaitForExit();
                    return child.ExitCode;
                }
            }
        } catch (Exception error) {
            File.AppendAllText(Path.Combine(work, "background-host.err"), error.ToString()+Environment.NewLine);
            return 1;
        }
    }
}
