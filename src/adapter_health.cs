// SPDX-License-Identifier: GPL-2.0-or-later
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace Xone {
    public sealed class AdapterSnapshot {
        public string InstanceId;
        public string Service;
        public string Status;
        public uint Problem;
    }
    public static class AdapterHealth {
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)]
        private static extern uint CM_Locate_DevNodeW(out uint node, string id, uint flags);
        [DllImport("cfgmgr32.dll")]
        private static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint node, uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)]
        private static extern uint CM_Get_DevNode_Registry_PropertyW(uint node, uint property, out uint type, [Out] byte[] value, ref uint length, uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)]
        private static extern uint CM_Get_Device_ID_List_SizeW(out uint length, string filter, uint flags);
        [DllImport("cfgmgr32.dll", CharSet=CharSet.Unicode)]
        private static extern uint CM_Get_Device_ID_ListW(string filter, [Out] char[] list, uint length, uint flags);

        public static AdapterSnapshot Read(string id) {
            const string prefix = "USB\\VID_045E&PID_02FE\\";
            if (String.IsNullOrEmpty(id) || !id.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ||
                id.Length == prefix.Length || id.IndexOf('\\', prefix.Length) >= 0 || id.IndexOf('\0') >= 0)
                throw new ArgumentException("Expected an external 045E:02FE adapter instance.");
            uint node, status, problem, type;
            uint result = CM_Locate_DevNodeW(out node, id, 0);
            if (result == 0x0d) return null;
            if (result != 0) throw new InvalidOperationException("Locate adapter failed: " + result);
            result = CM_Get_DevNode_Status(out status, out problem, node, 0);
            if (result == 0x0d) return null;
            if (result != 0) throw new InvalidOperationException("Read adapter state failed: " + result);
            byte[] value = new byte[1024]; uint length = (uint)value.Length;
            result = CM_Get_DevNode_Registry_PropertyW(node, 5, out type, value, ref length, 0);
            string service = result == 0 && type == 1 && length <= value.Length ? Encoding.Unicode.GetString(value, 0, (int)length).TrimEnd('\0') : "";
            return new AdapterSnapshot {InstanceId=id, Service=service, Problem=problem,
                Status=(status & 8) != 0 && (status & 0x400) == 0 ? "OK" : "Error"};
        }

        public static AdapterSnapshot Find() {
            uint length;
            uint result = CM_Get_Device_ID_List_SizeW(out length, "USB", 0x101);
            if (result != 0 || length > 1048576) throw new InvalidOperationException("USB list size failed: " + result);
            char[] list = new char[length];
            result = CM_Get_Device_ID_ListW("USB", list, length, 0x101);
            if (result == 0x1a) return null; // Bus changed; retry on next supervisor tick.
            if (result != 0) throw new InvalidOperationException("USB list failed: " + result);
            AdapterSnapshot found = null;
            foreach (string id in new string(list).Split('\0')) {
                if (!id.StartsWith("USB\\VID_045E&PID_02FE\\", StringComparison.OrdinalIgnoreCase)) continue;
                AdapterSnapshot current = Read(id);
                if (current == null) continue;
                if (found != null) throw new InvalidOperationException("Select one adapter in the control panel.");
                found = current;
            }
            return found;
        }
    }
}
