######################################
#
# neural-amp-modeler
#
######################################

# Pin to a specific commit (or branch tip) of the fork we publish from.
# Update both VERSION and SITE when bumping.
NEURAL_AMP_MODELER_VERSION = claude/neural-amp-moddwarf-port-SS1D1
NEURAL_AMP_MODELER_SITE = https://github.com/pilali/neural-amp-modeler-lv2.git
NEURAL_AMP_MODELER_SITE_METHOD = git
NEURAL_AMP_MODELER_BUNDLES = neural_amp_modeler.lv2

# Custom optimization flags. Matches the convention used by other neural / DSP
# packages (e.g. aidadsp-lv2): drop -funsafe-loop-optimizations (which trips
# Eigen/RTNeural template-heavy code on gcc 12), keep -fPIC, and enable LTO
# unless the user opts out via BR2_SKIP_LTO.
NEURAL_AMP_MODELER_TARGET_OPT  = $(filter-out -funsafe-loop-optimizations,$(subst ",,$(BR2_TARGET_OPTIMIZATION)))
NEURAL_AMP_MODELER_TARGET_OPT += -fno-unsafe-loop-optimizations
NEURAL_AMP_MODELER_TARGET_OPT += -fPIC

ifndef BR2_SKIP_LTO
NEURAL_AMP_MODELER_TARGET_OPT += -fno-strict-aliasing -flto -ffat-lto-objects
endif

# CMake options
NEURAL_AMP_MODELER_CONF_OPTS  = -DCMAKE_BUILD_TYPE=Release
NEURAL_AMP_MODELER_CONF_OPTS += -DCMAKE_INSTALL_PREFIX=/usr
NEURAL_AMP_MODELER_CONF_OPTS += -DMOD_BUILD=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DUSE_NATIVE_ARCH=OFF
NEURAL_AMP_MODELER_CONF_OPTS += -DSMART_BYPASS_ENABLED=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DDISABLE_DENORMALS=ON

# NeuralAudio knobs (forwarded by add_subdirectory)
NEURAL_AMP_MODELER_CONF_OPTS += -DBUILD_NAMCORE=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DBUILD_INTERNAL_STATIC_WAVENET=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DBUILD_INTERNAL_STATIC_LSTM=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DBUILD_STATIC_RTNEURAL=OFF
NEURAL_AMP_MODELER_CONF_OPTS += -DNAM_ENABLE_A2_FAST=ON
NEURAL_AMP_MODELER_CONF_OPTS += -DWAVENET_MATH=FastMath
NEURAL_AMP_MODELER_CONF_OPTS += -DLSTM_MATH=FastMath

# RTNeural backend: Eigen (NEON-vectorised on aarch64)
NEURAL_AMP_MODELER_CONF_OPTS += -DRTNEURAL_EIGEN=ON

# Toolchain flags. moddwarf-new ships gcc 9.4 (crosstool-ng 1.25.0), which
# predates the final C++20 standard. We pass -std=c++2a (the draft alias gcc
# 9.4 understands) explicitly; the C++20 features NeuralAudio actually uses
# work under this draft (if constexpr is C++17 anyway, and math_approx's
# std::bit_cast usage has a #if !__cpp_lib_bit_cast fallback).
NEURAL_AMP_MODELER_CONF_OPTS += -DCMAKE_C_FLAGS="$(TARGET_CFLAGS) $(NEURAL_AMP_MODELER_TARGET_OPT)"
NEURAL_AMP_MODELER_CONF_OPTS += -DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) $(NEURAL_AMP_MODELER_TARGET_OPT) -std=c++2a"
NEURAL_AMP_MODELER_CONF_OPTS += -DCMAKE_SHARED_LINKER_FLAGS="$(TARGET_LDFLAGS) $(NEURAL_AMP_MODELER_TARGET_OPT)"

# NeuralAudio + RTNeural + Eigen are git submodules; we need them all.
NEURAL_AMP_MODELER_PRE_DOWNLOAD_HOOKS += MOD_PLUGIN_BUILDER_DOWNLOAD_WITH_SUBMODULES

$(eval $(cmake-package))
