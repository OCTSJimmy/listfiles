# P0-003: fpbin 二次崩溃恢复路径
# 行为驱动开发 (BDD) 规范

Feature: fpbin 二次崩溃恢复
  作为扫描系统
  我需要确保第一次续传期间产生的 fpbin 在二次崩溃后不会导致目录永久漏扫
  通过 fpbin+dfpbin 原子对机制保证恢复安全

  Background:
    Given 系统已启用 fpbin 作为可抛弃工作区
    And fpbin 格式与 pbin 相同（目录发现记录）
    And dfpbin 格式与 dpbin 相同（已完成目录记录）
    And Footer 校验机制已启用

  # ===== 正常 fpbin 生命周期 =====

  Scenario: 正常 fpbin 生成与转正
    Given 系统处于续传恢复阶段
    And 从旧 pbin 加载未完成任务
    When Worker 发现新目录并写入 fpbin
    And 父目录扫描完成后写入 dfpbin
    And 所有 fpbin 目录处理完毕
    Then fpbin + dfpbin 通过 Footer 校验
    And fpbin 内容合并到 pbin
    And dfpbin 内容合并到 dpbin
    And fpbin/dfpbin 被清空
    And 系统继续正常运行

  # ===== 二次崩溃场景 =====

  Scenario: 二次崩溃后 fpbin+dfpbin 完整
    Given 第一次续传期间已生成 fpbin 和 dfpbin
    And 新目录 "/data/new1"、"/data/new2" 已写入 fpbin
    And 父目录 "/data/parent1" 已写入 dfpbin
    When 系统在 fpbin 处理期间再次崩溃
    And 系统第二次恢复启动
    Then 检查 fpbin + dfpbin 原子对完整性
    And Footer 校验通过
    Then fpbin 内容合并到 pbin
    And dfpbin 内容合并到 dpbin
    And 新目录被正确纳入扫描历史

  Scenario: 二次崩溃后 fpbin+dfpbin 不完整
    Given 第一次续传期间已生成 fpbin
    And fpbin 写入过程中系统崩溃
    And dfpbin 未生成或损坏
    When 系统第二次恢复启动
    Then 检查 fpbin + dfpbin 原子对完整性
    And Footer 校验失败
    Then 整对抛弃 fpbin 和 dfpbin
    And 重新从旧 pbin 恢复（pbin - dpbin 差集）
    And 新目录在下次全量扫描时重新发现
    And 不会无限套娃（不递归产生新的 fpbin）

  Scenario: 仅 fpbin 存在，dfpbin 缺失
    Given 系统崩溃前只写入了 fpbin
    And dfpbin 文件不存在
    When 恢复启动
    Then 完整性检查失败
    And fpbin 被抛弃
    And 从旧 pbin 恢复

  Scenario: 仅 dfpbin 存在，fpbin 缺失
    Given 系统崩溃前只写入了 dfpbin
    And fpbin 文件不存在
    When 恢复启动
    Then 完整性检查失败
    And dfpbin 被抛弃
    And 从旧 pbin 恢复

  # ===== 防止无限套娃 =====

  Scenario: 恢复时不产生新的 fpbin
    Given 系统从旧 pbin 恢复（非 fpbin 转正路径）
    When 恢复过程中发现新目录
    Then 新目录直接写入 pbin（而非 fpbin）
    And 不创建新的 fpbin 文件
    And 恢复完成后系统进入正常运行模式

  Scenario: fpbin 转正后清理
    Given fpbin 成功合并到 pbin
    When 合并完成
    Then fpbin 文件被截断或删除
    And dfpbin 文件被截断或删除
    And 下次正常运行不产生 fpbin

  # ===== 与 P0-010 Reset 机制交互 =====

  Scenario: fpbin 不完整时 Reset 援救
    Given 二次崩溃后 fpbin+dfpbin 不完整
    When 执行 Reset 援救（P0-010）
    Then 所有非 COMPLETED 目录重新入队
    And fpbin 中的目录（若被抛弃）通过旧 pbin 重新扫描
    And 输出文件截断到最后确认的 dpbin offset
    And 系统可安全继续

  # ===== 验收标准 =====

  Scenario: fpbin 恢复完整性验证
    Given 模拟一次完整的一次续传 + 二次崩溃 + 二次恢复
    When 系统最终稳定运行
    Then 所有应被发现的目录最终都被发现
    And 没有目录永久漏扫
    And 如果 fpbin 被抛弃，新目录在下次扫描时重新发现
    And 如果 fpbin 转正，新目录正确进入扫描历史
    And 系统不会进入 fpbin 递归生成循环
