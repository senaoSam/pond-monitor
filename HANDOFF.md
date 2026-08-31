# 交接筆記（2026-09-01,凌晨更新:B 的 OTA 已修復）

給接手的 session。專案背景看 [README.md](README.md),這裡只寫**當下狀態**和
**踩過的坑**。

**B 無法遠端更新的問題已解決**——根因是這片板子的 flash 在 QIO 讀取指令下
會把跨 32-byte 邊界的 SPI1 讀取「繞回」邊界(量測細節見下)。修法:改用
`dio`。已用連續三次拉取更新(v36→app1、v37→app0、v38→app1)實測驗證。

## 系統現況

| | A | B |
|---|---|---|
| DEVICE_ID | `pond-site` | `watchdog` |
| 板子序號 | CH343 `5CBC033443` | CH343 `5CBC033428` |
| IP | 192.168.0.37 | 192.168.0.38 |
| 韌體版本 | **6** | **38**(拉取式 OTA 裝入) |
| 拉取式 OTA | ✅ 正常 | ✅ **已修復**(dio;連續三次、兩個 slot 皆驗證) |
| flash 讀取模式 | qio(此片正常) | **dio(必要,勿改回 qio)** |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |

兩片都在台北家中運作,資料持續進 RTDB。

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

## 硬體注意事項(本次有更正)

- **B 的 CH343 支援 esptool 自動進下載模式**——「兩片都需手動 BOOT+RST」
  是錯的(A 未重測)。今晚 B 的所有 USB 燒錄都是全自動完成的。
- **開啟 B 的 CH343 序列埠(COM6)會直接 reset 板子**(DTR/RTS 有作用)。
  部署後遠端 session 別隨手開埠;08-31 fwlog 裡每次失敗後幾分鐘的神祕重開機
  就是當時開序列埠監看造成的。
- COM6(CH343)= UART0,載送 ESP-IDF 錯誤 log(`esp_image` 那些 E 行);
  `Serial.print`(HWCDC)走原生 USB 孔(COM5)。
- A 的原生 USB 孔(左)不能傳資料(推測焊接影響 GPIO19/20),用 CH343 孔。
- 細的 USB 線會讓 A 連不上 WiFi(電壓降)。
- **之後採購新板**:先燒含 `/rawprobe` 的韌體,用跨界讀取
  (`/rawprobe?part=app1&off=0x1c&n=8`)確認 raw 讀健康,再決定 qio/dio。

## 下一步:A 的更新(2026-09-01 深夜,使用者睡前授權的自主任務)

**現場狀態:** 使用者已將 **B 暫時斷電**(勿連 192.168.0.38、勿因它離線而
除錯;RTDB 的 watchdog 心跳停在最後一筆屬預期)。A 已用可傳資料的線接到
PC 的 CH343 孔(序號 `5CBC033443`,埠號自己查)。使用者在睡覺,**遇到需要
人手的情況就停手、把狀態寫進這份筆記**,不要等待也不要冒進。

**目標:** A 從 v6 更新到 v7(新版 `shared/PullOta` + 診斷工具),並順便
完成 A 的 qio 體檢。B 的部分已全部完成,**不要動 watchdog/ 的任何設定**。

**步驟與授權門檻(依序):**

1. 讀 sensor-node v6 的原始碼(git 裡 `ac32e46` 前後):確認有無 task WDT、
   有無 `/fwcheck` 端點——這決定卡死時的自癒能力與觸發更新的方式
2. **門檻測試(先驗救援能力,再做有風險的事):** 開一次 A 的 COM 埠,
   預期看到 reset 開機橫幅(證明 DTR/RTS 有通);再跑 `esptool chip_id`
   (證明能自動進下載模式)。每次會讓 A 斷線 10–20 秒,無妨(B 已斷電,
   不會有人發告警)。**兩者都通 = 有和 B 同級的全自動救援,後續可放手做;
   沒通 = 只做第 4–5 步(rollback 保底的拉取),其餘停手記錄**
3. sensor-node/src/main.cpp 加上與 watchdog 相同的診斷(開機 raw sample
   兩行、`/verify`、`/rawprobe`;都在 shared 與 watchdog 的程式裡有現成
   範本),版號 bump 到 v7,commit
4. `tools/release.sh pond-site` 發布 v7;觸發:有 `/fwcheck` 就 curl,
   沒有就開埠 reset 一次(重開機 60 秒後會自己拉),再不行等 30 分輪詢
5. 驗證:v7 起來、`marked valid`、fwlog 有 `installed`、狀態頁
   `raw sample ctor/setup` 兩行與真實內容一致(= A 的 qio 體檢)
6. 若 raw sample 顯示繞回(A 也有 B 的缺陷):照 B 的做法改 dio 發 v8,
   同樣要連續多次拉取驗證——**先量測後結論,成功次數不足不寫結案**
7. 全程更新這份筆記;記憶裡的紀律照舊:不接受沒有實測支撐的假說

## 部署前必須完成(東西還在台北時才能做)

- [x] ~~修好 B 的 OTA~~ **完成**(v36/37/38 三連拉取實證)
- [ ] **WiFi 後備連線**——清單裡最可能真實發生的。連不上主 WiFi 超過 N 分鐘
      就開 AP,用手機連上去改設定。能救掉整類「網路變動」情境
- [ ] **破壞性測試(只有現在能做)**:
  - [ ] 推一版故意在 `setup()` crash 的 → 驗證 rollback 能自動退回
  - [ ] 推一版故意連不上 WiFi 的 → 驗證能否自救
  - [ ] OTA 進行到一半拔電 → 驗證半寫入狀態能恢復
  - [ ] 改掉 WiFi 密碼 → 驗證後備機制
- [ ] **A 更新到新版 `shared/PullOta`**——目前部署前最大的殘餘風險。
      A 仍跑 v6 舊版:下載遇斷線會卡死 loop(舊 `writeStream` 不返回,
      B 之前就是這樣當機的),也沒有任何診斷工具。發 pond-site 新版走
      同一套 `tools/release.sh pond-site`,更新動作本身同時驗證 A 的
      拉取路徑仍健康。
      注意:**A 的 qio 讀取從未實測**(v5→v6 成功史只是間接證據);
      platformio 先不動,更新後看狀態頁的 `raw sample ctor/setup` 兩行
      確認 raw 讀健康,再決定要不要跟進 dio
- [ ] A 的板子若要重燒,先試 esptool 自動 reset(B 已證明可行,A 未測)

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
