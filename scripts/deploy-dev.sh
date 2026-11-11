#!/bin/sh
# Install the package files straight onto a pfSense box, without building a pkg.
#
#   scripts/deploy-dev.sh root@pfsense.lan            copy files + register (menu entry)
#   scripts/deploy-dev.sh root@pfsense.lan --files    copy files only (fast iteration)
#   scripts/deploy-dev.sh root@pfsense.lan --remove   unregister and delete the files
#
# Registering runs pfSense's own `rc.packages <pkg> POST-INSTALL`, which adds the
# package and its Status menu entry to config.xml (pfSense keeps a config backup).
set -eu
HOST="${1:?usage: $0 user@pfsense-host [--files|--remove]}"
MODE="${2:-}"
PKG=pfSense-pkg-dhcp-lease-manager
VERSION=$(awk '/^PORTVERSION=/ {print $2}' "$(dirname "$0")/../Makefile")
FILES="usr/local/pkg/dhcp_lease_manager.inc
usr/local/pkg/dhcp_lease_manager.xml
usr/local/www/status_dhcp_lease_manager.php
usr/local/www/dhcp_lease_manager
etc/inc/priv/dhcp_lease_manager.priv.inc
usr/local/share/${PKG}"

if [ "$MODE" = "--remove" ]; then
	ssh "$HOST" "/usr/local/bin/php -f /etc/rc.packages ${PKG} DEINSTALL; cd / && rm -rf $(echo $FILES)"
	echo "Removed from ${HOST}."
	exit 0
fi

cd "$(dirname "$0")/../files"
tar -cf - $FILES | ssh "$HOST" "tar -xpf - -C / \
	&& sed -i '' 's|%%PKGVERSION%%|${VERSION}|' /usr/local/share/${PKG}/info.xml \
	&& php -l /usr/local/pkg/dhcp_lease_manager.inc >/dev/null \
	&& php -l /usr/local/www/status_dhcp_lease_manager.php >/dev/null"
if [ "$MODE" != "--files" ]; then
	ssh "$HOST" "/usr/local/bin/php -f /etc/rc.packages ${PKG} POST-INSTALL"
fi
echo "Deployed ${VERSION}. Open https://${HOST#*@}/status_dhcp_lease_manager.php"
