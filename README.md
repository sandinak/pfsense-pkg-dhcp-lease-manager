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

## Requirements

pfSense 2.7 or later (tested on 2.9.0 with Kea). The package reads leases with
pfSense's own `system_get_dhcpleases()`, which covers both the ISC DHCP and Kea
backends, so it follows whichever DHCP server is configured.

## Permissions

| Action | Needs |
|---|---|
| View the page | *WebCfg - Status: DHCP Lease Manager* |
| Set or reset device types | ...and no *User - Config: Deny Config Write* |
| Add/edit/remove static mappings, Apply Changes | ...and *WebCfg - Services: DHCP Server: Edit static mapping* |

Adding and editing static mappings opens pfSense's own editor
(`services_dhcp_edit.php`), so its validation applies. Removal mirrors the
core delete action.

## Layout

The repository root is the port directory, laid out like the packages in
[pfsense/FreeBSD-ports](https://github.com/pfsense/FreeBSD-ports)
(`net-mgmt/pfSense-pkg-dhcp-lease-manager`). Only `Makefile`, `pkg-descr`,
`pkg-plist` and `files/` go upstream. `tests/` and `scripts/` are development
aids.

```
Makefile, pkg-descr, pkg-plist           port metadata
files/pkg-{install,deinstall}.in         rc.packages hooks
files/usr/local/pkg/*.xml|*.inc          package definition + logic
files/usr/local/www/                     status page, JS, CSS
files/etc/inc/priv/                      privilege definition
files/usr/local/share/<port>/info.xml    package registry info
tests/                                   unit tests for the pure functions
scripts/                                 deploy / package build helpers
```

## Development

```sh
php tests/run_tests.php                         # unit tests (PHP 8.x, no pfSense needed)
scripts/deploy-dev.sh root@fw [--files|--remove]  # copy files onto a test firewall
scripts/build-pkg.sh  root@fw [--install]         # build a real .pkg on the firewall
```

`build-pkg.sh` stages the files the same way the Makefile's `do-install`
does, checks that `pkg-plist` matches exactly, and runs `pkg create` on the
firewall. Use it to test the package without a FreeBSD ports tree. To build
from the port itself, copy this directory into a pfSense FreeBSD-ports checkout
and run `make package`.

## License

Apache License 2.0, the same license as pfSense. See [LICENSE](LICENSE).
