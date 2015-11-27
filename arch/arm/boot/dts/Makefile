ifeq ($(CONFIG_OF),y)

dtb-$(CONFIG_ARCH_MXC) += \
	imx6dl-sabreauto.dtb \
	imx6dl-sabreauto-ecspi.dtb \
	imx6dl-sabreauto-flexcan1.dtb \
	imx6dl-sabreauto-gpmi-weim.dtb \
	imx6dl-sabresd.dtb \
	imx6dl-sabresd-enetirq.dtb \
	imx6dl-sabresd-hdcp.dtb \
	imx6dl-sabresd-ldo.dtb \
	imx6dl-sabresd-pf200.dtb \
	imx6dl-wandboard.dtb \
	imx6dl-maxle.dtb \
	imx6q-arm2.dtb \
	imx6q-sabreauto.dtb \
	imx6q-sabreauto-ecspi.dtb \
	imx6q-sabreauto-flexcan1.dtb \
	imx6q-sabreauto-gpmi-weim.dtb \
	imx6q-sabrelite.dtb \
	imx6q-sabresd.dtb \
	imx6q-sabresd-enetirq.dtb \
	imx6q-sabresd-uart.dtb \
	imx6q-sabresd-hdcp.dtb \
	imx6q-sabresd-ldo.dtb \
	imx6q-maxle.dtb 


targets += dtbs
targets += $(dtb-y)
endif

# *.dtb used to be generated in the directory above. Clean out the
# old build results so people don't accidentally use them.
dtbs: $(addprefix $(obj)/, $(dtb-y))
	$(Q)rm -f $(obj)/../*.dtb

clean-files := *.dtb
