include $(TOPDIR)/rules.mk

PKG_NAME:=rtsproxy
PKG_VERSION:=1.0.0

PKG_MAINTAINER:=plsy1
PKG_LICENSE:=GPL-3.0-only

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/meson.mk

define Package/rtsproxy
  SECTION:=net
  CATEGORY:=Network
  TITLE:=High Performance RTSP Proxy
  DEPENDS:=+libstdcpp +libpthread +libatomic
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src $(PKG_BUILD_DIR)/
	$(CP) ./include $(PKG_BUILD_DIR)/
	$(CP) ./webui $(PKG_BUILD_DIR)/
	$(CP) ./meson.build $(PKG_BUILD_DIR)/
	$(CP) ./meson_options.txt $(PKG_BUILD_DIR)/
endef

define Package/rtsproxy/description
  A high-performance RTSP proxy with unified port support, URL rewriting, and NAT traversal.
endef

define Package/rtsproxy/conffiles
/etc/config/rtsproxy
/etc/rtsproxy/config.json
endef

define Package/rtsproxy/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/rtsproxy $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) files/rtsproxy.init $(1)/etc/init.d/rtsproxy
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) files/rtsproxy.config $(1)/etc/config/rtsproxy
	$(INSTALL_DIR) $(1)/etc/rtsproxy
	$(INSTALL_CONF) files/config.json $(1)/etc/rtsproxy/config.json
	
	$(INSTALL_DIR) $(1)/usr/share/rtsproxy/www
	$(CP) $(PKG_BUILD_DIR)/webui/* $(1)/usr/share/rtsproxy/www/
endef

$(eval $(call BuildPackage,rtsproxy))
