# BonDriver_EDCB の内部仕様と実測値

使い方は [README.md](README.md) を参照してください。このファイルには、動作のしくみと、
なぜその作りにしたのかの根拠(と、その裏付けになった実測値)をまとめています。

## 動作のしくみ

```
TVTest ── SetChannel ──▶ BonDriver_EDCB
                            │  GET /legacy/bonstream.lua?key=..&n=0&id=<ONID>-<TSID>-<SID>
                            ▼
                       EpgTimerSrv (civetweb + Lua)
                            │  edcb.OpenNetworkTV(2, onid, tsid, sid, n)
                            ▼
                       EpgDataCap_Bon ──▶ SendTSTCP 名前付きパイプ
                            │              \\.\pipe\SendTSTCP_<port>_<pid>
                            ▼
                       tsreadex ──▶ HTTP レスポンスボディ (MPEG2-TS 垂れ流し)
```

* チャンネル一覧は `bonstream.lua?list=1` が返す **ChSet5**(`edcb.GetChDataList()`)から作ります。
  EPG データの有無に依存しないので、EPG を取っていないチャンネルや BS4K も漏れなく並びます。
  `bonstream.lua` を置いていない場合は `api/EnumService`(EPG 由来)へフォールバックします。
* 選局は「今の接続を切って、同じ `NWTV_ID` で開き直す」だけです。EDCB 側は同じ ID の
  NetworkTV モードが生きていれば同じチューナーで選局し直すので、これがそのままチャンネル変更になります。
* 切断すると `bonstream.lua` が 5 秒後に `CloseNetworkTV` します(チャンネル変更のための
  張り直しを妨げないように遅らせています。遅延中に開き直された場合は終了しません)。
* 受信スレッドがエラーで抜けたときは `WaitTsStream()` の待ち手を起こして `WAIT_ABANDONED` を
  返します。EDCB は録画予約が始まると視聴中の NetworkTV モードを打ち切るので、
  ストリームが死んだことを視聴アプリが即座に知れるようにしてあります。

## 1 チャンネル = 1 トランスポートストリーム にしている理由

EDCB の ChSet5 はサービス単位なので、素直に「1 チャンネル = 1 サービス」にすると、同じ TS の
サービスの数だけ同じチャンネルが並びます。これは見た目の問題ではなく、TVTest のチャンネル
スキャンが破綻します。

EDCB の ServiceFilter は PID 0x30 未満(PAT/CAT/NIT/SDT/EIT/TOT…)をフィルタの対象外にしているため、
サービスを絞って配信しても **SDT には TS 内の全サービスが載ったまま**になります。TVTest は
チャンネルの素性を SDT から判断するので、同じ TS を指す複数のチャンネルがすべて SDT の
先頭サービスとして扱われてしまいます。実際、BS 朝日 1/2/3 に対応する 3 チャンネルはいずれも
サービス ID 151 と判定され、「サービスを検索する」を外すと局名も付きませんでした。
(BonDriver_Mirakurun の `SERVICE_SPLIT=1` が破綻しないのは、Mirakurun 側が SDT まで
書き換えて配信しているからです)

そこで既定では同じ (ONID, TSID) を 1 チャンネルにまとめ、その TS の最初のサービスで選局します。
TVTest から見れば「1 チャンネル = 1 物理チャンネル」という本来の形になり、TS 内のサービスは
TVTest 側が SDT から正しく展開します。なお BS 朝日 1/2/3 のように、同じ TS の複数サービスが
同一の PID(映像 320・音声 321…)を共有しているだけ、というケースも珍しくありません。

## チャンネル名を番号にしている理由

1 チャンネル = 1 TS なので、代表サービスの名前をチャンネル名にすると実態と合いません
(CS の 1 つの中継器には十数局が入っています)。そこで既定ではチャンネル名を番号にします。
個々の局名は TVTest が SDT から拾うので、チャンネルスキャンの「名前」列には正しく並びます。

* **CS110** は放送規格の ND 番号: `ND2` `ND4` … `ND24`。TSID の下位 12bit の上位 8bit が
  そのまま ND 番号になります(`ND = (TSID & 0x0FFF) >> 4`)。実データでも 0x6020→ND2、
  0x60A0→ND10、0x7180→ND24 と全 12 中継器が一致しました。この空間だけは ND 番号順に並べ替えます。
* **それ以外** は空間ごとの連番: `GR 01` `BS 01` `BS4K 01` `SKY 01` …。
  BS の「BS01/TS0」形式は TSID から一意に導けない(0x4030 と 0x4631 がどちらも中継器 3 相当)
  ため、連番にしています。

## view.lua ではなく bonstream.lua を使う理由

EDCB 標準の `view.lua` は tsreadex に `-x 18/38/39` を渡しており、EIT (PID 0x12 / 0x26 / 0x27) を
除去します。ブラウザ再生には不要なので妥当ですが、TVTest では番組情報も EPG も出なくなります。
また `-n <SID>` で指定サービスのみに絞られるため、TVTest 側でのサービス切り替えもできません。

`bonstream.lua` は `-x` を渡さず、既定で `-n` も渡しません。実測では `view.lua` が 12 秒間で
PID 0x12 のパケット 0 個だったのに対し、`bonstream.lua` では 1820 個(12 秒)得られました。

なお、パイプ自体に全サービスを流すかどうかは EpgDataCap_Bon 側の設定です。
`EpgDataCap_Bon.ini` の `[SET] AllService=1`(設定画面の「全サービス出力」)にすると、
トランスポートストリームの全サービスが TVTest から見えるようになります。0 のままでも、
PID 0x30 未満(PAT/CAT/NIT/SDT/EIT/TOT/SDTT/BIT など)は落とされないので EPG は取得できます。

この設定はネットワーク出力専用のサービス制御(`CBonCtrl::nwCtrlID` = `CreateServiceCtrl(TRUE)` で
作られるもの)にしか効かないため、録画側には影響しません。

## tsreadex の -n について

`-n -1` は「絞り込みなし」ではなく **先頭のプログラムだけを残す**指定です(EDCB の `view.lua` は
必ずサービスを 1 つに絞るのでこれで問題ありません)。本プロジェクトの `bonstream.lua` は
全サービスを通したいので、絞り込まない場合は `-n` 自体を渡しません。`AllService=1` にしても
PAT にサービスが 1 つしか出てこない場合は、ここを疑ってください。

実際に `AllService=1` と `-n` 省略の両方が揃ってはじめて全サービスが出ました。

```
AllService=0                : PAT programs = [NIT, 151]
AllService=1 かつ -n -1     : PAT programs = [NIT, 151]
AllService=1 かつ -n 省略   : PAT programs = [NIT, 151, 152, 153]
```

## HTTP のやりとり

* チャンネル一覧と CSRF トークンの取得には `BonDriver_EDCB.cpp` の `HttpGet()` を使います。
  接続は選局と同じ `ConnectToServer()` を通すので、DLL 読み込み時のこの 1 回で
  アドレスファミリのキャッシュが埋まり、その後の選局の接続は 0ms になります。
  応答は接続が閉じるまで読んでからヘッダとボディに分け、`Content-Length` と
  チャンク転送(リバースプロキシを挟むと来ることがある)の両方を解釈します。
* リクエストは HTTP/1.0 固定です。レスポンスは `Content-Length` なし・`Connection: close` の
  垂れ流しなので、チャンク転送や Keep-Alive を考えなくて済みます。
* `EARLY_RESPONSE=1` のときは、`bonstream.lua` が `edcb.OpenNetworkTV()` を呼ぶ前に
  ステータス行とヘッダを返します。チューナーの起動待ちが `SetChannel()` の所要時間に
  乗らなくなる代わりに、選局に失敗しても 200 になります。
* HTTP ヘッダと同じ `recv()` で読み過ぎたストリームデータは捨てずにリングバッファの先頭へ
  積みます。捨てると TS パケットの先頭が欠けて、再生開始時に再同期が必要になるためです。
* 接続先は `AF_UNSPEC` で名前解決し、候補を順に試します。一度成功したアドレスファミリを
  覚えて次回はそちらを先に試すので、2 回目以降の接続は 0ms になります。

## 実測値

EDCB と TVTest が同一 PC、`test/bontest.exe` による計測。

### ストリームの品質 (BS の HD チャンネル)

| 項目 | 値 |
| --- | --- |
| 受信レート | 10〜13 Mbps (CS の中継器は 30 Mbps 前後) |
| TS 同期 | ストリーム全体で 188 バイトごとの同期バイトが 100% 一致 (85120/0, 84864/0, 61952/0) |
| スクランブル | 選局直後の 0.5%(先頭 476 パケット)を除き `transport_scrambling_control` は 0 |
| PAT | `AllService=1` + `-n` 省略で TS 内の全サービスを収録 (BS 朝日 TS = 151/152/153) |
| EIT | 12 秒で PID 0x12 のパケット 1469 個 (`view.lua` 経由では 0 個) |

### 所要時間

| | `SetChannel()` | 映像が出るまで |
| --- | --- | --- |
| 地デジ / BS / CS 初回 | 15〜50ms | 約 4 秒 |
| 地デジ / BS / CS チャンネル変更 | 15〜50ms | 約 1.9 秒 |
| BS4K 初回 | 15〜50ms | 約 30 秒 |
| BS4K チャンネル変更 | 15〜50ms | 約 9.4 秒 |

`SetChannel()` がチャンネルによらず一定なのは `EARLY_RESPONSE=1` のためです。0 にすると
「映像が出るまで」とほぼ同じ時間だけ `SetChannel()` がブロックします(BS4K は実測 29.5 秒)。

`SERVER_HOST` に `localhost` を指定していると、先に `::1` へ解決されるのに EDCB の HTTP
サーバーが IPv4 だけで待ち受けているため、最初の接続で `CONNECT_TIMEOUT` 分(既定 1.5 秒)
待たされます。ただしこれを踏むのは DLL 読み込み時のチャンネル一覧取得で、そこで
アドレスファミリが記憶されるため選局には影響しません。気になる場合は `SERVER_HOST` に
IP アドレス(`127.0.0.1` など)を直接書けば消せます。

BS4K が極端に遅いのは `edcb.OpenNetworkTV()` が BonDriver_dantto4k のチューナープロセス起動と
ACAS 初期化を同期的に待つためで、プラグイン側では短縮できません。
