#
# Samsung Common file.
#

obj-y += ss_self_display_common.o
obj-y += ss_dsi_panel_sysfs.o
obj-y += ss_dsi_panel_debug.o
obj-y += ss_dsi_panel_common.o
obj-y += ss_dsi_mdnie_lite_common.o
obj-y += ss_dsi_smart_dimming_common.o
obj-y += ss_regulator_common.o
obj-$(CONFIG_SAMSUNG_DEBUG_DISPLAY) += dlog.o
obj-$(CONFIG_DISPLAY_USE_INFO) += ss_dpui_common.o

# TO SUPPORT POC FLASH
obj-$(CONFIG_SUPPORT_POC_FLASH) += ss_ddi_poc_common.o

# TO SUPPORT SPI INTERFACE
# obj-y  += ss_ddi_spi_common.o

#
# panel directory make file
#

# TO SUPPORT PBA BOOINT
obj-y += PBA_BOOTING/

#Jackpot2
obj-$(CONFIG_PANEL_S6E3FA7_AMS604NL01_FHD) += S6E3FA7_AMS604NL01/
