#!/bin/sh
# Build a real .pkg on a FreeBSD/pfSense host without a ports tree, staging
# files the same way the port Makefile's do-install does. Use it to check
# pkg-plist and the install hooks; Netgate's builders use the Makefile itself.
#
#   scripts/build-pkg.sh root@pfsense.lan            build into /tmp/dlm-build on the host
#   scripts/build-pkg.sh root@pfsense.lan --install  build, then `pkg add` it
set -eu
HOST="${1:?usage: $0 user@freebsd-host [--install]}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mk() { awk -v k="$1" -F'=[ \t]*' '$1==k {print $2}' "$ROOT/Makefile"; }
PORTNAME=$(mk PORTNAME); VERSION=$(mk PORTVERSION); COMMENT=$(mk COMMENT)
MAINTAINER=$(mk MAINTAINER); CATEGORY=$(mk CATEGORIES)
WWW=$(mk WWW)

cd "$ROOT"
tar -cf - Makefile pkg-descr pkg-plist files | ssh "$HOST" "
set -e
B=/tmp/dlm-build; rm -rf \$B; mkdir -p \$B/src \$B/stage; cd \$B/src; tar -xf -
S=\$B/stage; P=/usr/local; D=\$P/share/${PORTNAME}
# --- mirror of do-install ---
mkdir -p \$S\$P/pkg \$S\$P/www/dhcp_lease_manager \$S/etc/inc/priv \$S\$D
install -m 0644 files\$P/pkg/dhcp_lease_manager.xml files\$P/pkg/dhcp_lease_manager.inc \$S\$P/pkg/
install -m 0644 files\$P/www/status_dhcp_lease_manager.php \$S\$P/www/
install -m 0644 files\$P/www/dhcp_lease_manager/lease_manager.js files\$P/www/dhcp_lease_manager/lease_manager.css \$S\$P/www/dhcp_lease_manager/
install -m 0644 files/etc/inc/priv/dhcp_lease_manager.priv.inc \$S/etc/inc/priv/
install -m 0644 files\$D/info.xml \$S\$D/
sed -i '' 's|%%PKGVERSION%%|${VERSION}|' \$S\$D/info.xml
# --- SUB_FILES ---
sed 's|%%PORTNAME%%|${PORTNAME}|g' files/pkg-install.in > \$B/install.sh
sed 's|%%PORTNAME%%|${PORTNAME}|g' files/pkg-deinstall.in > \$B/deinstall.sh
# --- plist: expand %%DATADIR%% and check it matches the staged files exactly ---
sed 's|%%DATADIR%%|share/${PORTNAME}|' pkg-plist > \$B/plist
grep -v '^@' \$B/plist | sed 's|^\([^/]\)|/usr/local/\1|' | sort > \$B/plist.files
(cd \$S && find . -type f | sed 's|^\.||' | sort) > \$B/stage.files
if ! diff -u \$B/plist.files \$B/stage.files; then echo 'pkg-plist does not match staged files' >&2; exit 1; fi
php -r '
\$d = trim(preg_replace(\"/^WWW:.*\$/m\", \"\", file_get_contents(\"pkg-descr\")));
echo json_encode(array(
  \"name\" => \"${PORTNAME}\", \"version\" => \"${VERSION}\",
  \"origin\" => \"${CATEGORY}/${PORTNAME}\", \"comment\" => \"${COMMENT}\",
  \"desc\" => \$d, \"maintainer\" => \"${MAINTAINER}\", \"www\" => \"${WWW}\",
  \"prefix\" => \"/usr/local\", \"arch\" => \"FreeBSD:*\",
  \"licenselogic\" => \"single\", \"licenses\" => array(\"APACHE20\"),
  \"scripts\" => array(\"install\" => file_get_contents(\"/tmp/dlm-build/install.sh\"),
                        \"deinstall\" => file_get_contents(\"/tmp/dlm-build/deinstall.sh\")),
), JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES);' > \$B/+MANIFEST
pkg create -M \$B/+MANIFEST -r \$S -p \$B/plist -o \$B
ls -l \$B/*.pkg
pkg info -l -F \$B/${PORTNAME}-${VERSION}.pkg
"
if [ "${2:-}" = "--install" ]; then
	ssh "$HOST" "pkg add -f /tmp/dlm-build/${PORTNAME}-${VERSION}.pkg && pkg info ${PORTNAME}"
fi
