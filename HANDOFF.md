# 交接筆記（2026-09-04 更新:WiFi 可從手機遠端設定 + 斷線事件記錄,A、B 皆已上線）

給接手的 session。專案背景看 [README.md](README.md),這裡只寫**當下狀態**和
**踩過的坑**。

**最新一輪(09-03):WiFi 憑證改存 NVS,可遠端改,不必重燒。** 這解決了原本
「部署前必須完成」清單裡的 WiFi 後備問題——見下方「WiFi 遠端設定」章節。
**A、B 的四層 fallback(含手機熱點救援)都已完整實測**,包含 A 沒有 Discord
token、由 B 從 RTDB 讀到 ssid 變化代為宣布的那條路。

**同一天稍晚加上斷線事件記錄(b46 / a14)**:板子沒網路時發生的事,現在會存進
NVS、連上後上傳 `/incidents/`——見「斷線事件記錄」章節。

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
| 韌體版本 | **a14**(拉取式 OTA 裝入) | **v46**(拉取式 OTA 裝入) |
| 拉取式 OTA | ✅ **已修復**(120s WDT;v9/v10 連續兩次、兩個 slot 皆驗證) | ✅ 已修復(dio;連續三次、兩個 slot 皆驗證) |
| flash 讀取模式 | qio(**本次已實測健康**,見 qio 體檢) | **dio(必要,勿改回 qio)** |
| task WDT | **120s,loop 有訂閱**(v8 起;v6 以前只有預設 5s/IDLE0) | 120s,loop 有訂閱 |
| PSRAM | ❌ 初始化失敗(本次 UART 實測,見下) | ❌ 初始化失敗(與 OTA 無關) |
| 功能 | 正常上傳水溫 | 正常監控 + Discord 告警 |
| WiFi 憑證 | NVS,`/wifi` 可改 | NVS,`/wifi` 可改 |
| 手機熱點救援 | ✅ 全程實測(09-04,a12 測試版) | ✅ 全程實測(09-03) |
| Discord | 無 token,由 B 代發 | 直接發(告警 + 狀態回報) |
| 斷線事件記錄 | ✅ b46/a14 起,含 reset reason 解碼(A 首次有) | ✅ |

兩片都在台北家中運作,連 `Wang3697`,資料持續進 RTDB。09-04 的連續觀測:
B 已連續運行 42 小時、2544 次檢查零遺漏、無意外重開;A 因使用者換線斷過一次,
重接後 30 次上傳零失敗。

**heap 值得盯**:B 的 free heap 從開機的 263K 降到 42 小時後的 252K。可能只是
碎片化(這片板子有 churn 的歷史),也可能是慢性洩漏——單次數據判斷不了,再觀察
一次就知道。有 120s WDT 兜底,最壞會自己重開。

**A 的濕度讀數超過 100%**(100.2%)。使用者判斷該感測器不一定真的是濕度、暫時
也用不到,**刻意不追**。若之後要用這個欄位做判斷,先確認感測器型號。

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

## WiFi 遠端設定(2026-09-03 完成,B 已全程板上實測)

原本 WiFi 憑證編譯在 `secrets.h` 裡,要改就得重燒——而重燒走 OTA、需要網路,
所以「連不上網路」正好是唯一無法修復的情況。現在憑證存 NVS,可遠端改。

**這解決了舊清單裡的「WiFi 後備連線」待辦**,但做法跟當初設想的不同:不是連
不上就開 AP,而是掉到一支手機開的熱點上,因為那樣板子仍是完整的 STA、
WebServer/RTDB/Discord 全都活著,而 AP 模式下板子是半死的。

### 連線順序(開機時依序試,第一個成功就停)

```
1. NVS target        /wifi 存的目標網路,平常都命中
2. NVS target_prev   上一組,所以打錯 SSID 會自己退回
3. secrets.h 三組    Wang3697 台北 / Xiaomi_2F 老家 / 20170330 魚塭
4. NVS rescue        手機熱點(預設 OPPO K9 Pro 5G / i83ux9cq)
5. 全失敗            紫燈慢閃,4 輪(約 3-5 分鐘)後重開
```

**順序是按「用起來要付出多少代價」排的,不是按可靠度。** 前三層不需要任何人
做事;熱點要有人站在旁邊開著,所以排最後——而且排最後也代表它那 12 秒只在其他
全失敗後才付出,不是每次開機都白等一個關著的熱點。

**真正的救援主線不是熱點,是把板子搬回老家。** 使用者講明的流程:板子壞了就叫
家人拔電、帶回老家、插電,`Xiaomi_2F` 在第 3 層接住它,人不必操作任何介面。
所以 **`secrets.h` 那三組是救援計畫,不是開發殘留,一組都不能刪**——尤其
`Xiaomi_2F`。每片板子都燒同一份,壞哪片搬哪片,不用查配對。

**4 輪就重開這件事不能改成無限重試。** 連不上 WiFi 的韌體正是 pull_ota 的
rollback 要救的情況,而 rollback 只在重開時發生;無限重試會把壞韌體困在它唯一
能逃脫的狀態裡。

### 端點

三個都要帶 `pass=<OTA_PASSWORD>`(目前 `pond-ota`)。這不是資安措施,是防止
魚塭區網上的誤觸把板子的網路改掉。

```
http://<ip>/wifi?pass=pond-ota                     開表單(中文,手機用)
http://<ip>/wifi?pass=pond-ota&ssid=X&wpass=Y      直接設定,存完重開
http://<ip>/wifi?pass=pond-ota&clear=1             清掉 target 和 prev
http://<ip>/rescue?pass=pond-ota                   改救援熱點(不重開)
```

`/wifi` 無參數時回一個中文表單,兩個欄位。這是給現場的人用的——**新網路的名稱
密碼在現場那邊,不在我們這邊**,所以讓他們填不是退路,是唯一有答案的地方。

`clear=1` 不只是測試工具。每次 `/wifi` 都會把當下能用的網路存進 prev,所以
板子換魚塭時,舊魚塭的憑證會永遠留在第 2 層——交接給新場地時該先清掉。

### Discord 訊息

只有**真告警**會 tag 使用者;狀態回報一律不 tag。使用者的理由:板子換地方供電
必定是人為的,當下就會主動查,tag 只會讓頻道被靜音。家人也在頻道裡,所以訊息
直接附設定網址讓他們點。

| 情況 | 訊息 |
|---|---|
| 連上 target(正常) | 不發 |
| 連上已知地點(被搬走了) | ℹ️ 不在目標網路上 + 網址 |
| 連上救援熱點 | ⚠️ 連不上目標網路 + 網址 |
| 新設定失敗、自動退回 | ⚠️ WiFi 設定失敗 |
| 新設定成功 | ✅ WiFi 已更新 |

**A 沒有 Discord token**(不想在魚塭的板子上再放一份憑證),所以 A 換網路是
**B 代為宣布**:B 每分鐘掃 `/devices`,比對每個節點回報的 `ssid`,變了就發。
心跳因此加上了 `ssid` 和 `ip` 兩個欄位——`ip` 是從 300 公里外唯一能拿到板子
設定頁位址的方法,那是個掃不到也猜不到的私有網段。

### 實測記錄(09-03,B)

四層全部走過,包含最後一層:

1. `/wifi?ssid=nonexistent` → 掉到 `Wang3697` → 收到 ℹ️ 訊息 → 表單改回
2. `/wifi?ssid=nope-a` → prev 接住 → 收到 ⚠️「已自動退回」
3. `secrets.h` 三組暫時換成假名字 + `clear=1` → **掉到手機熱點** → 板子自報
   新 IP(`192.168.67.249`)→ **用手機點連結、填表單、板子回家**

第 3 步是完整的救援循環,也是家人之後要做的事。手機上的表單使用者確認可用。

### 這輪抓到的 bug(全都是上板才看得見的)

- **兩處 JSON 跳脫寫成單反斜線** → 編譯器把 `\uXXXX` 和 `\n` 變成真的位元組,
  Discord 拒收整個 body,訊息無聲消失。修了 `\u` 之後 `\n` 還錯,又發一版。
  **`discordSay` 現在會記錄送出失敗**——之前回傳值沒人看,「發失敗」和「還沒
  到時間發」長得一模一樣,這是為什麼第一個 bug 能上線、修完還躲著第二個
- **`revertTarget()` 沒清 pending**:沒有前一組時提早 return,所以第一次設定的
  target 若失敗,板子每次開機都重跑同一套失敗流程,狀態頁永遠 `[unproven]`
- **Discord 連結沒帶密碼** → 家人點了會看到 `bad pass`。是使用者實際用手機點
  一次才發現的
- **告警訊息只有 `11:18` 沒有日期**:節點通常斷很久才有人看,那時「哪一天的
  11:18」是必要資訊。順手把「2305 分鐘」改成「1 天 14 小時」

### 實測記錄(09-04,A)

跟 B 同一套做法:`secrets.h` 三組暫時換成假名字、發 a12。A 的 NVS target 本來
就是空的,所以**不需要 `clear=1`**,更新完重開就直接掉到熱點。

真正在測的不是 fallback 本身(那是從 B 逐字複製的),而是 **`announceNodeMove()`**
——A 沒有 Discord token,要靠 B 從 RTDB 讀到 `ssid` 變化代為宣布。這個函式在此
之前**從來沒被觸發過**,結果一次就對,而且雙向都對:

```
22:36:32 pond-site moved Wang3697 -> OPPO K9 Pro 5G, announced
22:41:30 pond-site moved OPPO K9 Pro 5G -> Wang3697, announced
```

(`announced` 只在 `discordSay()` 回 true 時才寫,所以那是送出成功,不只是嘗試。)

順便驗證了 v45 那個「連結補上密碼」的修正:使用者用手機點 Discord 裡的連結,
直接看到表單,不再是 `bad pass`。

**發佈時 `release.sh` 擋下了一次**:GitHub 的 release asset 已經 uploaded,但
CDN 還沒同步,下載回來是 92 bytes 的 504 頁面。腳本因此**沒有更新 RTDB**——
這正是它存在的理由,否則 A 會抓到壞檔、失敗、把該版本永久標記成 bad。等 CDN
就緒(約一兩分鐘)再手動 PUT 一次就好。

### 還沒測的

- [ ] `/rescue` 端點實際改過熱點(目前用的還是編譯期預設值)
- [ ] OTA 進行到一半拔電 → 半寫入狀態能否恢復

## 斷線事件記錄(2026-09-04,b46 / a14)

板子唯一無法回報的失效,就是**沒有網路的那一次**。魚塭跳電、路由器半夜掛掉、
韌體卡死——等到有人發現,通常已經被手動重開過、看起來一切正常,而當時發生什麼
事完全沒有留下痕跡。

所以每次開機都寫一筆「上次是怎麼結束的」,連上網後上傳到
`/incidents/<device>/<unix ts>`,然後清掉。

**它不試圖給根因,而是給分類**:電力 / 網路 / 韌體 / 人為。這四類的處置方式
完全不同(換電源、找人重開路由器、看韌體、不用管),而分類是板子知道的資訊
就足以判斷的。根因往往需要板子看不到的東西——路由器裡發生什麼事,板子只知道
「連不上」。

### 欄位怎麼讀

| 欄位 | 判讀 |
|---|---|
| `reset` | `BROWNOUT` = 電壓掉;`*_WDT (hang)` = 卡死超過 120s;`SW_CPU` = 我們自己重開的(OTA 或連不上 WiFi 四輪);`POWERON` = 上電 |
| `gap` | 距離上次健康心跳幾秒。**這是最有用的欄位** |
| `prevUptime` | 上一輪活了多久。只有幾秒 = 重開迴圈,不是單一事件 |
| `attempts` | 每個 SSID 的結果與耗時 |
| `boots` | 累計開機次數,數字暴增就是在反覆重開 |
| `wifiPasses` | 整輪候選清單失敗幾次(0 = 第一層就中) |

**`gap` 為什麼關鍵**:`POWERON` 本身分不出「跳電」和「有人拔插頭又插回去」。
但心跳每分鐘寫一次 NVS,所以:

- `POWERON` + `gap` 只有幾秒 → **電源毫無預警消失**。電壓掉得太快,brownout
  偵測器來不及記錄。這是電力問題,不是人為
- `POWERON` + `gap` 好幾小時 → 有人拔了插頭,晚點才插回來。正常操作

**`attempts` 怎麼分辨三種網路問題**:

```
20170330:fail(0.8s)              密碼錯 —— 路由器主動拒絕,失敗得很快
20170330:fail(12.0s)             路由器沒回應 —— 跑滿整個 timeout
20170330:ok(9.4s,-89dBm)         訊號弱 —— 連得上但慢,RSSI 很差
```

### 只在真的異常時才寫

正常重開**不記錄**——我們自己的 OTA 每次都會重開,全記的話重要的會被淹沒。
觸發條件(見 `incidentWorthReporting`):WiFi 不是第一次就通、任何 watchdog、
brownout、panic、或上面說的「POWERON 但 gap 很短」。

### flash 壽命

每次開機 1 次 + 健康時每分鐘 1 次心跳。NVS 有磨損平均、規格每 sector 10 萬次
擦寫,以這個頻率**就算每小時重開一次也能撐十幾年**。真正會寫壞的是從 `loop()`
裡連續寫(每秒一次 = 三天報銷),所以 `incidentHeartbeat()` 內建每分鐘一次的
節流,而其他寫入都只在開機時發生。

### 同樣的資訊也在狀態頁上

`/` 會顯示 `last reset` / `boot count` / `wifi at boot` / `prev run`,沒出事的
時候也看得到。A 這次才**第一次有 reset reason 解碼**——在此之前它為什麼重開
是完全的黑箱。

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
- `watchdog-v43` / `v44`、`pond-site-v12`:**測試版,secrets.h 被暫時換成連不上
  的假 SSID** 以驗證救援熱點。secrets.h 是 gitignored,所以 repo 裡看不出差別
  ——這些 tag 的 .bin **不要拿來裝任何板子**。正式版:B 從 v45 起、A 從 a13 起
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
- [x] ~~**WiFi 後備連線**~~ **完成**(09-03,做法不同於原構想:不開 AP,改成
      掉到手機熱點 + NVS 憑證 + `/wifi` 表單。B 全程實測,見「WiFi 遠端設定」)
- [ ] **破壞性測試(只有現在能做)**:
  - [ ] 推一版故意在 `setup()` crash 的 → 驗證 rollback 能自動退回
  - [x] ~~推一版故意連不上 WiFi 的 → 驗證能否自救~~ **完成**(09-03:
        `secrets.h` 換假名字 + `clear=1`,B 掉到手機熱點並從表單救回)
  - [ ] OTA 進行到一半拔電 → 驗證半寫入狀態能恢復
  - [x] ~~改掉 WiFi 密碼 → 驗證後備機制~~ **完成**(09-03:`/wifi` 設成
        連不上的 SSID,prev 與已知地點兩層都實測接住)
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

## 出貨去南部之前

- [x] ~~**測 A 的手機熱點路徑**~~ **完成**(09-04,a12 測試版;
      `announceNodeMove()` 首次觸發,雙向皆正確——見上方實測記錄)
- [ ] 用 `/rescue` 把救援熱點改成家人的手機——**或**請家人把熱點名稱密碼設成
      板子裡已有的那組。後者不用動板子,比較安全
- [ ] 貼一張小卡在板子上,給家人看的兩句話:
      「紫燈一直閃 = 連不上網路,請拔電帶回家插電」
      「Discord 出現連結時,點進去填 WiFi 名稱密碼」
- [ ] 若板子要換場地,先 `/wifi?clear=1` 清掉舊魚塭的憑證

## 其他待辦(部署之後才做)

- [ ] **把 WiFi 那套抽到 `shared/SiteWiFi/`**。A 和 B 現在各有一份約 350 行的
      逐字複本,改一個 bug 要改兩邊。當初刻意不抽:B 那份還沒上板驗證,同時
      重構會讓失敗分不清是功能還是搬移。**現在 B 驗證過了,可以抽了**
- [ ] **`DEVICE_ID` 拆成身分與韌體分組**。現在一個欄位兼兩用:`/devices/<id>`
      要每片唯一,`/firmware/<id>` 卻要多片共用(使用者的規劃是「魚塭 1、2 同
      一個 code,魚塭 3、4、5 同一個 code」)。**燒第三片之前必須先做完**,
      否則心跳會互相覆蓋、B 也分不清誰是誰

- [ ] 前端圖表(使用者主場):讀 `history/pond-site/<YYYY-MM>`,`orderByKey` 取範圍
- [ ] C 節點:燒 watchdog 韌體、改 `DEVICE_ID`,B/C 自動互相監控
- [ ] 溶氧、pH:可掛同一條 RS485(改站號),或依地理位置分組
- [ ] RTDB 規則全開放(Spark 方案最壞是服務中斷非計費)。**升 Blaze 前必須收緊**
