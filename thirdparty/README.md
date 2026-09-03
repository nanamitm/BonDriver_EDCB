# 同梱している第三者のソース

ビルドにサブモジュールや外部の取得を必要としないよう、以下をソースごと同梱しています。
本体(`BonDriver_EDCB.cpp` など)の MIT ライセンスとは別に、それぞれの条件が適用されます。

## nlohmann/json (`nlohmann/json.hpp`)

* JSON for Modern C++ version 3.12.0
* <https://github.com/nlohmann/json>
* SPDX-FileCopyrightText: 2013 - 2025 Niels Lohmann
* SPDX-License-Identifier: **MIT**

ライセンス条文はヘッダ先頭のコメントに含まれています。EDCB の API が返す JSON の
パースにだけ使っています。

## binzume (`binzume/http.h`, `binzume/socket.h`)

* 小さな HTTP クライアント / ソケットラッパー
* [BonDriver_Mirakurun](https://github.com/nanamitm/BonDriver_Mirakurun)(さらにその元は
  BonDriver_HTTP)から引き継いだもので、本プロジェクトでは無改変です
* **ファイルにライセンス表記がありません**

チャンネル一覧の取得(`api/EnumService` と `bonstream.lua?list=1`)にだけ使っています。
ストリームの受信はこのライブラリを通さず、`BonDriver_EDCB.cpp` が Winsock を直接叩いています。

公開前に、上流での扱いを確認して必要ならライセンス表記を補ってください。
自前の実装に置き換える場合、使っているのは `HttpClient::get()` による
「URL を GET してステータスとボディを得る」だけなので、差し替えは容易です。
