# listfiles — 行为驱动开发 (BDD) 规格说明

> 版本: 12.2.x  
> 语言: C11 (GNU11)  
> 平台: Linux (依赖 fork, epoll, pipe2, pthread)

---

## 1. 文件系统扫描 (File System Scanning)

### Feature: 递归目录遍历

```gherkin
Feature: 递归目录遍历
  As a 系统管理员
  I want 对指定路径进行任务队列驱动的并行递归遍历
  So that 我可以获取目录树中所有文件和目录的元数据

  Background:
    Given 系统为 Linux 内核
    And 目标路径存在于文件系统中

  Scenario: 扫描单个目录
    Given 用户指定了目标路径 "/data"
    When Master 进程向 Worker 发送 IPC_MSG_SCAN 消息
    Then Worker 应该打开该目录
    And 遍历其中所有非 "." 和 ".." 的条目
    And 将每个条目的路径和 stat 信息批量返回给 Master

  Scenario: 扫描深层嵌套目录
    Given 目录树深度超过 8 层
    When Worker 逐层扫描并将子目录作为新任务入队
    Then 应该正确处理每一层的子目录
    And 不应发生栈溢出或路径截断
    And 不同分支可由多个 Worker 并行处理

  Scenario: 扫描符号链接
    Given 目录中存在指向其他目录的符号链接
    And 用户未指定 --follow-symlinks
    When Worker 遇到符号链接
    Then 应该使用 lstat 获取链接本身的信息
    And 不跟踪进入链接指向的目录

  Scenario: 跟踪符号链接进入目标目录
    Given 用户指定了 --follow-symlinks
    When Worker 遇到指向目录的符号链接
    Then 应该使用 stat 解析链接目标
    And 将目标目录作为普通目录继续遍历
```

### Feature: 单文件目标扫描

```gherkin
Feature: 单文件目标扫描
  As a 系统管理员
  I want 直接扫描单个文件而非目录
  So that 我可以统一处理文件和目录的元数据输出

  Scenario: 目标为普通文件
    Given 用户指定了单文件路径 "/data/single_file.txt"
    When 程序启动
    Then 应该跳过 Worker 进程分配
    And 直接将该文件提交到输出队列
    And 任务立即标记为完成
```

---

## 2. 断点续传 (Resume / Checkpoint)

### Feature: 两级游标追踪（当前架构下的正确设计）

```gherkin
Feature: 两级游标追踪
  As a 系统管理员
  I want 精确追踪扫描任务在"已处理/已持久化"两个阶段的进度
  So that 崩溃恢复时的重复扫描量控制在可接受范围内（如 < 1000 条）

  Background:
    Given 用户指定了 --continue 和 --progress-file=task1
    And Worker 内部将 opendir+readdir 与 per-entry lstat 线性耦合执行
    And 以目录为原子粒度返回 batch，不存在"已扫描但未lstat"的中间状态

  Scenario: Worker 内部无独立 Scan/Lstat 阶段
    Given Worker 收到 IPC_MSG_SCAN("/data/subdir")
    When Worker 执行 scan_and_send()
    Then 先执行 opendir+readdir 遍历所有条目
    And 对每个条目立即执行 lstat 或 try_blind_trust
    And 收集完整 batch 后一次性返回 Master
    And 不存在"已readdir但尚未lstat"的时间窗口
    And 因此不需要独立的 Scan Cursor 或 Lstat Cursor

  Scenario: 首次扫描创建游标索引
    Given 这是首次扫描
    When 程序启动
    Then 应该创建 task1.config 记录会话配置
    And 创建 task1.idx 记录两级游标初始值
      | 游标类型          | 字段名                    | 初始值 | 说明                          |
      | Process Cursor    | process_slice/line        | 0 0    | Master 已去重完成的目录位置   |
      | Persist Cursor    | write_slice/line_count    | 0 0    | 已写入 pbin 并落盘的位置      |
      | Output Cursor     | output_slice_num/count    | 0 0    | 输出切片位置（可选）          |

  Scenario: Process Cursor 递进
    Given Worker 返回了一个 batch 包含 50 条记录
    When Master 完成去重、黑名单检查、输出提交
    Then Process Cursor 应该递进 50 条记录
    And 这些记录对应的 fingerprint 已加入 visited_set
    And task1.idx 应该在每批处理完成后同步该游标

  Scenario: Persist Cursor 递进与分片封口
    Given RecordBatch 缓冲已满（4096 条或 1MB）
    When 缓冲被 flush 到 pbin 活跃分片
    Then Persist Cursor 应该更新为 flush 后的最新位置
    And 如果活跃分片达到 100,000 条阈值
    Then 应该向当前分片 O_APPEND 写入 Footer
    And 原子更新 task1.idx 中的所有游标
    And 创建新的活跃分片继续写入

  Scenario: .idx 定期刷新
    Given 扫描正在进行中
    When 距离上次 .idx 刷新已超过 5 秒
    Or 已累积超过 1000 条新处理记录
    Then 应该触发 atomic_update_index()
    And 先 fflush(write_slice_file) 确保 pbin 数据落盘
    And 再通过 write-to-temp + rename 原子更新 task1.idx

  Scenario: 中断后精确恢复
    Given 上次扫描因 SIGKILL 中断
    And task1.idx 中记录了 Process Cursor = slice 3, line 1500
    When 用户再次执行相同命令
    Then 应该读取 task1.idx 获取 Process Cursor 作为恢复起点
    And 将 process_slice 及之前所有分片加载到 visited_set
    And 对于 process_slice 分片，只加载 process_line 行
    And 从 process_cursor 位置继续 pump 历史目录
    And 真空带（重复扫描量）不应超过最近一次 idx 刷新的间隔

  Scenario: 历史目录泵送期间的新发现隔离
    Given 处于恢复模式且正在消费历史 pbin
    When Worker 扫描历史目录时发现新的子目录
    Then 新子目录不应直接混入当前 pbin
    And 应该追加到 task1.fpbin_000XXX 临时分片
    And 由 fpbin.idx 跟踪当前分片号与行数

  Scenario: 历史 pbin 消费完毕后的 fpbin 转正
    Given 所有历史 pbin 分片已消费完毕
    When 触发扫尾流程
    Then 应该冻结 fpbin 不再接收新记录
    And 为每个 fpbin 分片写入 Footer
    And 执行 fsync 确保落盘
    And 将 task1.fpbin_000XXX rename 为 task1_00000N.pbin
    And 以 O_RDONLY 重新打开校验 Footer
    And 全部校验通过后删除 fpbin.idx
    And 更新 Process Cursor 指向转正后的起始位置
```

### Feature: 为什么不分离 Scan-Worker 与 Stat-Worker

```gherkin
Feature: Worker 耦合架构的合理性论证
  As a 架构师
  I want 评估将 Scan 与 Stat 分离为独立 Worker 的价值
  So that 避免为不存在的"速度差"引入过度设计

  Scenario: 分离 Worker 的 IPC 开销暴增
    Given 一个平均目录包含 1000 个文件
    When 使用耦合 Worker（当前设计）
    Then 每目录产生 1 条 SCAN 请求 + 1 条 BATCH 返回
    When 使用分离 Worker（Scan + Stat）
    Then 每目录产生 1 条 SCAN + 1 条 NAME_LIST + 1000 条 STAT + 1000 条 RESULT
    And IPC 消息数增长约 1000 倍
    And Master 调度负载从"目录级"变为"文件级"

  Scenario: 分离 Worker 破坏 blind-trust 效率
    Given 半增量扫描启用 blind-trust
    And 历史索引通过 COW 共享给 Worker
    When 使用耦合 Worker
    Then try_blind_trust() 在 Worker 本地执行，零 IPC 开销
    When 使用分离 Worker
    Then Scan-Worker 只返回 d_ino + d_type
    And Master 需为每个文件查 reference_map 决定是否发 STAT 请求
    And 未命中文件还要再发一次 IPC 给 Stat-Worker
    And 延迟增加，吞吐量下降

  Scenario: 当前耦合设计是刻意的工程权衡
    Given 项目目标是"数亿文件级的大规模扫描"
    And 核心痛点是"NFS 设备挂起导致 D-State"
    When 评估分离 Worker 的收益与代价
    Then 收益仅为"理论上的文件级游标精度"
    And 代价为"1000 倍 IPC 开销 + 两套进程池 + 两套熔断逻辑"
    And 游标精度问题完全可通过 Master 端"已分配未返回"追踪解决
    And 结论：分离 Worker 在当前需求下 ROI 为负
```

### Feature: 进度持久化与恢复（当前实现的缺陷描述）

```gherkin
Feature: 进度持久化与恢复
  As a 系统管理员
  I want 了解当前断点续传实现的真实行为与局限
  So that 我能评估在关键场景下的恢复精度

  Background:
    Given 当前实现仅使用单级 Persist Cursor（write_slice_index + line_count）
    And process_slice_index 是 write_slice_index 的硬绑定镜像（僵尸字段）

  Scenario: 当前 .idx 更新频率极低
    Given 扫描一个 9 万文件的目录
    When 整个扫描过程中未达分片轮转阈值
    Then atomic_update_index() 在运行期间一次都不会被调用
    And 如果此时发生 SIGKILL 崩溃
    Then 恢复时 .idx 要么不存在，要么停留在旧位置
    And 重复扫描量可能达到数万条

  Scenario: RecordBatch 缓冲引入额外真空带
    Given RecordBatch 缓冲 4096 条记录
    When batch 已满并触发 flush 到 pbin
    Then flush 后不会触发 atomic_update_index()
    And 如果此时崩溃，这 4096 条已写入 pbin 的记录
    And .idx 却未反映，恢复时会重复处理

  Scenario: 活跃分片内缺乏游标追踪
    Given 当前活跃分片内已写入 50,000 条记录
    And line_count 仅在内存中累加
    When 程序异常退出
    Then 下次恢复时 .idx 中的 line_count 可能是 0 或旧值
    And 活跃分片会被从头重新解析到 visited_set
    And 导致最多 50,000 条记录的重复扫描

  Scenario: 输出线程与进度游标不同步
    Given output_slice_num 已递增到 5
    And rotate_output_slice() 未触发 atomic_update_index()
    When 程序崩溃后恢复
    Then 输出可能从旧的 output_slice_num 重新开始
    And 导致输出分片编号与内容不一致
```

### Feature: 会话一致性校验

```gherkin
Feature: 会话一致性校验
  As a 系统管理员
  I want 防止误用不匹配的进度文件恢复
  So that 避免路径或配置变更导致的数据混乱

  Scenario: 路径不一致拒绝恢复
    Given 历史进度文件的 path=/data/old
    And 当前命令指定了 --path=/data/new
    When 程序尝试加载会话配置
    Then 应该向 stderr 输出路径不一致错误
    And 建议用户使用 --runone 强制重跑
    And 以 exit code 1 终止程序

  Scenario: 归档策略不一致拒绝恢复
    Given 历史进度文件 archive=1
    And 当前命令未指定 -Z
    When 程序尝试加载会话配置
    Then 应该向 stderr 输出归档策略不一致错误
    And 以 exit code 1 终止程序
```

---

## 3. 半增量扫描 (Semi-Incremental Scan)

### Feature: Blind-Trust 跳过机制

```gherkin
Feature: Blind-Trust 跳过机制
  As a 系统管理员
  I want 对长时间未变更的子树跳过 lstat 调用
  So that 显著降低 I/O 开销，加速增量扫描

  Background:
    Given 上次任务状态为 Success
    And 用户指定了 --skip-interval=604800（7天）
    And 历史索引已加载到 reference_set 和 reference_map

  Scenario: 符合条件的条目启用 blind-trust
    Given Worker 扫描目录时遇到条目 entry
    And entry 的 d_ino 和 d_type 已知
    And 计算出的 fingerprint 存在于 reference_set
    And reference_map 中该 fingerprint 的 mtime 与 d_type 匹配
    And 当前时间 - mtime > skip_interval
    When Worker 处理该条目
    Then 应该跳过 lstat/stat 系统调用
    And 直接使用历史记录的 mtime 和 d_type 构造 stat 结构
    And 将该条目视为已处理并返回 Master

  Scenario: 不符合 blind-trust 条件的条目正常处理
    Given 条目 fingerprint 不在 reference_set
    Or 条目的 mtime 在 skip_interval 内发生过变更
    When Worker 处理该条目
    Then 应该执行正常的 lstat/stat 调用
    And 获取最新的文件元数据
```

---

## 4. 设备熔断与恢复 (Device Circuit Breaker)

### Feature: 设备无响应自动熔断

```gherkin
Feature: 设备无响应自动熔断
  As a 系统管理员
  I want 当底层存储（NFS/SAN LUN）无响应时自动跳过
  So that 避免整个扫描进程陷入 D-State 不可杀死的状态

  Background:
    Given Worker 正在扫描挂载于 dev=0x901 的 NFS 目录
    And NFS 使用了 soft 挂载选项

  Scenario: 设备超时触发熔断
    Given Worker 在 readdir 或 lstat 时遇到 ETIMEDOUT 或 EIO
    When Worker 通过 IPC_MSG_ERROR 上报错误
    Then Master 应该将该设备标记为 DEAD
    And 记录该目录到 spbin（跳过记录）
    And 向 ProbeScheduler 注册探测任务
    And 设置初始探测间隔为 5 秒

  Scenario: 超时 Worker 被替换
    Given Worker 超过 30 秒未发送心跳
    When Monitor 巡检发现该 Worker 心跳超时
    Then 应该向该 Worker 进程发送 SIGKILL
    And 以 WNOHANG 方式 reap 僵尸进程
    And 重新 fork 一个新的 Worker 进程填补该 slot
    And 新 Worker 的 fd_out 重新注册到 epoll

  Scenario: 熔断设备上的任务不阻塞主循环
    Given 设备已被标记为 DEAD
    When Master 从 Worker batch 中收到该设备的条目
    Then 线程池去重时发现设备在黑名单中
    And 将该条目标记为 blacklisted
    And 主循环跳过该条目的后续处理
    And 不向输出队列提交
```

### Feature: 渐进探测与自动恢复

```gherkin
Feature: 渐进探测与自动恢复
  As a 系统管理员
  I want 熔断设备恢复后自动重新扫描积压目录
  So that 无需人工干预即可完成全部扫描

  Scenario: 指数退避探测
    Given 设备已被标记为 DEAD
    And 探测调度器中有该设备的探测任务
    When 上次探测失败后重新调度
    Then 探测间隔应该翻倍：5s → 10s → 20s → ... → 300s（上限）
    And 下次探测时间写入小根堆按时间排序

  Scenario: 敢死队探测成功
    Given 探测任务已到期
    And 当前没有活跃的探测进程
    When Master fork 敢死队子进程执行 lstat(probe_path)
    And 子进程在 PROBE_TIMEOUT_SEC（5秒）内正常返回
    Then 应该将该设备状态恢复为 NORMAL
    And 将该设备在 spbin 中的所有积压目录重新入队扫描

  Scenario: 敢死队探测被杀死
    Given 探测子进程因设备仍无响应而被 alarm 杀死
    When waitpid 发现子进程非正常退出
    Then 应该重新向 ProbeScheduler 推送探测任务
    And retry_count 递增
    And 下次探测时间按指数退避计算

  Scenario: 设备被永久判死
    Given 设备多次探测失败
    And 退避间隔已达到上限 300 秒
    And 持续探测仍失败
    When 调度器将该设备标记为 CONDEMNED
    Then 不再对该设备发起任何探测
    And 该设备的所有积压目录永久跳过
```

---

## 5. 多进程并发模型 (Multi-Process Concurrency)

### Feature: Master-Worker 进程模型

```gherkin
Feature: Master-Worker 进程模型
  As a 系统管理员
  I want 利用多核并发扫描目录树
  So that 最大化 I/O 并行度

  Background:
    Given 系统有 N 个 CPU 核心
    And Master 进程创建了 2*N 个 Worker 进程

  Scenario: Worker 进程创建与 IPC 通道建立
    Given Master 调用 worker_pool_spawn
    When fork 子进程
    Then 父子之间应该建立 pipe2(O_CLOEXEC) 双向通道
    And 子进程关闭除管道外的所有继承文件描述符
    And 子进程进入 worker_main 消息循环

  Scenario: Master 分发扫描任务
    Given Master 从任务队列中取出一个目录路径
    When Master 通过 fd_in 向空闲 Worker 发送 IPC_MSG_SCAN
    Then Worker 应该阻塞读取 fd_in 上的消息
    And 收到消息后执行 scan_and_send

  Scenario: Worker 返回批量结果
    Given Worker 完成一个目录的扫描
    And 收集到 batch_size 条记录
    When Worker 通过 fd_out 发送 IPC_MSG_BATCH
    Then Master 的 epoll_wait 应该被唤醒
    And 主循环读取 batch payload
    And 解析出 paths 和 stats 数组

  Scenario: Worker 正常退出
    Given Worker 收到 IPC_MSG_STOP
    When Worker 发送 IPC_MSG_EXIT 后调用 _exit(0)
    Then Master 的 epoll 触发 EPOLLIN
    And 主循环调用 main_loop_handle_exit
    And 将该 Worker slot 标记为 is_alive=false
```

### Feature: 内核 COW 共享只读上下文

```gherkin
Feature: 内核 COW 共享只读上下文
  As a 开发者
  I want Worker 进程共享 Master 的只读数据结构
  So that 避免进程间显式序列化大量数据

  Scenario: fork 后共享配置与索引
    Given Master 在 fork 前设置了 worker_set_context
    And 上下文包含 Config、reference_set、reference_map
    When fork 创建 Worker 子进程
    Then 子进程通过 COW 共享这些只读结构
    And 子进程不修改这些结构（只读保证）
    And 无需通过 IPC 传输配置数据
```

---

## 6. 去重与防环 (Deduplication)

### Feature: 基于指纹的目录去重

```gherkin
Feature: 基于指纹的目录去重
  As a 系统管理员
  I want 避免通过硬链接或绑定挂载访问同一目录多次
  So that 防止循环遍历和重复输出

  Background:
    Given 指纹计算方式为 xxHash3_128bits(path + dev + ino)

  Scenario: 首次遇到目录
    Given Master 从 Worker batch 中收到一个新路径
    When 线程池计算 fingerprint 并尝试插入 visited_set
    Then visited_set 返回 false（新条目）
    And 该目录被标记为已访问
    And 如果是目录则分配新的 IPC_MSG_SCAN 任务

  Scenario: 重复目录被过滤
    Given 该路径的 fingerprint 已存在于 visited_set
    When 线程池计算 fingerprint 并尝试插入
    Then visited_set 返回 true（已存在）
    And 主循环将该条目标记为 duplicate
    And 跳过输出和子目录分发

  Scenario: 分片开放寻址哈希集合扩容
    Given FingerprintSet 由 64 个 shard 组成
    And 某个 shard 的负载因子超过阈值
    When 插入新 fingerprint 时触发扩容
    Then 应该对该 shard 的 mutex 加锁
    And 重新分配更大的 table 和 meta 数组
    And 重新哈希所有现有条目
    And 释放旧数组
```

---

## 7. 输出格式化与写入 (Output Formatting)

### Feature: 灵活的输出格式

```gherkin
Feature: 灵活的输出格式
  As a 系统管理员
  I want 按需选择输出字段和格式
  So that 输出可以直接用于下游分析或导入

  Scenario: CSV 输出
    Given 用户指定了 --csv
    When 输出线程处理一条记录
    Then 应该将路径和元数据格式化为标准 CSV
    And 对包含逗号或引号的字段进行 RFC-4180 转义

  Scenario: 自定义格式模板
    Given 用户指定了 --format="%p\t%s\t%u\t%g\t%m"
    When 输出线程处理记录
    Then 应该按顺序输出 路径、大小、用户、组、修改时间
    And 字段之间使用制表符分隔

  Scenario: 元数据开关动态格式
    Given 用户未指定 --format
    And 用户指定了 --size --user --mtime
    When 输出线程处理记录
    Then 默认格式自动变为 path|size|user|mtime
    And 每个开关对应字段按固定顺序出现

  Scenario: 包含目录本身信息
    Given 用户指定了 -D (--dirs)
    When 扫描到一个目录
    Then 该目录本身的路径和元数据也应该进入输出队列
    And 与文件记录一起按发现顺序输出

  Scenario: 静默模式
    Given 用户指定了 -M (--mute)
    When 扫描执行期间
    Then 所有正常扫描数据不输出到 stdout 或文件
    And 错误和诊断信息仍输出到 stderr
```

### Feature: 异步批量输出

```gherkin
Feature: 异步批量输出
  As a 开发者
  I want 将格式化写入操作 offload 到独立线程
  So that 避免阻塞 epoll 主循环

  Scenario: 批量提交减少锁竞争
    Given 主循环收集到 256 条待输出记录
    When 调用 async_writer_submit_batch
    Then 应该只获取一次 mutex
    And 将整条链表一次性加入输出队列
    And 通过 pthread_cond_signal 唤醒输出线程

  Scenario: 输出线程格式化写入
    Given 输出线程被唤醒
    When 从队列中取出 OutputTask
    Then 调用 print_to_stream 格式化该记录
    And 写入到 output_fp（文件或 stdout）
    And 释放 OutputTask 内存
```

### Feature: 分片输出

```gherkin
Feature: 分片输出
  As a 系统管理员
  I want 输出按固定行数自动切分到多个文件
  So that 便于并行处理下游任务

  Scenario: 达到切片阈值自动轮转
    Given 用户指定了 --output-split=output_dir
    And 当前输出切片已达到 output_slice_lines 行
    When 下一条记录准备输出
    Then 应该关闭当前切片文件
    And 生成新的切片文件名（如 000001.txt）
    And 将后续记录写入新切片
```

---

## 8. 监控与诊断 (Monitoring & Diagnostics)

### Feature: 独立监控线程

```gherkin
Feature: 独立监控线程
  As a 系统管理员
  I want 通过独立线程持续监控扫描状态
  So that 主循环专注处理 IPC 消息，不被监控逻辑干扰

  Background:
    Given 系统为进程模型（fork + pipe + epoll）
    And Monitor 是独立的 pthread 线程

  Scenario: 监控线程启动
    Given 程序初始化完成
    When Master 启动 epoll 主循环前
    Then 应该创建 Monitor 线程
    And Monitor 线程持有 AppContext 指针（只读 + 有限写）

  Scenario: 监控线程主循环
    Given Monitor 线程正在运行
    When 每 500ms 触发一次循环
    Then 应该调用 print_progress() 输出统计面板
    And 每秒触发一次 check_workers_health()
    And 每秒触发一次 dispatch_probes()
    And 每轮触发一次 reap_probes()

  Scenario: 静默模式下监控线程仍输出面板到文件
    Given 用户指定了 -M (--mute)
    When Monitor 线程执行 print_progress()
    Then 仍然向 stderr 输出统计面板
    And 不输出到 stdout（避免污染管道数据）
```

### Feature: 统计面板输出

```gherkin
Feature: 统计面板输出
  As a 系统管理员
  I want 在终端实时查看扫描进度和系统状态
  So that 及时发现性能瓶颈和设备异常

  Scenario: 终端环境下的 ANSI 清屏面板
    Given stderr 是终端（isatty）
    When Monitor 线程输出统计面板
    Then 应该先发送 ANSI 清屏序列（\033[2J\033[H）
    And 显示运行时间、Worker 活跃度、待处理任务数
    And 显示目录速率、文件速率、消费速率（当前值 + 历史最大值）
    And 显示已扫描目录数、文件数、当前输出分片
    And 显示死设备数和判死设备数（如果有）

  Scenario: 非终端环境下的顺序输出
    Given stderr 被重定向到文件
    When Monitor 线程输出统计面板
    Then 不发送 ANSI 控制序列
    And 以纯文本格式顺序追加输出

  Scenario: 统计采样与滑动窗口速率计算
    Given 扫描正在进行
    When Monitor 每秒采集一次统计快照
    Then 记录当前 dir_count、file_count、dequeued_count
    And 存入 60 秒环形缓冲区
    And 计算滑动窗口内的平均速率
    And 更新 current_dir_rate、current_file_rate、current_dequeue_rate
    And 如果当前速率超过历史最大值，更新 max 记录
```

### Feature: Worker 心跳监控

```gherkin
Feature: Worker 心跳监控
  As a 系统管理员
  I want 及时发现并替换卡死的 Worker
  So that 扫描任务不会无限期停滞

  Scenario: Worker 正常心跳
    Given Worker 开始扫描目录前
    And Worker 完成扫描目录后
    When Worker 发送 IPC_MSG_HEARTBEAT
    Then Master 更新该 slot 的 last_heartbeat 时间戳

  Scenario: Worker 心跳超时（Monitor 线程检测）
    Given Worker 因 D-State 阻塞无法发送心跳
    And 当前时间 - last_heartbeat > heartbeat_timeout（默认30秒）
    When Monitor 线程的 check_workers_health() 执行
    Then 判定该 Worker 卡死
    And 发送 SIGKILL 强制终止
    And 以 WNOHANG 方式 reap 僵尸进程
    And 标记 slot->is_alive = false, slot->pid = -1
    And WorkerPool->active_count 递减
    And 主循环的 "Replace dead workers" 段检测到 pid=-1 后启动替换 Worker
```

### Feature: 敢死队探测调度与收割

```gherkin
Feature: 敢死队探测调度与收割
  As a 系统管理员
  I want 监控线程自动调度设备探测并收割结果
  So that 主循环无需关心探测生命周期

  Scenario: 监控线程调度探测
    Given 探测调度器中有到期的探测任务
    And 当前没有活跃的探测进程
    When Monitor 线程执行 dispatch_probes()
    Then 从 ProbeScheduler 取出到期任务
    And fork 敢死队子进程执行 lstat(probe_path)
    And 子进程设置 alarm(PROBE_TIMEOUT_SEC)
    And 记录活跃探测 pid 和 dev 到 Monitor 结构体

  Scenario: 监控线程收割探测
    Given 有一个活跃的探测子进程
    When Monitor 线程执行 reap_probes()
    Then 以 WNOHANG 方式 waitpid 检查子进程状态
    And 如果子进程正常退出，标记设备为 ALIVE 并重入队积压目录
    And 如果子进程被杀死，重新将探测任务推入 ProbeScheduler
    And 清除 Monitor 中的活跃探测记录
```

---

## 9. 归档与清理 (Archiving & Cleanup)

### Feature: 进度分片压缩归档

```gherkin
Feature: 进度分片压缩归档
  As a 系统管理员
  I want 将已完成的进度分片压缩存储
  So that 节省磁盘空间

  Background:
    Given 用户指定了 -Z (--archive)

  Scenario: 分片完成后归档
    Given 一个 pbin 分片已封口并写入 Footer
    When 程序处理旧分片
    Then 应该读取 Footer 获取 row_count
    And 使用 zlib 压缩分片数据
    And 写入 task1.archive，块头包含 block_type 和 row_count

  Scenario: 恢复时从归档解压
    Given 存在 task1.archive 文件
    When 恢复进度时
    Then 应该读取 archive 块头
    And 解压每个块到临时文件
    And 读取临时文件的 Footer 校验
```

### Feature: 清理历史进度

```gherkin
Feature: 清理历史进度
  As a 系统管理员
  I want 强制删除所有历史进度文件重新开始
  So that 在数据发生重大变更时避免增量误差

  Scenario: 使用 --clean 启动
    Given 用户指定了 -C (--clean)
    When 程序启动
    Then 应该删除所有 progress 相关文件（.idx, .pbin, .spbin, .archive, .config）
    And 将 continue_mode 重置为 false
    And 执行全新的全量扫描

  Scenario: 使用 --runone 强制全量
    Given 用户指定了 --runone
    When 程序启动
    Then 清理历史进度文件
    And 忽略任何现有进度
    And 执行全量扫描但保留进度文件供后续 --continue 使用
```

---

## 10. 信号处理与优雅退出 (Signal Handling)

### Feature: 响应系统信号

```gherkin
Feature: 响应系统信号
  As a 系统管理员
  I want 程序能响应 SIGINT/SIGTERM 优雅退出
  So that 中断时不会损坏进度文件

  Scenario: 收到 SIGINT 或 SIGTERM
    Given 扫描正在执行
    When 用户按下 Ctrl-C 或发送 SIGTERM
    Then 信号处理器设置全局退出标志
    And 主循环检测到标志后停止 epoll_wait
    And 刷出所有缓冲数据（record_path batch、async output queue）
    And 更新 task1.idx 记录最新游标
    And 正常退出，进度文件保持一致性
```

---

## 附录 A: 核心模块职责映射

| 模块 (文件) | BDD 领域 | 核心行为 |
|------------|---------|---------|
| `cmdline` | 配置解析 | Given 命令行参数，Then 填充 Config 结构体 |
| `main_loop` | 事件循环 | Given epoll 事件，Then 分发到对应 Handler |
| `worker_proc` | Worker 生命周期 | Given SCAN 消息，Then 执行目录遍历并返回 BATCH |
| `fingerprint_set` | 去重 | Given 路径+dev+ino，Then 计算指纹并判断存在性 |
| `reference_map` | 半增量索引 | Given 指纹，Then 返回历史 mtime/d_type |
| `device_manager` | 设备状态机 | Given dev_t，Then 返回 NORMAL/DEAD/CONDEMNED |
| `probe_scheduler` | 探测调度 | Given 当前时间，Then 返回到期的探测任务 |
| `thread_pool` | CPU 去重 | Given batch，Then 异步计算指纹和黑名单检查 |
| `async_worker` | 输出流水线 | Given OutputBatch，Then 批量入队、异步格式化写入 |
| `progress` | 进度持久化 | Given 记录，Then 写入 pbin/spbin 并更新 idx |
| `output` | 格式化引擎 | Given 路径+stat+格式模板，Then 输出到流 |
| `signals` | 信号处理 | Given SIGINT/SIGTERM，Then 设置退出标志 |
| `utils` | 基础设施 | Given 内存请求，Then 安全分配或 fatal 退出 |

## 附录 B: IPC 消息类型

| 消息类型 | 方向 | 行为描述 |
|---------|------|---------|
| `IPC_MSG_SCAN` | Master → Worker | Given 目录路径，Then Worker 执行扫描 |
| `IPC_MSG_BATCH` | Worker → Master | Given 扫描结果，Then Master 解析并去重 |
| `IPC_MSG_HEARTBEAT` | Worker → Master | Given 时间戳，Then Master 更新存活状态 |
| `IPC_MSG_ERROR` | Worker → Master | Given errno+dev+path，Then Master 触发熔断 |
| `IPC_MSG_EXIT` | Worker → Master | Given 退出信号，Then Master 回收 slot |
| `IPC_MSG_STOP` | Master → Worker | Given 停止指令，Then Worker 优雅退出 |

## 附录 C: 设备状态转换

```
[NORMAL] --(ETIMEDOUT/EIO)--> [DEAD] --(probe success)--> [NORMAL]
                                    |
                                    +--(probe fail xN)--> [CONDEMNED]
```

---

*本 BDD 文档描述的是 listfiles 的行为规格，而非测试用例。实际测试实现可参考各模块的单元测试和集成测试。*
