# P0-007: RET_ERROR 状态机补全
# 行为驱动开发 (BDD) 规范

Feature: RET_ERROR 状态机完整处理
  作为扫描系统
  我需要定义 Worker 返回 ERROR 后的完整状态流转
  以避免 pending_tasks 泄漏、重试风暴和设备状态误判

  Background:
    Given Worker 状态机：BUSY → (FINISH/ERROR) → IDLE
    And 目录任务生命周期状态机已启用（P0-009）
    And spbin 扩展格式已启用（P0-005）

  # ===== RET_ERROR 标准处理 =====

  Scenario: Worker 返回 RET_ERROR 的标准路径
    Given Worker #5 正在扫描目录 "/data/err_dir"
    And Worker #5 状态为 BUSY
    And pending_tasks = 5
    When Worker #5 发送 RET_ERROR（probe 失败）
    Then Master 接收 ERROR
    And pending_tasks 减 1（变为 4）
    And Worker #5 状态转移至 IDLE
    And 当前目录 "/data/err_dir" 不重入队（避免重试风暴）
    And 目录状态转移至 SKIPPED_FAILED
    And 写入 spbin：
      | 字段      | 值               |
      | path      | /data/err_dir    |
      | reason    | SP_REASON_PROBE_FAIL(3) |
      | timestamp | 当前时间          |
      | device_key| 该目录所属设备     |
    And 设备进入 PROBING 态

  Scenario: RET_ERROR 后 pending_tasks 正确递减
    Given 初始 pending_tasks = 3
    And Worker #1 和 #2 为 BUSY，#3 为 IDLE
    When Worker #1 发送 RET_ERROR
    Then pending_tasks = 2
    And Worker #1 状态 = IDLE
    When Worker #2 发送 FINISH
    Then pending_tasks = 1
    And Worker #2 状态 = IDLE
    When 最后一个目录完成
    Then pending_tasks = 0
    And 终止条件触发（P0-001 中 dispatch_queue 为空）

  # ===== 防止重试风暴 =====

  Scenario: 错误目录不重入队
    Given 目录 "/data/fail_once" 导致 RET_ERROR
    And 已写入 spbin（reason=PROBE_FAIL）
    When 检查是否需要重入队
    Then 不重入队
    And 等待敢死队探测结果（P0-005）
    And 探测成功后由探测机制入队

  Scenario: 连续 ERROR 设备熔断
    Given 设备 dev_X 在 1 分钟内产生 10 个 RET_ERROR
    When 错误计数达到熔断阈值
    Then 设备状态转移至 CIRCUIT_BREAKER
    And 该设备所有待处理目录写入 spbin（reason=CIRCUIT_BREAKER）
    And 该设备新目录不再派发
    And 已 BUSY 的 Worker 继续完成（不中断）

  # ===== 与 spbin 交互 =====

  Scenario: RET_ERROR 与 spbin 写入原子性
    Given Worker 返回 RET_ERROR
    When 处理 ERROR 时
    Then pending_tasks-- 和 spbin 写入应在同一临界区
    And 防止崩溃后 pending_tasks 与 spbin 不一致
    And 或采用"先写 spbin，后 pending_tasks--"顺序

  # ===== 异常分支 =====

  Scenario: 未知 reason 的 RET_ERROR
    Given Worker 返回 RET_ERROR 携带未知 reason 码
    When Master 处理
    Then 记录 warning 日志
    And 按 PROBE_FAIL 处理（保守策略）
    And 写入 spbin（reason=PROBE_FAIL）
    And 不重入队

  Scenario: RET_ERROR 后 Worker 异常死亡
    Given Worker 已发送 RET_ERROR
    And Master 尚未处理完毕
    When Worker 进程崩溃
    Then Master 通过 waitpid 回收（P0-012）
    And ERROR 处理继续完成
    And 不重复减 pending_tasks

  # ===== 验收标准 =====

  Scenario: RET_ERROR 处理完整性验证
    Given 模拟多个 Worker 返回 ERROR 的场景
    When 系统处理完毕
    Then 验证：
      | 检查项 | 期望结果 |
      | pending_tasks 最终为 0 | 是 |
      | 无目录无限重试 | 是 |
      | 所有 ERROR 目录进 spbin | 是 |
      | 设备状态正确转移 | 是 |
      | 无内存泄漏 | 是 |
