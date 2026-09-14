# Wi-Fi 已关联但未获得 IPv4 租约/默认路由

## 状态

- 发现日期：2026-09-14
- 状态：配置权威、启动收敛、Ethernet carrier 生命周期和 rcS 发布迁移已完成；目标板 DHCP/重启/进程恢复 HIL 通过，物理拔线与新镜像冷启动待执行
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

## 最终根因分层结论（取代早期临时判断）

### 1. 同一路由器与“可用二层网络”

“两个接口接入同一个路由器”一般不能单独证明它们位于同一可互通二层广播域：路由器可能对有线和无线使用不同 VLAN、访客隔离、AP client isolation 或端口安全策略。因此早期把二者视为可能不同二层，是一个待验证假设。

但在本次设备现场，`eth0`、`wlan0` 的 ARP 探测均能得到响应，并观察到跨接口广播；这些证据表明二者实际接入同一个二层广播域。故“它们并不连接同一个可用二层网络”不是本案的最终结论。准确说法是：**二层广播可达，不等于路由器会接受两个接口当前使用的三层源地址并为它们都转发流量。**

### 2. `ping -I eth0 192.168.68.1` 为何 100% 丢包

`-I eth0` 已把 ICMP 请求固定到 `eth0`，所以这不是默认路由误选 `wlan0`。现场抓取到的事实是：ARP 可解析网关，请求从 `eth0` 发出，但没有 Echo Reply 返回；相同网关经 `wlan0` 可达。因此丢包边界位于 `eth0` 请求发出之后、回复进入板端之前。

`eth0=192.168.68.90/24` 是 `rcS` 无条件写入的静态地址，不是路由器 DHCP 给出的、由 NetworkService generation/lease 机制可验证的租约。与之相对，`wlan0=192.168.68.199/24` 来自 `192.168.68.1` 的 DHCP Offer/ACK。

修复版 NetworkService 上板后，eth0 从同一 DHCP 服务器获得 `.196`。租约刚收敛后的第一轮 ping 仍出现过 100% 丢包，但随后的 eth0 抓包同时看到三组 Echo Request 与 Echo Reply，立即复测网关和 `8.8.8.8` 均为 0% 丢包。这说明原 100% 丢包不是“回复永远到不了 eth0”，而是旧静态地址或路由器/邻居学习窗口中的瞬时状态；现有证据不足以再细分路由器内部机制。确定的软件问题是旧服务没有消费 DHCP 权威配置，并可能把不可用 eth0 路由错误提升为 metric 10；修复后 eth0 DHCP 可稳定成为主链路。

### 3. 配置权威错位才是代码根因

板端真实权威配置为：

```text
/data/network-service/ethernet.json
{"mode":"dhcp"}
```

`/dnake/data` 虽链接到 `/data`，旧实现却读取不存在的 `/dnake/data/smart_hmi_ethernet.conf`，并且守护进程启动时根本不应用 Ethernet 持久化配置。因此：

- `ethernet.json` 中的 `dhcp` 意图没有进入新 NetworkService；
- `eth0` 的 `.90` 只能来自 `/etc/init.d/rcS`；
- NetworkService 只管理了 `wlan0` 的 DHCP，无法把 `eth0` 收敛为产品要求的高优先级上行链路；
- 人工给 eth0 加 metric 10 默认路由后，内核会优先选择实际不可达的 eth0，造成“Wi-Fi 已拿到租约但设备离线”。

### 4. `eth0` 的产品角色

`eth0` 不是永久的“仅调试管理链路”。它是正常上行接口，默认优先级高于 Wi-Fi：健康的 Ethernet 使用 metric 10，Wi-Fi 使用 metric 20。此前只保留到开发机的 eth0 `/32` 路由，是现场排障期间为了不丢失 telnet/ADB 的临时措施，不是产品策略。

## 已实施的正确修复

1. `/data/network-service/ethernet.json` 成为唯一写入权威；支持 DHCP/static JSON 校验与原子提交。JSON 缺失时可一次性导入旧 `smart_hmi_ethernet.conf`，JSON 一旦存在就绝不回退。
2. NetworkService 启动先校验权威配置，再启动/接管 eth0 DHCP 或应用静态配置；无效 JSON 不修改仍存活的 brownfield 链路。
3. 新增 Ethernet carrier 生命周期：断线时撤销 NetworkService 精确拥有的 IP/默认路由/DNS，恢复时重启 DHCP 或重放静态配置；DHCP 进程意外退出后按 1/2/4/8/8 秒最多重试 5 次。这样健康 eth0 优先，eth0 失效时 Wi-Fi 才能真实接管。
4. 守护进程和监督脚本默认配置根目录改为 `/data`。新增 `ethernet-json-v1` 能力检查和配置校验入口。
5. 提供 `network_service_migrate_rcs_ethernet` 发布迁移工具。它仅删除精确的 `ifconfig eth0 192.168.68.90` 命令，保留其他 rcS 内容和权限，首次执行保留恢复备份；新服务能力或 `ethernet.json` 校验失败时拒绝修改。

## `/etc/init.d/rcS` 静态 `.90` 是否还有必要

结论分两个发布阶段：

- **新 NetworkService 部署和目标板回归完成之前：暂时保留。** 它仍是旧镜像唯一的 eth0 IPv4 来源，过早删除可能失去调试入口。
- **新服务通过冷启动、网线拔插、服务重启、DHCP 失败和 Wi-Fi failover 后：必须删除。** 继续保留会形成 rcS 与 `ethernet.json` 两个配置权威，并可能在 DHCP 前后留下冲突地址或错误的高优先级路由。

目标板迁移命令为：

```sh
NETWORK_SERVICE_BIN=/dnake/bin/network_service \
NETWORK_SERVICE_CONFIG_DIR=/data \
RCS_PATH=/etc/init.d/rcS \
/dnake/bin/network_service_migrate_rcs_ethernet
```

迁移后应确认 `/etc/init.d/rcS.pre-network-service` 备份存在，并执行完整冷启动验证；不要在当前远程调试会话中直接手工删行来代替发布迁移。

## 修复版目标板 HIL 记录

使用 OrbStack `ubuntu-amd64` 虚拟机内的
`arm-linux-gnueabihf-g++ 8.2.1` 与仓库
`Components/hardware/ssd20x/toolchain.cmake` 完成交叉编译。产物为 ARM EABI5、
`/lib/ld-linux-armhf.so.3` 动态加载；放入板端 `/tmp` 后能力检查与
`/data/network-service/ethernet.json` 校验均通过。

在不修改只读 `/dnake/bin` 和真实 `rcS` 的前提下，通过现有 supervisor 的
`NETWORK_SERVICE_BIN=/tmp/network_service.new` 注入修复版，得到以下结果：

- eth0 DHCP 获得 `192.168.68.196/24`，网关/DNS 为 `192.168.68.1`；
- eth0 默认路由 metric 10，wlan0 默认路由 metric 20，
  `primary_iface=eth0`、`online=true`；
- eth0 到网关和 `8.8.8.8` 最终连续 3/3 成功；
- 重启守护进程后，eth0/wlan0 原 DHCP PID 被原位 adopt，没有重复启动；
- 强杀 eth0 `udhcpc` 后，服务按退避启动新 PID，恢复 `.196`、metric 10 和网关连通；
- `ip link set eth0 down` 后生命周期迅速完成停止并重新启动，DHCP generation 发生变化；由于物理 carrier 仍在，该操作不能替代持续拔线测试；
- 迁移工具在板端 BusyBox 上对 `/tmp/rcS.test` 精确删除一行并保留权限/备份，真实 `/etc/init.d/rcS` 第 4 行保持不变。

板端 `/dnake/bin` 为只读镜像，无法在线持久替换。因此剩余发布动作是把新二进制、
监督脚本和迁移工具纳入下一版 rootfs，先完成真实网线拔插和新镜像冷启动，再运行
迁移工具修改真实 `rcS`。

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

## 2026-09-14 板端现场复现结论

通过 telnet 9900 完成了以下现场操作：

1. 采集基线：`wpa_state=COMPLETED`、`wlan0=192.168.68.199`、`udhcpc` 正常运行、DNS 为 `192.168.68.1`；
2. 执行 `networkctl wifi-off`：返回 `{"enabled":false}`，wlan0 地址、默认路由和 DHCP 进程均清除；
3. 执行 `networkctl wifi-on` 并等待收敛；连续两轮以及后续一轮接口级验证均重新获得 `.199`，`networkctl status` 为 `online=true`；
4. 在 `any` 接口抓包，3 次重连都观察到完整的 DHCP Discover → Offer → ACK。Offer/ACK 均来自 `192.168.68.1`，并携带 `192.168.68.199`、`255.255.255.0`、网关 `192.168.68.1` 和 DNS `192.168.68.1`；
5. 在 `eth0` 专用接口抓包时没有观察到 DHCP 报文，排除了 DHCP 报文实际从 wlan0 泄漏到 eth0 的假设。

另做了错误默认路由对照实验：临时加入 `default via 192.168.68.1 dev eth0 metric 10` 后，Wi-Fi 仍能收到 DHCP Offer/ACK 并取得 `.199`，但 `networkctl status` 变为 `primary_iface=eth0`、`online=false`、`dns_available=false`。删除该路由并恢复 `wlan0` 默认路由后，状态恢复为 `primary_iface=wlan0`、`online=true`。因此：

- 错误的 eth0 默认路由是当前“已拿到 Wi-Fi 地址但设备不在线/公网不可达”的确定根因；
- 它不是本次 DHCP Discover 无 Offer 的直接根因，因为在该路由存在时 DHCP 仍完成了 Offer/ACK；
- 初始日志中的 `udhcpc: no lease, failing` 在本次现场未复现，当前证据已排除“AP/DHCP 服务持续不发 Offer”，但无法排除当时的瞬时 DHCP 服务异常或驱动偶发丢包。若再次失败，必须在失败窗口保留 `tcpdump -ni any` 抓包，不能只凭 `udhcpc` 文本判断。

Wi-Fi 断开/重连时内核日志还重复出现：

```text
RTW: ERROR Free disconnecting network of scanned_queue failed due to pwlan == NULL
```

该 Realtek 驱动清理错误目前未阻止后续认证、四次握手或 DHCP，但可能与扫描/断开时序的偶发问题相关，应作为驱动侧次要问题单独跟进；在没有失败窗口抓包前，不应把它直接认定为 DHCP 失败根因。

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
tcpdump -ni any -vv 'udp port 67 or 68'
```

接口关闭期间不要使用 `tcpdump -ni wlan0` 作为唯一抓包入口；当前镜像会直接返回 `That device is not up`。使用 `any` 可覆盖 wlan0 重新启用前后的 DHCP 阶段，抓包完成后删除临时文件。

判定规则：

- 只有 Discover、没有 Offer：优先检查 AP DHCP、VLAN、客户端隔离、MAC 过滤和 DHCP 地址池；
- 有 Offer 但无地址：检查驱动收包、`udhcpc` 回调和 `NetworkControlPlane` 的 IPv4 应用；
- 已有地址但无默认路由：检查 DHCP lease 中的 router 字段、默认路由创建及路由策略。

## 代码侧验证

当前 host 回归测试中，`networkctl_cli`、Wi-Fi 生命周期、网络状态归一化和状态事件集成测试均通过。现有代码没有把 WPA 认证成功误判为网络就绪；因此在板端抓包前不应增加盲目 DHCP 重试来掩盖根因。
