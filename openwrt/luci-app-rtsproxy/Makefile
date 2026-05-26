include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-rtsproxy
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=$(USER)
PKG_LICENSE:=GPL-3.0-only

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-rtsproxy
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI support for rtsproxy
  DEPENDS:=+rtsproxy
  PKGARCH:=all
endef

define Package/luci-app-rtsproxy/description
  LuCI support for rtsproxy.
endef

define Build/Compile
endef

define Package/luci-app-rtsproxy/install
	$(INSTALL_DIR) $(1)/usr/share/luci/menu.d
	$(INSTALL_DATA) ./root/usr/share/luci/menu.d/luci-app-rtsproxy.json $(1)/usr/share/luci/menu.d/
	$(INSTALL_DIR) $(1)/usr/share/rpcd/acl.d
	$(INSTALL_DATA) ./root/usr/share/rpcd/acl.d/luci-app-rtsproxy.json $(1)/usr/share/rpcd/acl.d/
	$(INSTALL_DIR) $(1)/www/luci-static/resources/view/rtsproxy
	$(INSTALL_DATA) ./htdocs/luci-static/resources/view/rtsproxy/config.js $(1)/www/luci-static/resources/view/rtsproxy/
endef

$(eval $(call BuildPackage,luci-app-rtsproxy))
