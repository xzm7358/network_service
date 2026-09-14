# Ethernet 配置权威与启动生命周期修复计划

## 目标

以 `/data/network-service/ethernet.json` 作为 Ethernet 唯一持久化配置，恢复
`eth0` 高于 Wi-Fi 的产品优先级，并在完成板端回归后移除 `rcS` 对
`192.168.68.90/24` 的无条件静态配置。

## 实施状态

- 阶段 1：已完成，`ethernet.json` 权威、校验、原子提交和旧格式一次性迁移已落地。
- 阶段 2：已完成，生产启动会应用或接管权威 Ethernet 配置。
- 阶段 3：已完成，DHCP/static carrier 生命周期与有界 DHCP 重试已接入 reactor。
- 阶段 4：host 发布工具已完成并通过 Linux/交叉编译验证；目标板 `rcS` 尚未修改，必须先完成本文 HIL 清单。

## 已确认现状

- `/dnake/data` 链接到 `/data`，板端权威文件为
  `/data/network-service/ethernet.json`，当前内容为 `{"mode":"dhcp"}`。
- 当前 NetworkService 只读写 `/dnake/data/smart_hmi_ethernet.conf`，不消费上述
  JSON，也不会在启动时根据持久化配置启动 Ethernet DHCP。
- `/etc/init.d/rcS` 当前提供 `eth0=192.168.68.90/24`；它是当前 eth0 唯一 IPv4
  来源，不能先于服务修复删除。
- 当前只有 Wi-Fi DHCP 生命周期管理；Ethernet 缺少 carrier 驱动的启动、停止和
  DHCP 退出重试。

## 测试 seam

1. `EthernetConfig` 持久化接口：JSON 配置的读取、校验、原子替换和旧格式一次性迁移。
2. `NetworkDaemon` 启动接口：DHCP/静态配置在启动时收敛，已运行的受管 DHCP
   生命周期只 adopt、不重复启动。
3. Ethernet 生命周期接口：carrier up/down、DHCP 进程退出和退避到期产生可观察的
   DHCP/route/DNS 结果。
4. 发布迁移检查：rootfs 启动脚本不得继续无条件写入固定 eth0 地址。

## 阶段 1：统一配置格式与路径

- 将 `ethernet.json` 设为唯一写入目标。
- 支持 `dhcp` 与现有 `static/address/prefix/gateway/dns` JSON 契约。
- 若 JSON 不存在但 `smart_hmi_ethernet.conf` 存在，读取并一次性迁移到 JSON；JSON
  存在时绝不回退到旧文件。
- 保持 stage -> runtime apply -> atomic commit 的事务顺序。

影响面：`src/config/`、Ethernet 配置 DTO、IPC 表示、配置事务测试。

## 阶段 2：启动时应用权威配置

- 生产构造读取配置并收敛 Ethernet runtime。
- `mode=dhcp` 启动或 adopt eth0 DHCP；`mode=static` 应用静态地址、metric 10 路由
  与 DNS。
- 配置无效时保留已存活的 brownfield 管理链路并报告明确错误，不静默改用其他格式。
- 为测试注入平台端口，避免测试操作 host 网络。

影响面：`NetworkDaemon` 装配、`NetworkControlPlane` 初始状态、启动错误报告。

## 阶段 3：补齐 Ethernet 生命周期

- carrier up 时按权威配置启动/恢复 DHCP；carrier down 时停止受管 DHCP、清理精确
  owned route/IP，并触发 Wi-Fi failover。
- L2 仍在线但 DHCP 进程退出时执行有上限的指数退避重试；carrier down 或显式配置
  变更立即取消重试。
- Ethernet 与 Wi-Fi 复用相同的单进程所有权、generation fencing 和 route/DNS
  控制面，不引入第二个网络管理者。

影响面：新增 Ethernet 生命周期深模块、Netlink/reconcile 触发、状态事件测试。

## 阶段 4：移除 rcS 静态地址

- 仓库发布物提供幂等迁移脚本，只删除精确的
  `ifconfig eth0 192.168.68.90` 行，不改其他启动逻辑。
- 迁移前置检查要求新 NetworkService 支持 `ethernet.json`；失败时拒绝修改。
- 实机完成冷启动、网线拔插、服务崩溃重启、DHCP 失败和 Wi-Fi failover 后，才在
  镜像构建中执行迁移。

影响面：packaging、rootfs 集成、HIL 验证文档。当前仓库不直接拥有板端
`/etc/init.d/rcS`，因此不能用源码提交假装设备已迁移。

## 验证清单

- JSON DHCP/static 读写、无效输入、原子提交、旧格式迁移。
- 启动 DHCP 不重复创建进程；静态配置正确应用。
- Ethernet lease 安装 metric 10；Wi-Fi 保持 metric 20。
- Ethernet carrier down 后默认路由和 DNS 切换到 Wi-Fi。
- carrier 恢复与 DHCP 瞬时失败可有限重试，无重复 udhcpc。
- 服务重启能 adopt generation 匹配的 eth0/wlan0 DHCP。
- 旧 `wpa_action.sh/default.script` 未参与生产 DHCP 生命周期。
- rcS 迁移脚本拒绝旧服务、精确修改、可重复执行。
- 板端 `primary_iface=eth0`（有线健康）与 `primary_iface=wlan0`（有线失效）均通过。

## 提交策略

计划、阶段 1、阶段 2、阶段 3、阶段 4 分别独立提交。每个实现阶段遵循
red -> green，并在提交前运行对应聚焦测试；最终运行 NetworkService 全量 CTest 和
板端只读验收探针。
