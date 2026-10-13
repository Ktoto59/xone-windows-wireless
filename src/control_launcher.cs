// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Diagnostics;
using System.IO;
using System.Windows.Forms;
using System.Reflection;
[assembly: AssemblyTitle("xone-windows-wireless")]
[assembly: AssemblyVersion("0.1.0.2")]
internal static class Program {
    [STAThread] private static void Main() {
        try {
            string root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
            if (Path.GetFileName(root).Equals("outputs", StringComparison.OrdinalIgnoreCase)) root = Path.GetDirectoryName(root);
            string script = Path.Combine(root, "scripts", "Control-Panel.ps1");
            if (!File.Exists(script)) throw new FileNotFoundException("Reinstall xone-windows-wireless: the control panel is missing.");
            using (Process panel = Process.Start(new ProcessStartInfo {
                FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "System32", "WindowsPowerShell", "v1.0", "powershell.exe"),
                Arguments = "-NoProfile -ExecutionPolicy Bypass -STA -File \"" + script + "\"",
                WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true
            })) {
                string errorText = panel.StandardError.ReadToEnd();
                panel.WaitForExit();
                if (panel.ExitCode != 0) throw new InvalidOperationException("The control panel could not start. " + errorText);
            }
        } catch (Exception error) { MessageBox.Show(error.Message, "xone-windows-wireless", MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }
}
