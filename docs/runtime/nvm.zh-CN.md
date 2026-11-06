# NVM 持久化

## 所有权与策略

App 独占 RAM 设置与 NVM 版本服务。RT-Thread worker 独占 EEPROM/文件 I/O 与槽缓存。原生单项消息队列传递请求和结果，同一时刻只有一个不可变在途副本；另一个 pending 缓冲保存较新设置。SDL Host 在独立线程中使用相同记录、服务与后端代码。worker 不读取活动 Core，UI 动作回调不做存储 I/O。

Reference-Demo 明确声明 namespace 0x444D、record type 1、schema 3、500 ms 防抖和 3000 ms 最大未保存窗口。其合成本机参数与语言、单位、亮度及待重启 CAN 速率组成完整替代记录。External Product 必须选择自己的 namespace/schema 和本机权威策略，不能照搬后把远端 ECU 参数变成本机权威。存储版本独立于遥测/Domain revision，内容不变不增版。

## 编码与提交

FMP2 使用小端。MSP3 payload 按稳定参数 ID 编码为 32 位（限制在 Domain uint16 范围），数值为 IEEE binary32，不依赖目录顺序。缺项、重复、未知身份或越界值导致整体拒绝。内层 FNV 校验仅检测缓冲损坏，不是认证。外层 CRC 复用固定 CANopenNode 的 CRC-16/XMODEM（多项式 0x1021，初值 0，标准 check 0x31C3）。header 与 payload 连续计算，不再异或两段独立 CRC。test_hmi_record.c 包含黄金字节与逐字节损坏验证。

| 偏移 | 长度 | 字段 |
|---|---|---|
| 0 | 4 | FMP2 魔数 |
| 4 | 2 | Version 2 和零保留字节 |
| 6 | 2 | Record type |
| 8 | 2 | Product namespace |
| 10 | 2 | Schema |
| 12 | 2 | Flags，当前为零 |
| 14 | 8 | 单调提交序号 |
| 22 | 4 | Payload 长度 |
| 26 | 2 | 头 0–25 字节接 payload 的 CRC |
| 28 | 4 | 保留，必须为零 |
| 32 | 可变 | MSP3 payload |
| 尾部 | 16 | 序号、长度、CRC 与 CRC 反码 |

每槽另有独立提交页，保存绑定的 16 字节 seal 副本。提交顺序为使 seal 失效并同步读回，写完整 body 并同步读回，最后写 seal 并同步读回，成功后才能切换活动槽。不写最后有效槽。扫描区分 EMPTY（全 0xFF）、VALID、CORRUPT、INCOMPATIBLE、I/O 错误和同代次内容冲突。一坏一好降级恢复；未知 schema 禁止自动覆盖。不迁移旧格式、不隐式格式化、不静默切换介质、不允许 sequence 回绕。

## 后端

| 后端 | 接入 | 保证与边界 |
|---|---|---|
| I2C EEPROM | 两字节寻址；总线、地址、容量、页大小与区域偏移来自 `AIC_HMI_NVM_EEPROM_*`（默认 i2c0、0x50、64 KiB、128 字节页、偏移 0） | 1 KiB 区域：A 在偏移+0x000，B 在偏移+0x200；seal 页位于各槽末尾。已在一块使用 BL24C512A 的 d13x 板上验证初始化、保存与软/冷重启；写入过程中掉电未验证。 |
| Host 文件 | Prefix.0 和 Prefix.1 | 检查完整写、fsync/_commit、close 与读回；不宣称文件系统元数据掉电原子性。 |
| NOR/LittleFS 文件 | SDK 已挂载文件系统 | 使用同一可选文件适配，不实现裸 NOR 驱动或格式化；设备 sync/XIP 与实物恢复待 Board 验证。 |
| NAND/FATFS 文件 | SDK NFTL/ECC 与已挂载文件系统 | 弱文件系统保证，双文件不能证明元数据独立冗余；要求已验证 power-safe 能力时拒绝配置。 |

EEPROM transport 仅在应用层复用现有 RT-Thread I2C API，不修改 SDK 或 HAL。不使用 SDK 的 `at24cxx` 包：它在编译期固定器件型号，也没有读重试，已验证 d13x 板上的控制器需要这些处理。传输路径将两字节地址发送与所需长度的连续读取分别作为单消息事务提交，通过 RT-Thread 可递归总线互斥锁保护每个地址/读取对。worker 在每个读事务对及每次页写前等待 1 ms，并在地址与读取之间持该锁短暂等待 1 ms。地址/读取短传输最多尝试三次完整读取对，每次失败均记录日志，失败尝试不发布输出字节；互斥锁错误不重试。写入不自动重放。页回调按 EEPROM 页边界切分，把地址和整页数据作为一次 I2C 事务提交，在总线锁外等待 10 ms，再读回整段写入范围后继续。短写、读重试耗尽或读回不一致立即终止，驱动成功不等于持久化成功。通用页边界、只读就绪检查及完整记录/seal 读回继续保留，不调用破坏性的 SDK AT24 probe。一条完整设置记录只需几次页周期，不再逐字节写数百次，用户确认后的保存可在几十毫秒内完成。真实控制器行为仍由未修改 SDK 决定，已验证的实板范围见下文。seal 不与 body 或活动槽共享写页。

AIC_HMI_NVM_FILE_BACKEND 显式选择已挂载文件后端，AIC_HMI_NVM_FILE_PREFIX 和 AIC_HMI_NVM_FILE_KIND 指定路径和标识。默认使用 EEPROM。挂载失败不格式化、不转用其他后端。首个 EEPROM 镜像运行前应确认新 1 KiB 布局和 WP；非空未知内容会被保留并报告。

## 结果与生命周期

LOADING 为异步启动操作。App 接收匹配 generation 后才允许设置动作。加载完成本身不观察或回写 RAM 默认值，后续设置变化的观察或显式保存才进入普通可靠保存流程。损坏或不兼容记录仍可使用 RAM 默认设置，但阻止写入；查询不初始化介质。自有设置存储的 Product 不绑定 Framework 存储策略。

Product 可在 `hmi_storage_profile_t` 置 `always_writable=true`，让介质状态不再阻止保存：读不出有效记录（CORRUPT、INCOMPATIBLE、IO_ERROR）时用当前设置写替代记录，序号从介质上见到的最大有效序号往后编；另一槽不兼容但本槽有效时照常读取；提交失败（FAILED、UNCERTAIN）保留 dirty，隔 `debounce_ms` 后再写，不需要 `hmi_settings retry`。状态名仍照实报告，供 `hmi storage` 诊断。代价：不兼容的旧记录会被覆盖，总线长期不通时持续重试。默认 false，上面的保守口径不变。

IN_PROGRESS 独占在途版本，直到匹配结果到达。保存 R11 时 App 已到 R12，R11 完成只推进 durable 到 R11，R12 仍 dirty。进入 I/O 后失败为 UNCERTAIN，保留 dirty，显式重扫协调后才能重试。协调不把旧介质值覆盖到新 RAM。barrier 针对明确目标版本，完整替代的更高版本可以满足目标，但不声称中间值精确写过。启动有效记录可满足对应 barrier。只支持 worker 派发前取消，不因超时删除阻塞 worker 或释放仍被使用的缓冲。

Host 退出前请求立即保存并有界等待；失败返回非零进程状态，不伪造 dirty 清除。板端由运行时线程使用 hmi_board_nvm_flush 和 hmi_board_nvm_barrier；升级激活前等待此门禁。用户确认的动作若改变了编码后的设置内容（例如改密码），经 hmi_board_nvm_action_applied 申请立即保存，不再等去抖窗口；app_tick 随后产生的变化仍按原去抖策略。

## 诊断与验证

使用以下原生 MSH 命令。设置修改与 UI 走同一个动作队列；save、retry、initialize 只给运行时线程置请求。QUEUED 不表示 DURABLE：用 `hmi storage` 查询。

~~~text
hmi storage
hmi_settings language zh
hmi_settings units imperial
hmi_settings brightness 65
hmi_settings save
hmi storage
hmi_settings retry
hmi storage
~~~

Storage 输出 backend、state、dirty、RAM/inflight/durable revision、队列深度、底层错误、服务结果、降级状态与实际语言/亮度/单位。查询只渲染发布副本，不访问介质。主动重启前确认 DURABLE 且 dirty=0，重启后核对设置和序号。保存目标版本与异步命令结果分开。

Host 测试只保留记录格式黄金字节与逐字节损坏检查（test_hmi_record.c）；槽轮换、防抖、取消、EEPROM I2C 行为与重启恢复由实板及 Product 测试验证。执行 CMake/CTest；Linux 仍为默认 CI 平台。

2026-09-28 当前验收：仅应用层有界读恢复镜像的 BOARD_INIT、SETTINGS_SAVE、NOOP_SAVE、VALID_INIT_REJECT、SOFT_REBOOT_RESTORE 和 NORMAL_CAN_SAVE 均为 PASS。Platform 提交为 4fca8762243f9bb40cb753abd553f0808865cce0，并绑定已记录的脏源码快照；image SHA256 为 bf5ef8efa339376ffe83bd526201f0e62f7b30ff69c85a5608bf534a39ce3635。SDK 提交 9b78386dcaa54326487c8b208b2b7104d4b56c9b 的受跟踪目录树与基线 7608a666 相同。本地镜像清单保留完整构建身份、源码快照和原始 UART/CAN 证据。

初始化至首条默认记录持久化的轮询观测耗时为 55.7 秒。地址 0x0207 的第一次地址传输失败，在最多三次读重试内恢复。初始累计错误来自旧布局损坏，最终底层错误为零。从第一条设置请求开始，中文、英制、亮度 65 在 15.3 秒内达到 durable revision 4，期间先后提交两个版本。重复相同设置及 save 不增加版本或写次数；已有有效记录时初始化被拒绝。

主动软件重启后，相同设置和 revision 4 加载为 READY，dirty=0、depth=0、errors=0、writes=0。READY 表示启动已加载有效记录，已满足对应 durable revision 门禁；DURABLE 表示运行期间成功提交，不是启动必需状态。首版测试脚本误要求 DURABLE，原始失败报告与按源码语义校正的结果同时保留。Host 回归覆盖 READY、payload/版本一致、门禁以及相同设置不重写。

使用既有正常 CAN 场景、标称 50 帧/秒时，两次追加保存分别在观测 9.4 和 6.9 秒达到 durable revision 5、6。Host 调度发送、原生接收、应用接收、Runtime 派发均为 890 帧；原生丢帧、应用错误及 Runtime overflow 均为零。速度保持 VALID，UART 诊断响应，UI present/flush 计数前进。这些观测不等于物理触摸或视觉验收。最终设置为中文、英制、亮度 65；COM 和 PCAN 已释放。

用户断电后重新上电，冷启动恢复为 PASS：固件身份一致，revision 6 与中文/英制/亮度 65 恢复，READY、dirty=0、errors=0、writes=0。写入过程中物理掉电与耐久测试仍为 NOT_RUN。逐字节传输保守且较慢，所列耗时包含命令与轮询开销，不是 EEPROM 裸吞吐。既有 CAN Burst FAIL 保留，正常负载测试不关闭生产 Runtime 或完整 HIL 门禁。Host 模拟不能单独证明硬件恢复。本工作是工程规范实践，不是授权 MISRA 认证。

保留历史失败：早期字节 transport 镜像（SHA256 0cf247370a4c9ba344435919a8df0b1f144d5eb70042b1f70a23ba96c2ef887e）初始化读回在 0x003C 失败，重扫在 0x0089 失败。实验性 SDK I2C 修复已完整撤回，对应镜像已撤销。当前应用层间隔和读重试通过上述限定范围板测，不宣称修复了 SDK 控制器实现。

首次部署遇到旧布局时，先导出备份，经布局授权后执行 hmi_settings initialize CONFIRM。它只在 CORRUPT 或 INCOMPATIBLE 状态下生效；写入、同步和读回都在 NVM worker，拒绝清除有效槽。之后用 hmi storage 确认 DURABLE；入队不等于完成。


## 板级设置与格式边界

MSP3 的字节 0..3 为魔数，4/5/6 为单位/亮度/语言，7 为 CAN 速率选择（0=125k、1=250k、2=500k），8..11 为参数数量，12 起为 ID/值项，尾部 4 字节为 FNV 校验。仅支持 MSP3，Demo 声明 FMP2 schema 3；按维护者要求不实现旧记录读取或迁移。旧版/损坏记录整体拒绝，必须显式初始化后才可写入。本次开发板设置区已获初始化授权，不引入启动自动擦除。

参考板启动先等待 NVM 首次读取，才锁存两个 CAN 设备共用的速率；运行中修改只保存，不重配控制器。损坏记录沿用默认 500k 和写入阻止策略。hmi storage 的 configured_can_rate 为待重启配置，hmi can 的 bitrate 才是实际已打开速率。重启前确认 dirty=0、ram_revision=durable_revision。

运行时线程经 SDK 背光 PWM 通道（开启 `AIC_HMI_BACKLIGHT` 时）应用亮度，并经同一 NVM 服务保存；寄存器读回失败会打印错误并重试，RAM 接受不代表背光成功或介质持久化。hmi_settings product id value 把意图交给 Product 的 local_action；通用端口不解释 Product 私有编号。


## Product 计数与目录扩展

`app_tick` 在运行时线程执行，位于初始 NVM 加载及 Core 超时处理之后、发布之前，维护期间也执行。回调修改数据须递增 snapshot revision。revision 变化时运行时编码设置；编码相同的内容不排队写入。现有去抖、最大派发延迟、worker 与持久化门禁不变。派发延迟不是硬件完成期限，也不保证掉电不丢。

默认解码仍要求参数完整。同 namespace、type 和 schema 下仅增加目录参数时，Product 可显式配置 `allow_parameter_extension=true`。已有 ID、含义、单位和允许范围须兼容。记录整体校验后应用，新缺项使用目录 `initial`；重复或未知 ID、错误计数或数值、校验损坏及未知 schema 仍拒绝。下一次普通提交写入完整替代记录。这不迁移旧 payload 格式、不支持 ID 改名或缩减目录，也不授权改变物理槽布局。

Host 集成使用 `hmi_host_nvm_open_profile`，传入 Product 存储配置与显式扩展策略；旧 open API 保持严格解码及原时序。调用方须等待加载后才首次计时、观察设置变化，并在主动退出前等待目标 durable revision。Host 测试不能证明物理掉电恢复或耐久性。
