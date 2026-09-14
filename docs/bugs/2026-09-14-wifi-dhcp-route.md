# Wi-Fi 已关联但未获得 IPv4 租约/默认路由

## 状态

- 发现日期：2026-09-14
- 状态：DHCP 初始失败仍待板端抓包确认；有线调试链路与默认路由分流已确认
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

开发机已安装 ADB。早期从开发机访问板端时，`192.168.68.90:5555`/`192.168.68.199:5555` 曾出现不可达或 `offline`，因此当时不能取得板端实时证据；后续通过 telnet 9900 登录板端并重新拉起 `adbd` 后，ADB 已恢复为 `device`。

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

## 板端路由表补充

后续板端输出确认：

- `wlan0` 的 `00000000` 目的项对应默认路由 `192.168.68.1`，metric 20；
- `eth0` 只有 `192.168.68.0/24` 直连项，没有默认路由；
- `wlan0` 为 `LOWER_UP`，到网关和 `8.8.8.8` 的 ping 均成功；
- 当前仅存在 `wlan0` 的 NetworkService-owned `udhcpc` 进程，`eth0` 的 `192.168.68.90` 不是该 DHCP 生命周期产生的可验证 lease。

因此后续 Wi-Fi 断开/重连测试应只把调试机回程固定到有线接口；不要仅因 `eth0` 有静态地址就把互联网默认路由切到 `eth0`。例如开发机地址为 `192.168.68.197` 时：

```sh
ip route replace 192.168.68.197/32 dev eth0 src 192.168.68.90
ip route get 192.168.68.197
ip route get 8.8.8.8
```

当前现场验证结果是：`192.168.68.197` 经 `eth0` 可达，但 `192.168.68.1` 经 `eth0` 不通；同一网关和公网地址经 `wlan0` 均可达。因此，若设备当前仍有无线互联网连接，应保持默认路由在 `wlan0`，仅保留上面的调试主机 `/32` 路由。上述命令只改变当前内核路由，重启或网络配置重载后会失效。

## 后续首选调试入口：telnet 9900

设备的 telnet 服务监听在 `9900`，可从开发机直接进入板端 shell：

```sh
telnet 192.168.68.90 9900
```

登录用户为 `root`；密码使用现场配置的 root 密码，不写入仓库、脚本或日志。登录后先执行以下最小检查：

```sh
id
ip addr show eth0
ip route
ps | grep adbd | grep -v grep
```

后续 Wi-Fi 断开/重连测试优先保持这个有线 telnet 会话，避免无线接口变化导致调试通道丢失。telnet 为明文协议，只应在受控的板端调试网段使用。

## ADB 恢复记录

### 现象

主机侧曾出现：

```text
adb connect 192.168.68.90:5555
already connected to 192.168.68.90:5555
adb devices -l
192.168.68.90:5555    offline
```

但 TCP 5555 端口可以建立连接，说明有线网络路径可达；问题发生在 ADB 协议握手阶段，而不是 `eth0` 的基本连通性。

### 根因与恢复

telnet 登录后确认 `/usr/sbin/adbd` 正在监听 `0.0.0.0:5555`，但该进程的父进程是交互式 `sh`，说明它是从 telnet shell 手工后台启动的，标准输入/输出没有脱离会话。终止旧进程并使用脱离终端的方式重新启动后，ADB 从 `offline` 恢复为 `device`：

```sh
ps | grep adbd | grep -v grep
kill -TERM <adbd-pid>
nohup /usr/sbin/adbd >/dev/null 2>&1 </dev/null &
```

然后在开发机清理旧 transport 并重新连接：

```sh
adb disconnect 192.168.68.90:5555
adb kill-server
adb start-server
adb connect 192.168.68.90:5555
adb devices -l
adb -s 192.168.68.90:5555 get-state
```

期望最后一条输出为 `device`。当前镜像未提供 Android 的 `setprop`、`stop`、`start` 命令，后续应沿用 `/usr/sbin/adbd` 的直接启动方式；若镜像增加了 supervisor，应优先使用 supervisor 的重启入口，避免重复启动监听进程。

### 当前确认结果

ADB 恢复后实测：

- `ip route get 192.168.68.197` 明确选择 `dev eth0 src 192.168.68.90`；
- 经 `eth0` ping 开发机 `3/3` 成功；
- `networkctl status` 在错误的 `eth0` 默认路由存在时为 `online=false`；删除该默认路由、恢复 `wlan0` 默认路由后为 `online=true`，并显示 DNS `192.168.68.1`；
- Wi-Fi 侧 `wpa_state=COMPLETED`，地址为 `192.168.68.199`，经 `wlan0` ping 网关和 `8.8.8.8` 均成功。

因此，后续定位 DHCP/Wi-Fi 时应通过 `192.168.68.90` 的 telnet 或 ADB 进入设备，同时让互联网默认流量继续走实际可用的 `wlan0`，两者不要混为同一条默认路由。

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
