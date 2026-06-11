# Zerofault

> 预分配物理页 + 一次 mmap 建立直接映射 → 用户态零缺页访问的 Linux 内核模块

[![Linux](https://img.shields.io/badge/Linux-6.16.2-blue)](https://kernel.org)
[![License](https://img.shields.io/badge/license-GPL--2.0-green)](LICENSE)
[![LOC](https://img.shields.io/badge/lines-813-lightgrey)]()
[![Version](https://img.shields.io/badge/version-v1.1-brightgreen)]()

---

## 解决什么问题

普通 `mmap` 在首次访问每页时触发缺页中断（page fault），逐页分配物理内存并填充 PTE——这对高频交易等低延迟场景不可接受。Zerofault 在 `insmod` 时一次性预分配所有物理页并 pin 住，`mmap` 时直接建立全部映射，用户态访问永不缺页。

```
传统 mmap:  mmap→[缺页→alloc→建PTE→TLB] × N页  (逐页，抖动)
Zerofault:  insmod[预分配全部物理页] → mmap[一次建PTE] → 零缺页访问
```

## 架构总览

```mermaid
flowchart TB
    subgraph User["用户态"]
        APP[测试程序 / HFT策略]
        DEV["/dev/ptemap"]
    end

    subgraph Kernel["内核态 (ptemap.ko)"]
        CDEV[ptemap_cdev.c<br/>cdev + mmap 回调]
        PTE[ptemap_pte.c<br/>v1.1: apply_to_page_range<br/>+ set_pte_at 直写]
        CORE[ptemap_core.c<br/>物理页分配/释放]
        MAIN[ptemap_main.c<br/>模块生命周期]
        DBGFS[ptemap_debugfs.c<br/>调试接口]
        PARAM[module_param<br/>phys_pages / target_pid<br/>use_direct_pte]
    end

    subgraph Debug["调试接口"]
        STATUS["/sys/kernel/debug/ptemap/status"]
        MAPS["/sys/kernel/debug/ptemap/mappings"]
        STATS["/sys/kernel/debug/ptemap/stats"]
    end

    subgraph HW["物理内存"]
        PAGES["预分配物理页<br/>alloc_page(GFP_KERNEL)<br/>+ get_page() pin住"]
    end

    APP -->|open/mmap| DEV --> CDEV
    CDEV -->|"v1.0: vm_insert_page"| PAGES
    CDEV -->|"v1.1: use_direct_pte=1"| PTE
    PTE -->|"apply_to_page_range<br/>逐页 pfn_pte + set_pte_at"| PAGES
    MAIN --> PARAM
    MAIN --> CORE
    MAIN --> CDEV
    MAIN --> DBGFS
    DBGFS --> STATUS
    DBGFS --> MAPS
    DBGFS --> STATS
```

## mmap 流程

### v1.1 PTE 直写（默认推荐）

```mermaid
sequenceDiagram
    participant U as 用户进程
    participant VFS as VFS 层
    participant CDEV as ptemap_cdev.c
    participant PTE as ptemap_pte.c
    participant MM as 物理页

    Note over MM: insmod 时已预分配<br/>alloc_page() + get_page()

    U->>VFS: open("/dev/ptemap")
    VFS->>CDEV: ptemap_open()
    CDEV-->>U: fd

    U->>VFS: mmap(size)
    VFS->>CDEV: ptemap_mmap(vma)
    CDEV->>CDEV: use_direct_pte=1 →<br/>ptemap_mmap_direct(vma)
    CDEV->>PTE: apply_to_page_range(mm, start, size, callback)
    Note over PTE: 内核分配 PGD→PUD→PMD→PTE 中间层级
    loop 每页
        PTE->>PTE: pfn_pte(pfn, pgprot) +<br/>set_pte_at(mm, addr, pte, new_pte)
        Note over PTE: 直接构造并写入 PTE<br/>零 rmap 开销
    end

    PTE-->>U: 虚拟地址 ptr
    Note over U: 访问 ptr[i] → MMU 页表命中 → 零缺页！
    U->>MM: load/store 直接命中
```

### v1.0 vm_insert_page（兼容保留）

```mermaid
sequenceDiagram
    participant U as 用户进程
    participant CDEV as ptemap_cdev.c
    participant MM as 物理页

    Note over MM: insmod 时已预分配

    U->>CDEV: mmap(size)
    loop 每页
        CDEV->>MM: vm_insert_page(vma, vaddr, page[i])
        Note over CDEV,MM: 内核标准 API<br/>有 rmap 开销 ~60 cycles/页
    end
    CDEV-->>U: 虚拟地址 ptr
    U->>MM: 访问 → 零缺页
```

## 源码结构

| 文件 | 行数 | 职责 |
|------|------|------|
| `ptemap_main.c` | 150 | 模块生命周期：init 参数校验 → 找目标进程 → 分配页 → 注册 cdev → 创建 debugfs；exit 逆序清理 |
| `ptemap_core.c` | 66 | 物理页管理：`kcalloc` 分配 page 指针数组 → `alloc_page(GFP_KERNEL)` + `get_page()` pin 页 → `put_page()` + `kfree` 释放 |
| `ptemap_cdev.c` | 157 | `/dev/ptemap` 字符设备：open（访问控制）、mmap（v1.0 `vm_insert_page` / v1.1 `apply_to_page_range`）、ioctl（v1.2 预留） |
| `ptemap_pte.c` | 113 | v1.1 PTE 直写：`apply_to_page_range` 遍历页表 → 逐页 `pfn_pte` + `set_pte_at` 直接构造并写入 PTE |
| `ptemap_debugfs.c` | 135 | 3 个 debugfs 文件：`status`（模块状态）、`mappings`（每页 VA/PFN 表）、`stats`（内存统计） |
| `ptemap_core.h` | 65 | 全局状态结构体 + API 声明 + 常量定义 |
| `test_ptemap.c` | 127 | 用户态测试：open → mmap → 写 pattern → 读验证 → 跨页边界测试 |
| **合计** | **813** | |

## 模块参数

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `phys_pages` | int | 256 | 预分配物理页数量（256 = 1MB） |
| `target_pid` | int | 0 | 允许访问的进程 PID（0 = 任意进程） |
| `use_direct_pte` | int | 0 | PTE 直写模式：0=vm_insert_page(v1.0) 1=apply_to_page_range+set_pte_at(v1.1) |

```sh
# v1.0 默认路径（vm_insert_page）
insmod ptemap.ko phys_pages=512 target_pid=0

# v1.1 PTE 直写路径（apply_to_page_range，零 rmap）
insmod ptemap.ko phys_pages=512 use_direct_pte=1
```

## 编译 & 测试

**前置条件**：Linux 6.16.2 内核编译环境，`KERNELDIR` 指向 pre-built kernel tree。

```sh
# ===== 编译 =====
cd ptemap
make KERNELDIR=/path/to/kernel/build

# ===== 加载 =====
insmod ptemap.ko phys_pages=256

# ===== 调试查看 =====
cat /sys/kernel/debug/ptemap/status
#  state:     LIVE
#  version:   1.0.0
#  pages:     256 (total)
#  size:      1048576 bytes (1 MB)
#  target_pid: 0
#  vaddr:     0x7f...-0x7f...
#  tlb_flush: 0

cat /sys/kernel/debug/ptemap/mappings
#  idx   vaddr              pfn                 size
#  ----- ------------------ ------------------ ----------
#  0     0x00007f...        0x0000000000123abc 4KB
#  ...

# ===== 运行测试（v1.1 直写路径）=====
insmod ptemap.ko phys_pages=256 use_direct_pte=1
./test_ptemap 64
#  === ptemap test ===
#  device:    /dev/ptemap
#  nr_pages:  64 (256 KB)
#  [1] open  OK (fd=3)
#  ptemap: mmap DIRECT OK: vaddr=0x7f...-0x7f... pages=64 pid=61
#  [2] mmap OK (vaddr=0x7f..., size=256 KB)
#  [3] writing pattern...
#  [3] write OK (64 pages)
#  [4] verifying...
#  [4] verify OK (0 errors)
#  [5] cross-page boundary test...
#  [5] boundary OK
#  === result: PASS ===

# ===== 验证零缺页 =====
perf stat -e page-faults ./test_ptemap 256
#  page-faults: 0  ← 关键指标

# ===== 卸载 =====
rmmod ptemap
```

## 关键设计决策

```mermaid
flowchart LR
    A[物理页何时分配？] -->|"insmod 时 (选择)"| B
    A -->|mmap 时| C

    B --> B1[✅ 访问路径零缺页]
    B --> B2[❌ 模块加载稍慢]
    B --> B3[❌ 占用物理内存]

    C --> C1[❌ 首次访问触发缺页]
    C --> C2[✅ 按需分配]

    style B fill:#c8e6c9
    style B1 fill:#a5d6a7
```

| 决策 | 选择 | 理由 |
|------|------|------|
| 物理页分配时机 | **insmod 时** | 避免 mmap 后首次访问的缺页延迟抖动 |
| 页映射方式(v1.0) | **vm_insert_page** | 内核标准 API，不用手动走 page table walk |
| 页映射方式(v1.1) | **apply_to_page_range + set_pte_at** | 直写 PTE，零 rmap 开销，逐页独立 pgprot_t 控制 |
| Cache 策略 | **writecombine** | UC 太慢，WB 有 cache 一致性开销，WC 折中 |
| 调试接口 | **debugfs** | 无 API 兼容性承诺，适合开发期快速迭代 |

## 版本状态

| 版本 | 状态 | 内容 |
|------|------|------|
| v1.0 | 完成 | 模块生命周期、物理页预分配/pin、cdev + mmap (`vm_insert_page`)、debugfs、用户态测试 |
| v1.1 | 完成 | PTE 直写 (`apply_to_page_range` + `set_pte_at` + `pfn_pte`)、双路径可切换 (`use_direct_pte`)、`remap_pfn_range` 已移除 |

## TODO (v1.2+)

- [x] **PTE 直写** — `apply_to_page_range` + `pfn_pte` + `set_pte_at` 直接构造 PTE（v1.1 已完成并测试通过）
- [x] **双路径保留** — v1.0 `vm_insert_page` 与 v1.1 直写均可通过 `use_direct_pte` 参数切换
- [ ] **ioctl 查询接口** — 查询映射信息（每页 PFN/VA/cache 策略）、运行时 TLB flush 控制
- [ ] **NUMA 感知** — `alloc_page_node()` 按 NUMA node 分配物理页
- [ ] **Huge page 支持** — 2MB/1GB 大页减少 TLB miss
- [ ] **逐页 cache 策略** — 利用 `apply_to_page_range` 回调的逐页 pgprot 参数，不同页可用不同 PAT（WC/WB/UC）
- [ ] **性能基准报告** — mmap 延迟对比（v1.0 vs v1.1）、读写吞吐、TLB miss rate

## License

GPL-2.0
