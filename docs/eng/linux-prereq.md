# Linux prerequisites

> Korean version: [`../kor/linux-prereq.md`](../kor/linux-prereq.md)

The OS-side prerequisites the Linux console client needs to run. The decision and its reasons are in [ADR 0035](decisions/0035-linux-console-client.md).
The Windows counterpart of this document is [`windows-prereq.md`](windows-prereq.md). The "Status" of each section records whether the claims of this document were confirmed on a real machine.

## 1. Target and build

**Status: partially measured.** Built without warnings and ran the tests on Ubuntu 24.04 (WSL, g++ 13) and Ubuntu 26.04 (g++ 15). It was a Debug build.
The Release configuration above was built on Ubuntu 26.04 (g++ 15) without a default server. It was not run on Ubuntu 24.04.

| Item | Value |
|------|-----|
| OS | Ubuntu 24.04 or later x64 |
| Compiler | g++ 13 or later. Warnings are errors |
| Build | CMake and Ninja. Wintun, Qt and PowerShell are not used |
| Executable | One `sangtachi_client`. There is no GUI |

```bash
cmake -S <repo> -B <build folder> -G Ninja -DCMAKE_BUILD_TYPE=Release -DSANGTACHI_BUILD_TESTS=OFF -DSANGTACHI_DEFAULT_SERVER=<server>
cmake --build <build folder> --target sangtachi_client
```

The record files and logs are created in `records/` and `logs/` under the executable folder. That folder must be writable.

## 2. Privileges

**Status: partially measured.** Started with `sudo`, it opened the adapter. Started without `sudo` after that, it failed to start. `setcap` was not used.

Creating the virtual adapter and attaching its address and route needs `CAP_NET_ADMIN`. At startup the client checks whether it holds this
privilege, and if not, it is a startup failure. Started with `--no-adapter`, no privilege is needed.

Grant it in one of two ways.

- Start it with `sudo`. `records/`, `logs/` and their files become owned by root. Started without `sudo` after that, it cannot write to those folders and it is a startup failure.
  To mix the two ways, first give the ownership back (`sudo chown -R $USER records logs`)
- Attach the privilege to the executable. It has to be attached again after every build

```bash
sudo setcap cap_net_admin+ep ./sangtachi_client
```

## 3. TUN device

**Status: partially measured.** Ended with a shutdown signal, the device, its address and route disappeared. The state after an abnormal exit was not looked at.

The client opens a non-persistent TUN device named `Sangtachi` through `/dev/net/tun`. When the process ends, the kernel deletes the device and its address and route.
So there is nothing that corresponds to the section 3 leftover cleanup of Windows. If `/dev/net/tun` is missing, load the `tun` kernel module.

Only one process on a machine uses the adapter. If a device with the same name is already open, the second fails to start. Start the second with `--no-adapter`.
If a persistent device with the same name (made with `ip tuntap add`) exists, the client does not attach to it and fails to start. Delete that device and start again.

## 4. Subnet overlap

**Status: partially measured.** A normal start was `proceed`. A second adapter process started while an adapter process was running failed to
start with `block`. `unknown` was not seen.

The decision table is the table of section 7 Subnet overlap in [`windows-prereq.md`](windows-prereq.md). Linux also picks a route by longest prefix match, so the same table
applies as is. What differs is the input and how it runs.

- The input is every IPv4 route in the main route table and the IPv4 addresses of all interfaces. Addresses go into the same overlap check as `/32`. The other route
  tables of policy routing are not looked at
- The client reads them over netlink and decides by itself. No script is called
- At the time of the check our device does not exist yet (section 3). So there is no step that excludes our own adapter
- If nothing to block was found but a lookup failed, the verdict is `unknown` and it is a startup failure

## 5. Firewall

**Status: not measured.** ufw was off on the test machine. In that state the Minecraft connection and `ping` in both directions worked. The state with ufw on
and the rule below added was not looked at.

The client neither checks nor changes the firewall. It writes `adapter.firewall verdict=skipped` to the log.

- **A player has nothing to do.** Tunnel UDP comes in as the reply to a flow this side sent first. A stateful firewall (the default policy of ufw) accepts it
- **The host opens the Minecraft inbound.** It is TCP 25565 coming in on the virtual adapter. With ufw it is the one line below. The default rules of ufw accept ICMP echo,
  so virtual IP `ping` needs nothing extra

```bash
sudo ufw allow in on Sangtachi to any port 25565 proto tcp
```

A machine that uses firewalld or nftables adds a rule with the same meaning with that tool.

## 6. Other VPNs and virtual interfaces

**Status: not measured.**

Another TUN device such as Tailscale drops out of the local candidates. Its interface type is virtual (`ARPHRD_NONE`), so it falls in the
excluded types of [`protocol.md`](protocol.md) 10.1. When looking at NAT in a field test, turn the other VPN off as on Windows. The reason is in [`field-test/README.md`](../../field-test/README.md).
