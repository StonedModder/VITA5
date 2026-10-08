# VITA5 art

Launcher files live in `app/sce_sys/`. Keep these names and sizes.

| File | Size | Use |
| --- | --- | --- |
| `icon0.png` | 512x512 | home-screen tile |
| `pic0.png` | 3840x2160 | selection background master |
| `pic1.png` | 3840x2160 | launch background master |
| `splash.png` | 1920x1080 | in-app splash master |

The PS5 package needs `pic0` and `pic1` as DXGI_FORMAT_BC7_UNORM DX10 DDS
(`pic0.dds`, `pic1.dds`), no mipmaps. Convert the PNG masters with
`tools/prepare-assets.sh`. Do not ship the PNGs as `pic0`/`pic1` in a package.

```sh
bash tools/prepare-assets.sh --output-directory app/sce_sys \
  --selection-background app/sce_sys/pic0.png \
  --launch-background app/sce_sys/pic1.png
```
