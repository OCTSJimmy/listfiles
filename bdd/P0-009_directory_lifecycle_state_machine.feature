# P0-009: 目录任务生命周期状态机
# 行为驱动开发 (BDD) 规范
# 关联: P0-001, P0-002

Feature: 目录任务生命周期状态机
  作为扫描系统
  我需要明确定义目录从发现到完成的完整状态流转
  以支撑崩溃恢复、背压控制和输出一致性

  Background:
    Given 目录状态机定义以下状态：
      | 状态 | 编码 | 说明 |
      | DISCOVERED | 0x01 | 已发现，尚未持久化 |
      | PERSISTED | 0x02 | 已写 pbin |
      | ENQUEUED | 0x04 | 已入队（dispatch_queue 或 dspill） |
      | DISPATCHED | 0x08 | 已派发给 Worker |
      | SCANNING | 0x10 | Worker 正在扫描 |
      | ALL_BATCHES_RECEIVED | 0x20 | 收到 FINISH，所有 BATCH 已接收 |
      | ALL_BATCHES_PROCESSED | 0x40 | 所有 BATCH 处理完成 |
      | OUTPUT_COMMITTED | 0x80 | 输出已刷盘 |
      | COMPLETED | 0x100 | 已写 dpbin |
    And 定义以下异常状态：
      | 状态 | 编码 | 说明 |
      | RETRYING | 0x1000 | 正在重试 |
      | BACKOFF | 0x2000 | 指数退避等待 |
      | DEVICE_WAITING | 0x4000 | 等待设备探测 |
      | SKIPPED_FAILED | 0x8000 | 永久跳过（失败/权限） |
      | UNKNOWN | 0x10000 | 状态未知（需 Reset） |

  # ===== 正常状态流转 =====

  Scenario: 完整成功流转
    Given 新目录 "/data/dir1" 被发现
    When 状态机推进
    Then 流转路径：
      | 步骤 | 触发条件 | 新状态 |
      | 1 | Worker 发现 | DISCOVERED |
      | 2 | 写 pbin | PERSISTED |
      | 3 | enqueue() | ENQUEUED |
      | 4 | 派发给 Worker | DISPATCHED |
      | 5 | Worker 开始扫描 | SCANNING |
      | 6 | 收到 FINISH | ALL_BATCHES_RECEIVED |
      | 7 | 所有 BATCH 处理完 | ALL_BATCHES_PROCESSED |
      | 8 | 输出刷盘 | OUTPUT_COMMITTED |
      | 9 | dpbin_append | COMPLETED |

  # ===== 状态屏障（P0-001） =====

  Scenario: FINISH 不直接跳转 COMPLETED
    Given 目录状态 = SCANNING
    When 收到 FINISH 信号
    Then 状态转移至 ALL_BATCHES_RECEIVED
    And 不直接跳转到 COMPLETED
    And 等待所有 BATCH 到达

  Scenario: ALL_BATCHES_RECEIVED 等待处理
    Given 目录状态 = ALL_BATCHES_RECEIVED
    And 还有 BATCH 未处理
    When 检查状态推进
    Then 保持 ALL_BATCHES_RECEIVED
    And 不转移到 ALL_BATCHES_PROCESSED

  # ===== 输出三态（P0-002） =====

  Scenario: OUTPUT_COMMITTED 阻塞 dpbin
    Given 目录状态 = ALL_BATCHES_PROCESSED
    And 输出尚未刷盘
    When dpbin_append 调用
    Then 阻塞等待
    And 不转移到 COMPLETED
    And 不写入 dpbin

  # ===== 异常状态流转 =====

  Scenario: Worker 错误导致 SKIPPED_FAILED
    Given 目录状态 = SCANNING
    When Worker 返回 RET_ERROR
    Then 状态转移至 SKIPPED_FAILED
    And 写入 spbin
    And 不重入队（防止重试风暴）

  Scenario: 设备熔断导致 DEVICE_WAITING
    Given 目录状态 = SCANNING
    When 设备达到熔断阈值
    Then Worker 被信号终止
    And 目录状态转移至 DEVICE_WAITING
    And 设备进入 PROBING
    And 等待敢死队探测结果

  Scenario: 超时目录进入 BACKOFF
    Given 目录扫描超时
    When 超时处理触发
    Then 状态转移至 BACKOFF
    And 记录 timestamp
    And 等待退避窗口（指数退避）
    And 窗口到期后转移至 RETRYING

  # ===== 崩溃恢复状态 =====

  Scenario: 崩溃后状态为 UNKNOWN
    Given 系统崩溃
    And 目录状态在崩溃前非 COMPLETED
    When 恢复启动
    Then 目录状态视为 UNKNOWN
    And 执行 Reset 援救（P0-010）
    And 重新入队扫描

  Scenario: 崩溃后 COMPLETED 目录保持不变
    Given 系统崩溃
    And 目录状态在崩溃前为 COMPLETED
    When 恢复启动
    Then 状态保持 COMPLETED
    And 在 completed_set 中
    And 不进差集
    And 不重新扫描

  # ===== 状态位运算 =====

  Scenario: 状态位检查
    Given 目录状态 = OUTPUT_COMMITTED | COMPLETED
    When 检查是否已完成
    Then (status & COMPLETED) != 0 → true
    And 盲信扫描可跳过

  Scenario: 状态位组合有效性
    Given 以下组合是有效的：
      | 组合 | 说明 |
      | ENQUEUED | 在队列中等待 |
      | DISPATCHED \| SCANNING | 已派发，正在扫描 |
      | ALL_BATCHES_RECEIVED \| ALL_BATCHES_PROCESSED | 等待输出 |
    And 以下组合是无效的：
      | 组合 | 原因 |
      | DISCOVERED \| COMPLETED | 未完成不能标记完成 |
      | SKIPPED_FAILED \| COMPLETED | 跳过不能标记完成 |

  # ===== 验收标准 =====

  Scenario: 状态机完整性验证
    Given 系统全生命周期运行
    When 验证所有目录状态
    Then 每个目录状态必须满足：
      | 条件 | 必须成立 |
      | 状态转换单向（不循环） | 是 |
      | COMPLETED 是终态（除 Reset 外不转移） | 是 |
      | 异常状态不重试超过上限 | 是 |
      | 无目录永远 stuck 在非终态 | 是 |
