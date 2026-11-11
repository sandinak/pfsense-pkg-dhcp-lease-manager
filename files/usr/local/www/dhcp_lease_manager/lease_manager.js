/*
 * lease_manager.js
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

(function () {
	'use strict';

	var STORE_KEY = 'dlm.prefs';
	var state = {
		data: JSON.parse(document.getElementById('dlm-data').textContent),
		sortKey: 'age',
		sortDir: 1,
		vendor: '',
		timer: null
	};

	var $ = function (id) { return document.getElementById(id); };
	var tbody = document.querySelector('#dlm-table tbody');
	var menu = $('dlm-menu');

	/* ------------------------------------------------------------ prefs */
	function loadPrefs() {
		try {
			var p = JSON.parse(localStorage.getItem(STORE_KEY) || '{}');
			if (p.sortKey) { state.sortKey = p.sortKey; state.sortDir = p.sortDir || 1; }
			if (p.refresh) { $('dlm-refresh').value = String(p.refresh); }
		} catch (e) { /* storage unavailable */ }
	}
	function savePrefs() {
		try {
			localStorage.setItem(STORE_KEY, JSON.stringify({
				sortKey: state.sortKey, sortDir: state.sortDir,
				refresh: Number($('dlm-refresh').value)
			}));
		} catch (e) { /* ignore */ }
	}

	/* -------------------------------------------------------- formatting */
	function esc(s) {
		return String(s == null ? '' : s).replace(/[&<>"']/g, function (c) {
			return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c];
		});
	}
	function duration(sec) {
		if (sec == null) { return ''; }
		var neg = sec < 0; sec = Math.abs(sec);
		var d = Math.floor(sec / 86400), h = Math.floor(sec % 86400 / 3600), m = Math.floor(sec % 3600 / 60);
		var s = d ? d + 'd ' + h + 'h' : h ? h + 'h ' + m + 'm' : m ? m + 'm' : Math.floor(sec) + 's';
		return neg ? '-' + s : s;
	}
	function ipKey(ip) {
		return ip.split('.').reduce(function (a, o) { return a * 256 + (parseInt(o, 10) || 0); }, 0);
	}
	function now() { return Math.floor(Date.now() / 1000); }

	/* ----------------------------------------------------------- sorting */
	function sortValue(l, key) {
		switch (key) {
		case 'ip': return ipKey(l.ip);
		case 'age': return l.age == null ? Infinity : l.age;
		case 'ends': return l.ends == null ? Infinity : l.ends;
		case 'online': return l.online ? 0 : 1;
		default: return String(l[key] || '￿').toLowerCase();
		}
	}
	function compare(a, b) {
		var k = state.sortKey, va = sortValue(a, k), vb = sortValue(b, k);
		if (va < vb) { return -state.sortDir; }
		if (va > vb) { return state.sortDir; }
		return ipKey(a.ip) - ipKey(b.ip);
	}

	/* --------------------------------------------------------- filtering */
	function filtered() {
		var q = $('dlm-search').value.trim().toLowerCase();
		var iface = $('dlm-iface').value, st = $('dlm-state').value;
		return state.data.leases.filter(function (l) {
			if (state.vendor && l.vendor !== state.vendor) { return false; }
			if (iface && l.if !== iface) { return false; }
			if (st === 'online' && !l.online) { return false; }
			if (st === 'dynamic' && l.is_static) { return false; }
			if (st && st !== 'online' && st !== 'dynamic' && l.state !== st) { return false; }
			if (q) {
				var hay = [l.ip, l.mac, l.hostname, l.descr, l.vendor, l.if_descr].join(' ').toLowerCase();
				if (hay.indexOf(q) === -1) { return false; }
			}
			return true;
		});
	}

	/* --------------------------------------------------------- rendering */
	function renderRows() {
		var rows = filtered().sort(compare), t = now(), html = [];
		rows.forEach(function (l, i) {
			var expires = l.ends == null ? (l.is_static ? '—' : '') : duration(l.ends - t);
			var vendorCls = l.vendor_kind === 'private' ? 'dlm-private' : l.vendor_kind === 'unknown' ? 'text-muted' : '';
			html.push(
				'<tr data-idx="' + state.data.leases.indexOf(l) + '" class="' + (l.state === 'expired' ? 'dlm-expired' : '') + '">' +
				'<td><i class="fa fa-circle ' + (l.online ? 'text-success' : 'text-muted') + '" title="' + (l.online ? 'online' : 'offline') + '"></i></td>' +
				'<td>' + esc(l.ip) + '</td>' +
				'<td class="dlm-mono">' + esc(l.mac) + '</td>' +
				'<td class="' + vendorCls + '">' + esc(l.vendor) + '</td>' +
				'<td>' + esc(l.hostname) + '</td>' +
				'<td>' + esc(l.descr) + '</td>' +
				'<td>' + esc(l.if_descr) + '</td>' +
				'<td title="' + (l.starts ? esc(new Date(l.starts * 1000).toLocaleString()) : '') + '">' + duration(l.age) + '</td>' +
				'<td title="' + (l.ends ? esc(new Date(l.ends * 1000).toLocaleString()) : '') + '">' + expires + '</td>' +
				'<td><span class="label ' + stateLabel(l) + '">' + esc(l.is_static && l.state !== 'static' ? l.state + ' · static' : l.state) + '</span></td>' +
				'</tr>'
			);
		});
		tbody.innerHTML = html.join('') || '<tr><td colspan="10" class="text-center text-muted">No leases match.</td></tr>';
		$('dlm-count').textContent = rows.length + ' of ' + state.data.leases.length + ' leases';

		document.querySelectorAll('#dlm-table th[data-key]').forEach(function (th) {
			th.classList.remove('dlm-asc', 'dlm-desc');
			if (th.dataset.key === state.sortKey) { th.classList.add(state.sortDir > 0 ? 'dlm-asc' : 'dlm-desc'); }
		});
	}
	function stateLabel(l) {
		return { active: 'label-success', static: 'label-info', expired: 'label-default', declined: 'label-danger' }[l.state] || 'label-warning';
	}

	function renderVendors() {
		var counts = {};
		state.data.leases.forEach(function (l) { counts[l.vendor] = (counts[l.vendor] || 0) + 1; });
		var list = Object.keys(counts).map(function (v) { return [v, counts[v]]; })
			.sort(function (a, b) { return b[1] - a[1] || a[0].localeCompare(b[0]); });
		var max = list.length ? list[0][1] : 1, top = list.slice(0, 15), rest = list.slice(15);
		var html = top.map(function (e) {
			return '<button type="button" class="dlm-vbar' + (state.vendor === e[0] ? ' active' : '') + '" data-vendor="' + esc(e[0]) + '">' +
				'<span class="dlm-vname">' + esc(e[0]) + '</span>' +
				'<span class="dlm-vtrack"><span class="dlm-vfill" style="width:' + (100 * e[1] / max).toFixed(1) + '%"></span></span>' +
				'<span class="dlm-vcount">' + e[1] + '</span></button>';
		});
		if (rest.length) {
			html.push('<div class="text-muted dlm-vrest">+ ' + rest.length + ' more manufacturers (' +
				rest.reduce(function (a, e) { return a + e[1]; }, 0) + ' leases)</div>');
		}
		if (state.vendor) {
			html.push('<button type="button" class="btn btn-xs btn-default dlm-vclear">Clear manufacturer filter</button>');
		}
		$('dlm-vendors').innerHTML = html.join('');
	}

	function renderInterfaces() {
		var sel = $('dlm-iface'), cur = sel.value, seen = {};
		state.data.leases.forEach(function (l) { if (l.if) { seen[l.if] = l.if_descr || l.if; } });
		while (sel.options.length > 1) { sel.remove(1); }
		Object.keys(seen).sort().forEach(function (k) { sel.add(new Option(seen[k], k)); });
		sel.value = cur;
	}

	function render() { renderVendors(); renderRows(); }

	/* ------------------------------------------------------ context menu */
	function staticUrl(l) {
		/* services_dhcp_edit.php pre-fills these from the query string; the
		 * IP is left blank because the editor rejects addresses inside the
		 * dynamic pool and does not read ipaddr from the request anyway. */
		var p = new URLSearchParams({ if: l.if, mac: l.mac });
		if (l.cid) { p.set('cid', l.cid); }
		if (l.hostname) { p.set('hostname', l.hostname); }
		if (l.descr) { p.set('descr', l.descr); }
		return '/services_dhcp_edit.php?' + p.toString();
	}

	function openMenu(ev, l) {
		var items = [];
		if (!l.is_static && l.if) {
			items.push(['fa-plus', 'Add static mapping…', function () { location.href = staticUrl(l); }]);
		} else if (l.is_static && l.if && l.staticmap_index != null) {
			items.push(['fa-pencil', 'Edit static mapping', function () {
				location.href = '/services_dhcp_edit.php?if=' + encodeURIComponent(l.if) + '&id=' + encodeURIComponent(l.staticmap_index);
			}]);
		} else if (l.is_static && l.if) {
			items.push(['fa-pencil', 'View static mappings on ' + (l.if_descr || l.if), function () {
				location.href = '/services_dhcp.php?if=' + encodeURIComponent(l.if);
			}]);
		} else {
			items.push(['fa-ban', 'Interface unknown – cannot add static mapping', null]);
		}
		items.push(null);
		items.push(['fa-filter', 'Show only ' + l.vendor, function () { state.vendor = l.vendor; render(); }]);
		items.push(['fa-copy', 'Copy IP address', function () { copy(l.ip); }]);
		items.push(['fa-copy', 'Copy MAC address', function () { copy(l.mac); }]);

		menu.innerHTML = '';
		items.forEach(function (it) {
			var li = document.createElement('li');
			if (!it) { li.className = 'divider'; menu.appendChild(li); return; }
			var a = document.createElement('a');
			a.href = '#';
			a.innerHTML = '<i class="fa ' + it[0] + ' fa-fw"></i> ' + esc(it[1]);
			if (it[2]) {
				a.addEventListener('click', function (e) { e.preventDefault(); closeMenu(); it[2](); });
			} else {
				li.className = 'disabled';
				a.addEventListener('click', function (e) { e.preventDefault(); });
			}
			li.appendChild(a);
			menu.appendChild(li);
		});
		menu.style.display = 'block';
		var w = menu.offsetWidth, h = menu.offsetHeight;
		menu.style.left = Math.min(ev.clientX, window.innerWidth - w - 8) + 'px';
		menu.style.top = Math.min(ev.clientY, window.innerHeight - h - 8) + 'px';
	}
	function closeMenu() { menu.style.display = 'none'; }
	function copy(text) {
		if (navigator.clipboard) { navigator.clipboard.writeText(text).catch(function () {}); }
	}

	/* ----------------------------------------------------------- refresh */
	function refresh() {
		fetch(location.pathname + '?format=json', { credentials: 'same-origin', cache: 'no-store' })
			.then(function (r) { if (!r.ok) { throw new Error(r.status); } return r.json(); })
			.then(function (d) { state.data = d; renderInterfaces(); render(); })
			.catch(function () { /* session expired or network error: keep last data */ });
	}
	function scheduleRefresh() {
		clearInterval(state.timer);
		var s = Number($('dlm-refresh').value);
		if (s > 0) { state.timer = setInterval(refresh, s * 1000); }
		savePrefs();
	}

	/* ------------------------------------------------------------ wiring */
	document.querySelectorAll('#dlm-table th[data-key]').forEach(function (th) {
		th.addEventListener('click', function () {
			var k = th.dataset.key;
			if (state.sortKey === k) { state.sortDir = -state.sortDir; } else { state.sortKey = k; state.sortDir = 1; }
			savePrefs();
			renderRows();
		});
	});
	['dlm-search', 'dlm-iface', 'dlm-state'].forEach(function (id) {
		$(id).addEventListener('input', renderRows);
		$(id).addEventListener('change', renderRows);
	});
	$('dlm-refresh').addEventListener('change', scheduleRefresh);

	$('dlm-vendors').addEventListener('click', function (e) {
		var b = e.target.closest('.dlm-vbar');
		if (b) { state.vendor = state.vendor === b.dataset.vendor ? '' : b.dataset.vendor; render(); return; }
		if (e.target.closest('.dlm-vclear')) { state.vendor = ''; render(); }
	});

	tbody.addEventListener('contextmenu', function (e) {
		var tr = e.target.closest('tr[data-idx]');
		if (!tr) { return; }
		e.preventDefault();
		openMenu(e, state.data.leases[Number(tr.dataset.idx)]);
	});
	document.addEventListener('click', function (e) { if (!menu.contains(e.target)) { closeMenu(); } });
	document.addEventListener('keydown', function (e) { if (e.key === 'Escape') { closeMenu(); } });
	window.addEventListener('scroll', closeMenu, true);

	loadPrefs();
	renderInterfaces();
	render();
	scheduleRefresh();
}());
