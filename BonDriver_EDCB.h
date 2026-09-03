#define _WINSOCK_DEPRECATED_NO_WARNINGS

#define _PCH_STATIC_CONST

#include <xstring>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <InitGuid.h>
#include "IBonDriver2.h"
#include "thirdparty/nlohmann/json.hpp"

using namespace std;
using json = nlohmann::json;

#if !defined(_BONTUNER_H_)
#define _BONTUNER_H_

#if _MSC_VER > 1000
#pragma once
#endif // _MSC_VER > 1000

#define dllimport dllexport

#define TUNER_NAME "BonDriver_EDCB"
#define TUNER_NAME_W L"BonDriver_EDCB"

// 受信サイズ
#define TSDATASIZE	48128	// TSデータのサイズ 188 * 256

// 配信エンドポイントの種別
enum EEndpoint {
	ENDPOINT_BONSTREAM = 0,		// legacy/bonstream.lua (本プロジェクト付属。EITを落とさない)
	ENDPOINT_VIEW = 1,			// legacy/view.lua (EDCB標準。EITが落ちる)
};

// チューニング空間
struct TSpace
{
	std::wstring name;
	std::wstring prefix;		// 番号形式のチャンネル名に使う接頭辞
	int rank;					// 表示順(小さいほど手前)
	std::vector<size_t> channels;	// g_Channels のインデックス
};

// チャンネル(EDCBのサービス1つ)
struct TChannel
{
	std::wstring name;
	int onid;
	int tsid;
	int sid;
};

extern std::vector<TSpace> g_Spaces;
extern std::vector<TChannel> g_Channels;

#define MAX_HOST_LEN	256
#define MAX_PORT_LEN	8
#define MAX_KEY_LEN		128

class CBonTuner : public IBonDriver2
{
public:
	CBonTuner();
	virtual ~CBonTuner();

	// チャンネル一覧の取得
	void InitChannel(void);

	// IBonDriver
	const BOOL OpenTuner(void) override;
	void CloseTuner(void) override;

	const BOOL SetChannel(const BYTE bCh) override;
	const float GetSignalLevel(void) override;

	const DWORD WaitTsStream(const DWORD dwTimeOut = 0) override;
	const DWORD GetReadyCount(void) override;

	const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain) override;
	const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain) override;

	void PurgeTsStream(void) override;

	// IBonDriver2
	LPCWSTR GetTunerName(void) override;

	const BOOL IsTunerOpening(void) override;

	LPCWSTR EnumTuningSpace(const DWORD dwSpace) override;
	LPCWSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel) override;

	const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel) override;

	const DWORD GetCurSpace(void) override;
	const DWORD GetCurChannel(void) override;

	void Release(void) override;

	static CBonTuner * m_pThis;
	static HINSTANCE m_hModule;

protected:
	// I/Oリクエストキューデータ
	struct AsyncIoReq
	{
		WSAOVERLAPPED OverLapped;
		DWORD dwState;
		DWORD dwRxdSize;
		BYTE RxdBuff[TSDATASIZE];
		AsyncIoReq *pNext;
	};

	AsyncIoReq * AllocIoReqBuff(const DWORD dwBuffNum);
	void FreeIoReqBuff(AsyncIoReq *pBuff);

	static DWORD WINAPI PushIoThread(LPVOID pParam);
	static DWORD WINAPI PopIoThread(LPVOID pParam);

	const BOOL PushIoRequest(SOCKET sock);
	const BOOL PopIoRequest(SOCKET sock);

	// ヘッダより後に読み過ぎた分をリングバッファへ積む
	void PreloadStreamData(const std::string &data);

	// ストリームが途切れたことを WaitTsStream() の待ち手に知らせる
	void AbortStream();

	bool m_bTunerOpen;
	bool m_bWsaInit;

	HANDLE m_hMutex;

	// SetChannel()/CloseTuner()の多重・再入呼び出しに対する排他制御
	// (同一スレッドからの再入はCRITICAL_SECTIONの性質上ブロックしない)
	CRITICAL_SECTION m_ChannelLock;

	AsyncIoReq *m_pIoReqBuff;
	AsyncIoReq *m_pIoPushReq;
	AsyncIoReq *m_pIoPopReq;
	AsyncIoReq *m_pIoGetReq;

	DWORD m_dwBusyReqNum;
	DWORD m_dwReadyReqNum;

	HANDLE m_hPushIoThread;
	HANDLE m_hPopIoThread;
	BOOL m_bLoopIoThread;

	HANDLE m_hOnStreamEvent;

	CRITICAL_SECTION m_CriticalSection;

	DWORD m_dwCurSpace;
	DWORD m_dwCurChannel;

	SOCKET m_sock;
	float m_fBitRate;

	void CalcBitRate();
	DWORD m_dwRecvBytes;
	ULONGLONG m_u64LastCalcTick;
};

#endif // !defined(_BONTUNER_H_)
