# SPDX-License-Identifier: GPL-2.0
dtb-$(CONFIG_CAMERA_STAR2) += exynos/exynos9810-star2lte_eur_open_16.dtb \
	exynos/exynos9810-star2lte_eur_open_17.dtb \
	exynos/exynos9810-star2lte_eur_open_18.dtb \
	exynos/exynos9810-star2lte_eur_open_20.dtb \
	exynos/exynos9810-star2lte_eur_open_23.dtb \
	exynos/exynos9810-star2lte_eur_open_26.dtb

dtb-$(CONFIG_CAMERA_STAR) += exynos/exynos9810-starlte_eur_open_16.dtb \
	exynos/exynos9810-starlte_eur_open_17.dtb \
	exynos/exynos9810-starlte_eur_open_18.dtb \
	exynos/exynos9810-starlte_eur_open_20.dtb \
	exynos/exynos9810-starlte_eur_open_23.dtb \
	exynos/exynos9810-starlte_eur_open_26.dtb

targets += dtbs
DTB_LIST  := $(dtb-y) $(dtbo-y)
always    := $(DTB_LIST)

dtbs: $(addprefix $(obj)/, $(DTB_LIST))

clean-files := *.dtb*
