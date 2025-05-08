#include <efi.h>
#include <efilib.h>

#define MAX_FILE_SIZE (UINT64)0xa0000000 /* 2684354560 bytes */
#define PATTERN (UINT64)0xaddeefbeaddeefbe
#define PAGE_SIZE 0x1000

static VOID Halt()
{
	while (1) asm volatile("cli; hlt" ::: "memory");
}

#define __L(x)	L##x
#define _L(x)	__L(x)
#define LFILE	_L(__FILE__)

#define Assert(exp)                                                    \
     ((exp)                                                            \
         ? ((VOID) 0)                                                  \
         : (Print(L"Assertion failed: %s:%d: %s\n",                    \
                  LFILE, __LINE__, _L(#exp)),                          \
            Halt()))

/*
 * WARNING: sizeof(EFI_MEMORY_DESCRIPTOR) isn't the same as DescSize.
 * In efiapi.h there is a macro: NextMemoryDescriptor(Ptr,Size), use it
 * instead. Because of that, mmap for N entries isn't actually big enough
 * for N entries.
 *
 * https://forum.osdev.org/viewtopic.php?f=1&t=32953
 * https://edk2-devel.narkive.com/BMqVNNak/efi-memory-descriptor-8-byte-padding-on-x86-64
 */
#define MEMORY_DESC_MAX		200

static EFI_MEMORY_DESCRIPTOR Mmap[MEMORY_DESC_MAX];
static UINTN MmapEntries = 0;
static UINTN TotalPages = 0;
static UINTN PagesDone = 0;

static VOID UpdateTotalPages(VOID)
{
	TotalPages = 0;
	for (UINTN I = 0; I < MmapEntries; I++)
		TotalPages += Mmap[I].NumberOfPages;
}

static VOID ShowProgress (VOID)
{
	static INTN Prev = -1;
	INTN Current = (PagesDone * 100)/TotalPages;
	if (Current != Prev) {
		Print(L"\r... %3.3d%%", Current);
		Prev = Current;
	}
}

static VOID InitMemmap (VOID)
{
	UINTN MMSize = sizeof(Mmap);
	UINTN MapKey;
	UINTN DescSize;
	UINT32 DescVer;
	EFI_MEMORY_DESCRIPTOR *Desc;
	EFI_STATUS Status;

	Status = uefi_call_wrapper(gBS->GetMemoryMap, 5, &MMSize, Mmap, &MapKey,
							   &DescSize, &DescVer);
	if (Status != EFI_SUCCESS) {
		Print(L"Error obtaining the memory map: %r\n", Status);
		return;
	}

	Assert(DescVer == EFI_MEMORY_DESCRIPTOR_VERSION);
	Assert(DescSize >= sizeof(EFI_MEMORY_DESCRIPTOR));
	Assert(MMSize <= MEMORY_DESC_MAX * sizeof(EFI_MEMORY_DESCRIPTOR));
	Assert((MMSize % DescSize) == 0);

        for (Desc = Mmap; (UINT8 *)Desc < (UINT8 *)Mmap + MMSize;
             Desc = NextMemoryDescriptor(Desc, DescSize)) {
                if (Desc->Type == EfiConventionalMemory) {
                       Print(L"Available RAM [%16llx - %16llx]\n", Desc->PhysicalStart,
                                 Desc->PhysicalStart + Desc->NumberOfPages * PAGE_SIZE - 1);
                        /*
                         * This is safe: CopyMem handles overlapping memory regions, asserts
                         * above made sure that size of memory descriptor is not bigger than
                         * DescSize, and Mmap[MmapEntries] will always be pointing behind
                         * Desc (except possibly first iteration, when they are equal).
                         */
                        CopyMem(&Mmap[MmapEntries], Desc, sizeof(EFI_MEMORY_DESCRIPTOR));
                        MmapEntries++;
                }
        }

	UpdateTotalPages();
	Print(L"Found %lld pages of available RAM (%lld MB)\n",
		  TotalPages, TotalPages >> 8);
}

static VOID GetFileName(CHAR16 *Name, UINT64 AddressStart)
{
	EFI_TIME Time;

	uefi_call_wrapper(gRT->GetTime, 2, &Time, NULL);

	UnicodeSPrint(Name, 0, L"%04d_%02d_%02d_%02d_%02d_0x%016llx.csv",
	              Time.Year, Time.Month, Time.Day,
	              Time.Hour, Time.Minute, AddressStart);
}

static VOID CreateResultFile(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **File, CHAR16 *Name)
{
	EFI_LOADED_IMAGE *Loaded = NULL;
	EFI_FILE_PROTOCOL *Root = NULL;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *SimpleFs = NULL;
	EFI_STATUS Status;

	Status = uefi_call_wrapper(gBS->HandleProtocol, 3, ImageHandle,
	                           &LoadedImageProtocol, (VOID **)&Loaded);
	Assert(Status == EFI_SUCCESS);
	Assert(Loaded != NULL);

	Status = uefi_call_wrapper(gBS->HandleProtocol, 3, Loaded->DeviceHandle,
	                           &gEfiSimpleFileSystemProtocolGuid,
	                           (VOID **)&SimpleFs);
	Assert(Status == EFI_SUCCESS);
	Assert(SimpleFs != NULL);

	Status = uefi_call_wrapper(SimpleFs->OpenVolume, 2, SimpleFs, &Root);
	Assert(Status == EFI_SUCCESS);
	Assert(Root != NULL);

	Status = uefi_call_wrapper(Root->Open, 5, Root, File, Name,
	                           EFI_FILE_MODE_CREATE | EFI_FILE_MODE_WRITE |
	                           EFI_FILE_MODE_READ, 0);
	Assert(Status == EFI_SUCCESS);
	Assert(*File != NULL);
}

static VOID Write8Bytes(EFI_FILE_PROTOCOL *File, UINT64 Bytes)
{
	UINTN Len = (UINTN)sizeof(UINT64);
	CHAR8 *Str = (CHAR8*)(&Bytes);
	EFI_STATUS Status;

	Status = uefi_call_wrapper(File->Write, 3, File, &Len, (void*)Str);
	Assert(Status == EFI_SUCCESS);
}

static VOID WriteOneEntry (UINTN I)
{
	for (UINT64 P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		for (UINT64 Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			Ptr[Q] = PATTERN;
		}

		PagesDone++;
		ShowProgress();
	}
}

static UINTN ExcludeRange (UINTN I, UINT64 Base, UINT64 NumPages)
{
	Print(L"\nExcluding range @ 0x%llx, %lld pages\n", Base, NumPages);
	/*
	 * There are 4 cases, sorted by increasing complexity:
	 * 1. Excluded range is at the end of an entry. Decrease entry's
	 *    NumberOfPages.
	 * 2. Excluded range is at the beginning of an entry. Decrease entry's
	 *    NumberOfPages and increase PhysicalStart.
	 * 3. Whole Mmap entry is removed. Decrease MmapEntries by one and shift
	 *    (copy) the remaining entries down by one place.
	 * 4. Excluded range is in the middle of an entry. Entry must be split into
	 *    two entries. 1st new entry has PhysicalStart same as the original and
	 *    the NumberOfPages modified to end just before Base. 2nd entry has
	 *    PhysicalStart equal to end of excluded range and NumberOfPages equal
	 *    to original NumberOfPages reduced by sum of NumPages and 1st entry's
	 *    NumberOfPages. MmapEntries is increased (up to MEMORY_DESC_MAX),
	 *    remaining entries are shifted (copied) up by one place, original entry
	 *    is overwritten by 1st new entry and 2nd entry is written immediately
	 *    after that.
	 *
	 * Case 3 is a subset of both cases 1 and 2, so it must be checked before
	 * them.
	 */
	EFI_MEMORY_DESCRIPTOR *OrigEntry = &Mmap[I];
	EFI_MEMORY_DESCRIPTOR NewEntries[2] = {0};

	Assert (OrigEntry->PhysicalStart <= Base);
	Assert (OrigEntry->NumberOfPages >= NumPages);
	Assert (OrigEntry->PhysicalStart + OrigEntry->NumberOfPages * PAGE_SIZE >=
	        Base + NumPages * PAGE_SIZE);

	if (Base == OrigEntry->PhysicalStart &&
	    NumPages == OrigEntry->NumberOfPages) {
		/* Case 3. */
		/*
		 * Test for strictly greater than 1, so we won't end up with
		 * MmapEntries == 0 after the operation.
		 */
		Assert (MmapEntries > 1);
		/* Safe, last entry would result in size equal to 0, no need to test. */
		CopyMem (OrigEntry, OrigEntry + 1,
		         (MmapEntries - I - 1) * sizeof(EFI_MEMORY_DESCRIPTOR));
		MmapEntries--;

		return 3;
	} else if (Base + NumPages * PAGE_SIZE ==
	           OrigEntry->PhysicalStart + OrigEntry->NumberOfPages * PAGE_SIZE) {
		/* Case 1. */
		OrigEntry->NumberOfPages -= NumPages;

		return 1;
	} else if (Base == OrigEntry->PhysicalStart &&
	           NumPages != OrigEntry->NumberOfPages) {
		/* Case 2. */
		OrigEntry->NumberOfPages -= NumPages;
		OrigEntry->PhysicalStart += NumPages * PAGE_SIZE;

		return 2;
	} else {
		/* Case 4. */
		Assert (MmapEntries < MEMORY_DESC_MAX);
		/* Create new entries with original one used as a template. */
		NewEntries[0] = *OrigEntry;
		NewEntries[0].NumberOfPages = (Base - OrigEntry->PhysicalStart) /
		                              PAGE_SIZE;
		NewEntries[1] = *OrigEntry;
		NewEntries[1].PhysicalStart = Base + NumPages * PAGE_SIZE;
		NewEntries[1].NumberOfPages = OrigEntry->NumberOfPages -
		                              NewEntries[0].NumberOfPages -
		                              NumPages;
		/*
		 * Move remaining entries to make room for new ones. Safe,
		 * I = MmapEntries - 1 would result in size equal to 0 so no copy,
		 * otherwise OrigEntry is at most pointing to Mmap[MmapEntries - 2],
		 * and Assert() above makes sure that
		 * OrigEntry + 2 < Mmap[MEMORY_DESC_MAX - 2 + 2], so
		 * OrigEntry + 2 < Mmap[MEMORY_DESC_MAX].
		 */
		CopyMem (OrigEntry + 2, OrigEntry + 1,
		         (MmapEntries - I - 1) * sizeof(EFI_MEMORY_DESCRIPTOR));
		/* Insert new entries and update number of entries. */
		CopyMem (OrigEntry, NewEntries, sizeof(NewEntries));
		MmapEntries++;

		return 4;
	}
}

static UINTN ExcludeOneEntry (UINTN I)
{
	BOOLEAN WasSame = TRUE;
	UINT64 First = (UINT64)-1, Last = 0;
	UINT64 *Ptr;
	for (UINTN P = 0; P < Mmap[I].NumberOfPages; P++) {
		Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		for (UINT64 Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			if (*Ptr != PATTERN) {
				if (WasSame == TRUE || (P == 0 && Q == 0)) {
					First = (UINT64)Ptr & ~(UINT64)(PAGE_SIZE - 1);
				}
				WasSame = FALSE;
			} else {
				if (WasSame == FALSE) {
					/*
					 * Last is actually the first address on new page that is
					 * the same as expected. This makes it easier to convert to
					 * number of pages.
					 */
					Last = (UINT64)Ptr + PAGE_SIZE - 1;
					Last &= ~(UINT64)(PAGE_SIZE - 1);

					UINTN Ret = ExcludeRange (I, First, (Last - First) / PAGE_SIZE);

					if (Ret == 2){
						First = (UINT64)-1;
						Last = 0;
						WasSame = TRUE;
						P = (UINT64)-1;
						break;
					}
					else if (Ret == 3)
						return 3;
					else if (Ret == 4)
						return 4;
					First = (UINT64)-1;
					Last = 0;
				}
				WasSame = TRUE;
			}
			Ptr++;
		}
		PagesDone++;
		ShowProgress();
	}
	if (First != (UINT64)-1) {
		return ExcludeRange (I, First, ((UINT64)Ptr - First) / PAGE_SIZE);
	}

	return 0;
}
static VOID FinalizeResults(EFI_FILE_PROTOCOL *File)
{
	EFI_STATUS Status;

	/* Close the file, which flushes it to disk */
	Status = uefi_call_wrapper(File->Close, 1, File);
	Assert(Status == EFI_SUCCESS);
}

static VOID DumpOneEntry (EFI_HANDLE ImageHandle, UINTN I)
{
	CHAR16 FileName[50];
	EFI_FILE_PROTOCOL *File = NULL;
	UINT64 CurrentFileSize = 0;

	GetFileName(FileName, Mmap[I].PhysicalStart);
	CreateResultFile(ImageHandle, &File, FileName);

	for (UINT64 P = 0; P < Mmap[I].NumberOfPages; P++) {
		UINT64 *Ptr = (UINT64 *)(Mmap[I].PhysicalStart + P * PAGE_SIZE);
		if (CurrentFileSize >= (MAX_FILE_SIZE - PAGE_SIZE)){
			FinalizeResults(File);
			GetFileName(FileName, (UINT64)Ptr);
			CreateResultFile(ImageHandle, &File, FileName);
			CurrentFileSize = 0;
		}

		for (UINT64 Q = 0; Q < PAGE_SIZE/sizeof(UINT64); Q++) {
			Write8Bytes(File, Ptr[Q]);
		}

		PagesDone++;
		CurrentFileSize+=PAGE_SIZE;
		ShowProgress();
	}

	FinalizeResults(File);
}


/* No EFIAPI here. Not sure why, but gnu-efi converts this to SysV */
EFI_STATUS
efi_main (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS Status = EFI_SUCCESS;
	EFI_INPUT_KEY Key;
	EFI_GUID VarGuid = { 0x865a4a83, 0x19e9, 0x4f5b, {0x84, 0x06, 0xbc, 0xa0, 0xdb, 0x86, 0x91, 0x5e} };
	CHAR16 VarName[] = L"TestedMemoryMap";
	UINTN VarSize;
	UINT32 NVAttr = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE;

	InitializeLib(ImageHandle, SystemTable);

	uefi_call_wrapper(ST->ConOut->ClearScreen, 1, ST->ConOut);

	/* Disable watchdog so it won't reboot the platform after 20 minutes. */
	Status = uefi_call_wrapper(gBS->SetWatchdogTimer, 4, 0, 0, 0, NULL);
	if (Status != EFI_SUCCESS) {
		Print(L"Error disabling the watchdog: %r\n", Status);
		return Status;
	}

	Print(L"Application for dumping RAM\n");

	InitMemmap();

	Print(L"\n\nChoose the mode:\n");
	Print(L"%H1%N. Pattern write\n");
	Print(L"%H2%N. Exclude modified by firmware\n");
	Print(L"%H3%N. Dump\n\n");

	WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
	uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);


	if (Key.UnicodeChar == L'1') {
		Print(L"Writing pattern was selected\n");
		for (UINTN I = 0; I < MmapEntries; I++) {
			WriteOneEntry(I);
		}
		Print(L"\nWriting done\n");
	} else if (Key.UnicodeChar == L'2') {
		Print(L"Exclude modified by firmware was selected\n");
		UINTN I = 0;

		while (I < MmapEntries) {
			if (ExcludeOneEntry(I) != 3)
				I++;
		}

		VarSize = MmapEntries * sizeof(EFI_MEMORY_DESCRIPTOR);
		Status = uefi_call_wrapper(gRT->SetVariable, 5, VarName, &VarGuid,
		                           NVAttr, VarSize, Mmap);
		Assert (Status == EFI_SUCCESS);
		Print(L"\nExclude modified by firmware done\n");
	}
	else if (Key.UnicodeChar == L'3') {
		VarSize = sizeof(Mmap);
		Status = uefi_call_wrapper(gRT->GetVariable, 5, VarName, &VarGuid,
		                           NULL, &VarSize, Mmap);
		Assert (Status == EFI_SUCCESS);
		Assert (VarSize % sizeof(EFI_MEMORY_DESCRIPTOR) == 0);
		MmapEntries = VarSize / sizeof(EFI_MEMORY_DESCRIPTOR);
		UpdateTotalPages();

		Print(L"\n\nDumping memory...\n");
		for (UINTN I = 0; I < MmapEntries; I++) {
			DumpOneEntry(ImageHandle, I);
		}

		Assert (Status == EFI_SUCCESS);
		Print(L"\nMemory dump done!\n");
	}

	Print(L"\nPress %HR%N to reboot, %HS%N to shut down\n");

	do{
		WaitForSingleEvent(ST->ConIn->WaitForKey, 0);
		uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &Key);
	} while (Key.UnicodeChar != L'r' && Key.UnicodeChar != L's');

	if (Key.UnicodeChar == L's') {
		Status = uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetShutdown, EFI_SUCCESS,
		                           0, NULL);
		Assert(Status == EFI_SUCCESS);
	}

	Status = uefi_call_wrapper(gRT->ResetSystem, 4, EfiResetWarm, EFI_SUCCESS,
	                           0, NULL);

	return Status;
}
