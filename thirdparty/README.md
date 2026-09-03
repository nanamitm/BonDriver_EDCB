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

---

同梱物はこれだけです。HTTP クライアントも以前は第三者のヘッダ(BonDriver_Mirakurun
から引き継いだ `binzume/http.h`・`socket.h`)を使っていましたが、ライセンス表記が
無かったため `BonDriver_EDCB.cpp` の `HttpGet()` として自前で実装し直しました。
