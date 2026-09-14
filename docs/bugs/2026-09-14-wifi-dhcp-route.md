# Wi-Fi 已关联但未获得 IPv4 租约/默认路由

## 状态

- 发现日期：2026-09-14
- 状态：待板端网络抓包确认
- 当前设备地址：`192.168.68.199`
- 早期调试地址：`192.168.68.90`

## 现象

执行 `networkctl connect <ssid> <psk>` 后，命令只返回请求已接受。板端日志显示：

- Realtek 驱动完成认证和关联；
- WPA 四次握手完成，pairwise/group key 已安装；
- `udhcpc` 在 `wlan0` 上连续发送 DHCP Discover；
- 未收到 DHCP Offer，最终输出 `udhcpc: no lease, failing`。

此前的 `networkctl status` 还显示：

- `wlan0` 没有 IPv4 地址和默认路由；
- `eth0` 虽有 `192.168.68.90`，但没有默认路由，整体 `online=false`。

## 当前判断

已确认 WPA 控制通道、Wi-Fi profile 选择和二层握手均已走通。失败点位于二层之上的 DHCP/路由收敛阶段。

目前不能仅凭日志区分以下两种情况：

1. AP/DHCP 服务没有返回 Offer，或访客网络/VLAN/客户端隔离阻断了 DHCP；
2. Offer 已到达无线网卡，但 Realtek 驱动或本地网络配置没有正确接收/应用。

开发机已安装 ADB，但从当前开发机到 `192.168.68.199:5555` 仍返回 `No route to host`，且 ARP 为 incomplete，因此尚未取得板端实时证据。

## 新增板端证据

板端随后报告 `wlan0` 已获得 `192.168.68.199/24`，状态为 `UP BROADCAST RUNNING`，收发包计数均在增长；同时 `eth0` 仍使用 `192.168.68.90/24`。这说明 Wi-Fi 当前已具备 IPv4 地址，但两个接口处于同一网段，仍需用 `ip route` 确认默认路由和网关实际走向。`ifconfig` 输出本身不能证明默认路由存在，也不能证明地址来自 NetworkService 的当前 DHCP lease。

## 临时路由修复

确认网关为 `192.168.68.1` 后，可在设备上执行以下临时修复；重启、网络服务重启或 DHCP 重新配置后可能失效：

```sh
ping -I wlan0 -c 3 192.168.68.1
ip route replace 192.168.68.1/32 dev wlan0 src 192.168.68.199
ip route replace default via 192.168.68.1 dev wlan0 src 192.168.68.199 metric 20
ip route get 8.8.8.8
```

若 BusyBox 的 `ip` 不支持 `replace`，使用等价命令：

```sh
route add -host 192.168.68.1 dev wlan0
route add default gw 192.168.68.1 dev wlan0 metric 20
```

若网关不是 `192.168.68.1`，必须替换为 `ip route` 或现场网络实际提供的网关。路由恢复后若只能访问 IP 不能解析域名，再单独检查 DNS；不要把 DNS 失败误判为路由失败。

## 复现与取证

设备恢复 ADB 或 USB 调试通道后，按以下顺序采集，避免泄露 Wi-Fi 密码：

```sh
./networkctl wifi-off
./networkctl wifi-on
./networkctl connect '<SSID>' '<REDACTED_PSK>'
sleep 20
./networkctl status
ip addr show wlan0
ip route
cat /proc/net/route
ps | grep '[u]dhcpc'
tcpdump -ni wlan0 -vv 'udp port 67 or 68'
```

判定规则：

- 只有 Discover、没有 Offer：优先检查 AP DHCP、VLAN、客户端隔离、MAC 过滤和 DHCP 地址池；
- 有 Offer 但无地址：检查驱动收包、`udhcpc` 回调和 `NetworkControlPlane` 的 IPv4 应用；
- 已有地址但无默认路由：检查 DHCP lease 中的 router 字段、默认路由创建及路由策略。

## 代码侧验证

当前 host 回归测试中，`networkctl_cli`、Wi-Fi 生命周期、网络状态归一化和状态事件集成测试均通过。现有代码没有把 WPA 认证成功误判为网络就绪；因此在板端抓包前不应增加盲目 DHCP 重试来掩盖根因。
