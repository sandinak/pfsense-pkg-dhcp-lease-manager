#!/bin/sh
# Copy the package files straight onto a pfSense box for quick iteration,
# without building a pkg. Usage: scripts/deploy-dev.sh root@pfsense.lan
#
# The page is then reachable at https://<host>/status_dhcp_lease_manager.php
# (the Status menu entry only appears when installed as a real package).
set -eu
HOST="${1:?usage: $0 user@pfsense-host}"
cd "$(dirname "$0")/../files"
tar -cf - \
	usr/local/pkg/dhcp_lease_manager.inc \
	usr/local/pkg/dhcp_lease_manager.xml \
	usr/local/www/status_dhcp_lease_manager.php \
	usr/local/www/dhcp_lease_manager \
	etc/inc/priv/dhcp_lease_manager.priv.inc \
	| ssh "$HOST" 'tar -xpf - -C / && php -l /usr/local/pkg/dhcp_lease_manager.inc && php -l /usr/local/www/status_dhcp_lease_manager.php'
echo "Deployed. Open https://${HOST#*@}/status_dhcp_lease_manager.php"
