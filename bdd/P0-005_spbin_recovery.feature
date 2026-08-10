# P0-005: spbin 纳入恢复路径
# 行为驱动开发 (BDD) 规范
# 关联: P1-002 errno 分类矩阵, P1-004 毒丸目录

Feature: spbin 恢复路径集成
  作为扫描系统
  我需要将跳过目录清单（spbin）纳入崩溃恢复逻辑
  以防止被熔断或跳过的目录在续传时永久丢失

  Background:
    Given spbin 格式已扩展：
      | 字段 | 类型 | 说明 |
      | path | 变长字符串 | 目录路径 |
      | reason | uint8_t | 跳过原因码 |
      | timestamp | time_t | 记录时间 |
      | device_key | 64字节 | 设备身份标识（fsid, server, export） |
    And 设备状态机：NORMAL → PROBING → DEGRADED → DEAD

  # ===== 跳过原因码 =====

  Scenario Outline: spbin 原因码分类处理
    Given 目录 "/data/skip/<type>" 因 <reason> 被跳过
    When 恢复时读取 spbin
    Then 应按 <action> 处理

    Examples:
      | type        | reason            | action                                  |
      | permission  | PERMISSION(1)     | 永久跳过，不重试                        |
      | circuit     | CIRCUIT_BREAKER(2)| 永久跳过，设备标记DEAD                  |
      | probe_fail  | PROBE_FAIL(3)     | 超窗后敢死队探测（30min→2h→6h→24h）    |
      | timeout     | TIMEOUT(4)        | 超窗后敢死队探测（30min→2h→6h→24h）    |
      | poison      | POISON(5)         | 永久跳过，不计入设备统计（P1-004）      |

  # ===== 恢复时 spbin 处理 =====

  Scenario: 按 device_key 分组恢复
    Given spbin 中有以下记录：
      | path              | reason          | device_key |
      | /data/dev1/dir1   | PROBE_FAIL      | dev1_key   |
      | /data/dev1/dir2   | TIMEOUT         | dev1_key   |
      | /data/dev2/dir3   | PERMISSION      | dev2_key   |
      | /data/dev3/dir4   | CIRCUIT_BREAKER | dev3_key   |
    When 系统恢复启动
    Then 按 device_key 分组处理：
      | device_key | 处理动作                          |
      | dev1_key   | 两个目录都进入探测队列（PROBING）  |
      | dev2_key   | 永久跳过（PERMISSION）            |
      | dev3_key   | 永久跳过，设备标记DEAD            |

  Scenario: 超窗探测成功
    Given 目录 "/data/retry1" 因 PROBE_FAIL 进入 spbin
    And 上次记录时间戳为 2 小时前
    And 探测退避窗口为 30min→2h（当前已过 2h 窗口）
 When 敢死队探测执行
    Then 探测成功 → 设备标记 NORMAL
    And spbin 中该设备条目删除
    And 目录重新入队扫描

  Scenario: 超窗探测失败
    Given 目录 "/data/retry2" 因 TIMEOUT 进入 spbin
    And 上次记录时间戳为 2 小时前
    And 当前退避窗口为 2h
    When 敢死队探测执行
    Then 探测失败 → timestamp 更新为当前时间
    And 退避窗口升级至 6h
    And 目录保持跳过状态
    And 等待下一个窗口

  Scenario: 永久跳过项不探测
    Given 目录 "/data/noretry1" 因 PERMISSION 进入 spbin
    When 系统恢复
    Then 直接标记为 SKIPPED_FAILED
    And 不进入探测队列
    And 不计入设备级错误统计

  # ===== 毒丸目录（P1-004） =====

  Scenario: 毒丸目录三次致死隔离
    Given 目录 "/data/poison" 已因 Worker 崩溃被跳过 2 次
    And 当前是第 3 次跳过
    When 跳过处理
    Then reason 标记为 POISON(5)
    And 永久跳过
    And 不计入设备级错误统计（避免拖垮熔断）
    And 记录警告日志

  # ===== spbin 压缩清理 =====

  Scenario: 正常退出时 spbin 压缩
    Given 系统正常退出（非崩溃）
 And spbin 中有已恢复条目
    When 执行 finalize()
    Then 删除已恢复（设备已 NORMAL）的 spbin 条目
    And 保留未恢复的条目
    And 压缩后的 spbin 重写

  Scenario: spbin 条目上限保护
    Given spbin 条目数接近上限（10万条）
    When 新条目需要写入
    Then 触发紧急压缩（删除已恢复条目）
    And 若仍超限，按 timestamp 最旧的 PERMISSION/POISON 条目丢弃
    And 记录警告日志

  # ===== 与 P0-010 Reset 交互 =====

  Scenario: Reset 时 spbin 目录处理
    Given 执行 Reset 援救（P0-010）
    And spbin 中有 PROBE_FAIL/TIMEOUT 条目
    When 恢复启动
    Then spbin 条目按 device_key 分组
    And 超窗条目进入敢死队探测
    And 未超窗条目保持跳过状态
    And PERMISSION/CIRCUIT_BREAKER/POISON 永久跳过

  # ===== 验收标准 =====

  Scenario: spbin 恢复完整性验证
    Given 系统在有跳过目录的场景下崩溃并恢复
    When 恢复完成且系统稳定运行
    Then 所有被跳过的目录都有明确去向：
      | 原因类型            | 最终状态      |
      | PERMISSION/POISON   | 永久跳过      |
      | CIRCUIT_BREAKER     | 永久跳过      |
      | PROBE_FAIL/TIMEOUT  | 探测后决定    |
    And 没有目录因 spbin 未被读取而永久丢失
    And 探测退避策略正确执行（指数退避）
