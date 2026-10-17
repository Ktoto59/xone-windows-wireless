# xone Wireless branding

The selected Radio Wave concept combines an X-shaped flowing connection with
two radio arcs. The white/green mark on a dark tile is shared by the repository,
Windows executables and Setup. It is independent project artwork.

| File | Purpose |
| --- | --- |
| [logo.png](logo.png) | Approved high-resolution artwork, with transparent outer corners |
| [app.ico](app.ico) | Windows icon containing 16, 24, 32, 48, 64, 128 and 256 px frames |
| [app.rc](app.rc) | Icon resource embedded in native executables |

Regenerate the ICO on Windows after changing the approved PNG:

```powershell
.\scripts\Export-AppIcon.ps1
```

This only encodes/resizes the artwork into Windows icon frames; builds use the
committed ICO and need no image tools or network access. Setup's own icon uses
the same ICO; Installed apps and uninstall shortcuts reference the embedded icon
in `xone-background.exe`. Changes appear when installing the updated build.

## Artwork provenance

Created with the built-in `image_gen` tool from the selected second concept,
then approved for the project. The production edit prompt was:

> Isolate the selected dark app icon from the Radio Wave concept. Preserve the
> diagonal green ribbon, two white crossing fragments and two green radio arcs.
> Center the mark on a dark charcoal rounded-square tile with generous padding
> and transparent outer corners. Use clean vector-like edges. No wordmark,
> additional symbols, shadows or mockups.

The artwork is distributed with the project under GPL-2.0-or-later; see the
root [license](../../LICENSE).
