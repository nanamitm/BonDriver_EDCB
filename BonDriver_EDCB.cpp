#include <string>
#include <stdexcept>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <strsafe.h>

#include "BonDriver_EDCB.h"

#pragma comment(lib, "ws2_32.lib")

//////////////////////////////////////////////////////////////////////
// 定数定義
//////////////////////////////////////////////////////////////////////

#define BITRATE_CALC_TIME	500		//ms

// ミューテックス名とチューナー名は Init() で組み立てる(下の g_MutexName / g_TunerName)

// FIFOバッファ設定
// EDCBが配信するのは(4K/8Kを含めても)MPEG2-TSなので、想定ピークレートは
// 48Mbps・バッファ長3秒あれば足りる(約18MB)
#define ASYNCBUFFTIME			3										// バッファ長 = 3秒
#define ASYNCBUFF_ASSUMED_BPS	( 48 * 1024 * 1024 / 8 )				// 想定ピークレート(バイト/秒) = 48Mbps
#define ASYNCBUFFSIZE			( ASYNCBUFF_ASSUMED_BPS / TSDATASIZE * ASYNCBUFFTIME )

#define REQRESERVNUM		8				// 非同期リクエスト予約数
#define REQPOLLINGWAIT		20				// 非同期リクエストポーリング間隔(ms)

// 非同期リクエスト状態
#define IORS_IDLE			0x00			// リクエスト空
#define IORS_BUSY			0x01			// リクエスト受信中
#define IORS_RECV			0x02			// 受信完了、ストア待ち

// EDCBはリクエストを受けてから EpgDataCap_Bon の起動・選局・スクランブル解除まで
// 同期的に行うため、最初のバイトが返るまで数秒かかる(実測: 空きチューナーがある
// 状態で約1.9秒、他の用途からチューナーを開け直す場合はさらに伸びる)。
// TVTest(LibISDB)側のBonDriver応答待ちは10秒固定でこちらから変更できないが、
// 途中で見切って接続を張り直すとサーバー側で後始末が走りかえって遅くなるため、
// 1回の接続で十分長く待つ。
#define HTTP_STATUS_TIMEOUT_MS			30000			// HTTPステータス受信タイムアウト(ms)

//////////////////////////////////////////////////////////////////////
// グローバル
//////////////////////////////////////////////////////////////////////

static char g_IniFilePath[MAX_PATH] = { '\0' };

static char g_ServerHost[MAX_HOST_LEN];
static char g_ServerPort[MAX_PORT_LEN];
static char g_StreamKey[MAX_KEY_LEN];
static char g_BasePath[MAX_PATH];
static int g_Endpoint = ENDPOINT_BONSTREAM;
static int g_NwtvID = 0;
static int g_ServiceFilter = 0;
static int g_ViewOption = 11;
static int g_SpaceMode = 0;
static int g_ShowRemoteKey = 0;
static int g_IncludePartial = 0;
static int g_GroupByTs = 1;
static int g_ChannelNameMode = 1;
static int g_EarlyResponse = 1;

// 実際に取り合うのは「どのEDCBのどのNetworkTVモードID」なので、
// ミューテックス名にはサーバーとIDを含める。DLLをコピーして
// NWTV_IDを分けた場合に、別インスタンス同士が使用中と誤認しないようにする
static wchar_t g_MutexName[256] = TUNER_NAME_W;
// チューナー名。DLLのファイル名から作る(コピーして使う場合の区別用)
static wchar_t g_TunerName[128] = TUNER_NAME_W;
static int g_ConnectTimeout = 1500;
static std::vector<int> g_ServiceTypes;

std::vector<TSpace> g_Spaces;
std::vector<TChannel> g_Channels;

//////////////////////////////////////////////////////////////////////
// Tools
//////////////////////////////////////////////////////////////////////

// CRITICAL_SECTIONのRAIIガード(途中returnの多い関数でも確実にLeaveする)
class CAutoLock
{
public:
	explicit CAutoLock(CRITICAL_SECTION &cs) : m_cs(cs) { ::EnterCriticalSection(&m_cs); }
	~CAutoLock() { ::LeaveCriticalSection(&m_cs); }
	CAutoLock(const CAutoLock &) = delete;
	CAutoLock &operator=(const CAutoLock &) = delete;
private:
	CRITICAL_SECTION &m_cs;
};

inline ULONGLONG DiffTime(ULONGLONG BeginTime, ULONGLONG EndTime)
{
	if (BeginTime <= EndTime)
		return EndTime - BeginTime;
	return (ULLONG_MAX - BeginTime) + EndTime + 1ULL;
}

// UTF-8 -> wstring
static std::wstring utf8_to_wstring(const std::string &utf8)
{
	int buf_size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);	// NULL文字を含むサイズ

	if (buf_size > 0) {
		std::wstring ret(buf_size - 1, L'\0');

		// C++20から*(data()+size())をNULL文字で上書きすることが認められる
		if (::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, ret.data(), buf_size) == buf_size) {
			return ret;
		}
	}
	return std::wstring();
}

// 環境変数 BONDRIVER_EDCB_DEBUG が設定されているときは標準エラー出力にも書く
// (DebugView を使わずに testontest.exe などで動作を追えるようにするため)
static bool DebugToStderr()
{
	static const bool enabled = ::GetEnvironmentVariableA("BONDRIVER_EDCB_DEBUG", NULL, 0) != 0;
	return enabled;
}

static void DebugOutA(const char *format, ...)
{
	char szDebugOut[512];
	va_list args;
	va_start(args, format);
	::StringCbVPrintfA(szDebugOut, sizeof(szDebugOut), format, args);
	va_end(args);
	::OutputDebugStringA(szDebugOut);

	if (DebugToStderr()) {
		fputs(szDebugOut, stderr);
		fflush(stderr);
	}
}

// カンマ区切りの整数リストを読む
static void ParseIntList(const char *s, std::vector<int> &list)
{
	list.clear();
	while (*s) {
		while (*s == ' ' || *s == ',') s++;
		if (!*s) break;
		list.push_back(atoi(s));
		while (*s && *s != ',') s++;
	}
}

// ONIDからチューニング空間名と表示順を決める
// (EDCBはBonDriverをまたいだサービス一覧を返すため、Mirakurunのtypeに相当する
//  情報がない。ネットワーク種別から機械的に振り分ける)
static void ClassifyOnid(int onid, const std::string &networkName, std::wstring &spaceName, std::wstring &prefix, int &rank)
{
	if (onid >= 0x7880 && onid <= 0x7FFF) {
		spaceName = L"地デジ";
		prefix = L"GR";
		rank = 0;
	} else if (onid == 4) {
		spaceName = L"BS";
		prefix = L"BS";
		rank = 1;
	} else if (onid == 6 || onid == 7) {
		spaceName = L"CS";
		prefix = L"CS";
		rank = 2;
	} else if (onid == 11 || onid == 12) {
		// 高度BS(BS4K) / 高度広帯域CS
		spaceName = L"BS4K";
		prefix = L"BS4K";
		rank = 3;
	} else if (onid == 10) {
		spaceName = L"SKY";
		prefix = L"SKY";
		rank = 4;
	} else {
		spaceName = networkName.empty() ? L"OTHER" : utf8_to_wstring(networkName);
		prefix = L"CH";
		rank = 5;
	}
}

// CS110(東経110度CSデジタル)の中継器番号(ND番号)をTSIDから求める
// (TSIDの下位12bitの上位8bitがそのままND番号。実データでも
//  0x6020→ND2、0x60A0→ND10、0x7180→ND24 と一致する)
static bool IsCs110(int onid)
{
	return onid == 6 || onid == 7;
}

static int Cs110NdNumber(int tsid)
{
	return (tsid & 0x0FFF) >> 4;
}

// HTTPレスポンスのステータスコードを取得する
// (ヘッダ終端(\r\n\r\n)まで読み進める。ヘッダより後ろに読み過ぎた分は
//  leftoverへ返し、呼び出し元でリングバッファの先頭に積む。捨てると
//  TSパケットの先頭が欠けて再同期が必要になるため)
// 戻り値: ステータスラインの受信・パースに成功したらtrue、タイムアウトや切断ならfalse
static bool RecvHttpStatusCode(SOCKET sock, int &statusCode, DWORD dwTimeoutMs, std::string &leftover)
{
	statusCode = 0;
	leftover.clear();

	DWORD dwOldTimeout = 0;
	int nOldTimeoutLen = sizeof(dwOldTimeout);
	::getsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&dwOldTimeout, &nOldTimeoutLen);
	::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwTimeoutMs, sizeof(dwTimeoutMs));

	std::string header;
	char buf[4096];
	size_t endPos = std::string::npos;

	// ヘッダ終端が見つかるまで受信する(異常に長いヘッダは打ち切る)
	while (header.size() < 16384) {
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		header.append(buf, n);
		endPos = header.find("\r\n\r\n");
		if (endPos != std::string::npos) {
			break;
		}
	}

	::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwOldTimeout, sizeof(dwOldTimeout));

	if (endPos == std::string::npos) {
		return false;
	}

	leftover = header.substr(endPos + 4);

	// ステータスライン("HTTP/1.1 200 OK")をパース
	size_t space1 = header.find(' ');
	if (space1 == std::string::npos) {
		return false;
	}

	statusCode = atoi(header.c_str() + space1 + 1);

	return statusCode != 0;
}

// Hostヘッダに入れる "ホスト:ポート"
static std::string ServerHostPort()
{
	return std::string(g_ServerHost) + ":" + std::string(g_ServerPort);
}

// 設定されたEDCBに対してHTTP GETし、ステータスコードとボディを得る
// (実体は下のほう。ConnectToServer()を使うのでその定義より後ろに置いている)
static bool HttpGet(const std::string &path, int &statusCode, std::string &body, DWORD dwTimeoutMs = 10000);

// "<ONID>-<TSID>-<SID>"
static std::string ChannelIdString(const TChannel &ch)
{
	return std::to_string(ch.onid) + "-" + std::to_string(ch.tsid) + "-" + std::to_string(ch.sid);
}

// view.lua に必要なCSRF対策トークンを取得する
// (トークンはサーバー側のランダム値と時刻から作られるためクライアントでは計算できない。
//  トークンを平文で返す stream.lua(外部プレーヤー用の.m3u生成)から拾う)
static std::string FetchCsrfToken(const TChannel &ch)
{
	std::string path = std::string(g_BasePath) + "/stream.lua?n=" + std::to_string(g_NwtvID)
		+ "&id=" + ChannelIdString(ch);

	int status = 0;
	std::string body;
	if (!HttpGet(path, status, body) || status != 200) {
		DebugOutA("%s: FetchCsrfToken() failed. status = %d\n", TUNER_NAME, status);
		return std::string();
	}

	size_t p = body.find("ctok=");
	if (p == std::string::npos) {
		DebugOutA("%s: FetchCsrfToken() no token in response\n", TUNER_NAME);
		return std::string();
	}
	p += 5;
	size_t e = p;
	while (e < body.size() && (isalnum((unsigned char)body[e]) || body[e] == '%')) {
		e++;
	}
	return body.substr(p, e - p);
}

// 直前に接続できたアドレスファミリ(AF_INET/AF_INET6)
// (EDCBのHTTPサーバーがIPv4だけで待ち受けている構成では、"localhost"が先に
//  ::1へ解決されるとIPv6側の接続失敗を待つ分だけ選局が遅くなる。一度成功した
//  ファミリを覚えておき、次回はそちらから試す)
static int g_LastGoodFamily = AF_UNSPEC;

// タイムアウト付きでTCP接続する
static SOCKET ConnectWithTimeout(const struct addrinfo *ai, DWORD dwTimeoutMs)
{
	SOCKET sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
	if (sock == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}

	// 非ブロッキングにしてconnect()の待ち時間を自前で制御する
	u_long nonBlocking = 1;
	::ioctlsocket(sock, FIONBIO, &nonBlocking);

	bool connected = false;
	if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
		connected = true;
	} else if (WSAGetLastError() == WSAEWOULDBLOCK) {
		fd_set writeSet, exceptSet;
		FD_ZERO(&writeSet);
		FD_ZERO(&exceptSet);
		FD_SET(sock, &writeSet);
		FD_SET(sock, &exceptSet);

		timeval tv;
		tv.tv_sec = dwTimeoutMs / 1000;
		tv.tv_usec = (dwTimeoutMs % 1000) * 1000;

		if (select(0, NULL, &writeSet, &exceptSet, &tv) > 0 && FD_ISSET(sock, &writeSet)) {
			int soError = 0;
			int len = sizeof(soError);
			if (::getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&soError, &len) == 0 && soError == 0) {
				connected = true;
			}
		}
	}

	// ブロッキングに戻す(以降の送受信は同期的に扱う)
	nonBlocking = 0;
	::ioctlsocket(sock, FIONBIO, &nonBlocking);

	if (!connected) {
		closesocket(sock);
		return INVALID_SOCKET;
	}

	return sock;
}

// EDCBのHTTPサーバーへ接続する
static SOCKET ConnectToServer()
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;

	// AF_UNSPECにしてIPv6/IPv4の候補をすべて受け取る
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_NUMERICSERV;
	if (getaddrinfo(g_ServerHost, g_ServerPort, &hints, &res) != 0) {
		DebugOutA("%s: ConnectToServer() getaddrinfo failed. error = %d\n", TUNER_NAME, WSAGetLastError());
		return INVALID_SOCKET;
	}

	// 前回成功したファミリを先に試す
	std::vector<const struct addrinfo *> candidates;
	for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		if (ai->ai_family == g_LastGoodFamily) {
			candidates.push_back(ai);
		}
	}
	for (const struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		if (ai->ai_family != g_LastGoodFamily) {
			candidates.push_back(ai);
		}
	}

	SOCKET sock = INVALID_SOCKET;
	for (const struct addrinfo *ai : candidates) {
		sock = ConnectWithTimeout(ai, (DWORD)g_ConnectTimeout);
		if (sock != INVALID_SOCKET) {
			g_LastGoodFamily = ai->ai_family;
			break;
		}
	}

	freeaddrinfo(res);

	if (sock == INVALID_SOCKET) {
		DebugOutA("%s: ConnectToServer() connection error %d\n", TUNER_NAME, WSAGetLastError());
	}

	return sock;
}

// 設定されたEDCBに対してHTTP GETし、ステータスコードとボディを得る
// (チャンネル一覧とCSRFトークンの取得にだけ使う小さなクライアント。
//  ストリームの受信はこれを通さず、SetChannel()が直接ソケットを読む)
// 戻り値: ステータス行を解釈できたらtrue(2xx以外でもtrue。statusCodeで判断すること)
static bool HttpGet(const std::string &path, int &statusCode, std::string &body, DWORD dwTimeoutMs)
{
	// 壊れた応答でメモリを食い潰さないための上限
	const size_t MAX_RESPONSE_SIZE = 8 * 1024 * 1024;

	statusCode = 0;
	body.clear();

	SOCKET sock = ConnectToServer();
	if (sock == INVALID_SOCKET) {
		return false;
	}

	::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&dwTimeoutMs, sizeof(dwTimeoutMs));
	::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char *)&dwTimeoutMs, sizeof(dwTimeoutMs));

	// HTTP/1.0 + Connection: close なので、応答は接続が閉じるまで読めばよい
	const std::string request = "GET " + path + " HTTP/1.0\r\n"
		"Host: " + ServerHostPort() + "\r\n"
		"User-Agent: " TUNER_NAME "\r\n"
		"Accept-Encoding: identity\r\n"
		"Connection: close\r\n"
		"\r\n";

	if (send(sock, request.c_str(), (int)request.length(), 0) < 0) {
		DebugOutA("%s: HttpGet() send error %d\n", TUNER_NAME, WSAGetLastError());
		closesocket(sock);
		return false;
	}

	std::string response;
	char buf[8192];
	for (;;) {
		int n = recv(sock, buf, sizeof(buf), 0);
		if (n <= 0) {
			break;
		}
		response.append(buf, n);
		if (response.size() > MAX_RESPONSE_SIZE) {
			DebugOutA("%s: HttpGet() response too large\n", TUNER_NAME);
			closesocket(sock);
			return false;
		}
	}
	closesocket(sock);

	// ヘッダとボディに分ける
	size_t headerEnd = response.find("\r\n\r\n");
	if (headerEnd == std::string::npos) {
		DebugOutA("%s: HttpGet() incomplete response (%u bytes)\n", TUNER_NAME, (unsigned)response.size());
		return false;
	}
	const std::string header = response.substr(0, headerEnd);
	body = response.substr(headerEnd + 4);

	// ステータスライン("HTTP/1.1 200 OK")
	size_t sp = header.find(' ');
	if (sp == std::string::npos) {
		return false;
	}
	statusCode = atoi(header.c_str() + sp + 1);
	if (statusCode == 0) {
		return false;
	}

	// ヘッダ名は大文字小文字を区別しないので、探す前に小文字にしておく
	std::string lower = header;
	std::transform(lower.begin(), lower.end(), lower.begin(),
	               [](char c) { return (char)tolower((unsigned char)c); });

	// リバースプロキシを挟むとチャンク転送で返ってくることがある
	if (lower.find("\r\ntransfer-encoding:") != std::string::npos &&
	    lower.find("chunked", lower.find("\r\ntransfer-encoding:")) != std::string::npos) {
		std::string decoded;
		size_t pos = 0;
		for (;;) {
			size_t eol = body.find("\r\n", pos);
			if (eol == std::string::npos) {
				break;
			}
			// チャンクサイズは16進。拡張(";"以降)は読み飛ばす
			const size_t size = strtoul(body.substr(pos, eol - pos).c_str(), NULL, 16);
			if (size == 0) {
				break;
			}
			pos = eol + 2;
			if (pos + size > body.size()) {
				DebugOutA("%s: HttpGet() truncated chunk\n", TUNER_NAME);
				break;
			}
			decoded.append(body, pos, size);
			pos += size + 2;
		}
		body.swap(decoded);
	} else {
		size_t lenPos = lower.find("\r\ncontent-length:");
		if (lenPos != std::string::npos) {
			const size_t length = strtoul(lower.c_str() + lenPos + 17, NULL, 10);
			if (length < body.size()) {
				body.resize(length);
			}
		}
	}

	return true;
}

// 選局用のリクエストパスを作る
static std::string MakeStreamPath(const DWORD dwSpace, const DWORD dwChannel)
{
	if (dwSpace >= g_Spaces.size() || dwChannel >= g_Spaces[dwSpace].channels.size()) {
		return std::string();
	}

	const TChannel &ch = g_Channels[g_Spaces[dwSpace].channels[dwChannel]];

	std::string path;

	if (g_Endpoint == ENDPOINT_VIEW) {
		// EDCB標準のview.lua。トランスコードなしの選択肢(TS-Live!)を指定する
		std::string ctok = FetchCsrfToken(ch);
		if (ctok.empty()) {
			return std::string();
		}
		path = std::string(g_BasePath) + "/view.lua?n=" + std::to_string(g_NwtvID)
			+ "&id=" + ChannelIdString(ch)
			+ "&option=" + std::to_string(g_ViewOption)
			+ "&ctok=" + ctok;
	} else {
		// 本プロジェクト付属のbonstream.lua
		path = std::string(g_BasePath) + "/bonstream.lua?key=" + std::string(g_StreamKey)
			+ "&n=" + std::to_string(g_NwtvID)
			+ "&id=" + ChannelIdString(ch)
			+ (g_ServiceFilter ? "&sv=1" : "")
			+ (g_EarlyResponse ? "&early=1" : "");
	}

	// STREAM_KEY や CSRF トークンを含む完全なURLはログへ出さない
	DebugOutA("%s: MakeStreamPath() endpoint=%s space=%lu channel=%lu\n",
	          TUNER_NAME, g_Endpoint == ENDPOINT_VIEW ? "view" : "bonstream",
	          dwSpace, dwChannel);

	return path;
}

static int Init(HMODULE hModule)
{
	::GetModuleFileNameA(hModule, g_IniFilePath, _countof(g_IniFilePath));

	char *p = strrchr(g_IniFilePath, '.');
	if (!p) return -1;
	p++;
	strcpy_s(p, 16, "ini");

	HANDLE hFile = ::CreateFileA(g_IniFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE) return -2;

	::CloseHandle(hFile);

	::GetPrivateProfileStringA("GLOBAL", "SERVER_HOST", "localhost", g_ServerHost, _countof(g_ServerHost), g_IniFilePath);
	::GetPrivateProfileStringA("GLOBAL", "SERVER_PORT", "5510", g_ServerPort, _countof(g_ServerPort), g_IniFilePath);
	::GetPrivateProfileStringA("GLOBAL", "STREAM_KEY", "", g_StreamKey, _countof(g_StreamKey), g_IniFilePath);
	::GetPrivateProfileStringA("GLOBAL", "BASE_PATH", "/legacy", g_BasePath, _countof(g_BasePath), g_IniFilePath);

	{
		char szEndpoint[32];
		::GetPrivateProfileStringA("GLOBAL", "ENDPOINT", "bonstream", szEndpoint, _countof(szEndpoint), g_IniFilePath);
		g_Endpoint = (_stricmp(szEndpoint, "view") == 0) ? ENDPOINT_VIEW : ENDPOINT_BONSTREAM;
	}

	g_NwtvID = ::GetPrivateProfileIntA("GLOBAL", "NWTV_ID", 0, g_IniFilePath);
	if (g_NwtvID < 0 || g_NwtvID > 99) g_NwtvID = 0;

	g_ServiceFilter = ::GetPrivateProfileIntA("GLOBAL", "SERVICE_FILTER", 0, g_IniFilePath);
	g_ViewOption = ::GetPrivateProfileIntA("GLOBAL", "VIEW_OPTION", 11, g_IniFilePath);
	g_ConnectTimeout = ::GetPrivateProfileIntA("GLOBAL", "CONNECT_TIMEOUT", 1500, g_IniFilePath);
	if (g_ConnectTimeout < 100) g_ConnectTimeout = 100;
	g_SpaceMode = ::GetPrivateProfileIntA("CHANNEL", "SPACE_MODE", 0, g_IniFilePath);
	g_ShowRemoteKey = ::GetPrivateProfileIntA("CHANNEL", "SHOW_REMOTE_KEY", 0, g_IniFilePath);
	g_IncludePartial = ::GetPrivateProfileIntA("CHANNEL", "INCLUDE_PARTIAL", 0, g_IniFilePath);
	g_GroupByTs = ::GetPrivateProfileIntA("CHANNEL", "GROUP_BY_TS", 1, g_IniFilePath);
	g_ChannelNameMode = ::GetPrivateProfileIntA("CHANNEL", "CHANNEL_NAME_MODE", 1, g_IniFilePath);
	if (g_GroupByTs) {
		// TS単位モードでサービスを絞ると、TVTest側で同じTS内のサービスを切り替えられなくなる
		g_ServiceFilter = 0;
	}

	{
		char szTypes[256];
		::GetPrivateProfileStringA("CHANNEL", "SERVICE_TYPES", "1,161,165,173", szTypes, _countof(szTypes), g_IniFilePath);
		ParseIntList(szTypes, g_ServiceTypes);
		// "0" は「すべて通す」の意味
		if (g_ServiceTypes.size() == 1 && g_ServiceTypes[0] == 0) {
			g_ServiceTypes.clear();
		}
	}

	g_EarlyResponse = ::GetPrivateProfileIntA("GLOBAL", "EARLY_RESPONSE", 1, g_IniFilePath);

	{
		// チューナー名はDLLのファイル名から作る(BonDriver_EDCB0.dll なら "BonDriver_EDCB0")
		wchar_t szModulePath[MAX_PATH];
		if (::GetModuleFileNameW(hModule, szModulePath, _countof(szModulePath))) {
			const wchar_t *name = wcsrchr(szModulePath, L'\\');
			name = name ? name + 1 : szModulePath;
			::StringCchCopyW(g_TunerName, _countof(g_TunerName), name);
			wchar_t *dot = wcsrchr(g_TunerName, L'.');
			if (dot) {
				*dot = L'\0';
			}
		}
		if (g_TunerName[0] == L'\0') {
			::StringCchCopyW(g_TunerName, _countof(g_TunerName), TUNER_NAME_W);
		}

		// ミューテックス名は「接続先サーバー + NetworkTVモードID」で決める。
		// カーネルオブジェクト名に使えない文字は '_' に置き換える
		char szMutex[256];
		::StringCbPrintfA(szMutex, sizeof(szMutex), "%s_%s_%s_%d",
		                  TUNER_NAME, g_ServerHost, g_ServerPort, g_NwtvID);
		for (char *q = szMutex; *q; q++) {
			if (!isalnum((unsigned char)*q) && *q != '_') {
				*q = '_';
			}
		}
		::MultiByteToWideChar(CP_ACP, 0, szMutex, -1, g_MutexName, _countof(g_MutexName));
	}

	setlocale(LC_ALL, "japanese");

	return 0;
}

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD fdwReason, LPVOID lpReserved)
{
	switch (fdwReason) {
		case DLL_PROCESS_ATTACH:
			if (Init(hModule) != 0) {
				return FALSE;
			}
			// モジュールハンドル保存
			CBonTuner::m_hModule = hModule;
			break;

		case DLL_PROCESS_DETACH:
			// 未開放の場合はインスタンス開放
			if (CBonTuner::m_pThis) {
				CBonTuner::m_pThis->Release();
			}
			break;
	}

	return TRUE;
}

//////////////////////////////////////////////////////////////////////
// インスタンス生成メソッド
//////////////////////////////////////////////////////////////////////

extern "C" __declspec(dllexport) IBonDriver * CreateBonDriver()
{
	// インスタンス生成(既存の場合はインスタンスのポインタを返す)
	return (CBonTuner::m_pThis) ? CBonTuner::m_pThis : ((IBonDriver *) new CBonTuner);
}

//////////////////////////////////////////////////////////////////////
// 構築/消滅
//////////////////////////////////////////////////////////////////////

// 静的メンバ初期化
CBonTuner * CBonTuner::m_pThis = NULL;
HINSTANCE CBonTuner::m_hModule = NULL;

CBonTuner::CBonTuner()
	: m_bTunerOpen(FALSE)
	, m_bWsaInit(false)
	, m_hMutex(NULL)
	, m_pIoReqBuff(NULL)
	, m_pIoPushReq(NULL)
	, m_pIoPopReq(NULL)
	, m_pIoGetReq(NULL)
	, m_dwBusyReqNum(0UL)
	, m_dwReadyReqNum(0UL)
	, m_hPushIoThread(NULL)
	, m_hPopIoThread(NULL)
	, m_bLoopIoThread(FALSE)
	, m_hOnStreamEvent(NULL)
	, m_dwCurSpace(0UL)
	, m_dwCurChannel(0xFFFFFFFFUL)
	, m_sock(INVALID_SOCKET)
	, m_fBitRate(0.0f)
	, m_dwRecvBytes(0UL)
	, m_u64LastCalcTick(0ULL)
{
	m_pThis = this;

	// クリティカルセクション初期化
	::InitializeCriticalSection(&m_CriticalSection);
	::InitializeCriticalSection(&m_ChannelLock);
	::InitializeCriticalSection(&m_BitRateLock);

	// Winsock初期化
	// (チャンネル一覧の取得でもソケットを使うので、InitChannel()より前に行う。
	//  DllMainからではなくCreateBonDriver()経由で呼ばれるので、ここで初期化してよい)
	WSADATA stWsa;
	m_bWsaInit = (WSAStartup(MAKEWORD(2, 2), &stWsa) == 0);

	// チャンネル一覧取得
	InitChannel();
}

CBonTuner::~CBonTuner()
{
	// 開かれてる場合は閉じる
	CloseTuner();

	// クリティカルセクション削除
	::DeleteCriticalSection(&m_CriticalSection);
	::DeleteCriticalSection(&m_ChannelLock);
	::DeleteCriticalSection(&m_BitRateLock);

	// Winsock終了
	if (m_bWsaInit) {
		WSACleanup();
	}

	m_pThis = NULL;
}

// サービス一覧をJSONで取得する
// 戻り値: 取得できたらtrue
static bool FetchServiceList(const std::string &path, json &items)
{
	int status = 0;
	std::string body;
	if (!HttpGet(path, status, body) || status != 200) {
		// bonstream.lua のパスには STREAM_KEY が含まれるため出力しない
		DebugOutA("%s: FetchServiceList() failed. status = %d\n", TUNER_NAME, status);
		return false;
	}

	try {
		json parsed = json::parse(body);
		if (!parsed.is_object() || !parsed.contains("items") || !parsed["items"].is_array()) {
			// EPG読み込み中などのときは {"err":"..."} が返る
			throw std::runtime_error("no items array");
		}
		items = std::move(parsed["items"]);
	} catch (const std::exception &e) {
		DebugOutA("%s: FetchServiceList() parse failed. %s\n", TUNER_NAME, e.what());
		return false;
	}

	return items.is_array() && !items.empty();
}

// JSONオブジェクトから名前の異なる同じ意味のフィールドを読む
// (bonstream.lua の list=1 と api/EnumService でキー名が違うため)
static int JsonInt(const json &v, const char *key1, const char *key2, int defValue)
{
	if (v.contains(key1) && v[key1].is_number()) return v[key1].get<int>();
	if (v.contains(key2) && v[key2].is_number()) return v[key2].get<int>();
	return defValue;
}

static bool JsonBool(const json &v, const char *key1, const char *key2)
{
	if (v.contains(key1) && v[key1].is_boolean()) return v[key1].get<bool>();
	if (v.contains(key2) && v[key2].is_boolean()) return v[key2].get<bool>();
	return false;
}

static std::string JsonStr(const json &v, const char *key1, const char *key2)
{
	if (v.contains(key1) && v[key1].is_string()) return v[key1].get<std::string>();
	if (v.contains(key2) && v[key2].is_string()) return v[key2].get<std::string>();
	return std::string();
}

// チャンネル名を番号にする
// (TS単位にまとめると1チャンネルに複数の放送が入るため、代表サービスの名前を
//  チャンネル名にすると実態と合わない。個々のサービス名はTVTestがSDTから拾う)
static void RenameChannelsByNumber()
{
	for (TSpace &space : g_Spaces) {
		// CS110だけは放送規格のND番号を使い、その順に並べ替える
		bool isCs110 = space.channels.empty() == false;
		for (size_t index : space.channels) {
			if (!IsCs110(g_Channels[index].onid)) {
				isCs110 = false;
				break;
			}
		}

		if (isCs110) {
			std::stable_sort(space.channels.begin(), space.channels.end(),
			                 [](size_t a, size_t b) {
				return Cs110NdNumber(g_Channels[a].tsid) < Cs110NdNumber(g_Channels[b].tsid);
			});

			for (size_t index : space.channels) {
				wchar_t szName[32];
				::StringCchPrintfW(szName, _countof(szName), L"ND%d", Cs110NdNumber(g_Channels[index].tsid));
				g_Channels[index].name = szName;
			}
		} else {
			int no = 1;
			for (size_t index : space.channels) {
				wchar_t szName[64];
				::StringCchPrintfW(szName, _countof(szName), L"%s %02d", space.prefix.c_str(), no++);
				g_Channels[index].name = szName;
			}
		}
	}
}

// EDCBのサービス一覧からチューニング空間とチャンネルを構築する
void CBonTuner::InitChannel()
{
	g_Spaces.clear();
	g_Channels.clear();

	// この関数はCBonTunerのコンストラクタから呼ばれる。EDCBが落ちている等で
	// 応答が得られなくても例外をDLLの外(TVTest)へ投げてはいけない
	json items = json::array();
	bool got = false;

	// bonstream.lua の list=1 は ChSet5 をそのまま返す。api/EnumService と違い
	// EPGデータの有無に依存しないので、EPGを取得していないチャンネル(BS4Kなど)も
	// 漏れなく列挙できる。こちらを優先する
	if (g_Endpoint == ENDPOINT_BONSTREAM && g_StreamKey[0] != '\0') {
		got = FetchServiceList(std::string(g_BasePath) + "/bonstream.lua?key="
		                       + std::string(g_StreamKey) + "&list=1", items);
	}

	// bonstream.lua を置いていない場合はEPG由来のサービス一覧で代用する
	if (!got) {
		got = FetchServiceList("/api/EnumService?json=1", items);
	}

	if (!got) {
		return;
	}

	for (const auto &v : items) {
		if (!v.is_object() || !v.contains("onid") || !v.contains("tsid") || !v.contains("sid")) {
			continue;
		}

		const int serviceType = JsonInt(v, "serviceType", "service_type", 0);

		// サービス種別で絞り込む(空リストのときは全部通す)
		if (!g_ServiceTypes.empty() &&
		    std::find(g_ServiceTypes.begin(), g_ServiceTypes.end(), serviceType) == g_ServiceTypes.end()) {
			continue;
		}

		// ワンセグ(部分受信)サービスは既定で除く
		const bool partial = JsonBool(v, "partialFlag", "partialReceptionFlag");
		if (partial && !g_IncludePartial) {
			continue;
		}

		TChannel ch = {};
		ch.onid = v["onid"].get<int>();
		ch.tsid = v["tsid"].get<int>();
		ch.sid = v["sid"].get<int>();

		// TS単位モードでは、同じ(ONID,TSID)のサービスは最初の1つだけをチャンネルにする。
		// (1チャンネル=1サービスにすると、TVTestが同じTSのチャンネルをすべてSDTの
		//  先頭サービスで代表させてしまい、チャンネルスキャンで重複や名前の欠落が起きる。
		//  EDCBはSDT(PID 0x11)をサービス絞り込みの対象外にしているため、
		//  サービスごとに配信を分けてもSDTには全サービスが載ったままになる)
		if (g_GroupByTs &&
		    std::find_if(g_Channels.begin(), g_Channels.end(),
		                 [&](const TChannel &c) { return c.onid == ch.onid && c.tsid == ch.tsid; })
		        != g_Channels.end()) {
			continue;
		}

		std::string serviceName = JsonStr(v, "serviceName", "service_name");
		std::string networkName = JsonStr(v, "networkName", "network_name");
		const int remoteKey = JsonInt(v, "remoconID", "remote_control_key_id", 0);

		if (serviceName.empty()) {
			serviceName = ChannelIdString(ch);
		}

		ch.name = utf8_to_wstring(serviceName);
		if (g_ShowRemoteKey && remoteKey > 0) {
			wchar_t szPrefix[16];
			::StringCchPrintfW(szPrefix, _countof(szPrefix), L"%d: ", remoteKey);
			ch.name = std::wstring(szPrefix) + ch.name;
		}

		// チューニング空間を決める
		std::wstring spaceName;
		std::wstring prefix;
		int rank = 0;
		if (g_SpaceMode == 1) {
			// ネットワーク名(EDCBが持っているNIT由来の名前)で分ける
			spaceName = networkName.empty() ? L"OTHER" : utf8_to_wstring(networkName);
			prefix = L"CH";
			rank = 0;
		} else {
			ClassifyOnid(ch.onid, networkName, spaceName, prefix, rank);
		}

		g_Channels.push_back(ch);
		const size_t chIndex = g_Channels.size() - 1;

		auto it = std::find_if(g_Spaces.begin(), g_Spaces.end(),
		                       [&](const TSpace &s) { return s.name == spaceName; });
		if (it == g_Spaces.end()) {
			TSpace space;
			space.name = spaceName;
			space.prefix = prefix;
			space.rank = rank;
			space.channels.push_back(chIndex);
			g_Spaces.push_back(std::move(space));
		} else {
			it->channels.push_back(chIndex);
		}
	}

	// 表示順(地デジ→BS→CS→BS4K→SKY→その他)に並べ替える。
	// 同一rank内は最初に現れた順(=EDCBのChSet5の順)を保つ
	std::stable_sort(g_Spaces.begin(), g_Spaces.end(),
	                 [](const TSpace &a, const TSpace &b) { return a.rank < b.rank; });

	if (g_ChannelNameMode == 1) {
		RenameChannelsByNumber();
	}

	DebugOutA("%s: InitChannel() spaces = %u, channels = %u\n",
	          TUNER_NAME, (unsigned)g_Spaces.size(), (unsigned)g_Channels.size());
}

const BOOL CBonTuner::OpenTuner()
{
	if (!m_bWsaInit) {
		// コンストラクタで失敗していた場合はここでやり直す
		WSADATA stWsa;
		m_bWsaInit = (WSAStartup(MAKEWORD(2, 2), &stWsa) == 0);
		if (!m_bWsaInit) {
			return FALSE;
		}
	}

	m_bTunerOpen = TRUE;

	// DLLロード時にEDCBが起動していなかった場合などのために、
	// チャンネル一覧が空ならここで取り直す
	if (g_Spaces.empty()) {
		InitChannel();
	}

	if (g_Spaces.empty()) {
		return FALSE;
	}

	return TRUE;
}

void CBonTuner::CloseTuner()
{
	// SetChannel()/CloseTuner()の多重呼び出しを排他する
	// (同一スレッドからSetChannel()経由で再入した場合はCRITICAL_SECTIONの
	//  性質上ブロックしない)
	CAutoLock lock(m_ChannelLock);

	const ULONGLONG dwCloseStart = ::GetTickCount64();

	// スレッド終了要求セット
	m_bLoopIoThread = FALSE;

	// サーバーへの切断通知を最優先で行う
	// (shutdown()でFINを即座に送出することで、万一スレッド終了処理が詰まった
	//  場合でもサーバー側には速やかに切断が伝わるようにする。また未読の受信
	//  データが残った状態でいきなりclosesocket()するとRSTが送出され、サーバーが
	//  切断を検知できないことがあるため、先に正常なshutdown()を行う)
	if (m_sock != INVALID_SOCKET) {
		::shutdown(m_sock, SD_BOTH);

		// 保留中の非同期WSARecvを強制キャンセルし、Push/PopIoThreadが速やかに
		// ループを抜けられるようにする(TerminateThreadでの強制終了はWinsockの
		// 内部状態を破壊しソケットが正しく閉じられなくなる恐れがあるため、
		// できる限り使わずに済むようにする)
		::CancelIoEx((HANDLE)m_sock, NULL);
	}

	// スレッド終了
	// (PopIoThreadはm_hOnStreamEventにSetEvent()するため、そのハンドルは
	//  スレッドが完全に終了した後でなければ閉じてはならない)
	if (m_hPushIoThread) {
		if (::WaitForSingleObject(m_hPushIoThread, 1000) != WAIT_OBJECT_0) {
#pragma warning(push)
#pragma warning(disable:6258)
			::TerminateThread(m_hPushIoThread, 0);
#pragma warning(pop)
			DebugOutA("%s: CBonTuner::CloseTuner() ::TerminateThread(m_hPushIoThread)\n", TUNER_NAME);
		}

		::CloseHandle(m_hPushIoThread);
		m_hPushIoThread = NULL;
	}

	if (m_hPopIoThread) {
		if (::WaitForSingleObject(m_hPopIoThread, 1000) != WAIT_OBJECT_0) {
#pragma warning(push)
#pragma warning(disable:6258)
			::TerminateThread(m_hPopIoThread, 0);
#pragma warning(pop)
			DebugOutA("%s: CBonTuner::CloseTuner() ::TerminateThread(m_hPopIoThread)\n", TUNER_NAME);
		}

		::CloseHandle(m_hPopIoThread);
		m_hPopIoThread = NULL;
	}

	// イベント開放(両スレッドの終了を待った後なので、SetEvent()との競合は起きない)
	if (m_hOnStreamEvent) {
		::CloseHandle(m_hOnStreamEvent);
		m_hOnStreamEvent = NULL;
	}

	// ソケットクローズ
	// (バッファ(AsyncIoReq)の解放より必ず前に行う。未完了のWSARecvが残ったまま
	//  バッファを解放すると、カーネルが解放済みヒープ上のWSAOVERLAPPED/RxdBuffへ
	//  完了状態や受信データを書き戻し、プロセスヒープを破壊する)
	if (m_sock != INVALID_SOCKET) {
		if (closesocket(m_sock) == SOCKET_ERROR) {
			DebugOutA("%s: CBonTuner::CloseTuner() closesocket error %d\n", TUNER_NAME, WSAGetLastError());
		}
		m_sock = INVALID_SOCKET;
	}

	// 未完了リクエストの完了待ちとイベント開放
	// (closesocket()により保留中のI/Oはキャンセルされるが、完了通知はごく短い間
	//  非同期に遅れる。HasOverlappedIoCompleted()がTRUEになるまで待ってから解放する。
	//  IORS_BUSY以外のスロットのhEventはPopIoRequest()で既に閉じられた古い値なので
	//  触ってはならない)
	if (m_pIoReqBuff) {
		for (DWORD dwIndex = 0; dwIndex < ASYNCBUFFSIZE; dwIndex++) {
			AsyncIoReq *pReq = &m_pIoReqBuff[dwIndex];

			if (pReq->dwState != IORS_BUSY || !pReq->OverLapped.hEvent) {
				continue;
			}

			if (!HasOverlappedIoCompleted(&pReq->OverLapped)
					&& ::WaitForSingleObject(pReq->OverLapped.hEvent, 1000) != WAIT_OBJECT_0
					&& !HasOverlappedIoCompleted(&pReq->OverLapped)) {
				// ここに来た場合バッファを安全に解放できない。起きないはずだが、
				// 起きたときに原因を追えるようログを残す
				DebugOutA("%s: CBonTuner::CloseTuner() pending I/O did not complete\n", TUNER_NAME);
			}

			::CloseHandle(pReq->OverLapped.hEvent);
			pReq->OverLapped.hEvent = NULL;
			pReq->dwState = IORS_IDLE;
		}
	}

	// バッファ開放
	FreeIoReqBuff(m_pIoReqBuff);
	m_pIoReqBuff = NULL;
	m_pIoPushReq = NULL;
	m_pIoPopReq = NULL;
	m_pIoGetReq = NULL;

	m_dwBusyReqNum = 0UL;
	m_dwReadyReqNum = 0UL;

	// チャンネル初期化
	m_dwCurSpace = 0UL;
	m_dwCurChannel = 0xFFFFFFFFUL;

	// ミューテックス開放
	if (m_hMutex) {
		::ReleaseMutex(m_hMutex);
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
	}

	{
		CAutoLock lock(m_BitRateLock);
		m_fBitRate = 0.0f;
		m_dwRecvBytes = 0UL;
	}

	DebugOutA("%s: CBonTuner::CloseTuner() total elapsed = %llu ms\n", TUNER_NAME, ::GetTickCount64() - dwCloseStart);
}

const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	// 終了チェック
	if (!m_hOnStreamEvent || !m_bLoopIoThread) {
		return WAIT_ABANDONED;
	}

	// イベントがシグナル状態になるのを待つ
	const DWORD dwRet = ::WaitForSingleObject(m_hOnStreamEvent, (dwTimeOut) ? dwTimeOut : INFINITE);

	switch (dwRet) {
		case WAIT_ABANDONED:
			// チューナが閉じられた
			return WAIT_ABANDONED;

		case WAIT_OBJECT_0:
		case WAIT_TIMEOUT:
			// ストリーム取得可能 or チューナが閉じられた
			return (m_bLoopIoThread) ? dwRet : WAIT_ABANDONED;

		case WAIT_FAILED:
		default:
			// 例外
			return WAIT_FAILED;
	}
}

const DWORD CBonTuner::GetReadyCount()
{
	// 取り出し可能TSデータ数を取得する
	CAutoLock lock(m_CriticalSection);
	return m_dwReadyReqNum;
}

const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	BYTE *pSrc = NULL;

	// TSデータをバッファから取り出す
	if (GetTsStream(&pSrc, pdwSize, pdwRemain)) {
		if (*pdwSize) {
			::CopyMemory(pDst, pSrc, *pdwSize);
		}

		return TRUE;
	}

	return FALSE;
}

const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!m_pIoGetReq) {
		return FALSE;
	}

	// TSデータをバッファから取り出す
	::EnterCriticalSection(&m_CriticalSection);
	if (m_dwReadyReqNum) {
		if (m_pIoGetReq->dwState == IORS_RECV) {

			BYTE *pRawData = m_pIoGetReq->RxdBuff;
			DWORD dwRawSize = m_pIoGetReq->dwRxdSize;

			// バッファ位置を進める
			m_pIoGetReq = m_pIoGetReq->pNext;
			m_dwReadyReqNum--;
			*pdwRemain = m_dwReadyReqNum;
			::LeaveCriticalSection(&m_CriticalSection);

			*ppDst = pRawData;
			*pdwSize = dwRawSize;

			return TRUE;
		}

		// 例外
		::LeaveCriticalSection(&m_CriticalSection);
		return FALSE;
	}
	::LeaveCriticalSection(&m_CriticalSection);

	// 取り出し可能なデータがない
	*pdwSize = 0;
	*pdwRemain = 0;

	return TRUE;
}

void CBonTuner::PurgeTsStream()
{
	// バッファから取り出し可能データをパージする
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoGetReq = m_pIoPopReq;
	m_dwReadyReqNum = 0;
	::LeaveCriticalSection(&m_CriticalSection);
}

void CBonTuner::Release()
{
	// インスタンス開放
	delete this;
}

LPCWSTR CBonTuner::GetTunerName(void)
{
	// チューナ名を返す
	return g_TunerName;
}

const BOOL CBonTuner::IsTunerOpening(void)
{
	// チューナの使用中の有無を返す(全プロセスを通して)
	HANDLE hMutex = ::OpenMutex(MUTEX_ALL_ACCESS, FALSE, g_MutexName);

	if (hMutex) {
		// 既にチューナは開かれている
		::CloseHandle(hMutex);
		return TRUE;
	}

	// チューナは開かれていない
	return FALSE;
}

// 使用可能なチューニング空間を返す
LPCWSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	if (dwSpace >= g_Spaces.size()) {
		return NULL;
	}

	return g_Spaces[dwSpace].name.c_str();
}

LPCWSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if (dwSpace >= g_Spaces.size() || dwChannel >= g_Spaces[dwSpace].channels.size()) {
		return NULL;
	}

	return g_Channels[g_Spaces[dwSpace].channels[dwChannel]].name.c_str();
}

const DWORD CBonTuner::GetCurSpace(void)
{
	// 現在のチューニング空間を返す
	return m_dwCurSpace;
}

const DWORD CBonTuner::GetCurChannel(void)
{
	// 現在のチャンネルを返す
	return m_dwCurChannel;
}

CBonTuner::AsyncIoReq * CBonTuner::AllocIoReqBuff(const DWORD dwBuffNum)
{
	if (dwBuffNum < 2) {
		return NULL;
	}

	// メモリを確保する
	AsyncIoReq *pNewBuff = new AsyncIoReq[dwBuffNum];
	if (!pNewBuff) {
		return NULL;
	}

	// ゼロクリア
	::ZeroMemory(pNewBuff, sizeof(AsyncIoReq) * dwBuffNum);

	// リンクを構築する
	DWORD dwIndex;
	for (dwIndex = 0; dwIndex < (dwBuffNum - 1); dwIndex++) {
		pNewBuff[dwIndex].pNext = &pNewBuff[dwIndex + 1];
	}

	pNewBuff[dwIndex].pNext = &pNewBuff[0];

	return pNewBuff;
}

void CBonTuner::FreeIoReqBuff(CBonTuner::AsyncIoReq *pBuff)
{
	if (!pBuff) {
		return;
	}

	// バッファを開放する
	delete[] pBuff;
}

// HTTPヘッダの直後に読み過ぎたストリームデータをリングバッファへ積んでおく
// (捨てるとTSパケットの先頭が欠け、再生開始時に再同期が必要になる)
// ※スレッド起動前に呼ぶこと
void CBonTuner::PreloadStreamData(const std::string &data)
{
	size_t offset = 0;

	while (offset < data.size()) {
		// リングを一周させない(先頭のスロットを上書きしてしまう)
		if (m_dwReadyReqNum + 1 >= ASYNCBUFFSIZE) {
			break;
		}

		const size_t remain = data.size() - offset;
		const size_t size = remain < (size_t)TSDATASIZE ? remain : (size_t)TSDATASIZE;

		::CopyMemory(m_pIoPushReq->RxdBuff, data.data() + offset, size);
		m_pIoPushReq->dwRxdSize = (DWORD)size;
		m_pIoPushReq->dwState = IORS_RECV;

		// Push/Popの両方を進める(このスロットに対応する非同期I/Oは存在しないので、
		// PopIoThreadに完了待ちさせてはならない)
		m_pIoPushReq = m_pIoPushReq->pNext;
		m_pIoPopReq = m_pIoPopReq->pNext;
		m_dwReadyReqNum++;

		offset += size;
	}

	{
		CAutoLock lock(m_BitRateLock);
		m_dwRecvBytes += (DWORD)offset;
	}
}

// ストリームが途切れたことを WaitTsStream() の待ち手に知らせる
// (受信スレッドがエラーで抜けても m_bLoopIoThread がTRUEのままだと、TVTest側は
//  WAIT_TIMEOUTを受け取り続けてストリームが死んだことに気づけない。EDCBは録画予約が
//  始まると視聴中のNetworkTVモードを打ち切るので、これは普通に起こりうる)
void CBonTuner::AbortStream()
{
	m_bLoopIoThread = FALSE;

	// WaitTsStream()を待っているスレッドを起こす
	// (両受信スレッドの終了を待ってからでないとCloseTuner()はイベントを閉じないので、
	//  ここでのSetEvent()が閉じたハンドルに当たることはない)
	if (m_hOnStreamEvent) {
		::SetEvent(m_hOnStreamEvent);
	}
}

DWORD WINAPI CBonTuner::PushIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

	// サーバーにTSデータリクエストを発行する
	while (pThis->m_bLoopIoThread) {

		// リクエスト処理待ちが規定未満、かつリング全体(処理待ち+ストア待ち)に
		// 1スロット以上の余裕がある場合のみ追加する。
		// (busy+readyがASYNCBUFFSIZEに達している状態で追加すると、m_pIoPushReqが
		//  指す先はGetTsStream()でまだ読まれていない(IORS_RECVの)スロットであり、
		//  そこへ新規WSARecvを発行すると未読データを上書き破壊してしまう。
		//  また満杯まで詰めると、GetTsStream()が払い出した直後のスロットを
		//  m_pIoPushReqが指すことになり、呼び出し元がまだ読んでいるRxdBuffへ
		//  WSARecvが書き込んでしまう。そのため常に1スロット空けておく。
		//  消費側が追いつかない場合はここで待たせ、TCPの受信バッファ側に
		//  自然にバックプレッシャーをかける)
		bool canPush;
		{
			CAutoLock lock(pThis->m_CriticalSection);
			canPush = pThis->m_dwBusyReqNum < REQRESERVNUM &&
				(pThis->m_dwBusyReqNum + pThis->m_dwReadyReqNum + 1) < ASYNCBUFFSIZE;
		}
		if (canPush) {

			if (!pThis->PushIoRequest(pThis->m_sock)) {
				// エラー発生
				pThis->AbortStream();
				break;
			}

		} else {
			// リクエスト処理待ちがフル、またはリングが満杯の場合はウェイト
			::Sleep(REQPOLLINGWAIT);
		}
	}

	return 0;
}

DWORD WINAPI CBonTuner::PopIoThread(LPVOID pParam)
{
	CBonTuner *pThis = (CBonTuner *)pParam;

	// 処理済リクエストをポーリングしてリクエストを完了させる
	while (pThis->m_bLoopIoThread) {

		bool hasBusy;
		{
			CAutoLock lock(pThis->m_CriticalSection);
			hasBusy = pThis->m_dwBusyReqNum != 0;
		}
		if (hasBusy) {

			if (!pThis->PopIoRequest(pThis->m_sock)) {
				// エラー発生(サーバー側の切断もここに来る)
				pThis->AbortStream();
				break;
			}
		} else {
			// 処理待ちリクエストがない間はビジーループにせず、CPUコアの
			// 占有を避ける(応答性を保つため短い間隔にする)
			::Sleep(1);
		}
	}

	return 0;
}

const BOOL CBonTuner::PushIoRequest(SOCKET sock)
{
	// 非同期リクエストを発行する

	// オープンチェック
	if (sock == INVALID_SOCKET) return FALSE;

	// リクエストセット
	m_pIoPushReq->dwRxdSize = 0;

	// イベント設定
	::ZeroMemory(&m_pIoPushReq->OverLapped, sizeof(WSAOVERLAPPED));
	if (!(m_pIoPushReq->OverLapped.hEvent = ::CreateEvent(NULL, TRUE, FALSE, NULL))) return FALSE;

	DWORD Flags = 0;
	WSABUF wsaBuf{};
	wsaBuf.buf = (char *)m_pIoPushReq->RxdBuff;
	wsaBuf.len = sizeof(m_pIoPushReq->RxdBuff);
	if (SOCKET_ERROR == WSARecv(sock, &wsaBuf, 1, &m_pIoPushReq->dwRxdSize, &Flags, &m_pIoPushReq->OverLapped, NULL)) {
		int sock_err = WSAGetLastError();
		if (sock_err != ERROR_IO_PENDING) {
			::CloseHandle(m_pIoPushReq->OverLapped.hEvent);
			m_pIoPushReq->OverLapped.hEvent = NULL;
			return FALSE;
		}
	}

	m_pIoPushReq->dwState = IORS_BUSY;

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPushReq = m_pIoPushReq->pNext;
	m_dwBusyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	return TRUE;
}

const BOOL CBonTuner::PopIoRequest(SOCKET sock)
{
	// 非同期リクエストを完了する

	// オープンチェック
	if (sock == INVALID_SOCKET) {
		return FALSE;
	}

	// 状態チェック
	if (m_pIoPopReq->dwState != IORS_BUSY) {
		// 例外
		return TRUE;
	}

	// リクエスト取得
	DWORD Flags = 0;
	const BOOL bRet = ::WSAGetOverlappedResult(sock, &m_pIoPopReq->OverLapped, &m_pIoPopReq->dwRxdSize, FALSE, &Flags);

	// エラーチェック
	if (!bRet) {
		int sock_err = WSAGetLastError();
		if (sock_err == ERROR_IO_INCOMPLETE) {
			// 処理未完了
			::Sleep(REQPOLLINGWAIT);
			return TRUE;
		}
	}

	// 総受信サイズ加算
	{
		CAutoLock lock(m_BitRateLock);
		m_dwRecvBytes += m_pIoPopReq->dwRxdSize;
	}

	// ビットレート計算
	CalcBitRate();

	// イベント削除
	::CloseHandle(m_pIoPopReq->OverLapped.hEvent);
	m_pIoPopReq->OverLapped.hEvent = NULL;

	if (!bRet) {
		// エラー発生(サーバー側の切断もここに来る)
		return FALSE;
	}

	// 0バイト受信はサーバーが正常に接続を閉じたことを意味する
	if (m_pIoPopReq->dwRxdSize == 0) {
		return FALSE;
	}

	m_pIoPopReq->dwState = IORS_RECV;

	// バッファ状態更新
	::EnterCriticalSection(&m_CriticalSection);
	m_pIoPopReq = m_pIoPopReq->pNext;
	m_dwBusyReqNum--;
	m_dwReadyReqNum++;
	::LeaveCriticalSection(&m_CriticalSection);

	// イベントセット
	::SetEvent(m_hOnStreamEvent);

	return TRUE;
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return SetChannel((DWORD)0, (DWORD)bCh - 13);
}

// チャンネル設定
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	// SetChannel()の多重呼び出しを排他する(内部で呼ぶCloseTuner()は同一
	// スレッドからの再入になるためブロックしない)
	CAutoLock lock(m_ChannelLock);

	if (g_Endpoint == ENDPOINT_BONSTREAM && g_StreamKey[0] == '\0') {
		// bonstream.lua は共有キーが必須
		DebugOutA("%s: CBonTuner::SetChannel() STREAM_KEY is not set\n", TUNER_NAME);
		return FALSE;
	}

	const ULONGLONG dwSetChannelStart = ::GetTickCount64();

	// 一旦クローズ
	// (EDCB側は同じNWTV_IDで開き直すと同一チューナーで選局し直すため、
	//  切断→再接続がそのままチャンネル変更になる)
	CloseTuner();

	// パス生成はCloseTuner()の後に行う
	// (view.lua用のCSRFトークン取得で通信が発生するので、先に古い接続を
	//  切ってサーバー側のチューナーを解放できる状態にしておく)
	std::string path = MakeStreamPath(dwSpace, dwChannel);
	if (path.empty()) {
		return FALSE;
	}

	// 同じ接続先とNWTV_IDを使う別プロセスとの競合を防ぐ。
	// CreateMutex()は既存オブジェクトでも成功するため、明示的に所有権を取得する
	// 必要がある。放棄されたミューテックスは所有権を引き継いで再利用できる。
	m_hMutex = ::CreateMutex(NULL, FALSE, g_MutexName);
	if (!m_hMutex) {
		DebugOutA("%s: CBonTuner::SetChannel() CreateMutex failed. error = %lu\n",
		          TUNER_NAME, ::GetLastError());
		return FALSE;
	}
	const DWORD dwMutexWait = ::WaitForSingleObject(m_hMutex, 0);
	if (dwMutexWait != WAIT_OBJECT_0 && dwMutexWait != WAIT_ABANDONED) {
		if (dwMutexWait == WAIT_TIMEOUT) {
			DebugOutA("%s: CBonTuner::SetChannel() tuner is already in use\n", TUNER_NAME);
		} else {
			DebugOutA("%s: CBonTuner::SetChannel() mutex wait failed. error = %lu\n",
			          TUNER_NAME, ::GetLastError());
		}
		::CloseHandle(m_hMutex);
		m_hMutex = NULL;
		return FALSE;
	}

	// バッファ確保
	if (!(m_pIoReqBuff = AllocIoReqBuff(ASYNCBUFFSIZE))) {
		CloseTuner();
		return FALSE;
	}

	// バッファ位置同期
	m_pIoPushReq = m_pIoReqBuff;
	m_pIoPopReq = m_pIoReqBuff;
	m_pIoGetReq = m_pIoReqBuff;
	m_dwBusyReqNum = 0;
	m_dwReadyReqNum = 0;

	try {
		// HTTP/1.0にしておけばチャンク転送やKeep-Aliveを考えなくてよい
		std::string serverRequest = "GET " + path + " HTTP/1.0\r\n"
			"Host: " + std::string(g_ServerHost) + ":" + std::string(g_ServerPort) + "\r\n"
			"User-Agent: " TUNER_NAME "\r\n"
			"\r\n";

		int httpStatus = 0;
		std::string leftover;
		{
			const ULONGLONG dwAttemptStart = ::GetTickCount64();

			m_sock = ConnectToServer();
			if (m_sock == INVALID_SOCKET) {
				throw 1UL;
			}

			const ULONGLONG dwConnectDone = ::GetTickCount64();

			if (send(m_sock, serverRequest.c_str(), (int)serverRequest.length(), 0) < 0) {
				DebugOutA("%s: CBonTuner::SetChannel() send error %d\n", TUNER_NAME, WSAGetLastError());
				throw 1UL;
			}

			const ULONGLONG dwSendDone = ::GetTickCount64();

			// レスポンスのステータスコードを確認する
			const bool bStatusOk = RecvHttpStatusCode(m_sock, httpStatus, HTTP_STATUS_TIMEOUT_MS, leftover) && httpStatus == 200;
			const ULONGLONG dwRecvDone = ::GetTickCount64();

			DebugOutA("%s: CBonTuner::SetChannel() status=%d connect=%llums send=%llums recv=%llums since_start=%llums\n",
			          TUNER_NAME, httpStatus,
			          dwConnectDone - dwAttemptStart, dwSendDone - dwConnectDone, dwRecvDone - dwSendDone,
			          dwRecvDone - dwSetChannelStart);

			if (!bStatusOk) {
				closesocket(m_sock);
				m_sock = INVALID_SOCKET;
				throw 6UL;
			}
		}

		// ヘッダと同じrecvで読み過ぎたストリームデータを取りこぼさない
		if (!leftover.empty()) {
			PreloadStreamData(leftover);
		}

		// イベント作成
		if (!(m_hOnStreamEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL))) {
			throw 2UL;
		}

		// スレッド起動
		DWORD dwPushIoThreadID = 0UL, dwPopIoThreadID = 0UL;
		m_hPushIoThread = ::CreateThread(NULL, 0UL, CBonTuner::PushIoThread, this, CREATE_SUSPENDED, &dwPushIoThreadID);
		m_hPopIoThread = ::CreateThread(NULL, 0UL, CBonTuner::PopIoThread, this, CREATE_SUSPENDED, &dwPopIoThreadID);

		if (!m_hPushIoThread || !m_hPopIoThread) {
			if (m_hPushIoThread) {
#pragma warning(push)
#pragma warning(disable:6258)
				::TerminateThread(m_hPushIoThread, 0UL);
#pragma warning(pop)
				::CloseHandle(m_hPushIoThread);
				m_hPushIoThread = NULL;
			}

			if (m_hPopIoThread) {
#pragma warning(push)
#pragma warning(disable:6258)
				::TerminateThread(m_hPopIoThread, 0UL);
#pragma warning(pop)
				::CloseHandle(m_hPopIoThread);
				m_hPopIoThread = NULL;
			}

			throw 3UL;
		}

		// スレッド開始
		m_bLoopIoThread = TRUE;
		if (::ResumeThread(m_hPushIoThread) == 0xFFFFFFFFUL || ::ResumeThread(m_hPopIoThread) == 0xFFFFFFFFUL) {
			throw 4UL;
		}

	} catch (const DWORD dwErrorStep) {
		// エラー発生
		DebugOutA("%s: CBonTuner::SetChannel() dwErrorStep = %lu (elapsed = %llu ms)\n",
		          TUNER_NAME, dwErrorStep, ::GetTickCount64() - dwSetChannelStart);

		CloseTuner();
		return FALSE;
	}

	// チャンネル情報更新
	m_dwCurSpace = dwSpace;
	m_dwCurChannel = dwChannel;

	// ストリーム受信の起点にする
	// (PreloadStreamDataで積んだ分はここでは捨てない。PurgeTsStream()は
	//  m_pIoGetReqをm_pIoPopReqに合わせるだけなので、先頭に積んだデータが
	//  読み飛ばされてしまう。そのため呼ばない)

	DebugOutA("%s: CBonTuner::SetChannel() total elapsed = %llu ms\n", TUNER_NAME, ::GetTickCount64() - dwSetChannelStart);

	return TRUE;
}

// 信号レベル(ビットレート)取得
const float CBonTuner::GetSignalLevel(void)
{
	CalcBitRate();
	CAutoLock lock(m_BitRateLock);
	return m_fBitRate;
}

void CBonTuner::CalcBitRate()
{
	CAutoLock lock(m_BitRateLock);
	ULONGLONG u64CurrentTick = ::GetTickCount64();
	ULONGLONG u64Span = DiffTime(m_u64LastCalcTick, u64CurrentTick);

	if (u64Span >= BITRATE_CALC_TIME) {
		m_fBitRate = (float)(((double)m_dwRecvBytes * (8 * 1000)) / ((double)u64Span * (1024 * 1024)));
		m_dwRecvBytes = 0;
		m_u64LastCalcTick = u64CurrentTick;
	}
}
