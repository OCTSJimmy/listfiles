# P0-001: FINISH/BATCH 竞态 — 目录任务完成屏障
# 行为驱动开发 (BDD) 规范
# 关联: P0-009 目录任务生命周期状态机

Feature: 目录任务完成屏障
  作为扫描系统
  我需要确保 FINISH 信号不早于所有 BATCH 数据处理完成
  以防止子树在竞态条件下永久漏扫

  Background:
    Given 系统处于正常运行状态
    And Worker 采用双通道独立 epoll（控制通道 + 数据通道）
    And 目录任务状态机已启用（P0-009）

  # ===== 核心竞态场景 =====

  Scenario: FINISH 先于 BATCH 到达 Master
    Given Worker 正在扫描目录 "/data/projectA"
    And Worker 已发送 BATCH 批次 #3（含子目录 /data/projectA/subdir1, /data/projectA/subdir2）
    And Worker 已发送 FINISH 信号标记目录扫描完成
    When 控制通道 epoll 先于数据通道触发
    Then Master 收到 FINISH 时目录状态应转移至 ALL_BATCHES_RECEIVED
    And 目录状态不应直接标记为 COMPLETED
    And 待处理 BATCH #3 仍应被接收和处理
    And 子目录 /data/projectA/subdir1 和 /data/projectA/subdir2 应被正确入队
    And 只有所有 BATCH 处理完成后，状态才能转移至 ALL_BATCHES_PROCESSED

  Scenario: BATCH 在 FINISH 之后到达
    Given 目录 "/data/projectB" 已收到 FINISH 信号
    And 目录状态当前为 ALL_BATCHES_RECEIVED
    When 滞后的 BATCH #2 到达 Master
    Then Master 应接受该 BATCH
    And 目录状态保持为 ALL_BATCHES_RECEIVED（不提前转移到后续状态）
    And BATCH 中的文件条目应被正常处理

  Scenario: 目录任务屏障阻止提前完成
    Given 目录 "/data/projectC" 正在扫描中
    And 已收到部分 BATCH（#1, #2）
    And FINISH 信号已到达
    And 目录状态为 ALL_BATCHES_RECEIVED
    When 检查是否可标记为 COMPLETED
    Then 系统应拒绝提前标记
    And 应等待剩余 BATCH 到达
    And 应等待所有子目录入队完成
    And 应等待输出偏移确认（OUTPUT_COMMITTED）

  # ===== 边界条件 =====

  Scenario: 空目录（无 BATCH）正常完成
    Given 目录 "/data/empty_dir" 不含任何文件或子目录
    When Worker 发送 FINISH 信号
    Then 目录状态转移至 ALL_BATCHES_RECEIVED
    And 由于没有待处理 BATCH，直接转移至 ALL_BATCHES_PROCESSED
    And 输出队列为空，直接转移至 OUTPUT_COMMITTED
    And 最终标记为 COMPLETED
    And 写入 dpbin

  Scenario: Worker 崩溃后屏障重建
    Given Worker #3 在处理目录 "/data/projectD" 时崩溃
    And 部分 BATCH 已发送但未到达 Master
    And 目录状态为 DISPATCHED（已派发）
    When Master 检测到 Worker 死亡
    And 目录被重新入队派发
    Then 新 Worker 应重新扫描该目录
    And 旧 Worker 残留的未完成 BATCH 应被忽略（epoch 机制，P0-012）
    And 新的 FINISH/BATCH 序列应重新建立屏障

  # ===== 并发安全 =====

  Scenario: 多 Worker 同时报告同一目录（异常场景）
    Given 目录 "/data/projectE" 被意外派发至两个 Worker
    When 两个 Worker 都发送 FINISH 信号
    Then 第二个 FINISH 应被识别为重复并忽略
    And 目录状态机应保持正确（不重复转移）
    And dpbin 只写入一次

  # ===== 验收标准 =====

  Scenario: 屏障完整性验证
    Given 系统完成一次完整扫描
    When 验证所有已标记 COMPLETED 的目录
    Then 每个目录应满足：
      | 条件 | 状态 |
      | 收到 FINISH 信号 | ALL_BATCHES_RECEIVED |
      | 所有 BATCH 已处理 | ALL_BATCHES_PROCESSED |
      | 输出已提交 | OUTPUT_COMMITTED |
      | dpbin 已记录 | COMPLETED |
    And 不应存在"FINISH 到达但 BATCH 丢失"的目录
    And 不应存在"子目录未入队但父目录已完成"的情况
