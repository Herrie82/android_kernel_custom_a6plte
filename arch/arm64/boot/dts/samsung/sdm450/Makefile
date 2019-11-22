# Samsung SDM450 Project

dtb-$(CONFIG_MACH_A6PLTE_EUR_OPEN) += sdm450-sec-a6plte_eur_open-r00.dtb \
					sdm450-sec-a6plte_eur_open-r01.dtb \
					sdm450-sec-a6plte_eur_open-r02.dtb \
					sdm450-sec-a6plte_eur_open-r03.dtb \
					sdm450-sec-a6plte_eur_open-r04.dtb

ifeq ($(CONFIG_ARM64),y)
always          := $(dtb-y)
subdir-y        := $(dts-dirs)
else
targets += dtbs
targets += $(addprefix ../, $(dtb-y))

$(obj)/../../%.dtb: $(src)/%.dts FORCE
        $(call if_changed_dep,dtc)

dtbs: $(addprefix $(obj)/../../,$(dtb-y))
endif
clean-files := *.dtb *.reserve.dts
