"""Run the actual bonstream.lua against stubbed EDCB APIs (requires lupa)."""
import pathlib
from lupa import LuaRuntime


SOURCE = (pathlib.Path(__file__).resolve().parents[1] / "server" / "bonstream.lua").read_text(encoding="utf-8")

STUBS = r'''
closed={}
commands={}
responses={}
currentID=101
WIN32=false
POPEN_BINARY=''
mg={
  script_name='server/bonstream.lua',
  request_info={query_string='',request_method='GET'},
  get_var=function(q,k) return k=='key' and 'test' or nil end,
  write=function(s) table.insert(responses,s);return true end
}
dofile=function() end -- util.luaのAPIだけを以下で模倣する
Response=function(code) return tostring(code)..'\r\n' end
GetVarInt=function(q,k)
  if k=='n' then return 0 end
  if k=='early' and early then return 1 end
end
GetVarServiceID=function() return 4,16400,101 end
FindToolsCommand=function() return 'tsreadex' end
QuoteCommandArgForPath=function(s) return s end
SendTSTCPPipePath=function(s) return s end
TerminateCommandlineLike=function() end
EdcbModulePath=function() return '/edcb' end
PathAppend=function(a,b) return a..'/'..b end
edcb={
  GetPrivateProfile=function() return 'test' end,
  OpenNetworkTV=function()
    if openFails then return false end
    return true,123,101
  end,
  FindFile=function()
    if pipeFails then currentID=replacementID or 101;return nil end
    return {{name='SendTSTCP_0_123'}}
  end,
  Sleep=function() end,
  IsOpenNetworkTV=function() return currentID~=nil,123,currentID end,
  CloseNetworkTV=function(n) table.insert(closed,{n=n,id=currentID});currentID=nil end,
  io={popen=function()
    currentID=replacementID or 101
    if readerFails then return nil end
    return {read=function() return nil end,close=function() end}
  end},
  os={execute=function(cmd) table.insert(commands,cmd) end}
}
'''


def run(name, settings, close_count, status=None, delayed=False):
    lua = LuaRuntime()
    lua.execute(STUBS)
    for key, value in settings.items():
        lua.globals()[key] = value
    lua.execute(SOURCE)
    state = lua.globals()
    assert len(state.closed) == close_count, f"{name}: closed another request's session"
    if close_count:
        assert state.closed[1]["id"] == 101, f"{name}: wrong openID closed"
    if status:
        assert state.responses[1].startswith(str(status)), f"{name}: wrong status"
    assert bool(len(state.commands)) == delayed, f"{name}: unexpected delayed cleanup"
    if delayed:
        assert 'openID==101' in state.commands[1], f"{name}: delayed cleanup lost its openID guard"
    print(f"{name}: PASS")


if __name__ == "__main__":
    run("current reader failure", {"readerFails": True}, 1, 503)
    run("replaced reader failure", {"readerFails": True, "replacementID": 102}, 0, 503)
    run("current pipe failure", {"pipeFails": True}, 1, 503)
    run("replaced pipe failure", {"pipeFails": True, "replacementID": 102}, 0, 503)
    run("early replaced failure", {"early": True, "readerFails": True, "replacementID": 102}, 0, 200)
    run("open failure", {"openFails": True}, 0, 503)
    run("current normal end", {}, 0, 200, delayed=True)
    run("replaced normal end", {"replacementID": 102}, 0, 200)
