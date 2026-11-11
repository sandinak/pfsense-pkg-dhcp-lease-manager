<?php
/*
 * status_dhcp_lease_manager.php
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

##|+PRIV
##|*IDENT=page-status-dhcpleasemanager
##|*NAME=Status: DHCP Lease Manager
##|*DESCR=Allow access to the 'Status: DHCP Lease Manager' page.
##|*MATCH=status_dhcp_lease_manager.php*
##|-PRIV

require_once("guiconfig.inc");
require_once("/usr/local/pkg/dhcp_lease_manager.inc");

$data = dlm_get_leases();

/* JSON endpoint used by the page's auto-refresh. */
if (($_GET['format'] ?? '') === 'json') {
	header('Content-Type: application/json');
	header('Cache-Control: no-store');
	echo json_encode($data);
	exit;
}

$pgtitle = array(gettext("Status"), gettext("DHCP Lease Manager"));
$shortcut_section = "dhcp";
include("head.inc");

$backend_label = array('kea' => 'Kea DHCP', 'isc' => 'ISC DHCP', 'unknown' => gettext('unknown backend'));
?>
<link rel="stylesheet" href="/dhcp_lease_manager/lease_manager.css" />

<div class="panel panel-default">
	<div class="panel-heading">
		<h2 class="panel-title">
			<?=gettext("Manufacturers")?>
			<small class="dlm-meta">
				<?=htmlspecialchars($backend_label[$data['backend']] ?? $data['backend'])?>
				&middot; <span id="dlm-count"></span>
			</small>
		</h2>
	</div>
	<div class="panel-body">
		<div id="dlm-vendors" class="dlm-vendors"></div>
	</div>
</div>

<div class="panel panel-default">
	<div class="panel-heading">
		<h2 class="panel-title"><?=gettext("Leases")?></h2>
	</div>
	<div class="panel-body">
		<form class="form-inline dlm-toolbar" onsubmit="return false;">
			<input type="search" id="dlm-search" class="form-control input-sm"
				placeholder="<?=gettext("Filter IP, MAC, hostname, vendor…")?>" />
			<select id="dlm-iface" class="form-control input-sm">
				<option value=""><?=gettext("All interfaces")?></option>
			</select>
			<select id="dlm-state" class="form-control input-sm">
				<option value=""><?=gettext("All states")?></option>
				<option value="active"><?=gettext("Active")?></option>
				<option value="static"><?=gettext("Static")?></option>
				<option value="expired"><?=gettext("Expired")?></option>
				<option value="online"><?=gettext("Online now")?></option>
				<option value="dynamic"><?=gettext("Dynamic only")?></option>
			</select>
			<select id="dlm-refresh" class="form-control input-sm" title="<?=gettext("Auto refresh")?>">
				<option value="0"><?=gettext("No auto refresh")?></option>
				<option value="15"><?=gettext("Refresh 15s")?></option>
				<option value="60"><?=gettext("Refresh 60s")?></option>
			</select>
			<span class="text-muted dlm-hint"><?=gettext("Click a column to sort; right-click a row for actions.")?></span>
		</form>
	</div>
	<div class="table-responsive">
		<table class="table table-striped table-hover table-condensed dlm-table" id="dlm-table">
			<thead>
				<tr>
					<th data-key="online" class="dlm-narrow" title="<?=gettext("Online")?>"></th>
					<th data-key="ip"><?=gettext("IP address")?></th>
					<th data-key="mac"><?=gettext("MAC address")?></th>
					<th data-key="vendor"><?=gettext("Manufacturer")?></th>
					<th data-key="hostname"><?=gettext("Hostname")?></th>
					<th data-key="descr"><?=gettext("Description")?></th>
					<th data-key="if_descr"><?=gettext("Interface")?></th>
					<th data-key="age"><?=gettext("Lease age")?></th>
					<th data-key="ends"><?=gettext("Expires")?></th>
					<th data-key="state"><?=gettext("State")?></th>
				</tr>
			</thead>
			<tbody></tbody>
		</table>
	</div>
</div>

<ul id="dlm-menu" class="dropdown-menu dlm-menu" role="menu"></ul>

<script type="application/json" id="dlm-data"><?=json_encode($data, JSON_HEX_TAG | JSON_HEX_AMP | JSON_HEX_APOS | JSON_HEX_QUOT)?></script>
<script src="/dhcp_lease_manager/lease_manager.js"></script>

<?php include("foot.inc"); ?>
