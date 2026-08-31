# 交接筆記（2026-09-01,凌晨第二次更新:A 已更新到 v10,拉取路徑修復並雙 slot 驗證）

給接手的 session。專案背景看 [README.md](README.md),這裡只寫**當下狀態**和
**踩過的坑**。

**兩片板子的遠端更新問題都已解決,且根因不同:**

- **B**:flash 在 QIO 讀取下跨 32-byte 邊界繞回 → 改 `dio`(見下方 B 章節)
- **A**(本次):v6 的 PullOta 在下載前同步抹除整個 6.4MB slot,把 IDLE0
  餓死超過預設 task WDT 的 5 秒 → abort 重開,**v6 拉什麼都必死**。修法:
  照 watchdog 的樣板把 task WDT 重設為 120 秒(v8)。詳見「A 的更新」章節

## 系統現況

| | A | B |
|---|---|---|
| DEVICE_ID | `pond-site` | `watchdog` |
| 板子序號 | CH343 `5CBC033443` | CH343 `5CBC033428` |
| IP | 192.168.0.37 | 192.168.0.38 |
| 韌體版本 | **10**(拉取式 OTA 裝入) | **38**(拉取式 OTA 裝入) |
| 拉取式 OTA | ✅ **已修復**(120s WDT;v9/v10 連續兩次、兩個 slot 皆驗證) | ✅ 已修復(dio;連續三次、兩個 slot 皆驗證) |
| flash 讀取模式 | qio(**本次已實測健康**,見 qio 體檢) | **dio(必要,勿改回 qio)** |
| task WDT | **120s,loop 有訂閱**(v8 起;v6 以前只有預設 5s/IDLE0) | 120s,loop 有訂閱 |
| PSRAM | ❌ 初始化失敗(本次 UART 實測,見下) | ❌ 初始化失敗(與 OTA 無關) |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

A 在台北家中運作、接在 PC 的 CH343 孔(COM3)上,資料持續進 RTDB。
**B 被使用者暫時斷電**(2026-09-01 凌晨)——它沒壞,是刻意斷的,重新上電
即恢復;RTDB 的 watchdog 心跳停在最後一筆屬預期。

## 已解決:B 的 `ESP_ERR_OTA_VALIDATE_FAILED`(誤判七次之後,靠量測收斂)

### 根因(每一句都有板上實測支撐)

**B 這片的 flash 晶片在 QIO fast-read 下,單筆 SPI1 交易的資料在 32-byte
邊界處繞回。** 用 v31 起內建的 `/rawprobe` 量到的精確幾何:

- 大塊讀取:每 64-byte 交易只有前 32 bytes 真實,後 32 bytes 是前半的複製
  (1KB 取樣逐段比對映像來源位址,規律無一例外)
- 小讀取(≤32B)只要跨過 32-byte 對齊邊界,尾端繞回區塊開頭
  (`/rawprobe?part=app1&off=0x1c&n=8` → `3c9b0200` + **`e905024f`**=映像開頭)
- 換成 **dio** 後,同樣的探測全部正確;`raw == mmap == release 檔 SHA256`
  逐 byte 一致(977152 bytes),`esp_image_verify` 在原本一直被拒的 slot 上
  回 **ESP_OK**

這解釋了當初所有矛盾:

| 舊謎團 | 解釋 |
|---|---|
| dump 逐 byte 正確、驗證卻失敗 | esptool stub 用別的讀取指令(不走 QIO)→ dump 不受影響;`esp_image_verify` 在 app 端用 SPI1 任意偏移讀段落表頭 → 中招 |
| 板子明明開得起來 | cache(SPI0)只做 32B 對齊的 line fill,永不跨界 → 免疫 |
| mmap 讀回完全正確 | 同上,mmap 走 cache |
| NVS 從沒壞過 | NVS 條目 32-byte 對齊 → 免疫 |
| 錯誤訊息時而 `Image hash failed` 時而 `invalid segment length` | 繞回讀到的垃圾隨映像版面而變,同一份映像則完全確定性 |
| A 同型晶片(`c2 2018`)沒事 | 單顆晶片(或該板 quad 線路)個體差異 |

### 修了什麼

- [watchdog/platformio.ini](watchdog/platformio.ini):`board_build.flash_mode = dio`
  (**這就是修復本體**,註解裡有完整量測紀錄)
- [shared/PullOta/flash_probe.h](shared/PullOta/flash_probe.h):三方 SHA 探針
  (下載串流 / raw 讀回 / mmap 讀回)+ 逐 chunk 比對;啟用失敗時自動執行,
  摘要寫進 fwlog,`/verify` 可隨時遠端重測
- `/rawprobe`(在 [watchdog/src/main.cpp](watchdog/src/main.cpp)):指定分區/
  偏移/長度/讀取粒度,回傳 raw 與 mmap 的原始 bytes —— 未來新板 qualification
  用它
- 狀態頁多了兩行 `raw sample ctor/setup`:開機時各做一次跨界讀取,一眼看出
  這片板子的 raw 讀是否健康

### 誠實註記(不要超出量測下結論)

08-31 深夜 v12→13→14→15 曾在 **qio 下連續成功三次**,與「qio 必壞」不符。
今晚(09-01)qio 在多次開機、兩種 PSRAM 變體下都穩定重現錯讀,dio 全綠。
最合理的解讀是**邊際性缺陷**(訊號/時序隨供電溫度等條件浮動),這正是選
dio(時序裕度大得多)而非嘗試修復 qio 的理由。此點無法遠端進一步驗證,
留給未來需要時再查。**別把它當成 qio 可以改回去的理由。**

另外:PSRAM 在 B 上兩種變體(qio_qspi / qio_opi)都初始化失敗
(`ESP.getPsramSize()=0`),與 OTA 無關、目前也不需要它,**未解決但不阻塞**。

### 已封案的舊線索

- **otadata「唯一差異」**:B 的 `seq=1+空白` 就是 PlatformIO 每次 USB 燒錄
  寫入的 `boot_app0.bin` 原樣(已逐 byte 對照);v12 當年就是從這個狀態
  OTA 成功的,所以它與失敗無關。「抹除 otadata」的候選動作作廢。
- 七次誤判清單與過程,留在 git 歷史(`a15f152` 之前的 HANDOFF 版本)。

## 硬體注意事項(09-01 凌晨:A 的部分已全部實測)

- **同款板、兩種體質**(都已實測,別互相套用):
  - **B 的 CH343 支援 esptool 自動進下載模式**;開它的埠(COM6)**會
    reset 板子**(DTR/RTS 有作用)——部署後遠端 session 別隨手開埠
  - **A 的 DTR/RTS 完全沒接到 EN/IO0**(esptool 30 秒換手 + EN 壓 3 秒,
    0 掉包實測):USB 燒錄**必須手動 BOOT+RST**,但反過來**開 A 的埠
    (本 PC 上是 COM3)百分之百安全**,可長掛序列監聽——本次抓 WDT
    crash 就是靠這個
- CH343 埠 = UART0,載送 ESP-IDF 錯誤 log(`task_wdt`/`esp_image`/
  `psram` 那些 E 行)與 ROM 開機橫幅(含 reset 原因);`Serial.print`
  (HWCDC)走原生 USB 孔。
- A 的原生 USB 孔(左)不能傳資料(推測焊接影響 GPIO19/20),用 CH343 孔。
- 細的 USB 線會讓 A 連不上 WiFi(電壓降)。本次整晚接 PC 的 USB 孔
  供電+資料,WiFi -64dBm 穩定、兩次 OTA 無異常——手上這條線沒問題。
- **之後採購新板**:先燒含 `/rawprobe` 的韌體,用跨界讀取
  (`/rawprobe?part=app1&off=0x1c&n=8`)確認 raw 讀健康,再決定 qio/dio。

## 已完成:A 的更新(2026-09-01 凌晨,使用者睡前授權的自主任務)

原計畫是 v6→v7 一次拉取。實際走了 v7→v8→v9→v10 四個版號,因為途中
量到一個計畫沒預料的根因。全程無人手介入(使用者睡前按過一次 BOOT+RST
做門檻測試、一次 RST 收尾,之後全遠端)。

### 門檻測試結果(每句都有實測)

- **A 沒有自動救援**:開埠、esptool 標準 reset 脈衝、RTS(EN)壓低 3 秒,
  A 全程 0 掉包、序列 0 bytes → **這片板的 DTR/RTS 沒接到 EN/IO0**
  (板上電路問題,與 USB 線無關;platformio.ini 的舊註解其實早寫了)。
  esptool 自動連線也因此失敗
- **手動 BOOT+RST 後 esptool 全功能**:chip_id / flash_id 正常
  (ESP32-S3 QFN56 rev v0.2、MAC `28:84:85:5c:95:f0`、flash `c2 2018`
  16MB、eFuse quad、內嵌 PSRAM 8MB AP_3v3)→ 救援=需人手起頭,之後全軟體
- **副產品:A 的 COM3 開埠完全不干擾板子**(正因 DTR/RTS 沒接)——
  可以放心隨時掛序列監聽,B 的「開埠會 reset」警告**不適用於 A**
- 依門檻邏輯走了保守路線:整晚只用拉取(rollback 保底)+ espota,
  沒碰 USB 燒錄、沒動 platformio 設定

### 根因:v6 拉不動 v7,是結構性必死(UART 實測三連發)

觸發 `/fwcheck` 後 curl 被 reset、A 重開回 v6、fwlog 無紀錄。掛 COM3
監聽重試,抓到三次一模一樣的死法(~73 秒一輪,A 每次開機 60 秒後自動
重試,等於**無限 crash 循環**;當下先把 RTDB 指回 v6 止血):

```
E task_wdt: Task watchdog got triggered ... - IDLE0 (CPU 0)
Tasks currently running: CPU 0: ipc0 / CPU 1: IDLE1 → Aborting.
```

機制(源碼對照確認):`5a94ba1`(v6)在下載前加了
`esp_ota_erase_last_boot_app_partition()`——**單一同步呼叫抹除整個
6.4MB slot**,期間 flash 操作經 ipc0 獨占 CPU0,IDLE0 遠超過預設
task WDT 的 5 秒 → abort。時間軸吻合(觸發後 ~9 秒死,2×TLS 約 4-5 秒
+ 抹除 5 秒)。這同時解釋:

| 謎團 | 解釋 |
|---|---|
| 昨晚 v5→v6 為何成功 | v5 的 PullOta **沒有**這個抹除呼叫 |
| B 為何連拉三次都活著 | watchdog 韌體把同一個 task WDT 重設為 120 秒 |
| fwlog 為何沒紀錄 | WDT 在 `pending` 寫入與任何記錄之前就 abort |
| 不是斷線卡死(記憶中 B 的舊病)| 這是 abort 重開,不是 writeStream 不返回 |

### 修法與驗證

- **v8**(commit `7037b27`)= v7 的全部內容 + watchdog 同款 task WDT
  (120s、loop 訂閱、espota onProgress 餵狗)。**經 espota 送上**
  (`pio run -e ota -t upload --upload-port 192.168.0.37`,espota 逐
  sector 懶抹除、不會餓死 IDLE0,也是專案文件寫的日常路徑)——v6 在板上
  時這是唯一可用的遠端路徑
- **v9**(`c0a9085`)驗證拉取:release.sh + `/fwcheck` → **55 秒完成**
  check+全 slot 抹除+下載+安裝+重開,marked valid
- **v10**(`ac3bacf`)第二次拉取落在另一個 slot → v9→app0、v10→app1,
  **連續兩次、兩個 slot 皆實測通過**;UART 全程只看到兩次 `rst:0xc`
  (正常軟體重啟),零 WDT 錯誤
- v9 還帶一個 shared/PullOta 修正:`pending` 標記在成功路徑從不清除,
  導致 A 的 v8 首次開機把昨晚 v5→v6 留下的 `pending=6` 誤判成
  「v6 rolled back」寫了**一筆假的 fwlog**(ts 1788206238,已在 RTDB
  加註 note)。現在 pending 開機吻合即消耗。NVS 裡殘留 `bad=6` 無害
  (版本只會往上)

### A 的 qio 體檢:健康,維持 qio(結案)

用 B 定案時的同一套探測,v8 起可隨時重測:

- `raw sample ctor/setup`:兩行 `ESP_OK` 且內容一致、等於真實 bytes
- `/rawprobe?part=app1&off=0x1c&n=8`:raw == mmap,**無** B 的繞回簽名
  (`e905024f` 沒有出現在尾端);app0 同窗口也乾淨
- 128 bytes 以 16B/64B 兩種粒度讀:與 mmap 逐 byte 一致
- `/verify` 全映像:raw SHA == mmap SHA(970992 與 982928 bytes 各一次,
  換 slot 後各測過)、diff chunks 0、`esp_image_verify: ESP_OK`

「B 的繞回是單板個體差異」的推論成立。A 的 dio 跟進**不需要**。

### 版號註記

- `pond-site-v7`:GitHub release 存在但**從未裝上任何板子**(v6 拉不動它,
  修好後直接跳過)。留著無害,RTDB 已指向 v10
- `pond-site-v8`:**沒有** GitHub release / tag(espota 直送,對應 commit
  `7037b27`)。tag 序列 v7→v9→v10 中間缺 8 是刻意的
- 教訓:**release.sh 前必先 push**——`gh release create` 的 tag 建在
  GitHub 遠端 main 上,v6 當年沒先 push,所以 `pond-site-v6` 這個 tag
  指錯 commit(指向 `a2965f7`,實際 v6 源碼在 `5a94ba1`)。本次 v7/v9/v10
  的 tag 都已核對正確

### 其他本次確立的事實

- **A 的 PSRAM 初始化也失敗**(UART 每次開機:`psram: PSRAM ID read
  error: 0x00ffffff`,qio_opi 變體)。與 B 相同、與 OTA 無關、目前不需要
  它,未解決但不阻塞。eFuse 明明說內嵌 8MB(AP_3v3)——之後有閒再查線路
  模式(`psram_type = opi` 可能不對)
- `.claude/settings.local.json`(已 gitignore)是本機 Claude Code 權限
  allowlist,使用者授權建立,供夜間自主作業用

## 部署前必須完成(東西還在台北時才能做)

- [x] ~~修好 B 的 OTA~~ **完成**(v36/37/38 三連拉取實證)
- [ ] **WiFi 後備連線**——清單裡最可能真實發生的。連不上主 WiFi 超過 N 分鐘
      就開 AP,用手機連上去改設定。能救掉整類「網路變動」情境
- [ ] **破壞性測試(只有現在能做)**:
  - [ ] 推一版故意在 `setup()` crash 的 → 驗證 rollback 能自動退回
  - [ ] 推一版故意連不上 WiFi 的 → 驗證能否自救
  - [ ] OTA 進行到一半拔電 → 驗證半寫入狀態能恢復
  - [ ] 改掉 WiFi 密碼 → 驗證後備機制
- [x] ~~A 更新到新版 `shared/PullOta`~~ **完成**(v10;v9/v10 雙 slot
      拉取實證,120s WDT 修復拉取路徑,qio 體檢通過維持 qio——見上方
      「A 的更新」章節)
- [x] ~~A 的板子若要重燒,先試 esptool 自動 reset~~ **已測:不可行**,
      A 必須手動 BOOT+RST(DTR/RTS 未接,量測見上)

## 使用者的偏好與已定決策

- **回覆用繁體中文。** 曾經誤用簡體字(断/静),已修正為 斷/靜
- 使用者熟 JS/Node,韌體交給 AI 實作,只在關鍵時刻 review
- 不要一直提醒等待中的事,超時沒關係,之後再看
- 判斷過的事不要重問:RTDB(非 Firestore)、公開 repo、憑證外洩可接受、
  不做溫度門檻告警、只做心跳、濕度已移除
- 之後會大量採購 ESP32,所以**根因比繞過更有價值**(本次成果:根因已知,
  且留下了逐板 qualification 的工具)
- 每天人工檢查兩次,所以 B 掛掉會被發現,不必做雲端監控

## 其他待辦(部署之後才做)

- [ ] 前端圖表(使用者主場):讀 `history/pond-site/<YYYY-MM>`,`orderByKey` 取範圍
- [ ] C 節點:燒 watchdog 韌體、改 `DEVICE_ID`,B/C 自動互相監控
- [ ] 溶氧、pH:可掛同一條 RS485(改站號),或依地理位置分組
- [ ] RTDB 規則全開放(Spark 方案最壞是服務中斷非計費)。**升 Blaze 前必須收緊**
