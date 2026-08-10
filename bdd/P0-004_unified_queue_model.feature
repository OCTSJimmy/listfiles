# P0-004: visited_set 背压竞态 — 统一队列模型
# 行为驱动开发 (BDD) 规范
# 关联: P0-003, P1-001

Feature: 统一队列模型与 visited_set 语义拆分
  作为扫描系统
  我需要消除"已发现"与"已入队"的语义混用
  通过统一调度入口和三态集合防止背压目录丢失

  Background:
    Given 系统已启用目录任务生命周期状态机（P0-009）
    And dspill 作为 dispatch_queue 的磁盘扩展
    And dspill 采用 append-only 格式，字节游标回填

  # ===== visited_set 语义拆分 =====

  Scenario: 目录进入 discovered_set
    Given Worker 扫描发现新目录 "/data/dir1"
    When batch_processor 处理 BATCH
    Then 目录写入 pbin
    And 目录进入 discovered_set
    And discovered_set 防止重复发现
    But 目录尚未进入 enqueued_set

  Scenario: 目录进入 enqueued_set
    Given 目录 "/data/dir1" 已在 discovered_set
    When 统一 enqueue() 调度入口被调用
    Then 目录进入 dispatch_queue 或 dspill
    And 目录进入 enqueued_set
    And enqueued_set 防止重复入队
    And 目录状态转移至 ENQUEUED

  Scenario: 目录进入 completed_set
    Given 目录 "/data/dir1" 已完成扫描
    And 输出已 COMMITTED（P0-002）
    When dpbin_append 执行
    Then 目录进入 completed_set
    And completed_set 防止重复扫描
    And 目录状态转移至 COMPLETED

  # ===== 统一 enqueue() 调度入口 =====

  Scenario: 恢复 pump 路径走统一入口
    Given 系统从 pbin-dpbin 差集恢复
    And 目录 "/data/recover1" 需要重新扫描
    When 恢复逻辑调用 enqueue()
    Then 目录进入 enqueued_set
    And 目录进入 dispatch_queue 或 dspill
    And 与运行时新发现目录走同一入口

  Scenario: 运行时新发现路径走统一入口
    Given Worker 扫描发现新目录 "/data/new1"
    When 统一 enqueue() 被调用
    Then 目录进入 enqueued_set
    And 目录进入 dispatch_queue 或 dspill
    And 与恢复 pump 路径走同一入口

  Scenario: dspill 回填路径走统一入口
    Given dspill 中有积压目录 "/data/backlog1"
    When dspill 回填逻辑调用 enqueue()
    Then 目录进入 enqueued_set
    And 目录进入 dispatch_queue
    And 与运行时新发现路径走同一入口

  # ===== 背压场景 =====

  Scenario: dispatch_queue 满时写入 dspill
    Given dispatch_queue 内存缓冲已达上限（1000条/1秒）
    And 新目录 "/data/pressure1" 需要入队
    When enqueue() 被调用
    Then 目录写入 dspill 磁盘文件
    And 目录进入 enqueued_set
    And discovered_set 已有该目录
    And 系统不阻塞（异步刷盘）

  Scenario: dspill 回填不触发重复扫描
    Given 目录 "/data/backlog2" 已在 dspill 中
    And 目录已在 enqueued_set
    When dspill 回填逻辑触发
    Then 检查 enqueued_set 发现已存在
    And 跳过重复入队
    And 从 dspill 中移除该条目（或标记已回填）

  # ===== 废除 pbin cursor 运行时回填 =====

  Scenario: 运行时只使用 dspill 扩展队列
    Given 系统正常运行（非恢复阶段）
    When dispatch_queue 满
    Then 目录写入 dspill
    And 不修改 pbin 游标
    And pbin 保持 append-only

  Scenario: 恢复时加载 dspill 并集
    Given 系统崩溃后恢复
    And dspill 文件存在且非空
    When 恢复逻辑执行
    Then 加载 dspill 内容到 dispatch_queue
    And 与 pbin-dpbin 差集合并
    And 去重后统一入队

  # ===== 崩溃丢失可接受性 =====

  Scenario: dspill 崩溃丢失不丢数据
    Given dspill 采用内存缓冲（1000条/1秒刷盘）
    And 系统崩溃时 dspill 缓冲未刷盘
    When 系统恢复
    Then 丢失的 dspill 条目从 pbin 重新恢复
    And pbin 是权威持久化
    And 丢失的目录会被重新发现
    And 不导致数据丢失（可能重复扫描）

  # ===== 验收标准 =====

  Scenario: 统一队列模型完整性验证
    Given 系统高负载运行（背压触发）
    When 监控目录状态流转
    Then 所有目录必须满足：
      | 条件 | 必须成立 |
      | discovered_set ⊇ enqueued_set | 已入队必先已发现 |
      | enqueued_set ⊇ completed_set | 已完成必先已入队 |
      | 无目录在 discovered_set 但不在 enqueued_set 且未背压 | 除非在 dspill 等待回填 |
      | 无目录在 completed_set 但不在 discovered_set | 完成必先发现 |
    And 不存在"背压写 dspill 但 visited_set 误判丢弃"的情况
    And 所有待扫描目录最终都进入 dispatch_queue 或被 Worker 消费
