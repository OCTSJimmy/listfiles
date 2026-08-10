# P0-012: epoch + waitpid 确认（旧 Worker 残留数据）
# 行为驱动开发 (BDD) 规范

Feature: Worker 纪元与生命周期确认
  作为扫描系统
  我需要防止 SIGKILL 后旧 Worker 的残留数据污染新任务
  通过 epoch 机制和 waitpid 确认保证 IPC 数据一致性

  Background:
    Given 每个 CMD_SCAN 携带递增 epoch（64 位原子计数器）
    And Worker 返回的所有消息携带该 epoch
    And Master 维护当前有效 epoch
    And IPC 管道为每个 Worker 独立

  # ===== epoch 机制 =====

  Scenario: CMD_SCAN 附带 epoch
    Given Master 派发目录 "/data/dir1" 给 Worker #3
    And 当前全局 epoch = 42
    When 发送 CMD_SCAN
    Then 消息包含 epoch=42
    And Worker #3 记录当前 epoch=42

  Scenario: Worker 消息携带 epoch
    Given Worker #3 正在处理 epoch=42 的任务
    When 发送 BATCH 或 FINISH
    Then 消息包含 epoch=42
    And Master 校验 epoch == 当前有效 epoch
    And 匹配则处理
    And 不匹配则丢弃

  Scenario: 过期 epoch 消息丢弃
    Given Worker #3 因超时被 SIGKILL
    And Master 已派发给 Worker #7（epoch=43）
    When 旧 Worker #3 的残留 BATCH（epoch=42）到达
    Then Master 发现 epoch=42 != 当前有效 epoch=43
    And 丢弃该 BATCH
    And 记录 debug 日志（"丢弃过期 epoch 消息"）

  # ===== waitpid 确认 =====

  Scenario: SIGKILL 后等待进程回收
    Given Worker #3 被发送 SIGKILL
    When Master 执行 waitpid(WNOHANG)
    Then 轮询直到旧进程被回收
    And 通常耗时 < 1ms
    And 确认旧进程已完全退出
    And 旧进程的内核状态已清理

  Scenario: waitpid 超时处理
    Given Worker #3 被 SIGKILL
    And waitpid(WNOHANG) 连续 10 次未回收
    When 达到超时阈值（100ms）
    Then 记录 warning 日志
    And 继续执行（不等了，epoch 机制保证安全）
    And 旧进程最终会被 init 回收

  # ===== pipe drain =====

  Scenario: CMD_REPLACE 时清空旧 pipe
    Given Worker #3 被替换
    When CMD_REPLACE 执行
    Then IPC 线程先 drain 旧 pipe 读缓冲区
    And 读取并丢弃所有残留数据
    And 关闭旧 pipe fd
    And 创建新 pipe
    And 新 Worker #7 使用新 pipe

  Scenario: pipe drain 不阻塞
    Given 旧 pipe 中有大量残留数据
    When drain 执行
    Then 使用非阻塞 read
    And 循环读取直到 EAGAIN
    And 不等待新数据
    And 快速完成

  # ===== epoch 与状态机交互 =====

  Scenario: 新 epoch 与目录状态
    Given 目录 "/data/dir1" 被 Worker #3 扫描（epoch=42）
    And 超时后被重新派发给 Worker #7（epoch=43）
 When Master 处理新消息
    Then 只接受 epoch=43 的消息
    And epoch=42 的 FINISH 被忽略
    And 目录状态机以 epoch=43 的消息为准

  Scenario: epoch 溢出保护
    Given epoch 是 64 位无符号整数
    When 系统长期运行
    Then epoch 在可预见未来不会溢出（2^64）
    And 不处理回绕情况（实际不可能）

  # ===== 与 P0-001 竞态交互 =====

  Scenario: epoch 防止 FINISH/BATCH 竞态恶化
    Given Worker #3（epoch=42）发送 FINISH 后崩溃
    And 残留 BATCH 在管道中
    And 新 Worker #7（epoch=43）接管
    When 残留 BATCH（epoch=42）到达
    Then 被 epoch 检查丢弃
    And 不干扰新 Worker 的扫描
    And FINISH/BATCH 竞态仅在同一 epoch 内存在（P0-001 处理）

  # ===== 验收标准 =====

  Scenario: epoch 机制完整性验证
    Given 模拟 Worker 频繁崩溃重启
    When 系统稳定运行
    Then 验证：
      | 检查项 | 期望结果 |
      | 无过期 epoch 消息被处理 | 是 |
      | 无新旧 Worker 数据混淆 | 是 |
      | waitpid 最终回收所有进程 | 是 |
      | pipe drain 不阻塞 | 是 |
      | epoch 单调递增 | 是 |
    And 系统可承受 Worker 频繁崩溃
