include $(TOPDIR)/rules.mk

PKG_NAME:=rtsproxy
PKG_VERSION:=1.0.0
PKG_RELEASE:=1

PKG_MAINTAINER:=plsy1
PKG_LICENSE:=GPL-3.0-only

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/meson.mk

define Package/rtsproxy
  SECTION:=net
  CATEGORY:=Network
  TITLE:=High Performance RTSP Proxy
  DEPENDS:=+libstdcpp +libpthread
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ../../* $(PKG_BUILD_DIR)/
endef

define Package/rtsproxy/description
  A high-performance RTSP proxy with unified port support, URL rewriting, and NAT traversal.
endef

define Package/rtsproxy/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/rtsproxy $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/rtsproxy.init $(1)/etc/init.d/rtsproxy
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/rtsproxy.config $(1)/etc/config/rtsproxy
endef

$(eval $(call BuildPackage,rtsproxy))
