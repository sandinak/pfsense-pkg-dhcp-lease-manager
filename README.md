# pfSense-pkg-dhcp-lease-manager

A pfSense package that gives you a better view of DHCP leases than the stock
**Status > DHCP Leases** page:

- **Sort by lease age**: how long ago each lease started. It also sorts by expiry, IP (numerically), hostname, and the other columns.
- **Sort and filter by manufacturer**: looks up each MAC's OUI. Private (randomized) MACs from phones and laptops get their own label instead of "Unknown". Hypervisor and container MACs (Proxmox, QEMU/KVM, VMware, Hyper-V, Xen, VirtualBox, Parallels, Docker) are labelled as virtual.
- **Right-click a lease** to turn it into a static mapping. The core *Static Mapping* editor opens with MAC, client ID, hostname and description filled in.
- **Device types** (TV, Apple TV/streaming, light switch, smart plug, camera, doorbell, thermostat, phone, computer, Raspberry Pi, virtual machine, container, access point and more). Types are guessed from the hostname first, then the manufacturer. The ordered rules are `DLM_TYPE_RULES` in the `.inc` file. Right-click > *Set device type* to override a guess; overrides are stored per MAC address in `config.xml`.
- **Select and clear static mappings**: tick rows (shift-click selects a range) and use *Clear static mappings*, or right-click > *Remove static mapping*. It behaves like pfSense's own delete: it clears the static ARP entry and drops the Kea lease, and you finish with pfSense's **Apply Changes** button. Each deletion is checked against the MAC address so a stale page can't remove the wrong entry. You can also set or reset the type for a whole selection.
- **Breakdown chart** by device type or manufacturer. Click a bar to filter.
- Filters by interface, state and online status. Free-text search. Optional auto-refresh.

## DHCP server support

The package works with any DHCP server pfSense uses:

| Source | Used when |
|---|---|
| `system_get_dhcpleases()` (pfSense core) | Available (2.7+). It already abstracts ISC and Kea. |
| ISC dhcpd lease file `/var/dhcpd/var/db/dhcpd.leases` | Core helper is missing and the backend is ISC |
| Kea memfile `/var/db/kea/dhcp4.leases` (+ LFC rotations) | Core helper is missing and the backend is Kea |

All sources are normalized to one lease record in
`files/usr/local/pkg/dhcp_lease_manager.inc`. To support a future backend, add
one `dlm_parse_*()` function that returns the same shape.

The package never writes the DHCP configuration itself. Static mappings are
created through pfSense's own editor (`services_dhcp_edit.php`), so validation
and the service reload stay with core.

## Layout

This repo is laid out as a FreeBSD port, the same as the packages in
[pfsense/FreeBSD-ports](https://github.com/pfsense/FreeBSD-ports)
(`net-mgmt/pfSense-pkg-dhcp-lease-manager`).

```
Makefile, pkg-descr, pkg-plist           port metadata
files/pkg-{install,deinstall}.in         rc.packages hooks
files/usr/local/pkg/*.xml|*.inc          package definition + lease logic
files/usr/local/www/                     status page, JS, CSS
files/etc/inc/priv/                      privilege definition
files/usr/local/share/<port>/info.xml    package registry info
tests/                                   standalone parser tests + fixtures
```

## Development

```sh
php tests/parse_test.php                 # parser tests (PHP 8.x, no pfSense needed)
scripts/deploy-dev.sh root@pfsense.lan   # copy files onto a test firewall
```

`deploy-dev.sh` copies the files only. Open
`https://<firewall>/status_dhcp_lease_manager.php` directly. The Status menu
entry appears only when the package is installed as a real package, built
with the pfSense ports tree on FreeBSD.

## License

Apache License 2.0, the same license as pfSense. See [LICENSE](LICENSE).
