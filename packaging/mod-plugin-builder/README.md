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
            ├── neural-amp-modeler.mk
            ├── 0001-nam-core-undef-major-minor.patch
            └── 0002-nam-core-atomic-shared-ptr-libstdcxx-pre-12.patch
```

The layout mirrors what `mod-plugin-builder` expects under
`plugins/package/<plugin-name>/`. Buildroot auto-discovers the package by
its directory name (no `Config.in` snippet needed) and auto-applies the
`*.patch` files after extract.

### About the patches

Two upstream NAM Core issues prevent the build from succeeding with the
moddwarf-new toolchain (gcc 9.4 + glibc 2.27):

1. **`0001-nam-core-undef-major-minor.patch`** — `<sys/types.h>` on
   glibc < 2.28 leaks `major()` / `minor()` as macros that expand to
   `gnu_dev_major` / `gnu_dev_minor`. They collide with the data members
   of `nam::Version`. The patch adds `#undef major` / `#undef minor`
   after the includes in `get_dsp.h`. No-op on newer glibc.

2. **`0002-nam-core-atomic-shared-ptr-libstdcxx-pre-12.patch`** —
   `SlimmableWavenet` uses `std::atomic<std::shared_ptr<T>>` (the
   C++20 P0718 specialisation). libstdc++ only ships it from gcc 12
   onward; gcc 9.4 errors with `std::atomic requires a trivially
   copyable type`. NAM Core already has a libc++ workaround using the
   deprecated `std::atomic_*` free function overloads on `shared_ptr`
   (declared in `<memory>`, available since C++11). The patch
   generalises that fallback to also trigger when
   `__cpp_lib_atomic_shared_ptr` is undefined (i.e. libstdc++ pre-12).

Both patches are safe upstream candidates; the second in particular
would benefit any libstdc++ < 12 environment. A PR against
NeuralAmpModelerCore would let us drop both patches eventually.

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
- The `moddwarf-new` toolchain shipped by mod-plugin-builder uses
  **gcc 9.4** (crosstool-ng 1.25.0). That predates the final C++20
  standard, but supports the draft via `-std=c++2a`. NeuralAudio's
  actual C++20 usage is minimal (mostly `if constexpr`, which is
  C++17, plus one `std::bit_cast` in `math_approx` that already has a
  `#if !__cpp_lib_bit_cast` fallback), so the build works with the
  draft flag. The recipe therefore does **not** force `-std=gnu++20`;
  it lets CMake's standard auto-selection pick `-std=c++2a`.
- `BUILD_STATIC_RTNEURAL` is intentionally OFF: it produces a large set of
  template instantiations that bloat the binary without helping NAM
  models. Flip it ON if you need static RTNeural model paths.
- LTO is enabled by default (via mod-plugin-builder's `BR2_SKIP_LTO`
  convention). Set `BR2_SKIP_LTO=1` in the build environment if you hit
  RAM pressure during the build.
- `DUSE_NATIVE_ARCH=OFF` is forced — `mod-plugin-builder` already injects
  the correct `-mcpu`/`-mtune` (Cortex-A35 baseline on the current
  moddwarf-new toolchain) and `-march=armv8-a+...` via `TARGET_CFLAGS` /
  `TARGET_CXXFLAGS`.

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
