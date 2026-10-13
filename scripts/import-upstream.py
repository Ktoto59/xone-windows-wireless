"""Import the pinned xone MT76 transport, with documented Windows adaptations."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "work/upstream-xone"
EXPECTED = "3484f603484782dd7551c64e5a33fc602b127051"
actual = subprocess.check_output(["git", "-C", str(SOURCE), "rev-parse", "HEAD"], text=True).strip()
if actual != EXPECTED:
    raise SystemExit(f"Expected {EXPECTED}, got {actual}")
out = ROOT / "src/mt76"
out.mkdir(parents=True, exist_ok=True)
for filename in ("mt76.c", "mt76.h", "mt76_defs.h"):
    text = (SOURCE / "transport" / filename).read_text(encoding="utf-8")
    text = re.sub(r"#include <linux/[^>]+>\n", "", text)
    if filename == "mt76.c":
        text = text.replace('#include "mt76.h"', '#include "../platform/xone_compat.h"\n#include "mt76.h"')
        text = text.replace("= {};", "= {0};")
        text = text.replace("memcpy(data + i,", "memcpy((u8 *)data + i,")
        # Pairing fixes from dlundqvist/xone f2aa9fe01103d7600553b505b298ff0bd47ff280:
        # expose channel switching, advertise the channel, relax RX while pairing.
        text = text.replace("static int xone_mt76_switch_channel", "int xone_mt76_switch_channel")
        beacon = text.index("static int xone_mt76_write_beacon")
        text = text[:beacon] + text[beacon:].replace("\tint err;\n", "\tint err;\n\n\tdata[14] = mt->channel ? mt->channel->index : 1;\n", 1)
        pairing = text.index("int xone_mt76_set_pairing")
        text = text[:pairing] + text[pairing:].replace("\tint err;\n", "\tint err;\n\n\txone_mt76_write_register(mt, MT_RX_FILTR_CFG, enable ? 0x014f13 : 0x017f17);\n", 1)
        text = text.replace('dev_dbg(mt->dev, "%s: address=%pM\\n", __func__, mt->address);',
                            'dev_dbg(mt->dev, "%s: address=%02x:%02x:%02x:%02x:%02x:%02x\\n", __func__,\n'
                            '            mt->address[0], mt->address[1], mt->address[2],\n'
                            '            mt->address[3], mt->address[4], mt->address[5]);')
        # Upstream logs control-transfer failures and continues. This port latches
        # the first I/O error and blocks subsequent hardware accesses in the shim.
    else:
        text = text.replace("#pragma once", '#pragma once\n#include "../platform/xone_compat.h"', 1)
        if filename == "mt76.h":
            text += "\nint xone_mt76_switch_channel(struct xone_mt76 *mt, struct xone_mt76_channel *chan);\n"
    if filename == "mt76_defs.h":
        text = text.replace("struct mt76_fw_header {", "#pragma pack(push, 1)\nstruct mt76_fw_header {")
        text += "\n#pragma pack(pop)\n"
    text = text.replace("/* SPDX", "/* SPDX", 1)
    (out / filename).write_text(text, encoding="utf-8", newline="\n")
(ROOT / "LICENSE").write_bytes((SOURCE / "LICENSE").read_bytes())
print(f"Imported MT76 transport from {actual}")
