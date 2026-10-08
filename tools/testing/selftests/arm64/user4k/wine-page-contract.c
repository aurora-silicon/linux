/* SPDX-License-Identifier: GPL-2.0-only */
/* Freestanding Windows x64 PE; Win32 imports, no CRT or external SDK. */
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned long long SIZE_T;
typedef void *HANDLE;
#define API __declspec(dllimport)
API void *VirtualAlloc(void *, SIZE_T, DWORD, DWORD);
API BOOL VirtualFree(void *, SIZE_T, DWORD);
API BOOL VirtualProtect(void *, SIZE_T, DWORD, DWORD *);
API SIZE_T VirtualQuery(const void *, void *, SIZE_T);
API void GetSystemInfo(void *);
API HANDLE GetStdHandle(DWORD);
API BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
API void ExitProcess(DWORD);
API DWORD GetLastError(void);
API void *AddVectoredExceptionHandler(DWORD, void *);
API DWORD RemoveVectoredExceptionHandler(void *);
API HANDLE CreateThread(void *, SIZE_T, DWORD (*)(void *), void *, DWORD,
			DWORD *);
API DWORD WaitForSingleObject(HANDLE, DWORD);
API BOOL GetExitCodeThread(HANDLE, DWORD *);
API BOOL CloseHandle(HANDLE);
API HANDLE CreateFileMappingA(HANDLE, void *, DWORD, DWORD, DWORD,
			      const char *);
API void *MapViewOfFile(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
API BOOL UnmapViewOfFile(const void *);
struct system_info {
	DWORD arch, page;
	void *min, *max;
	SIZE_T mask;
	DWORD cpus, type, granule;
	unsigned short level, revision;
};
struct memory_info {
	void *base, *allocation;
	DWORD allocation_protect, pad;
	SIZE_T size;
	DWORD state, protect, type, pad2;
};
struct exception_record {
	DWORD code, flags;
	void *record, *address;
	DWORD count, pad;
	SIZE_T info[15];
};
struct exception_pointers {
	struct exception_record *record;
	void *context;
};
/* Keep the freestanding declarations consistent with the Windows x64 ABI. */
_Static_assert(sizeof(DWORD) == 4, "DWORD must be 32 bits");
_Static_assert(sizeof(SIZE_T) == 8, "SIZE_T must be 64 bits");
_Static_assert(sizeof(struct system_info) == 48, "SYSTEM_INFO layout");
_Static_assert(sizeof(struct memory_info) == 48,
	       "MEMORY_BASIC_INFORMATION layout");
_Static_assert(sizeof(struct exception_record) == 152,
	       "EXCEPTION_RECORD layout");
static volatile unsigned char *pages;
static volatile DWORD handled;
static void say(const char *s)
{
	DWORD n = 0, w = 0;
	while (s[n])
		n++;
	WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0);
}
static void number(DWORD n)
{
	char buf[11], *p = buf + 10;
	*p = 0;
	do {
		*--p = '0' + n % 10;
		n /= 10;
	} while (n);
	say(p);
}
static void fail(DWORD stage)
{
	DWORD e = GetLastError();
	say("WINE PAGE CONTRACT FAIL stage=");
	number(stage);
	say(" error=");
	number(e);
	say("\r\n");
	ExitProcess(1);
}
#define CHECK(x, n)              \
	do {                     \
		if (!(x))        \
			fail(n); \
	} while (0)
static long handler(struct exception_pointers *e)
{
	DWORD old;
	if (e->record->code != 0xc0000005 || e->record->count < 2 ||
	    e->record->info[0] != 1 ||
	    e->record->info[1] != (SIZE_T)(pages + 4103))
		return 0;
	if (!VirtualProtect((void *)(pages + 4096), 4096, 4, &old))
		return 0;
	handled++;
	return -1;
}
static DWORD thread(void *arg)
{
	volatile unsigned char *p = arg;
	for (unsigned int i = 0; i < 4096; i++)
		p[i] = (unsigned char)(i ^ 0x5a);
	return 37;
}
void mainCRTStartup(void)
{
	struct system_info si;
	struct memory_info mi;
	DWORD old, status;
	GetSystemInfo(&si);
	CHECK(si.page == 4096 && si.granule == 65536, 1);
	say("ok - Wine reports 4K pages and 64K allocation granularity\r\n");
	pages = VirtualAlloc(0, 65536, 0x2000, 1);
	CHECK(pages, 2);
	CHECK(VirtualAlloc((void *)pages, 12288, 0x1000, 4) == pages, 3);
	for (unsigned int i = 0; i < 12288; i++)
		pages[i] = (unsigned char)(i ^ (i >> 12));
	CHECK(VirtualProtect((void *)(pages + 4096), 4096, 2, &old) && old == 4,
	      4);
	CHECK(VirtualQuery((void *)(pages + 4096), &mi, sizeof(mi)) ==
			      sizeof(mi) &&
		      mi.base == pages + 4096 && mi.size == 4096 &&
		      mi.protect == 2,
	      5);
	CHECK(VirtualQuery((void *)(pages + 8192), &mi, sizeof(mi)) ==
			      sizeof(mi) &&
		      mi.protect == 4,
	      6);
	void *veh = AddVectoredExceptionHandler(1, handler);
	CHECK(veh, 7);
	pages[4103] = 0x6d;
	CHECK(handled == 1 && pages[4103] == 0x6d, 8);
	CHECK(RemoveVectoredExceptionHandler(veh), 9);
	say("ok - Wine independent 4K protection and handled access violation\r\n");
	CHECK(VirtualFree((void *)(pages + 4096), 4096, 0x4000), 10);
	CHECK(VirtualQuery((void *)(pages + 4096), &mi, sizeof(mi)) ==
			      sizeof(mi) &&
		      mi.state == 0x2000,
	      11);
	CHECK(VirtualAlloc((void *)(pages + 4096), 4096, 0x1000, 4) ==
		      pages + 4096,
	      12);
	for (unsigned int i = 0; i < 4096; i++)
		CHECK(!pages[4096 + i], 13);
	for (unsigned int i = 0; i < 4096; i++)
		CHECK(pages[i] == (unsigned char)i &&
			      pages[8192 + i] == (unsigned char)(i ^ 2),
		      14);
	say("ok - Wine 4K decommit and zero-filled recommit preserve neighbours\r\n");
	HANDLE t = CreateThread(0, 0, thread, (void *)(pages + 4096), 0, 0);
	CHECK(t, 15);
	CHECK(WaitForSingleObject(t, 30000) == 0 &&
		      GetExitCodeThread(t, &status) && status == 37,
	      16);
	CHECK(CloseHandle(t), 17);
	for (unsigned int i = 0; i < 4096; i++)
		CHECK(pages[4096 + i] == (unsigned char)(i ^ 0x5a), 18);
	say("ok - Wine threads share committed 4K pages\r\n");
	HANDLE file = CreateFileMappingA((HANDLE)(SIZE_T)-1, 0, 4, 0, 16384, 0);
	CHECK(file, 19);
	unsigned char *a = MapViewOfFile(file, 2, 0, 0, 16384),
		      *b = MapViewOfFile(file, 2, 0, 0, 16384);
	CHECK(a && b, 20);
	a[4099] = 0xa7;
	CHECK(b[4099] == 0xa7, 21);
	CHECK(VirtualProtect(a + 4096, 4096, 2, &old), 22);
	b[4099] = 0x53;
	CHECK(a[4099] == 0x53, 23);
	CHECK(UnmapViewOfFile(a) && UnmapViewOfFile(b) && CloseHandle(file),
	      24);
	CHECK(VirtualFree((void *)pages, 0, 0x8000), 25);
	say("ok - Wine shared section aliases and 4K protection\r\nWINE PAGE CONTRACT PASS\r\n");
	ExitProcess(0);
}
