# Custom hooks for qcom-x1e kernel build
#
# LLVM/Clang toolchain support
# When LLVM=1 is set in the environment, use Clang instead of GCC
#

# Import LLVM from environment if not already set as a make variable
LLVM ?= $(shell echo $$LLVM)

ifeq ($(LLVM),1)
    # Override compiler to use Clang
    # Now that we have aarch64-linux-gnu-clang symlink, this will work
    export gcc = clang
    
    # Set HOSTCC to clang
    export HOSTCC = clang
    
    # Always pass LLVM=1 to kernel make
    override kmake += LLVM=1
    
    $(info *** LLVM/Clang toolchain enabled for kernel build ***)
    $(info *** Using: CC=clang with LLVM=1, LD will be ld.lld ***)
endif
