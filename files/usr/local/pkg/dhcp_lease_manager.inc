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

/* ------------------------------------------------------- device types */

/* type id => array(label, Font Awesome 6 icon) */
define('DLM_TYPES', [
	'tv'           => ['TV', 'fa-tv'],
	'streaming'    => ['Apple TV / streaming', 'fa-circle-play'],
	'audio'        => ['Speaker / audio', 'fa-volume-high'],
	'light-switch' => ['Light switch / dimmer', 'fa-toggle-on'],
	'smart-plug'   => ['Smart plug', 'fa-plug'],
	'light'        => ['Light / bulb', 'fa-lightbulb'],
	'camera'       => ['Camera', 'fa-video'],
	'doorbell'     => ['Doorbell', 'fa-bell'],
	'thermostat'   => ['Thermostat', 'fa-temperature-half'],
	'energy'       => ['Energy monitor', 'fa-bolt'],
	'iot'          => ['IoT / sensor', 'fa-microchip'],
	'phone'        => ['Phone', 'fa-mobile-screen-button'],
	'tablet'       => ['Tablet', 'fa-tablet-screen-button'],
	'watch'        => ['Watch', 'fa-clock'],
	'computer'     => ['Computer', 'fa-laptop'],
	'server'       => ['Server / NAS', 'fa-server'],
	'sbc'          => ['Raspberry Pi / SBC', 'fa-memory'],
	'printer'      => ['Printer', 'fa-print'],
	'3d-printer'   => ['3D printer', 'fa-cube'],
	'access-point' => ['Access point', 'fa-wifi'],
	'network'      => ['Network gear', 'fa-network-wired'],
	'game'         => ['Game console', 'fa-gamepad'],
	'unknown'      => ['Unknown', 'fa-circle-question'],
]);

/*
 * Ordered classification rules; the first rule whose conditions ALL match wins.
 * Conditions are case-insensitive regexes on 'hostname', 'vendor' or 'descr'.
 * Hostname rules come before vendor-only rules because a name chosen by the
 * owner ("kitchen-tv") says more than the radio module's manufacturer.
 */
define('DLM_TYPE_RULES', [
	/* --- hostname-driven (most specific first) --- */
	['3d-printer',   ['hostname' => '/(^|[^a-z])(ender|prusa|octopi|octoprint|bambu|voron|klipper|mainsail)/']],
	['printer',      ['hostname' => '/printer|laserjet|officejet|deskjet|^prn[-\d]/']],
	['doorbell',     ['hostname' => '/doorbell|^ring-/']],
	['camera',       ['hostname' => '/(^|[^a-z])(cam|camera|ipcam|nvr)([^a-z]|$)|webcam/']],
	['streaming',    ['hostname' => '/apple-?tv|roku|fire-?tv|firestick|chromecast|nvidia-?shield|google-?tv|onn-?4k/']],
	['game',         ['hostname' => '/xbox|playstation|ps[45]|nintendo|switch-?oled|steamdeck|steam-?deck/']],
	['tablet',       ['hostname' => '/ipad|tablet|kindle|galaxy-?tab|fire-?hd/']],
	['phone',        ['hostname' => '/iphone|android|galaxy|pixel-?\d|(^|-)s2\d(-|$)|phone/']],
	['watch',        ['hostname' => '/watch/']],
	['computer',     ['hostname' => '/macbook|mbp|imac|mac-?mini|mac-?studio|^mac\d*$|^mac-|desktop|laptop|notebook|workstation|windows|thinkpad|surface|(^|-)pc(-|$)|comp$/']],
	['access-point', ['hostname' => '/^ap-|(^|-)ap(\d|-|$)|(^|-)wap|unifi-?ap|(^|-)mr\d+/']],
	['network',      ['hostname' => '/^(sw|switch)-|router|gateway|firewall|(^|-)(mx|ms)\d+/',
	                  'vendor' => '/netgear|tp-link|cisco|meraki|ubiquiti|aruba|mikrotik|juniper|d-link|zyxel/']],
	['network',      ['hostname' => '/router|gateway|firewall|openwrt|(^|-)(mx|ms)\d+/']],
	['audio',        ['hostname' => '/sonos|homepod|speaker|soundbar|castaudio|studiolive|echo-?dot|alexa|(^|-)audio/']],
	['tv',           ['hostname' => '/(^|[^a-z])tv([^a-z]|$)|tv$|bravia|television/']],
	['smart-plug',   ['hostname' => '/plug|outlet|mss(1|6)\d\d|kasa|wemo/']],
	['light-switch', ['hostname' => '/switch|dimmer|(^|-)sw(-|$)|^sw-|mss5\d\d/']],
	['light',        ['hostname' => '/bulb|lamp|light|wled|(^|-)hue|lifx|nanoleaf|govee/']],
	['thermostat',   ['hostname' => '/thermostat|ecobee|(^|-)nest/']],
	['energy',       ['hostname' => '/envoy|enphase|(^|-)sense-|powerwall|solar|emporia/']],
	['server',       ['hostname' => '/(^|-)pve|proxmox|esxi|(^|-)nas(-|\d|$)|synology|qnap|truenas|unraid|server|buildhost|docker|k8s|homeassistant/']],
	['sbc',          ['hostname' => '/(^|-)pi(\d|-|$)|rpi\d?|raspberry/']],
	['iot',          ['hostname' => '/^esp|esp32|esp8266|tasmota|shelly|sensor|weather|zigbee|zwave|homeiq/']],
	/* Apple devices named after a room are almost always an Apple TV or HomePod. */
	['streaming',    ['vendor' => '/^apple/',
	                  'hostname' => '/kitchen|family|living|bed|room|den$|office|basement|garage|great|loft|patio|theater|theatre/']],

	/* --- manufacturer-only fallbacks --- */
	['printer',      ['vendor' => '/brother|epson|canon|lexmark|xerox|kyocera/']],
	['camera',       ['vendor' => '/amcrest|reolink|hikvision|dahua|wyze|axis comm|eufy|altobeam/']],
	['light',        ['vendor' => '/philips lighting|signify|lifx|nanoleaf|govee/']],
	['thermostat',   ['vendor' => '/ecobee|nest labs/']],
	['energy',       ['vendor' => '/enphase/']],
	['tv',           ['vendor' => '/hisense|vizio|tcl |^tcl|lg electronics|sony visual/']],
	['streaming',    ['vendor' => '/roku/']],
	['audio',        ['vendor' => '/sonos|presonus|bose|denon|yamaha/']],
	['sbc',          ['vendor' => '/raspberry pi/']],
	['iot',          ['vendor' => '/espressif|tuya|shelly/']],
	['smart-plug',   ['vendor' => '/meross|belkin/']],
	['access-point', ['vendor' => '/ubiquiti|meraki/']],
	['network',      ['vendor' => '/netgear|tp-link|cisco|mikrotik|aruba|juniper|zyxel/']],
	['server',       ['vendor' => '/synology|qnap|supermicro/']],
	['computer',     ['vendor' => '/intel corporate|micro-star|asustek|elitegroup|dell|lenovo|hewlett|gigabyte|framework/']],
	['game',         ['vendor' => '/nintendo|sony interactive|microsoft/']],
]);

/* Returns array(type, source) where source is rule|unknown. */
function dlm_classify($lease) {
	foreach (DLM_TYPE_RULES as $rule) {
		list($type, $conds) = $rule;
		$hit = true;
		foreach ($conds as $field => $re) {
			$val = strtolower((string)($lease[$field] ?? ''));
			if ($val === '' || !preg_match($re . 'i', $val)) {
				$hit = false;
				break;
			}
		}
		if ($hit) {
			return array($type, 'rule');
		}
	}
	return array('unknown', 'unknown');
}

/* Per-MAC manual type overrides, stored in config.xml as base64 JSON. */
function dlm_type_overrides() {
	$raw = dlm_config_get('installedpackages/dhcpleasemanager/typemap', '');
	$map = $raw ? json_decode(base64_decode($raw), true) : null;
	return is_array($map) ? $map : array();
}

/* Set ($type) or reset ($type === '') the type of one or more MACs. Returns an error string or null. */
function dlm_set_type_override($macs, $type) {
	if ($type !== '' && !array_key_exists($type, DLM_TYPES)) {
		return 'Invalid device type.';
	}
	$map = dlm_type_overrides();
	$n = 0;
	foreach ((array)$macs as $mac) {
		$mac = dlm_normalize_mac($mac);
		if (!preg_match('/^([0-9a-f]{2}:){5}[0-9a-f]{2}$/', $mac)) {
			return 'Invalid MAC address.';
		}
		if ($type === '') {
			unset($map[$mac]);
		} else {
			$map[$mac] = $type;
		}
		$n++;
	}
	if ($n === 0) {
		return 'No devices selected.';
	}
	ksort($map);
	config_set_path('installedpackages/dhcpleasemanager/typemap', base64_encode(json_encode($map)));
	write_config(sprintf('DHCP Lease Manager: device type %s for %d device(s)',
		$type === '' ? 'reset' : "set to {$type}", $n));
	return null;
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

/*
 * Remove static mappings. $items is a list of array(if, id, mac); each entry
 * is only removed if the mapping at that index still has that MAC, so a stale
 * page can never delete the wrong mapping. Mirrors services_dhcp.php "del":
 * static ARP cleanup, Kea lease removal, and marking DHCP (and DNS host
 * overrides) dirty so the user applies the change with pfSense's Apply button.
 * Returns array(removed_count, errors[]).
 */
function dlm_delete_static_maps($items) {
	$removed = array();
	$errors = array();
	foreach ((array)$items as $it) {
		$if = (string)($it['if'] ?? '');
		$id = $it['id'] ?? null;
		$mac = dlm_normalize_mac($it['mac'] ?? '');
		if (!preg_match('/^[a-z0-9_]+$/i', $if) || !is_numeric($id)) {
			$errors[] = "Invalid request for {$mac}.";
			continue;
		}
		$id = (int)$id;
		$map = dlm_config_get("dhcpd/{$if}/staticmap/{$id}");
		if (!is_array($map) || dlm_normalize_mac($map['mac'] ?? '') !== $mac) {
			$errors[] = "Static mapping for {$mac} changed since the page loaded; reload and try again.";
			continue;
		}
		if (isset($map['arp_table_static_entry']) && is_ipaddrv4($map['ipaddr'] ?? '')) {
			mwexec('/usr/sbin/arp -d ' . escapeshellarg($map['ipaddr']));
		}
		/* Unset leaves the other indices untouched, so the batch order does not matter. */
		config_del_path("dhcpd/{$if}/staticmap/{$id}");
		$removed[] = array('if' => $if, 'map' => $map);
	}
	if (empty($removed)) {
		return array(0, $errors);
	}

	write_config(sprintf('DHCP Lease Manager: removed %d static mapping(s)', count($removed)));

	foreach ($removed as $r) {
		$if = $r['if'];
		$map = $r['map'];
		if (function_exists('config_path_enabled') ? config_path_enabled("dhcpd/{$if}") : true) {
			mark_subsystem_dirty('dhcpd');
			if (dlm_config_get('dnsmasq/enable') !== null && dlm_config_get('dnsmasq/regdhcpstatic')) {
				mark_subsystem_dirty('hosts');
			}
		}
		if (function_exists('dhcp_is_backend') && dhcp_is_backend('kea')) {
			require_once('system.inc');
			if (!function_exists('system_del_kea4lease')) {
				continue;
			}
			if (is_ipaddrv4($map['ipaddr'] ?? '')) {
				system_del_kea4lease($map['ipaddr']);
			} elseif (!empty($map['mac']) && function_exists('get_keasubnetid_from_interface')) {
				system_del_kea4lease($map['mac'], get_keasubnetid_from_interface($if));
			}
		}
	}
	return array(count($removed), $errors);
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

	$overrides = dlm_type_overrides();
	foreach ($out as &$l) {
		list($l['vendor'], $l['vendor_kind']) = dlm_vendor_for_mac($l['mac'], $oui);
		if (isset($overrides[$l['mac']]) && isset(DLM_TYPES[$overrides[$l['mac']]])) {
			$l['type'] = $overrides[$l['mac']];
			$l['type_source'] = 'manual';
		} else {
			list($l['type'], $l['type_source']) = dlm_classify($l);
		}
		$l['age'] = ($l['starts'] !== null) ? max(0, $now - $l['starts']) : null;
		$l['if_descr'] = ($l['if'] !== '' && function_exists('convert_friendly_interface_to_friendly_descr'))
			? convert_friendly_interface_to_friendly_descr($l['if']) : strtoupper($l['if']);
	}
	unset($l);

	$types = array();
	foreach (DLM_TYPES as $id => $t) {
		$types[$id] = array('label' => $t[0], 'icon' => $t[1]);
	}
	return array('backend' => $backend, 'generated' => $now, 'types' => $types, 'leases' => $out);
}
