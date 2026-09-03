-- vim:set ft=lua:
-- BonDriver_EDCB 用のライブストリーム配信スクリプト
--
-- EDCB の NetworkTV モードを開始し、EpgDataCap_Bon が SendTSTCP に出力する
-- 名前付きパイプの内容を、そのまま HTTP で垂れ流す。
--
-- view.lua との違い:
--   * トランスコーダを一切通さない (常に MPEG2-TS のまま)
--   * tsreadex に -x を渡さないので EIT (PID 0x12/0x26/0x27) が除去されない
--     → TVTest 側で番組情報・EPG が取得できる
--   * 既定でサービス絞り込みをしない (tsreadex に -n を渡さない) ので、パイプに
--     複数サービスが流れていれば TVTest 側でサービス切り替えができる
--     ※tsreadex の -n -1 は「絞り込みなし」ではなく先頭プログラムだけを残す指定なので、
--       全サービスを通したい場合はオプションごと省く必要がある
--     ※パイプ自体を全サービスにするには EpgDataCap_Bon.ini の [SET] AllService=1
--   * ブラウザから呼ぶものではないので CSRF トークンではなく共有キーで認証する
--
-- 設置場所: EDCB/HttpPublic/legacy/bonstream.lua
-- 共有キー: EDCB/Setting/BonStream.ini に
--             [BONSTREAM]
--             Key=<英数字の共有キー>
--           を書く (Key が未設定のときは常に 403 を返す)
--
-- クエリパラメータ:
--   key    共有キー (必須)
--   list   1 のときチャンネル一覧(ChSet5)をJSONで返す
--   n      NetworkTV モードのID (0～99)。BonDriver のインスタンスごとに変える
--   id     "<ONID>-<TSID>-<SID>"
--   sv     1 のとき指定サービスだけに絞り込む (既定 0 = 絞り込まない)
--   early  1 のときチューナーを開く前にHTTPヘッダを返す
--          (BS4Kのように edcb.OpenNetworkTV() が30秒近くブロックする環境で、
--           BonDriver側のSetChannel()が視聴アプリのタイムアウトを超えないようにするため。
--           かわりに選局失敗も200になり、「成功したがデータが来ない」ように見える)
--   close  1 のとき n のNetworkTVモードを終了して 204 を返す (ストリームは返さない)

dofile(mg.script_name:gsub('[^\\/]*$','')..'util.lua')

BONSTREAM_INI='Setting/BonStream.ini'

query=mg.request_info.query_string or ''

-- 認証。キーが未設定の環境では機能そのものを無効にする
-- (ブラウザからの意図しないアクセスでチューナーを掴まれるのを防ぐため)
streamKey=edcb.GetPrivateProfile('BONSTREAM','Key','',BONSTREAM_INI)
if streamKey=='' or mg.get_var(query,'key')~=streamKey then
  mg.write(Response(403,nil,nil,0)..'\r\n')
  return
end

-- 文字列をJSONの文字列リテラルにする
function JsonString(s)
  return '"'..s:gsub('[%c"\\]',function(c)
    if c=='"' then return '\\"' end
    if c=='\\' then return '\\\\' end
    if c=='\n' then return '\\n' end
    if c=='\r' then return '\\r' end
    if c=='\t' then return '\\t' end
    return ('\\u%04x'):format(c:byte())
  end)..'"'
end

if GetVarInt(query,'list')==1 then
  -- チャンネル一覧(ChSet5)を返す。
  -- api/EnumService と違いEPGの有無に依存しないので、EPGを取得していない
  -- チャンネルや、EPG未対応のチューナーのチャンネルも漏れなく列挙できる
  edcb.htmlEscape=0
  local ct=CreateContentBuilder()
  ct:Append('{"items":[')
  for i,v in ipairs(edcb.GetChDataList()) do
    ct:Append((i>1 and ',' or '')..'{"onid":'..v.onid
      ..',"tsid":'..v.tsid
      ..',"sid":'..v.sid
      ..',"serviceType":'..v.serviceType
      ..',"partialFlag":'..(v.partialFlag and 'true' or 'false')
      ..',"remoconID":'..v.remoconID
      ..',"serviceName":'..JsonString(v.serviceName)
      ..',"networkName":'..JsonString(v.networkName)..'}')
  end
  ct:Append(']}')
  ct:Finish()
  mg.write(ct:Pop(Response(200,'application/json','utf-8',ct.len,ct.gzip)..'\r\n'))
  return
end

n=GetVarInt(query,'n',0,99)
onid,tsid,sid=GetVarServiceID(query,'id')
serviceOnly=GetVarInt(query,'sv')==1
early=GetVarInt(query,'early')==1

if not n then
  mg.write(Response(400,nil,nil,0)..'\r\n')
  return
end

if GetVarInt(query,'close')==1 then
  -- NetworkTVモードを終了する
  edcb.CloseNetworkTV(n)
  mg.write(Response(204,nil,nil,0)..'\r\n')
  return
end

if not onid or (onid==0 and tsid==0 and sid==0) then
  mg.write(Response(400,nil,nil,0)..'\r\n')
  return
end

-- 名前付きパイプをそのまま転送するプロセスを起動する
-- (tsreadex はパイプの接続待ち・切断検知・パケット単位の整形を面倒見てくれるので
--  Lua から直接パイプを読むのではなくこれを通す。view.lua と違い -x は渡さない)
function OpenReader(pipeName,searchName,nwtvclose,targetSID)
  local tsreadex=FindToolsCommand('tsreadex')
  -- targetSIDがないときは -n 自体を渡さない
  -- (tsreadexの -n -1 は「絞り込みなし」ではなく先頭プログラムだけを残す指定なので、
  --  全サービスを通したい場合はオプションごと省く必要がある)
  local cmd=tsreadex..' -z edcb-bonstream-'..searchName..' -t 10 -m 2'
    ..(targetSID and ' -n '..targetSID or '')
    ..' '..QuoteCommandArgForPath(SendTSTCPPipePath(pipeName,0))
  return edcb.io.popen(WIN32 and '"'..cmd..'"' or cmd,'r'..POPEN_BINARY)
end

-- レスポンスヘッダは1回だけ書く
headerSent=false
function SendHeader()
  if not headerSent then
    headerSent=true
    mg.write(Response(200,'video/mp2t')..'Content-Disposition: attachment; filename=bonstream.m2t\r\n\r\n')
  end
end

f=nil
myOpenID=nil

if sid==0 then
  -- NetworkTVモードを終了
  edcb.CloseNetworkTV(n)
  mg.write(Response(204,nil,nil,0)..'\r\n')
  return
end

if early then
  -- チューナーを開く前に応答しておく
  SendHeader()
end

-- 前回のプロセスが残っていたら終わらせる
TerminateCommandlineLike('tsreadex',' -z edcb-bonstream-nwtv-'..n..' ')

-- NetworkTVモードを開始
ok,pid,myOpenID=edcb.OpenNetworkTV(2,onid,tsid,sid,n)
if ok then
  -- 名前付きパイプができるまで待つ
  pipeName=nil
  for i=1,50 do
    ff=edcb.FindFile(SendTSTCPPipePath('*_'..pid,0),1)
    if ff and ff[1].name:find('^[^_]+_%d+_%d+') then
      pipeName=ff[1].name:match('^[^_]+_(%d+_)%d+')..pid
      break
    elseif WIN32 and i%10==0 then
      -- FindFileで見つけられない環境があるかもしれないのでポートを予想して開いてみる
      for j=0,29 do
        ff=edcb.io.open(SendTSTCPPipePath(j..'_'..pid,0),'rb')
        if ff then
          ff:close()
          -- 再び開けるようになるまで少しラグがある
          edcb.Sleep(2000)
          pipeName=j..'_'..pid
          break
        end
      end
      if ff then break end
    end
    edcb.Sleep(200)
  end
  if pipeName then
    f=OpenReader(pipeName,'nwtv-'..n,{n,myOpenID},serviceOnly and sid or nil)
  end
  if not f then
    edcb.CloseNetworkTV(n)
  end
end

if not f then
  -- ヘッダを先に返している場合は、ボディなしで閉じるしかない
  if not headerSent then
    mg.write(Response(503,nil,nil,0)..'\r\n')
  end
  return
end

SendHeader()
if mg.request_info.request_method~='HEAD' then
  while true do
    buf=f:read(188*128)
    if not buf or #buf==0 then
      -- 終端に達した
      break
    end
    if not mg.write(buf) then
      -- キャンセルされた
      break
    end
  end
end
f:close()

-- NetworkTVモードを終了する
-- チャンネル変更のための張り直しを妨げないよう、view.lua と同じく少し遅らせる。
-- 遅延中に同じIDで開き直された場合は openID が変わるので終了しない
ok,pid,openID=edcb.IsOpenNetworkTV(n)
if ok and openID==myOpenID then
  edcb.os.execute((WIN32 and 'start "" /b cmd /s /c "timeout 5 & cd /d "'..EdcbModulePath()..'" && .\\EpgTimerSrv.exe /luapost ' or '(sleep 5 ; echo "')
    ..'ok,pid,openID=edcb.IsOpenNetworkTV('..n..');if(ok)and(openID=='..(myOpenID or 'nil')..')then;edcb.CloseNetworkTV('..n..');end'
    ..(WIN32 and '"' or '" >>"'..PathAppend(EdcbModulePath(),'EpgTimerSrvLuaPost.fifo')..'") &'))
end
