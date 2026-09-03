// BonDriver の動作確認用のごく小さなコンソールテスタ
//
// TVTest を起動しなくても、チャンネル一覧の取得・選局・TS受信までを確認できる。
//
// ビルド:
//   "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
//   cl /EHsc /std:c++20 /utf-8 test\bontest.cpp
//
// 使い方:
//   bontest <BonDriver.dll のパス> [空間番号] [チャンネル番号] [受信秒数] [チャンネル番号2]
//     引数が dll のみのときはチャンネル一覧を表示して終了する
//     チャンネル番号2を指定すると、受信後にそのチャンネルへ選局し直してもう一度受信する
//     (チャンネル切り替えの所要時間を測るため)
//
// 環境変数 BONDRIVER_EDCB_DEBUG を設定しておくと BonDriver_EDCB のログも標準エラーに出る

#include <windows.h>
#include <cstdio>
#include <string>

#include "../IBonDriver2.h"

typedef IBonDriver *(*CREATEBONDRIVER)();

// 標準出力へUTF-8で書く
// (wprintf は CRT のロケール依存でチャンネル名が "?" に化けるため使わない。
//  コンソールで日本語を見たいときは事前に chcp 65001 しておく)
static void Print(const wchar_t *format, ...)
{
	wchar_t szBuf[1024];
	va_list args;
	va_start(args, format);
	::_vsnwprintf_s(szBuf, _countof(szBuf), _TRUNCATE, format, args);
	va_end(args);

	char szUtf8[4096];
	const int len = ::WideCharToMultiByte(CP_UTF8, 0, szBuf, -1, szUtf8, sizeof(szUtf8), NULL, NULL);
	if (len > 0) {
		fwrite(szUtf8, 1, len - 1, stdout);
	}
}

// 選局して一定時間TSを受信する
static bool TuneAndReceive(IBonDriver2 *pBon2, DWORD dwSpace, DWORD dwChannel, DWORD dwSeconds)
{
	LPCWSTR chName = pBon2->EnumChannelName(dwSpace, dwChannel);
	Print(L"SetChannel(%lu, %lu) -> %s\n", dwSpace, dwChannel, chName ? chName : L"(unknown)");

	const ULONGLONG start = ::GetTickCount64();
	if (!pBon2->SetChannel(dwSpace, dwChannel)) {
		Print(L"SetChannel failed (%llu ms)\n", ::GetTickCount64() - start);
		return false;
	}
	Print(L"SetChannel ok (%llu ms)\n", ::GetTickCount64() - start);

	ULONGLONG total = 0;
	ULONGLONG firstByteTick = 0;
	DWORD syncOk = 0, syncNg = 0;

	const ULONGLONG deadline = ::GetTickCount64() + dwSeconds * 1000;
	while (::GetTickCount64() < deadline) {
		if (pBon2->WaitTsStream(1000) == WAIT_ABANDONED) {
			Print(L"stream abandoned\n");
			break;
		}
		for (;;) {
			BYTE *pBuf = NULL;
			DWORD dwSize = 0, dwRemain = 0;
			if (!pBon2->GetTsStream(&pBuf, &dwSize, &dwRemain) || dwSize == 0) {
				break;
			}
			if (!firstByteTick) {
				firstByteTick = ::GetTickCount64();
			}
			// 先頭が同期バイトかどうかだけ数える(受信の切れ目を見るため)
			if (pBuf[0] == 0x47) syncOk++; else syncNg++;
			total += dwSize;
			if (!dwRemain) break;
		}
	}

	Print(L"received %llu bytes (%.2f Mbps), first byte at %llu ms, chunks sync ok/ng = %lu/%lu\n",
	      total, (double)total * 8 / (dwSeconds * 1000000.0),
	      firstByteTick ? firstByteTick - start : 0, syncOk, syncNg);
	Print(L"GetSignalLevel = %.2f\n", pBon2->GetSignalLevel());

	return true;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2) {
		Print(L"usage: bontest <BonDriver.dll> [space] [channel] [seconds] [channel2]\n");
		return 1;
	}

	HMODULE hDll = ::LoadLibraryW(argv[1]);
	if (!hDll) {
		Print(L"LoadLibrary failed. error = %lu\n", ::GetLastError());
		return 1;
	}

	CREATEBONDRIVER pCreate = (CREATEBONDRIVER)::GetProcAddress(hDll, "CreateBonDriver");
	if (!pCreate) {
		Print(L"CreateBonDriver not found\n");
		return 1;
	}

	IBonDriver *pBon = pCreate();
	if (!pBon) {
		Print(L"CreateBonDriver returned NULL\n");
		return 1;
	}

	IBonDriver2 *pBon2 = dynamic_cast<IBonDriver2 *>(pBon);
	if (!pBon2) {
		Print(L"IBonDriver2 not supported\n");
		pBon->Release();
		return 1;
	}

	Print(L"TunerName: %s\n", pBon2->GetTunerName());

	if (!pBon2->OpenTuner()) {
		Print(L"OpenTuner failed\n");
		pBon->Release();
		return 1;
	}

	if (argc < 4) {
		// チャンネル一覧の表示
		for (DWORD s = 0; ; s++) {
			LPCWSTR spaceName = pBon2->EnumTuningSpace(s);
			if (!spaceName) break;
			Print(L"[%lu] %s\n", s, spaceName);
			for (DWORD c = 0; ; c++) {
				LPCWSTR chName = pBon2->EnumChannelName(s, c);
				if (!chName) break;
				Print(L"      %3lu: %s\n", c, chName);
			}
		}
		pBon2->CloseTuner();
		pBon->Release();
		return 0;
	}

	const DWORD dwSpace = (DWORD)_wtoi(argv[2]);
	const DWORD dwChannel = (DWORD)_wtoi(argv[3]);
	const DWORD dwSeconds = (argc >= 5) ? (DWORD)_wtoi(argv[4]) : 10;

	if (!TuneAndReceive(pBon2, dwSpace, dwChannel, dwSeconds)) {
		pBon2->CloseTuner();
		pBon->Release();
		return 1;
	}

	if (argc >= 6) {
		// チャンネル切り替え(EDCB側は同じNetworkTVモードIDのまま選局し直す)
		const DWORD dwChannel2 = (DWORD)_wtoi(argv[5]);
		if (!TuneAndReceive(pBon2, dwSpace, dwChannel2, dwSeconds)) {
			pBon2->CloseTuner();
			pBon->Release();
			return 1;
		}
	}

	pBon2->CloseTuner();
	pBon->Release();
	::FreeLibrary(hDll);

	return 0;
}
