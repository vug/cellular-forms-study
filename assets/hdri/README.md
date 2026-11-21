# Environment maps

Equirectangular HDR images for the viewer's environment light (`cf_view --env <file>`, or the
Environment panel, which lists the `.hdr` files in this folder).

All six are from [Poly Haven](https://polyhaven.com/hdris), downloaded on 2026-11-21 at 1k
resolution (1024 x 512). Poly Haven assets are CC0 (public domain); no attribution is required,
it is given here so the source can be found again.

| File | Asset | What it is |
| --- | --- | --- |
| `satara_night_1k.hdr` | https://polyhaven.com/a/satara_night | Night sky with stars and a lamp |
| `shanghai_bund_1k.hdr` | https://polyhaven.com/a/shanghai_bund | City skyline at night, neon |
| `studio_small_08_1k.hdr` | https://polyhaven.com/a/studio_small_08 | Photo studio, soft light, low contrast |
| `fireplace_1k.hdr` | https://polyhaven.com/a/fireplace | Room at night lit by a fire |
| `neon_photostudio_1k.hdr` | https://polyhaven.com/a/neon_photostudio | Studio with fluorescent tubes |
| `wooden_studio_10_1k.hdr` | https://polyhaven.com/a/wooden_studio_10 | Dark studio with blue, pink and orange panels |

At 1k the maps are meant for lighting; as a background they are soft. Any other `.hdr` (or
`.png`, `.jpg`) placed here shows up in the panel; the 2k and 4k versions are at
`https://dl.polyhaven.org/file/ph-assets/HDRIs/hdr/<2k|4k>/<asset>_<2k|4k>.hdr`.
