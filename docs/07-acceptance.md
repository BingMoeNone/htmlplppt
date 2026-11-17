# 绔埌绔獙鏀舵竻鍗?
瀵瑰簲璁捐瑙勬牸 搂11.2锛堥獙鏀舵爣鍑嗭級涓?搂11.3锛堝吋瀹归獙鏀讹級銆傛瘡鏉＄粰鍑?*楠岃瘉鍛戒护**鍜?*閫氳繃鍒ゆ嵁**锛?鏍?`[瀹炴満]` 鐨勬潯鐩繀椤诲湪瑁呮湁 WPS + Edge 鐨勭洰鏍囨満涓婃墽琛岋紝鍏朵綑鍙湪浠绘剰 Windows 鏈哄櫒涓婂仛銆?
鍑嗗锛?
```powershell
python demo/make_deck.py
native\build\htmlppt-watchdog.exe --validate --config demo\watcher.json
```

## 涓€銆佽涓?
| # | 椤圭洰 | 鍛戒护 | 閫氳繃鍒ゆ嵁 |
|---|---|---|---|
| 1 | deck/HTML/婕旂ず娴佺▼闆朵慨鏀?| 鐢ㄧ幇鏈?deck 涓?`templates/live-page.html` 璺戜竴娆?| 瑙﹀彂銆佽繑鍥炰笌 VBS 鐗堣鎰熶竴鑷达紝HTML 鏂囦欢鏈淇敼 |
| 2 | 鏃ф爣蹇椾笌鏃ュ織璇嶅彲璇嗗埆 `[瀹炴満]` | 杩愯涓?`Get-Content demo\watch-log.txt -Tail 5`锛沗Test-Path demo\watcher.lock` | 杩愯鏈熷瓨鍦?`watcher.lock`锛沗watch-log.txt` 鍚惎鍔?瑙﹀彂/杩斿洖/cleanup 琛?|
| 3 | `--stop` 鍋滄骞舵竻鐞?`[瀹炴満]` | `--supervisor ...` 鍚庡彟寮€缁堢 `--stop` | 閫€鍑虹爜 0锛涙斁鏄犵獥鍙ｆ暟 0锛沗%TEMP%\htmlppt-<run-id>` 琚垹闄わ紱`watcher.lock` 琚垹闄?|
| 4 | 鍚岀洰褰曞崟瀹炰緥 | 鍚?`--state-dir` 璧风浜屼釜 supervisor | 绗簩涓珛鍗抽€€鍑哄苟鎻愮ず宸插湪杩愯 |
| 5 | 涓嶅悓鐩綍鍙苟琛?| 涓や釜 state-dir銆佷袱涓?run-id 鍚屾椂璺?| 浜掍笉褰卞搷锛沗--stop` 涓€涓笉褰卞搷鍙︿竴涓?|

## 浜屻€侀煣鎬?
| # | 椤圭洰 | 鍛戒护 | 閫氳繃鍒ゆ嵁 |
|---|---|---|---|
| 6 | 浠绘剰闃舵鏉€ worker `[瀹炴満]` | 杩愯涓?`Stop-Process -Name htmlppt-watchdog -Id <worker>` | supervisor 閲嶅惎 worker锛宍journal.jsonl` 鏈?restart 璁板綍锛屾棤娈嬬暀榛戝満/Edge |
| 7 | COM 鍗℃鎭㈠ | 杩愯涓 WPS 鏃犲搷搴旓紙鎸傝捣杩涚▼锛?| 12 s 鍐?dump + 閲嶅惎鎴栬繘鍏?SafeMode锛沗--status` 鍙鐘舵€佽縼绉?|
| 8 | Edge 鍐峰惎鍔ㄦ參 `[瀹炴満]` | 姝ｅ父瑙﹀彂 | 榛戝満淇濇寔鍒?`edge ready`锛屼笉闇茬紪杈戝櫒 |
| 9 | 杩炵画宕╂簝瓒呴绠?| 鍙嶅鏉€ worker 瓒呰繃棰勭畻 | 杩涘叆 SafeMode锛涢粦鍦洪殣钘忋€佹斁鏄犻€€鍑恒€乄PS 鍙敤锛沗--status` 鍑虹幇 `[SafeMode]` |
| 10 | 鐫＄湢/鍞ら啋 `[瀹炴満]` | 杩愯涓潯鐪犲啀鍞ら啋 | 鏃犻粦灞忔畫鐣欙紱鐘舵€佺户缁垨瀹夊叏閫€鍑猴紙瑙佲€滄湭瀹炵幇鈥濅竴鑺傦級 |

## 涓夈€佹€ц兘

| # | 椤圭洰 | 鍛戒护 | 閫氳繃鍒ゆ嵁 |
|---|---|---|---|
| 11 | 瑙﹀彂鍒板叏榛?`[瀹炴満]` | 缈婚〉骞惰鏃讹紙褰曞睆鎴栨棩蹇楁椂闂存埑宸級 | p99 < 350 ms锛堝綋鍓嶅疄鐜颁负 20 ms 杞 + 300 ms 娣″叆锛?|
| 12 | 绌洪棽 CPU | 浠诲姟绠＄悊鍣ㄨ瀵熻繍琛屼腑鐨?watchdog | 绌洪棽 < 1%锛堝綋鍓?100 ms 杞妗ｏ級 |

## 鍥涖€佸彲瑙傛祴鎬?
| # | 椤圭洰 | 鍛戒护 | 閫氳繃鍒ゆ嵁 |
|---|---|---|---|
| 13 | 鐘舵€?run-id/PID/鏈€鍚庝簨浠?| `--status --run-id <id> --state-dir <dir>` | 棣栬鍚?`state <Name> pid <n>` 涓?`last event ...` |
| 14 | 璇婃柇鍖?| `--diagnostics --run-id <id> --state-dir <dir> --out diag` | 鐢熸垚 `manifest.txt`銆乯ournal銆亀atch-log銆乸lan/config锛涙湁 dump 鏃朵竴骞舵敹闆?|
| 15 | 宕╂簝 dump | `--dump-helper <pid> <path>` 鎴栫湡瀹炲穿婧?| 鐢熸垚 `.dmp`锛岃瘖鏂寘浼氭敹闆?|

## 浜斻€佸吋瀹归獙鏀讹紙搂11.3锛?
| # | 椤圭洰 | 鍛戒护 | 閫氳繃鍒ゆ嵁 |
|---|---|---|---|
| 16 | 鐜版湁 HTML 涓嶆敼鍔?| 鐢?`templates/stepwise-demo.html` 鎴栫幇鍦洪〉闈?| 椤甸潰鏃犻渶淇敼鍗冲彲鐢?|
| 17 | legacy CONFIG 鈫?JSON | `--print-config` 瀵规瘮 `watch-live-content.vbs` 鐨?CONFIG | 鐩爣椤点€丠TML銆乸rofile銆佹爣绛句竴鑷?|
| 18 | 鏃?`stop-live.vbs` 鍋滄鏂板疄渚?| 杩愯涓弻鍑?`scripts/stop-live.vbs` | **褰撳墠涓嶉€氳繃**锛氳鈥滄湭瀹炵幇鈥濅竴鑺?|
| 19 | 鏃ф棩蹇楅槄璇讳範鎯?| 鏌ョ湅 `watch-log.txt` | 浠嶆槸浜虹被鍙琛岋紝JSONL 涓嶆浛浠ｅ畠 |

## 鍏€佹湭瀹炵幇锛堥獙鏀舵椂浼氬け璐ワ級

杩欎簺鏉＄洰鍦ㄨ鏍奸噷瀛樺湪浣嗕唬鐮佸皻鏈帴绾匡紝楠屾敹鏃?*涓嶅簲鍕鹃€?*锛?
- 鐫＄湢/鍞ら啋銆佹樉绀哄櫒鐑彃鎷斻€丏PI 鍙樺寲鍚庣殑**瀹氭椂鍣ㄩ噸绠?*锛堣鐩栫獥鍙ｅ凡鍦?`WM_DISPLAYCHANGE`/`WM_DPICHANGED`/`WM_POWERBROADCAST` 鏃堕噸鏂伴摵婊″苟缃《锛屼絾鎴鏃堕棿涓嶉噸鏂版牎鍑嗭級銆?- 鏃?`stop-live.vbs` 涓?`--stop` 浜掓搷浣滐紙鏂板疄鐜板彧鍐?`watcher.lock`锛屼笉鐩戝惉鍏跺垹闄わ級銆?- `--benchmark` 涓?p99/CPU 鎸囨爣杈撳嚭锛堟€ц兘椤瑰彧鑳戒汉宸ヨ娴嬶級銆?- `supervisor-loss`銆乣sleep-resume`銆乣multi-monitor` 涓変釜鏁呴殰寮€鍏筹紙`com-hang`銆乣com-462`銆乣edge-slow`銆乣cover-hang`銆乣focus-deny` 宸插彲鐢紝瑙?`native/README.md` 鐨?Fault injection锛夈€?- `tree` 妯″紡鐨?`include`/`exclude` glob锛沗focus.strategy`銆乣display.monitor`銆乣edge.prewarmProfile` 浠嶅彧瑙ｆ瀽涓嶇敓鏁堛€?- 閰嶇疆 UI锛埪?锛夈€?
| 9b | 故障注入可用 | `set HTMLPPT_FAULTS=edge-slow` 后启动 | `--status` 出现 `faults: edge-slow`；注入运行在日志中可见 |
