# STM32H7 SD Card Raw Data → FATFS File Generator

![STM32](https://img.shields.io/badge/MCU-STM32H755ZI--Q-blue)
![SDMMC](https://img.shields.io/badge/SDMMC-4bit-green)
![FATFS](https://img.shields.io/badge/FATFS-enabled-orange)
![DMA](https://img.shields.io/badge/DMA-supported-red)

---

# 專案目的

本專案目標：

> 將 STM32H7 以 Raw Sector 方式高速寫入 SD 卡的資料，
> 重新讀出後，
> 轉換成 Windows 可辨識的 FATFS 檔案。

此架構主要應用於：

* 高速資料記錄器（Data Logger）
* 長時間連續記錄
* 工業資料擷取
* AI Sensor Recorder
* 高速DAQ
* FTP Log Server
* 黑盒子資料保存系統

---

# 系統設計概念

本系統分成兩個階段：

---

# 第一階段：高速 Raw Data 寫入 SD 卡

目標：

> 不透過 FATFS，
> 直接以 Sector 方式高速寫入 SD 卡。

原因：

FATFS 在每次寫入時：

* 需要 FAT table 更新
* 需要 directory 更新
* 需要 cluster allocation
* 容易產生 latency
* 寫入時間不穩定

因此：

本專案改用：

```text
HAL_SD_WriteBlocks_DMA()
```

直接寫入 sector。

---

# Raw Data 寫入格式

每一筆資料固定 64 Bytes。

資料結構：

```c
typedef struct
{
    uint8_t payload[54];

    uint16_t tail;

    uint32_t record_id;

    uint32_t file_id;

} log_record_t;
```

---

# 每筆資料內容

| 欄位        | 大小       | 說明          |
| --------- | -------- | ----------- |
| payload   | 54 Bytes | 真正資料內容      |
| tail      | 2 Bytes  | 尾端標記 0xAA55 |
| record_id | 4 Bytes  | 全域流水號       |
| file_id   | 4 Bytes  | 所屬檔案編號      |

總大小：

```text
54 + 2 + 4 + 4 = 64 Bytes
```

---

# Tail Marker 設計

```c
record->tail = 0xAA55;
```

作用：

* 驗證資料是否合法
* 判斷 sector 是否有效
* 防止讀取空白區域
* 用於掃描 Raw Data

---

# Raw Data Buffer 設計

Buffer 大小：

```c
#define BUFFER_SIZE_BYTES (32 * 1024)
```

即：

```text
32KB
```

---

# 每次 DMA 寫入大小

```text
32KB
```

換算：

```text
32KB / 512 = 64 sectors
```

因此：

```c
#define SD_BLOCK_COUNT 64
```

---

# 每次寫入包含多少筆資料

每筆資料：

```text
64 Bytes
```

每次 DMA：

```text
32768 Bytes
```

因此：

```text
32768 / 64 = 512 records
```

即：

```c
#define RECORD_COUNT 512
```

---

# 第二階段：Raw Data 生成 FATFS 檔案

本專案第二階段：

> 將 SD 卡內 Raw Data 重新讀出，
> 並生成 Windows 可見檔案。

---

# 生成 FATFS 檔案流程

流程如下：

```text
SD Card Raw Sector
        ↓
HAL_SD_ReadBlocks()
        ↓
read_buffer[]
        ↓
驗證 0xAA55
        ↓
提取 payload
        ↓
f_write()
        ↓
LOGxxxx.TXT
```

---

# 檔案生成方式

每次讀取：

```text
32KB
```

每次：

```text
512 records
```

每個 record：

```text
54 Bytes payload
```

因此：

每次真正寫入檔案：

```text
512 × 54 = 27648 Bytes
```

即：

```text
27KB
```

---

# CHUNKS_PER_FILE 概念

此參數：

```c
#define CHUNKS_PER_FILE 64
```

代表：

```text
每個檔案由 64 個 chunk 組成
```

每個 chunk：

```text
32KB Raw Data
```

因此：

```text
64 × 32KB = 2MB Raw Data
```

但：

實際寫入 FATFS 的只有 payload。

因此：

最終檔案大小約：

```text
64 × 27KB ≈ 1.7MB
```

---

# 為何選擇 1.7MB 檔案

測試後發現：

## 64MB 檔案缺點

* Editor 開啟很慢
* FTP 傳輸較久
* Windows 操作不方便
* 不利 log 管理

---

## 1~2MB 檔案優點

* 容易開啟
* FTP 傳輸快
* Windows 操作順暢
* 容易分段保存
* 更適合 log 系統

因此：

目前建議：

```c
#define CHUNKS_PER_FILE 64
```

---

# 如何定位每個檔案

起始位置：

```c
#define START_SECTOR 0x1000
```

每個 chunk：

```text
64 sectors
```

每個檔案：

```text
64 chunks
```

因此：

每個檔案使用：

```text
64 × 64 = 4096 sectors
```

---

# 檔案位置公式

```c
current_sector =
    START_SECTOR +
    (file_id * CHUNKS_PER_FILE * SD_BLOCK_COUNT);
```

例如：

| file_id | 起始 sector |
| ------- | --------- |
| 0       | 0x1000    |
| 1       | 0x2000    |
| 2       | 0x3000    |
| 3       | 0x4000    |

---

# GenerateOneFile() 工作原理

函式：

```c
GenerateOneFile(file_id);
```

作用：

```text
讀取一個 Raw Data Segment
並生成一個 FATFS 檔案
```

流程：

```text
計算起始 sector
        ↓
建立 LOGxxxx.TXT
        ↓
循環讀取 chunk
        ↓
檢查 0xAA55
        ↓
提取 payload
        ↓
寫入 TXT 檔案
        ↓
完成檔案生成
```

---

# 資料合法性驗證

每次讀取後：

```c
if(read_buffer[0].tail != 0xAA55)
{
    break;
}
```

作用：

* 防止讀取空白區域
* 防止讀取未初始化資料
* 自動停止生成
* 避免 SD 卡損壞資料

---

# Pulse Output 設計

PA6 作為 pulse output。

用途：

* 示波器觀察
* 邏輯分析儀觀察
* 驗證檔案生成完成
* 觀察 chunk processing

---

# 每生成一個檔案

PA6：

```text
HIGH → LOW
```

形成一個 pulse。

---

# Cache 與 MPU 設計

STM32H7 使用 DCache。

DMA 與 Cache 可能發生 coherency 問題。

因此：

本專案：

* Buffer 放在 RAM_D1
* 32-byte aligned
* MPU region 設為 non-cacheable

---

# Buffer 宣告方式

```c
__attribute__((section(".RAM_D1")))
__attribute__((aligned(32)))
```

---

# DCache 處理

DMA Read 後：

```c
SCB_InvalidateDCache_by_Addr()
```

DMA Write 前：

```c
SCB_CleanDCache_by_Addr()
```

---

# FATFS 檔案命名規則

```text
LOG0000.TXT
LOG0001.TXT
LOG0002.TXT
```

生成方式：

```c
sprintf(filename,
        "LOG%04lu.TXT",
        file_id);
```

---

# 檔案內容格式

目前：

僅輸出 payload。

不輸出：

* tail
* record_id
* file_id

因此：

Windows 看到：

```text
ABCDEFGHIJKLMNOPQRSTUVWXYZ...
```

純文字 log。

---

# 系統優點

本架構具有：

## 高速寫入

Raw sector write：

* 不經 FATFS
* latency 低
* throughput 高

---

## 穩定資料保存

使用：

```text
0xAA55
```

驗證資料合法性。

---

## 後處理彈性

後續：

* 可重新生成 FATFS
* 可 FTP 傳輸
* 可建立 log server
* 可建立 indexing system

---

# 未來可擴充功能

後續可加入：

* FTP Server
* Ethernet Transfer
* File Compression
* Timestamp
* CRC Validation
* Circular Buffer
* Automatic Segment Scan
* File Recovery
* Real-time Streaming

---

# 開發平台

* MCU：
  STM32H755ZIT6U

* Board：
  NUCLEO-H755ZI-Q

* Interface：
  SDMMC1 4-bit

* File System：
  FATFS

* IDE：
  STM32CubeIDE

---

# 專案完成成果

目前已成功：

* SDMMC 4-bit 高速寫入
* DMA Raw Write
* Raw Sector Read
* FATFS 檔案生成
* Payload 提取
* Windows 可讀檔案
* Pulse Debug
* Segment 分割

---
