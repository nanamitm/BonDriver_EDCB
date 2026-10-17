// test/regression.py が起動する、模擬HTTPサーバー用の回帰テスト。
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <atomic>
#include "../IBonDriver2.h"

static void Check(bool ok, const char *message)
{
	if (!ok) throw std::runtime_error(message);
}

static IBonDriver2 *LoadDriver(const char *path, HMODULE &module)
{
	module = ::LoadLibraryA(path);
	Check(module != NULL, "LoadLibrary failed");
	auto create = reinterpret_cast<IBonDriver *(*)()>(::GetProcAddress(module, "CreateBonDriver"));
	Check(create != nullptr, "CreateBonDriver export missing");
	auto driver = dynamic_cast<IBonDriver2 *>(create());
	Check(driver && driver->OpenTuner(), "OpenTuner failed");
	return driver;
}

struct Waiter
{
	IBonDriver2 *driver;
	std::atomic_bool entered{false};
	DWORD result = WAIT_FAILED;
	static DWORD WINAPI Run(void *arg)
	{
		auto &self = *static_cast<Waiter *>(arg);
		self.entered = true;
		self.result = self.driver->WaitTsStream();
		return 0;
	}
};

struct TuneWorker
{
	IBonDriver2 *driver;
	BOOL result = FALSE;
	static DWORD WINAPI Run(void *arg)
	{
		auto &self = *static_cast<TuneWorker *>(arg);
		self.result = self.driver->SetChannel(DWORD(0), DWORD(0));
		return 0;
	}
};

int main(int argc, char **argv)
{
	try {
		Check(argc >= 3, "usage: regression <dll> <close|ready|owner|contender|available|duplicate> [second dll]");
		HMODULE module;
		auto driver = LoadDriver(argv[1], module);
		if (strcmp(argv[2], "close") == 0) {
			for (int cycle = 0; cycle < 5; ++cycle) {
				Check(driver->SetChannel(DWORD(0), DWORD(0)), "SetChannel failed");
				Waiter waiters[4];
				HANDLE threads[4];
				for (int i = 0; i < 4; ++i) {
					waiters[i].driver = driver;
					threads[i] = ::CreateThread(NULL, 0, Waiter::Run, &waiters[i], 0, NULL);
					Check(threads[i] != NULL, "CreateThread failed");
				}
				for (auto &waiter : waiters) while (!waiter.entered) ::Sleep(1);
				::Sleep(50);
				driver->CloseTuner();
				// 古い待機が、選局し直した新しいセッションへ移らないことも確認。
				Check(driver->SetChannel(DWORD(0), DWORD(0)), "reopen failed");
				Check(::WaitForMultipleObjects(4, threads, TRUE, 2000) == WAIT_OBJECT_0,
				      "CloseTuner left WaitTsStream blocked");
				for (int i = 0; i < 4; ++i) {
					Check(waiters[i].result == WAIT_ABANDONED, "old waiter observed a new stream");
					::CloseHandle(threads[i]);
				}
			}
		} else if (strcmp(argv[2], "ready") == 0) {
			Check(driver->SetChannel(DWORD(0), DWORD(0)), "SetChannel failed");
			for (int i = 0; i < 200 && !driver->GetReadyCount(); ++i) ::Sleep(10);
			Check(driver->GetReadyCount() > 0, "test stream did not arrive");
			Check(driver->WaitTsStream(100) == WAIT_OBJECT_0, "buffered TS was not signaled");
			Check(driver->WaitTsStream(100) == WAIT_OBJECT_0, "wait consumed unread TS notification");
			BYTE *data = nullptr;
			DWORD size = 0, remain = 0;
			Check(driver->GetTsStream(&data, &size, &remain) && size == 188 && data[0] == 0x47,
			      "unexpected TS data");
			Check(driver->WaitTsStream(50) == WAIT_TIMEOUT, "empty buffer remained signaled");
			Check(driver->SetChannel(DWORD(0), DWORD(0)), "reopen failed");
			for (int i = 0; i < 200 && !driver->GetReadyCount(); ++i) ::Sleep(10);
			Check(driver->GetReadyCount() > 0, "second test stream did not arrive");
			driver->PurgeTsStream();
			Check(driver->WaitTsStream(50) == WAIT_TIMEOUT, "purged buffer remained signaled");
		} else if (strcmp(argv[2], "owner") == 0) {
			TuneWorker worker{driver};
			auto thread = ::CreateThread(NULL, 0, TuneWorker::Run, &worker, 0, NULL);
			Check(thread && ::WaitForSingleObject(thread, 3000) == WAIT_OBJECT_0 && worker.result,
			      "worker tune failed");
			::CloseHandle(thread);
			puts("READY");
			fflush(stdout);
			getchar(); // 別プロセスで競合を検査している間、セッションを保持。
			driver->CloseTuner();
			Check(driver->SetChannel(DWORD(0), DWORD(0)), "cross-thread release did not unlock tuner");
		} else if (strcmp(argv[2], "contender") == 0) {
			Check(!driver->SetChannel(DWORD(0), DWORD(0)), "active tuner was acquired by a second process");
		} else if (strcmp(argv[2], "available") == 0) {
			Check(driver->SetChannel(DWORD(0), DWORD(0)), "released tuner remained locked");
		} else if (strcmp(argv[2], "duplicate") == 0) {
			Check(argc >= 4, "second DLL required");
			Check(driver->SetChannel(DWORD(0), DWORD(0)), "first tune failed");
			HMODULE secondModule;
			auto second = LoadDriver(argv[3], secondModule);
			Check(!second->SetChannel(DWORD(0), DWORD(0)), "same thread acquired tuner through another DLL");
			second->Release();
			::FreeLibrary(secondModule);
		} else {
			Check(false, "unknown test mode");
		}
		driver->CloseTuner();
		driver->Release();
		::FreeLibrary(module);
		puts("PASS");
		return 0;
	} catch (const std::exception &e) {
		fprintf(stderr, "FAIL: %s\n", e.what());
		// 他スレッドが残る失敗でも、このテストプロセスだけを終了する。
		::ExitProcess(1);
	}
}
