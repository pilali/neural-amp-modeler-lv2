# mod-plugin-builder packaging

Recipe to cross-compile `neural-amp-modeler.lv2` for MOD devices (Dwarf
in particular) using
[mod-plugin-builder](https://github.com/moddevices/mod-plugin-builder).

## Layout

```
packaging/mod-plugin-builder/
└── plugins/
    └── package/
        └── neural-amp-modeler/
            └── neural-amp-modeler.mk
```

The layout mirrors what `mod-plugin-builder` expects under
`plugins/package/<plugin-name>/`. Buildroot auto-discovers the package by
its directory name, no `Config.in` snippet is needed.

## Installing the recipe

```bash
# from a fresh clone of mod-plugin-builder
cp -r /path/to/neural-amp-modeler-lv2/packaging/mod-plugin-builder/plugins/package/neural-amp-modeler \
      plugins/package/
```

Or symlink it if you want changes in the source tree to flow through.

## Building

After `./bootstrap.sh moddwarf-new` has finished (one-time, ~1h):

```bash
./build moddwarf-new neural-amp-modeler
```

The resulting bundle ends up in `~/mod-workdir/moddwarf-new/plugins/neural_amp_modeler.lv2/`.

To clean: `./build moddwarf-new neural-amp-modeler-dirclean`.

To deploy onto a Dwarf on the local network:

```bash
./build moddwarf-new neural-amp-modeler-publish
```

## Notes

- The recipe pins to a specific branch of the fork. Bump
  `NEURAL_AMP_MODELER_VERSION` (commit SHA preferred for reproducibility)
  and `NEURAL_AMP_MODELER_SITE` when promoting a release.
- gcc 12 toolchain (provided by mod-plugin-builder for `moddwarf-new`)
  fully supports the C++20 features used by NeuralAudio.
- `BUILD_STATIC_RTNEURAL` is intentionally OFF: it produces a large set of
  template instantiations that bloat the binary without helping NAM
  models. Flip it ON if you need static RTNeural model paths.
- LTO is enabled by default (via mod-plugin-builder's `BR2_SKIP_LTO`
  convention). Set `BR2_SKIP_LTO=1` in the build environment if you hit
  RAM pressure during the build.
- `DUSE_NATIVE_ARCH=OFF` is forced — `mod-plugin-builder` already injects
  the correct `-mcpu=cortex-a53 -mtune=cortex-a53` (or equivalent) via
  `TARGET_CFLAGS` / `TARGET_CXXFLAGS`.

## Performance expectations on the Dwarf

The Dwarf's i.MX 8M Mini (4× Cortex-A53 @ 1.6 GHz, NEON, no FP16) can
run NAM models, but only the smaller variants comfortably:

| Model size  | Typical CPU load on Dwarf |
|-------------|---------------------------|
| `nano`      | low                       |
| `feather`   | comfortable               |
| `lite`      | tight                     |
| `standard`  | usually too heavy         |

Use the `Quality` knob (port `quality_scale`, range 0..1) to trade
fidelity for CPU at runtime. It is wired through NeuralAudio's
`SetQualityScaleFactor()` and applies to any model that reports
`HasQualityScaling() == true`:

- **NAM A2 WaveNet (slimmable)** — the knob calls
  `nam::SlimmableModel::SetSlimmableSize()` and dynamically resizes the
  network's channels/bottleneck. This is the most useful target on the
  Dwarf.
- **Dynamic RTNeural models** that expose a slimmable variant.

Older non-slimmable NAM models ignore the knob (their
`HasQualityScaling()` returns `false`); pick a smaller pre-trained model
in that case.

The `NAM_ENABLE_A2_FAST=ON` option in the recipe also enables NAM Core's
A2 fast-path WaveNet implementation, which is hand-tuned around NEON
register usage on aarch64.
