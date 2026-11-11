<?php
/*
 * Standalone parser tests (no pfSense needed): php tests/parse_test.php
 */
function gettext($s) { return $s; }
require __DIR__ . '/../files/usr/local/pkg/dhcp_lease_manager.inc';

$fail = 0;
function check($cond, $msg) {
	global $fail;
	echo ($cond ? "ok   " : "FAIL ") . $msg . "\n";
	if (!$cond) { $fail++; }
}

$isc = dlm_parse_isc_leases(__DIR__ . '/fixtures/dhcpd.leases');
check(count($isc) === 2, 'ISC: duplicate lease blocks collapse to latest');
$byip = array_column($isc, null, 'ip');
check($byip['192.168.1.100']['starts'] === gmmktime(11, 0, 0, 11, 11, 2026), 'ISC: latest starts wins, parsed as UTC');
check($byip['192.168.1.100']['hostname'] === 'macbook', 'ISC: hostname');
check($byip['192.168.1.101']['state'] === 'free', 'ISC: binding state');

$kea = dlm_parse_kea_leases(__DIR__ . '/fixtures');
$byip = array_column($kea, null, 'ip');
check(count($kea) === 3, 'Kea: later rows override earlier for same address');
check($byip['192.168.2.50']['starts'] === 1762862400 - 7200, 'Kea: starts = expire - valid_lifetime');
check($byip['192.168.2.50']['hostname'] === 'raspberrypi', 'Kea: trailing dot stripped from hostname');
check($byip['192.168.2.51']['ends'] === 1762866000, 'Kea: newest row used');
check($byip['192.168.2.52']['state'] === 'declined', 'Kea: state 1 => declined');

$oui = array('001B63' => 'Apple');
check(dlm_vendor_for_mac('00:1b:63:aa:bb:cc', $oui) === array('Apple', 'oui'), 'Vendor: OUI hit');
check(dlm_vendor_for_mac('3a:11:22:33:44:55', $oui)[1] === 'private', 'Vendor: locally administered => private');
check(dlm_vendor_for_mac('00:11:22:33:44:55', $oui)[1] === 'unknown', 'Vendor: miss => unknown');
check(dlm_normalize_mac('00-1B-63-AA-BB-CC') === '00:1b:63:aa:bb:cc', 'MAC normalization');

exit($fail ? 1 : 0);
