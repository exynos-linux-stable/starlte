dtb-$(CONFIG_ARCH_EXYNOS) += $(patsubst $(srctree)/$(src)/%.dts,%.dtb, $(wildcard $(srctree)/$(src)/$(subst $\",,$(CONFIG_ARCH_EXYNOS_DTS_NAME))*.dts))

always		:= $(dtb-y)
subdir-y	:= $(dts-dirs)
clean-files	:= *.dtb
