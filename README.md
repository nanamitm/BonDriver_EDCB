# BonDriver_EDCB

[![build](https://github.com/nanamitm/BonDriver_EDCB/actions/workflows/build.yml/badge.svg)](https://github.com/nanamitm/BonDriver_EDCB/actions/workflows/build.yml)

TVTest から [EDCB](https://github.com/xtne6f/EDCB) の Legacy WebUI 経由でライブ視聴するための BonDriver です。

EpgTimerSrv が持っている HTTP サーバー(既定 5510 番)だけを使って、チャンネル一覧の取得から
TS の受信までを行います。UDP/TCP の直接送信(BonDriver_UDP / BonDriver_TCP)と違い、

* ポートが 1 本で済むので、リバースプロキシや VPN 越しでもそのまま通る
* チャンネル一覧を EDCB の ChSet5 から自動取得するので `.ch2` を手で作らなくてよい
* チューナーの割り当て・排他は EDCB 側(予約優先)にまかせられる

という利点があります。ソースコードのベースは
[BonDriver_Mirakurun](https://github.com/nanamitm/BonDriver_Mirakurun)(元は BonDriver_HTTP)です。

動作のしくみ・設計の根拠・実測値は [INTERNALS.md](INTERNALS.md) にまとめてあります。

## 構成

```
BonDriver_EDCB.cpp / .h      BonDriver 本体
BonDriver_EDCB.ini           設定ファイルのひな形
IBonDriver.h / IBonDriver2.h BonDriver インターフェイス定義
server/bonstream.lua         EDCB に置く配信用エンドポイント
test/bontest.cpp             TVTest なしで動作確認するテスタ
thirdparty/                  同梱している第三者のソース (thirdparty/README.md 参照)
INTERNALS.md                 動作のしくみ・設計の根拠・実測値
```

ビルドしたあとの配置先は次のとおりです。

| ファイル | 置き場所 |
| --- | --- |
| `BonDriver_EDCB.dll` / `BonDriver_EDCB.ini` | TVTest の BonDriver フォルダ |
| `server/bonstream.lua` | EDCB の `HttpPublic/legacy/` |
| `Setting/BonStream.ini`(新規作成) | EDCB の `Setting/` |

## セットアップ

### 1. サーバー側 (EDCB)

1. `server/bonstream.lua` を EDCB の `HttpPublic/legacy/` にコピーする。
2. EDCB の `Setting/BonStream.ini` を新規作成し、共有キーを書く。

   ```ini
   [BONSTREAM]
   Key=<英数字の任意の文字列>
   ```

   キーが未設定のあいだ `bonstream.lua` は常に 403 を返します(ブラウザからの意図しない
   アクセスでチューナーを掴まれないようにするため、無効が既定です)。

   リポジトリに含まれる `BonDriver_EDCB.ini` の `STREAM_KEY` は空のままにしておいてください
   (自分の環境用に書き込んだキーをそのままコミットしないよう注意)。

3. EpgTimerSrv の設定で以下を確認する。
   * HTTP サーバーが有効(`EpgTimerSrv.ini` の `EnableHttpSrv=1`)で、
     `HttpAccessControlList` に TVTest を動かす端末が含まれていること。
   * 「視聴に使用する BonDriver」(EpgTimerSrv 設定の視聴用 BonDriver 一覧)に、
     使いたいチューナーの BonDriver が登録されていること。ここに入っていない BonDriver は
     NetworkTV モードで使われません。
   * `Tools/tsreadex.exe` があること(Legacy WebUI の視聴機能と同じものを使います)。
   * **`HttpNumThreads` を増やしておくこと**。civetweb は 1 リクエストに 1 ワーカースレッドを
     割り当て、ライブ配信中はそれを占有し続けます。既定は **5** なので、TVTest を 2〜3 台
     つなぐと WebUI やスマホからのアクセスが応答しなくなります。`EpgTimerSrv.ini` の
     `[SET]` に `HttpNumThreads=16` などを追加してください(上限 50、変更後は EpgTimerSrv の再起動が必要)。

4. `EpgDataCap_Bon.ini` の `[SET] AllService=1` にする(設定画面の「全サービス出力」)。
   既定の `0` だと EpgDataCap_Bon が選局中のサービス以外を落としてしまい、TVTest 側で
   同じ TS 内のサービスを切り替えられません。この設定はネットワーク出力にしか効かないので
   **録画には影響しません**。反映されるのは次に EpgDataCap_Bon が起動したときからです。

EpgTimerSrv 自体の再起動は不要です(Lua スクリプトはリクエストごとに読まれます)。

### 2. クライアント側 (TVTest)

1. `BonDriver_EDCB.dll` と `BonDriver_EDCB.ini` を TVTest の BonDriver フォルダに置く。
2. `BonDriver_EDCB.ini` を編集する。

   ```ini
   [GLOBAL]
   SERVER_HOST="192.168.0.10"   ; EDCB のアドレス
   SERVER_PORT=5510
   STREAM_KEY=<BonStream.ini に書いたのと同じキー>
   NWTV_ID=0
   ```

3. TVTest を起動し、BonDriver として選ぶ。EDCB が持っているチャンネルが
   「地デジ / BS / CS / BS4K / SKY」のチューニング空間に分かれて並びます。
4. チャンネルスキャンを実行する。**「サービスを検索する」は ON のまま**にしてください。
   1 チャンネル = 1 トランスポートストリームなので、TS 内の各局はここで展開されます。

TVTest 側の B25 デコードは **無効** にしてください。EDCB が配信する時点で
スクランブルは解除済みです。選局直後の 0.5 秒ほどだけ、EDCB が鍵を得るまでの分が
スクランブルされたまま流れますが、これは通常の挙動です。

### 3. TVTest を複数同時に使う場合

EDCB は NetworkTV モードの ID ごとに別のチューナーを割り当てます。DLL と ini を
`BonDriver_EDCB0.dll` / `BonDriver_EDCB0.ini`、`BonDriver_EDCB1.dll` / … のようにコピーし、
それぞれ `NWTV_ID` を 0, 1, 2 … と変えてください(BonDriver_Proxy の InstanceID と同じ考え方です)。
同じ `NWTV_ID` を 2 つの TVTest で使うと、後から選局したほうにチューナーを取られます。

チューナー名は DLL のファイル名になるので、TVTest 上でもコピーごとに区別できます。
`IsTunerOpening()` 用のミューテックス名は「接続先サーバー + `NWTV_ID`」で決まるため、
`NWTV_ID` を分けたコピー同士がお互いを「使用中」と誤認することはありません。

## 設定 (BonDriver_EDCB.ini)

### [GLOBAL]

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `SERVER_HOST` | `localhost` | EpgTimerSrv のアドレス |
| `SERVER_PORT` | `5510` | `EpgTimerSrv.ini` の `HttpPort` |
| `ENDPOINT` | `bonstream` | `bonstream` = 付属の `bonstream.lua`、`view` = EDCB 標準の `view.lua` |
| `STREAM_KEY` | (空) | `bonstream.lua` の共有キー |
| `NWTV_ID` | `0` | NetworkTV モードの ID (0〜99) |
| `SERVICE_FILTER` | `0` | 1 で指定サービスのみに絞り込む(`bonstream` のときのみ) |
| `VIEW_OPTION` | `11` | `ENDPOINT=view` のときのトランスコード設定番号 |
| `EARLY_RESPONSE` | `1` | 1 でチューナーを開く前に HTTP ヘッダを返させる(選局の待ち時間対策) |
| `BASE_PATH` | `/legacy` | WebUI のパス |
| `CONNECT_TIMEOUT` | `1500` | 1 アドレスあたりの接続タイムアウト(ms) |

### [CHANNEL]

| キー | 既定値 | 説明 |
| --- | --- | --- |
| `GROUP_BY_TS` | `1` | 1 = 同じ (ONID, TSID) を 1 チャンネルにまとめる、0 = サービスごとに 1 チャンネル |
| `CHANNEL_NAME_MODE` | `1` | 1 = チャンネル名を番号にする、0 = その TS の最初のサービス名にする |
| `SPACE_MODE` | `0` | 0 = ONID で分類、1 = ネットワーク名で分類 |
| `SHOW_REMOTE_KEY` | `0` | 1 でチャンネル名の先頭にリモコン番号を付ける(`CHANNEL_NAME_MODE=0` のときのみ) |
| `SERVICE_TYPES` | `1,161,165,173` | 一覧に載せる service_type。`0` だけ書くと全部載せる |
| `INCLUDE_PARTIAL` | `0` | 1 でワンセグ(部分受信)サービスも載せる |

## 運用上の注意

### 録画予約が始まると視聴が切られる

EDCB は録画予約を最優先します。視聴に使っているチューナーで録画が始まると、
`CTunerBankCtrl` が NetworkTV モードを解除するため(EDCB の `EpgTimerSrv/TunerBankCtrl.cpp` の
`TR_NWTV` 分岐。条件が合えばチューナープロセスを録画へ引き継ぎ、合わなければ `CloseNWTV()`)、
その瞬間に HTTP ストリームが切れます。

本 BonDriver は自動再接続しません。ストリームが途切れたことは `WaitTsStream()` が
`WAIT_ABANDONED` を返して即座に通知するので、TVTest 側でチャンネルを選び直してください
(空きチューナーがあれば選局し直せます)。裏で予約が始まりそうな時間帯は、
EpgTimer で使用チューナーを確認しておくと安全です。

### 同時視聴数

* EDCB 側: NetworkTV モードは ID ごとに 1 チューナーを占有します。空きチューナーがなければ
  選局に失敗します(`EARLY_RESPONSE=1` のときは「選局できたが映像が来ない」形になります)。
* HTTP 側: 上記の `HttpNumThreads` の本数までしか同時にストリームを張れません。

### 選局にかかる時間

チューナーの起動待ちは EDCB 側の事情で決まります。実測:

| | `SetChannel()` | 映像が出るまで |
| --- | --- | --- |
| 地デジ / BS / CS | 15〜50ms | 約 4 秒(チャンネル変更は約 1.9 秒) |
| BS4K (BonDriver_dantto4k 経由) | 15〜50ms | 約 30 秒(チャンネル変更は約 9.4 秒) |

BS4K が極端に遅いのは EDCB 側が dantto4k のチューナープロセス起動と ACAS 初期化を
同期的に待つためで、プラグイン側では短縮できません。そのままだと視聴アプリの応答待ちを
超えてしまうため、既定の `EARLY_RESPONSE=1` ではチューナーを開く前に HTTP ヘッダを返させ、
`SetChannel()` 自体はすぐ返るようにしています。選局の成否を `SetChannel()` の
戻り値で正しく受け取りたい場合は 0 にしてください(BS4K では約 29.5 秒ブロックします)。

## 制限・未対応

* **HTTPS と認証に未対応**。EpgTimerSrv は `global_auth_file`(civetweb の Digest 認証)と
  https ポートに対応していますが、本 BonDriver は平文 HTTP のみです。LAN の外へ出す場合は
  VPN か、TLS 終端するリバースプロキシを別途用意してください。
* `bonstream.lua` の共有キーはクエリ文字列で平文送信されます。上と同じ理由で、
  信頼できないネットワークをまたぐ用途には向きません。
* チャンネルのインデックスは ChSet5 の並び順です。EDCB 側でチャンネルスキャンをやり直して
  並びが変わると、TVTest に保存済みのチャンネル設定とずれることがあります。
* `GetSignalLevel()` は実際の信号レベルではなく受信ビットレート(Mbps)を返します
  (BonDriver_Mirakurun と同じ挙動)。

## ビルド

Visual Studio 2022 以降 / C++20。x64・Win32 のどちらも通ります。
プラットフォームツールセットは `$(DefaultPlatformToolset)` にしてあるので、
インストールされている Visual Studio のもの(VS2022 なら v143、VS2026 なら v145)が使われます。
外部ライブラリは JSON パーサ(nlohmann/json)だけで、`thirdparty/` にソースごと同梱しています。
サブモジュールの取得は不要です(内訳は [thirdparty/README.md](thirdparty/README.md))。

```
msbuild BonDriver_EDCB.sln /p:Configuration=Release /p:Platform=x64
```

push と pull request のたびに GitHub Actions で x64 / x86 の両方をビルドしています
(`.github/workflows/build.yml`)。DLL・ini・`server/bonstream.lua`・ドキュメントをまとめたものが
各実行の Artifacts から取得できます。

## 動作確認 (test/bontest.cpp)

TVTest を起動しなくても、チャンネル一覧の取得・選局・TS 受信を確認できます。

```
cl /EHsc /std:c++20 /utf-8 test\bontest.cpp
```

```
bontest BonDriver_EDCB.dll                  … チャンネル一覧を表示
bontest BonDriver_EDCB.dll 1 0 10           … 空間1のチャンネル0を10秒受信
bontest BonDriver_EDCB.dll 1 0 10 3         … さらにチャンネル3へ切り替えてもう10秒受信
```

出力は UTF-8 です。コンソールで日本語を読みたい場合は先に `chcp 65001` してください。
環境変数 `BONDRIVER_EDCB_DEBUG` を設定すると、BonDriver 側のログ(選局 URL、HTTP ステータス、
各段階の所要時間)も標準エラー出力に出ます。TVTest から使うときは DebugView で同じログが見えます。

## License

MIT License. `LICENSE` を参照してください。
`thirdparty/` に同梱している第三者のソースは別の条件が適用されます
([thirdparty/README.md](thirdparty/README.md))。
