<?php
/*
 * dhcp_lease_manager.inc
 *
 * Copyright (c) 2026 Branson Matheson
 * All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Backend-agnostic lease collection.
 *
 * Every lease, whatever its source, is normalized to:
 *   ip, mac, cid, hostname, descr, if, if_descr,
 *   starts (epoch|null), ends (epoch|null), age (seconds|null),
 *   state (active|expired|static|reserved|declined|free|backup|unknown),
 *   online (bool|null), is_static (bool),
 *   vendor (string), vendor_kind (oui|private|unknown), source
 *
 * Lease sources, in order of preference:
 *   1. system_get_dhcpleases()  - pfSense's own abstraction (2.7+), which
 *                                 already handles ISC dhcpd and Kea.
 *   2. Direct parse of the active backend's lease database:
 *        ISC dhcpd : /var/dhcpd/var/db/dhcpd.leases
 *        Kea       : /var/db/kea/dhcp4.leases (memfile CSV, incl. LFC files)
 *   Any future backend only needs a new dlm_parse_*() function that returns
 *   the same normalized shape.
 */

define('DLM_ISC_LEASES', '/var/dhcpd/var/db/dhcpd.leases');
/* pfSense keeps Kea's memfile in /var/db/kea; /var/lib/kea is Kea's upstream default. */
define('DLM_KEA_LEASE_DIRS', ['/var/db/kea', '/var/lib/kea']);
define('DLM_OUI_DB_CANDIDATES', [
	'/usr/local/share/nmap/nmap-mac-prefixes',
]);

function dhcp_lease_manager_install() {
	/* Nothing to set up; page is read-only and config edits go through core pages. */
}

function dhcp_lease_manager_deinstall() {
}

/* ---------------------------------------------------------------- helpers */

function dlm_config_get($path, $default = null) {
	if (function_exists('config_get_path')) {
		return config_get_path($path, $default);
	}
	global $config;
	$node = $config;
	foreach (explode('/', $path) as $key) {
		if (!is_array($node) || !array_key_exists($key, $node)) {
			return $default;
		}
		$node = $node[$key];
	}
	return $node;
}

/* Which DHCPv4 server is in use: 'kea', 'isc', or 'unknown'. */
function dlm_detect_backend() {
	if (function_exists('dhcp_is_backend')) {
		if (dhcp_is_backend('kea')) {
			return 'kea';
		}
		if (dhcp_is_backend('isc')) {
			return 'isc';
		}
	}
	$cfg = dlm_config_get('dhcpbackend');
	if ($cfg === 'kea' || $cfg === 'isc') {
		return $cfg;
	}
	/* Older releases without the setting are ISC-only. */
	if (file_exists(DLM_ISC_LEASES)) {
		return 'isc';
	}
	if (dlm_kea_lease_dir() !== null) {
		return 'kea';
	}
	return 'unknown';
}

function dlm_normalize_mac($mac) {
	$hex = strtolower(preg_replace('/[^0-9a-fA-F]/', '', (string)$mac));
	if (strlen($hex) < 12) {
		return strtolower(trim((string)$mac));
	}
	return implode(':', str_split(substr($hex, 0, 12), 2));
}

/* Accepts epoch ints, "YYYY/MM/DD HH:MM:SS" (UTC, ISC style) or other strtotime-able text. */
function dlm_to_epoch($value) {
	if ($value === null || $value === '' || $value === 'n/a' || $value === 'never') {
		return null;
	}
	if (is_int($value) || (is_string($value) && ctype_digit($value))) {
		return (int)$value;
	}
	$value = preg_replace('/^\d\s+/', '', trim((string)$value)); /* strip ISC weekday digit */
	$ts = strtotime($value . ' UTC');
	if ($ts === false) {
		$ts = strtotime($value);
	}
	return ($ts === false) ? null : $ts;
}

/* -------------------------------------------------------- manufacturer */

function dlm_load_oui_table() {
	static $table = null;
	if ($table !== null) {
		return $table;
	}
	/* pfSense core helper (util.inc) reads the nmap prefix list. */
	if (function_exists('load_mac_manufacturer_table')) {
		$t = load_mac_manufacturer_table();
		if (is_array($t) && count($t) > 0) {
			$table = $t;
			return $table;
		}
	}
	$table = array();
	foreach (DLM_OUI_DB_CANDIDATES as $file) {
		if (!is_readable($file)) {
			continue;
		}
		foreach (file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
			if ($line[0] === '#') {
				continue;
			}
			$parts = preg_split('/\s+/', $line, 2);
			if (count($parts) === 2) {
				$table[strtoupper($parts[0])] = $parts[1];
			}
		}
		break;
	}
	return $table;
}

/* Returns array(vendor, kind) where kind is oui|private|unknown. */
function dlm_vendor_for_mac($mac, $table) {
	$hex = strtoupper(str_replace(':', '', $mac));
	if (strlen($hex) < 6) {
		return array('Unknown', 'unknown');
	}
	if (isset($table[substr($hex, 0, 6)])) {
		return array($table[substr($hex, 0, 6)], 'oui');
	}
	/* Locally administered bit set: phones/laptops with MAC randomization. */
	if (hexdec(substr($hex, 0, 2)) & 0x02) {
		return array('Private (randomized MAC)', 'private');
	}
	return array('Unknown', 'unknown');
}

/* ------------------------------------------------------- static mappings */

/* Map of mac => array(if, ip, hostname, descr) for every configured static mapping. */
function dlm_static_maps() {
	$out = array();
	$dhcpd = dlm_config_get('dhcpd', array());
	if (!is_array($dhcpd)) {
		return $out;
	}
	foreach ($dhcpd as $if => $ifcfg) {
		if (!is_array($ifcfg) || empty($ifcfg['staticmap']) || !is_array($ifcfg['staticmap'])) {
			continue;
		}
		foreach ($ifcfg['staticmap'] as $idx => $sm) {
			if (empty($sm['mac'])) {
				continue;
			}
			$out[dlm_normalize_mac($sm['mac'])] = array(
				'if' => $if,
				'index' => $idx,
				'ip' => $sm['ipaddr'] ?? '',
				'hostname' => $sm['hostname'] ?? '',
				'descr' => $sm['descr'] ?? '',
			);
		}
	}
	return $out;
}

/* Interface (pfSense name, e.g. "lan", "opt1") whose subnet contains $ip. */
function dlm_interface_for_ip($ip) {
	static $nets = null;
	if ($nets === null) {
		$nets = array();
		$dhcpd = dlm_config_get('dhcpd', array());
		if (is_array($dhcpd) && function_exists('get_interface_ip')) {
			foreach (array_keys($dhcpd) as $if) {
				$ifip = get_interface_ip($if);
				$bits = get_interface_subnet($if);
				if (is_ipaddrv4($ifip) && is_numeric($bits)) {
					$nets[$if] = gen_subnet($ifip, $bits) . "/{$bits}";
				}
			}
		}
	}
	foreach ($nets as $if => $cidr) {
		if (function_exists('ip_in_subnet') && ip_in_subnet($ip, $cidr)) {
			return $if;
		}
	}
	return '';
}

function dlm_arp_online_ips() {
	$online = array();
	exec('/usr/sbin/arp -an 2>/dev/null', $lines);
	foreach ((array)$lines as $line) {
		if (preg_match('/\((\d+\.\d+\.\d+\.\d+)\) at ([0-9a-f:]+)/i', $line, $m)) {
			$online[$m[1]] = true;
		}
	}
	return $online;
}

/* ------------------------------------------------------- lease sources */

/* Source 1: pfSense core abstraction (covers ISC and Kea on 2.7+). */
function dlm_leases_from_core() {
	if (!function_exists('system_get_dhcpleases')) {
		return null;
	}
	$res = system_get_dhcpleases();
	if (!is_array($res)) {
		return null;
	}
	$list = isset($res['lease']) ? $res['lease'] : $res;
	$out = array();
	foreach ((array)$list as $l) {
		if (!is_array($l) || empty($l['ip'])) {
			continue;
		}
		$act = strtolower((string)($l['act'] ?? $l['state'] ?? ''));
		$online = $l['online'] ?? null;
		if (is_string($online)) {
			/* core reports e.g. "active/online" or "idle/offline" */
			$online = (bool)preg_match('/(^|\W)(online|' . preg_quote(gettext('online'), '/') . ')$/iu', trim($online));
		}
		$out[] = array(
			'ip' => $l['ip'],
			'mac' => $l['mac'] ?? '',
			'cid' => $l['cid'] ?? '',
			'hostname' => $l['hostname'] ?? '',
			'descr' => $l['descr'] ?? '',
			'if' => $l['if'] ?? '',
			'starts' => dlm_to_epoch($l['start'] ?? $l['starts'] ?? null),
			'ends' => dlm_to_epoch($l['end'] ?? $l['ends'] ?? null),
			'state' => dlm_canonical_state($act, ($l['type'] ?? '') === 'static'),
			'online' => $online,
			'staticmap_index' => $l['staticmap_array_index'] ?? null,
			'source' => 'core',
		);
	}
	return $out;
}

function dlm_canonical_state($raw, $is_static = false) {
	if ($is_static) {
		return 'static';
	}
	$map = array(
		'active' => 'active', 'expired' => 'expired', 'static' => 'static',
		'reserved' => 'reserved', 'free' => 'free', 'backup' => 'backup',
		'abandoned' => 'declined', 'declined' => 'declined', 'released' => 'expired',
	);
	foreach ($map as $needle => $state) {
		if ($raw === $needle || ($raw !== '' && $raw === strtolower(gettext($needle)))) {
			return $state;
		}
	}
	return $raw === '' ? 'unknown' : $raw;
}

/* Source 2a: ISC dhcpd lease file. Later blocks for the same IP win. */
function dlm_parse_isc_leases($file = DLM_ISC_LEASES) {
	if (!is_readable($file)) {
		return array();
	}
	$text = file_get_contents($file);
	$by_ip = array();
	if (!preg_match_all('/^lease\s+(\S+)\s*\{(.*?)^\}/ms', $text, $blocks, PREG_SET_ORDER)) {
		return array();
	}
	foreach ($blocks as $b) {
		$body = $b[2];
		$get = function ($re) use ($body) {
			return preg_match($re, $body, $m) ? $m[1] : null;
		};
		$by_ip[$b[1]] = array(
			'ip' => $b[1],
			'mac' => $get('/hardware ethernet ([0-9a-f:]+);/i') ?? '',
			'cid' => $get('/uid "((?:[^"\\\\]|\\\\.)*)";/') ?? '',
			'hostname' => $get('/client-hostname "([^"]*)";/') ?? '',
			'descr' => '',
			'if' => '',
			'starts' => dlm_to_epoch($get('/starts ([^;]+);/')),
			'ends' => dlm_to_epoch($get('/ends ([^;]+);/')),
			'state' => dlm_canonical_state(strtolower($get('/^\s*binding state (\w+);/m') ?? '')),
			'online' => null,
			'source' => 'isc',
		);
	}
	return array_values($by_ip);
}

/*
 * Source 2b: Kea memfile CSV.
 * Columns: address,hwaddr,client_id,valid_lifetime,expire,subnet_id,
 *          fqdn_fwd,fqdn_rev,hostname,state,user_context[,pool_id]
 * Kea's lease file cleanup (LFC) rotates into .completed/.2/.1, so read those
 * oldest-first and let newer rows override.
 */
function dlm_kea_lease_dir() {
	foreach (DLM_KEA_LEASE_DIRS as $d) {
		if (file_exists("{$d}/dhcp4.leases")) {
			return $d;
		}
	}
	return null;
}

function dlm_parse_kea_leases($dir = null) {
	$dir = $dir ?? dlm_kea_lease_dir();
	if ($dir === null) {
		return array();
	}
	$files = array();
	foreach (array('dhcp4.leases.completed', 'dhcp4.leases.2', 'dhcp4.leases.1', 'dhcp4.leases') as $f) {
		if (is_readable("{$dir}/{$f}")) {
			$files[] = "{$dir}/{$f}";
		}
	}
	$by_ip = array();
	foreach ($files as $file) {
		$fh = fopen($file, 'r');
		if (!$fh) {
			continue;
		}
		$header = null;
		while (($row = fgetcsv($fh, 0, ',', '"', '\\')) !== false) {
			if ($header === null) {
				$header = $row;
				continue;
			}
			if (count($row) < count($header)) {
				$row = array_pad($row, count($header), '');
			}
			$r = array_combine($header, array_slice($row, 0, count($header)));
			if (empty($r['address'])) {
				continue;
			}
			$expire = (int)$r['expire'];
			$valid = (int)$r['valid_lifetime'];
			$kea_state = (int)($r['state'] ?? 0);
			/* valid_lifetime 0 = lease deleted/released */
			if ($valid === 0 && $expire === 0) {
				unset($by_ip[$r['address']]);
				continue;
			}
			$state = ($kea_state === 1) ? 'declined' : (($kea_state === 2 || $expire < time()) ? 'expired' : 'active');
			$by_ip[$r['address']] = array(
				'ip' => $r['address'],
				'mac' => $r['hwaddr'] ?? '',
				'cid' => $r['client_id'] ?? '',
				'hostname' => rtrim(stripcslashes($r['hostname'] ?? ''), '.'),
				'descr' => '',
				'if' => '',
				'starts' => $expire ? ($expire - $valid) : null,
				'ends' => $expire ?: null,
				'state' => $state,
				'online' => null,
				'source' => 'kea',
			);
		}
		fclose($fh);
	}
	return array_values($by_ip);
}

/* -------------------------------------------------------------- facade */

function dlm_get_leases($opts = array()) {
	$backend = dlm_detect_backend();
	$leases = empty($opts['force_parse']) ? dlm_leases_from_core() : null;

	if ($leases === null) {
		switch ($backend) {
			case 'kea':
				$leases = dlm_parse_kea_leases();
				break;
			case 'isc':
				$leases = dlm_parse_isc_leases();
				break;
			default:
				$leases = array_merge(dlm_parse_isc_leases(), dlm_parse_kea_leases());
		}
	}

	$oui = dlm_load_oui_table();
	$statics = dlm_static_maps();
	$arp = null;
	$now = time();
	$out = array();
	$seen_static = array();

	foreach ($leases as $l) {
		$l['mac'] = dlm_normalize_mac($l['mac']);
		$l['hostname'] = rtrim((string)$l['hostname'], '.');
		$l['staticmap_index'] = $l['staticmap_index'] ?? null;
		if ($l['if'] === '') {
			$l['if'] = dlm_interface_for_ip($l['ip']);
		}
		if ($l['online'] === null) {
			if ($arp === null) {
				$arp = dlm_arp_online_ips();
			}
			$l['online'] = isset($arp[$l['ip']]);
		}
		$sm = $statics[$l['mac']] ?? null;
		$l['is_static'] = ($sm !== null) || $l['state'] === 'static';
		if ($sm !== null) {
			$seen_static[$l['mac']] = true;
			if ($l['descr'] === '') {
				$l['descr'] = $sm['descr'];
			}
			if ($l['if'] === '') {
				$l['if'] = $sm['if'];
			}
			if ($l['staticmap_index'] === null && $l['if'] === $sm['if']) {
				$l['staticmap_index'] = $sm['index'];
			}
		}
		$out[] = $l;
	}

	/* Static mappings without a current lease (parsed sources only list leases). */
	foreach ($statics as $mac => $sm) {
		if (isset($seen_static[$mac]) || $sm['ip'] === '') {
			continue;
		}
		$dupe = false;
		foreach ($out as $l) {
			if ($l['ip'] === $sm['ip'] && $l['is_static']) {
				$dupe = true;
				break;
			}
		}
		if ($dupe) {
			continue;
		}
		if ($arp === null) {
			$arp = dlm_arp_online_ips();
		}
		$out[] = array(
			'ip' => $sm['ip'], 'mac' => $mac, 'cid' => '',
			'hostname' => $sm['hostname'], 'descr' => $sm['descr'], 'if' => $sm['if'],
			'starts' => null, 'ends' => null, 'state' => 'static',
			'online' => isset($arp[$sm['ip']]), 'is_static' => true,
			'staticmap_index' => $sm['index'], 'source' => 'config',
		);
	}

	foreach ($out as &$l) {
		list($l['vendor'], $l['vendor_kind']) = dlm_vendor_for_mac($l['mac'], $oui);
		$l['age'] = ($l['starts'] !== null) ? max(0, $now - $l['starts']) : null;
		$l['if_descr'] = ($l['if'] !== '' && function_exists('convert_friendly_interface_to_friendly_descr'))
			? convert_friendly_interface_to_friendly_descr($l['if']) : strtoupper($l['if']);
	}
	unset($l);

	return array('backend' => $backend, 'generated' => $now, 'leases' => $out);
}
