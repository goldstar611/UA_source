# UA_source
![icon image](https://github.com/Marisa-Chan/UA_source/blob/master/svg/icon.svg?raw=true)

Opensource reimplementation of **UrbanAssault** engine. You needed copy of original game files for play.

**License** GPLv2

<a href="https://scan.coverity.com/projects/marisa-chan-ua_source">
  <img alt="Coverity Scan Build Status"
       src="https://scan.coverity.com/projects/8507/badge.svg"/>
</a>


**Dependings**: SDL2, SDL2_ttf, OpenGL, OpenAL, vorbisfile(ogg), ffmpeg


**Building**:
- linux native: **make nix**
- windows msys compile: **make msys**

[**Wiki page with instructions**](https://github.com/Marisa-Chan/UA_source/wiki)


**Fonts** directory contains:
- Liberation Mono, Liberation Sans, Liberation Serif.  Version 2.00.1
- Xolonium
- Press Start 2P
- Textar


**Licenses of used fonts**:
- The Liberation(tm) version 2.00.0 onward are Licensed under the SIL Open Font License, Version 1.1.
- Xolonium by Severin Meyer. License: SIL Open Font License (OFL)
- Press Start 2P by Cody 'CodeMan38' Boisclair. License: SIL Open Font License (OFL)
- Textar (https://github.com/yamacraft/textar-font) License: IPA-1


**Additional keys (nucleus.ini)**:
- gfx.blending   0(default)/1/2 - blending mode for transparent things. 0 - default multiply, 1 - additive, 2 - sharp edged
- gfx.solidfont   on/off(default) - enable(on) pointy font rasterizing without of pixel blending
- gfx.vsync 0/1(default)/2 - vertical sync type. 0 - disable, 1 - default sync, 2 - adaptive(if you've already missed the vertical retrace for a given frame, it swaps buffers immediately)
- gfx.maxfps 60(default) - maximal fps. Strongly recommended do not up it more than 60 - currently it will break helicopters rotations and AI
- gfx.newsky on/off(default) - enable new fade effect of far grounds
- gfx.skydistance 3000(default) - start distance of sky fade effect (do not rise it too strong, because of limits of sky sphere)
- gfx.skylength 500(default) - length of sky fade effect
- gfx.color_effects 0(default)/1/2 - emulate color effects like in original game in software rendering. 0 - disable, 1 - affects all, 2 - do not affect GUI
- gfx.color_eff_pwr[1] 0-100(default) - power of red color effect
- gfx.color_eff_pwr[2] 0-100(default) - power of blue color effect
- gfx.color_eff_pwr[3] 0-100(default) - power of green color effect
- gfx.color_eff_pwr[4] 0-100(default) - power of inverse color effect
- gfx.color_eff_pwr[5] 0-100(default) - power of invdark color effect
- gfx.color_eff_pwr[6] 0-100(default) - power of sw color effect
- gfx.color_eff_pwr[7] 0-100(default) - power of invtuerk color effect
- particles.limit 5000(default) - limit amount of particles 
- menu.windowed on/off(default) - always open game menu in windowed mode
- gfx.vbo on(default)/off - disable modern VBO(upload vertex data into video card)



- world.ini : begin_misc : fix_weapon_radius - on/off(default) - enable(on) fix of original bug that prevent working of different weapon radius
- world.ini : begin_misc : hidden_fractions - mask for hidden fractions for whole world
- levelfile : begin_level : hidden_fractions - mask for hidden fractions for this level
- vehicle : hidden (yes/no) - hide this unit type
- vehicle : unhide_radar (0 disable) - unhide number of sectors


## ua_export — asset conversion tool (goldstar611 fork)

Standalone console tool built from the same source tree as the game
(no game window needed for export/verify; the `--view` mode opens a real
640x480 window). Built via the same podman image used for the game:

```sh
podman run --rm -v /path/to/UA_source:/work:Z ua_build_trixie \
    bash -c 'cd build-trixie && cmake ../src >/dev/null && make -j2 ua_export'
```

The binary is `build-trixie/ua_export` (linux-aarch64 in this setup).

### Export a set (SET.BAS → per-model OBJ + attachments)

```sh
podman run --rm -v UA_source:/work:Z -v "game-package:/gamedata:ro" \
    ua_build_trixie /work/build-trixie/ua_export \
    /gamedata/DATA/SET1/OBJECTS/SET.BAS --outdir /work/export/set1
```

Writes, per named kid of the blob (vehicles/buildings/slurps):
`<NAME>.obj` + `<NAME>.mtl` + `<NAME>.mat.json` + `<NAME>.uattach`
(sidecar: skeleton/ANM/ADE attachments), `<MODEL>_<ORIG>.png` per texture,
`<MODEL>_<ORIG>.anm` per texture animation, `<MODEL>_ade<i>.ade` per particle
effect, `<MODEL>_<SKLT>.skl` per skeleton, plus the blob's EMBD pool
(`<ORIG>.skl`, `<ORIG>.anm`, `<ORIG>.png`). Zero-mesh kids get `<NAME>.EMPTY`.
Exit code 0 only if every kid exported.

### Verification modes

```sh
# OBJ round-trip (mesh counts, mesh merge WARNs expected)
ua_export <SET.BAS> --outdir <dir> --verify
# reconstructed-vs-original model equality (authoritative .skl skeleton,
# ADE binds, materials, textures, ANM frames)
ua_export <SET.BAS> --outdir <dir> --verify2
# like --verify2 but loads models through the REAL game chain
# (Utils::ProxyLoadBase("rsrc:objects/<NAME>.obj") against the converted
# working dir layout)
ua_export <SET.BAS> --verify3 --gamepath <converted-workdir>
```

All three must exit 0 with `619/619`-style summaries per set.

### Interactive model viewer

```sh
ua_export --view <NAME>            # converted model (game path, uattach loader)
ua_export --view <NAME> --blob <path/to/SET.BAS>   # original blob kid
ua_export --view <NAME> --set 2    # model from another set
```

Run from inside the converted working dir (or pass `--gamepath <dir>`).
640x480 windowed. Controls: mouse-drag / arrows = orbit, wheel / PgUp-PgDn =
zoom, `R` = reset view, `ESC`/`Q` = quit, `F` = flip view basis, `W` = front
face CW/CCW, `C` = culling on/off (current state printed to stdout).
Debug knobs: `UA_EXPORT_PITCH`, `UA_EXPORT_YAW`, `UA_EXPORT_SNAPSHOT=n`
(save a PNG after n frames and exit), `UA_EXPORT_FLIPY`, `UA_EXPORT_CWFRONT`,
`UA_EXPORT_NOCULL`.

**Note:** the model-viewer camera/orientation state is known to be not fully
understood — the current default (CW front faces + steep pitch) renders
models correctly for inspection, but the exact convention mismatch vs the
in-game renderer (front-face winding vs view-basis up-row sign) is not yet
root-caused. If a model renders wrong, try the `F`/`W`/`C` toggles.

### Python assembly (separate repo: urban_assault_decompiled)

`objconvert.py` copies the game package into a working dir, runs the engine
export per set (DATA and dataxp trees), assembles the converted layout
(per-model assets, regenerated `visproto.lst` / `set.sdf` / `slurps.lst`,
`RSRCPOOL` fill, `MC2RES` collision skeletons) and validates every reference.
`docs/texture-mapping.md` there documents per-model texture usage.
