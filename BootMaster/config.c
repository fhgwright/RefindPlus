/*
 * BootMaster/config.c
 * Configuration file functions
 *
 * Copyright (c) 2006 Christoph Pfisterer
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the
 *    distribution.
 *
 *  * Neither the name of Christoph Pfisterer nor the names of the
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Modifications copyright (c) 2012-2021 Roderick W. Smith
 *
 * Modifications distributed under the terms of the GNU General Public
 * License (GPL) version 3 (GPLv3) or (at your option) any later version.
 *
 */
/*
 * Modified for RefindPlus
 * Copyright (c) 2020-2021 Dayo Akanji (sf.net/u/dakanji/profile)
 * Portions Copyright (c) 2021 Joe van Tunen (joevt@shaw.ca)
 *
 * Modifications distributed under the preceding terms.
 */

#include "global.h"
#include "lib.h"
#include "icns.h"
#include "menu.h"
#include "config.h"
#include "screenmgt.h"
#include "apple.h"
#include "mystrings.h"
#include "scan.h"
#include "../include/refit_call_wrapper.h"
#include "../mok/mok.h"

// constants

#define LINUX_OPTIONS_FILENAMES  L"refind_linux.conf,refind-linux.conf"
#define MAXCONFIGFILESIZE        (128*1024)

#define ENCODING_ISO8859_1  (0)
#define ENCODING_UTF8       (1)
#define ENCODING_UTF16_LE   (2)

#define MAXLOGLEVEL         (5)

#define GetTime gST->RuntimeServices->GetTime
#define LAST_MINUTE 1439 /* Last minute of a day */

BOOLEAN SilenceAPFS;
BOOLEAN InnerScan = FALSE;

// extern REFIT_MENU_ENTRY MenuEntryReturn;
//static REFIT_MENU_ENTRY MenuEntryReturn   = { L"Return to Main Menu", TAG_RETURN, 0, 0, 0, NULL, NULL, NULL };

//
// read a file into a buffer
//

EFI_STATUS RefitReadFile (
    IN EFI_FILE_HANDLE  BaseDir,
    IN CHAR16          *FileName,
    IN OUT REFIT_FILE  *File,
    OUT UINTN          *size
) {
    EFI_STATUS       Status;
    EFI_FILE_HANDLE  FileHandle;
    EFI_FILE_INFO   *FileInfo;
    CHAR16          *Message;
    UINT64           ReadSize;

    File->Buffer     = NULL;
    File->BufferSize = 0;

    // read the file, allocating a buffer on the way
    Status = REFIT_CALL_5_WRAPPER(
        BaseDir->Open, BaseDir,
        &FileHandle, FileName,
        EFI_FILE_MODE_READ, 0
    );

    Message = PoolPrint (L"While Loading File:- '%s'", FileName);
    if (CheckError (Status, Message)) {
        MyFreePool (&Message);

        return Status;
    }

    FileInfo = LibFileInfo (FileHandle);
    if (FileInfo == NULL) {
        // TODO: print and register the error
        REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);
        return EFI_LOAD_ERROR;
    }
    ReadSize = FileInfo->FileSize;
    MyFreePool (&FileInfo);

    File->BufferSize = (UINTN) ReadSize;
    File->Buffer = AllocatePool (File->BufferSize);
    if (File->Buffer == NULL) {
       size = 0;
       return EFI_OUT_OF_RESOURCES;
    }
    else {
       *size = File->BufferSize;
    } // if/else

    Status = REFIT_CALL_3_WRAPPER(FileHandle->Read, FileHandle, &File->BufferSize, File->Buffer);
    if (CheckError (Status, Message)) {
        MyFreePool (&Message);
        MyFreePool (&File->Buffer);
        File->Buffer = NULL;
        REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);

        return Status;
    }
    MyFreePool (&Message);

    REFIT_CALL_1_WRAPPER(FileHandle->Close, FileHandle);

    // setup for reading
    File->Current8Ptr  = (CHAR8 *)File->Buffer;
    File->End8Ptr      = File->Current8Ptr + File->BufferSize;
    File->Current16Ptr = (CHAR16 *)File->Buffer;
    File->End16Ptr     = File->Current16Ptr + (File->BufferSize >> 1);

    // detect encoding
    File->Encoding = ENCODING_ISO8859_1;   // default: 1:1 translation of CHAR8 to CHAR16
    if (File->BufferSize >= 4) {
        if (File->Buffer[0] == 0xFF && File->Buffer[1] == 0xFE) {
            // BOM in UTF-16 little endian (or UTF-32 little endian)
            File->Encoding = ENCODING_UTF16_LE;   // use CHAR16 as is
            File->Current16Ptr++;
        }
        else if (File->Buffer[0] == 0xEF && File->Buffer[1] == 0xBB && File->Buffer[2] == 0xBF) {
            // BOM in UTF-8
            File->Encoding = ENCODING_UTF8;       // translate from UTF-8 to UTF-16
            File->Current8Ptr += 3;
        }
        else if (File->Buffer[1] == 0 && File->Buffer[3] == 0) {
            File->Encoding = ENCODING_UTF16_LE;   // use CHAR16 as is
        }
        // TODO: detect other encodings as they are implemented
    }

    return EFI_SUCCESS;
}

//
// get a single line of text from a file
//

static
CHAR16 * ReadLine (
    REFIT_FILE *File
) {
    CHAR16  *Line, *q;
    UINTN   LineLength;

    if (File->Buffer == NULL) {
        return NULL;
    }

    if (File->Encoding == ENCODING_ISO8859_1 || File->Encoding == ENCODING_UTF8) {

        CHAR8 *p, *LineStart, *LineEnd;

        p = File->Current8Ptr;
        if (p >= File->End8Ptr) {
            return NULL;
        }

        LineStart = p;
        for (; p < File->End8Ptr; p++) {
            if (*p == 13 || *p == 10) {
                break;
            }
        }
        LineEnd = p;
        for (; p < File->End8Ptr; p++) {
            if (*p != 13 && *p != 10) {
                break;
            }
        }
        File->Current8Ptr = p;

        LineLength = (UINTN)(LineEnd - LineStart) + 1;
        Line = AllocatePool (LineLength * sizeof (CHAR16));
        if (Line == NULL) {
            return NULL;
        }

        q = Line;
        if (File->Encoding == ENCODING_ISO8859_1) {
            for (p = LineStart; p < LineEnd; ) {
                *q++ = *p++;
            }
        }
        else if (File->Encoding == ENCODING_UTF8) {
            // TODO: actually handle UTF-8
            for (p = LineStart; p < LineEnd; ) {
                *q++ = *p++;
            }
        }
        *q = 0;

    }
    else if (File->Encoding == ENCODING_UTF16_LE) {

        CHAR16 *p, *LineStart, *LineEnd;

        p = File->Current16Ptr;
        if (p >= File->End16Ptr) {
            return NULL;
        }

        LineStart = p;
        for (; p < File->End16Ptr; p++) {
            if (*p == 13 || *p == 10) {
                break;
            }
        }
        LineEnd = p;
        for (; p < File->End16Ptr; p++) {
            if (*p != 13 && *p != 10) {
                break;
            }
        }
        File->Current16Ptr = p;

        LineLength = (UINTN)(LineEnd - LineStart) + 1;
        Line = AllocatePool (LineLength * sizeof (CHAR16));
        if (Line == NULL) {
            return NULL;
        }

        for (p = LineStart, q = Line; p < LineEnd; ) {
            *q++ = *p++;
        }
        *q = 0;

    }
    else {
        return NULL;   // unsupported encoding
    }

    return Line;
}

// Returns FALSE if *p points to the end of a token, TRUE otherwise.
// Also modifies *p **IF** the first and second characters are both
// quotes ('"'); it deletes one of them.
static
BOOLEAN KeepReading (
    IN OUT CHAR16  *p,
    IN OUT BOOLEAN *IsQuoted
) {
   BOOLEAN  MoreToRead = FALSE;
   CHAR16  *Temp       = NULL;

   if ((p == NULL) || (IsQuoted == NULL)) {
       return FALSE;
   }

   if (*p == L'\0') {
       return FALSE;
   }

   if ((*p != ' ' && *p != '\t' && *p != '=' && *p != '#' && *p != ',') || *IsQuoted) {
      MoreToRead = TRUE;
   }

   if (*p == L'"') {
      if (p[1] == L'"') {
         Temp = StrDuplicate (&p[1]);
         if (Temp != NULL) {
            StrCpy (p, Temp);
            MyFreePool (&Temp);
         }
         MoreToRead = TRUE;
      }
      else {
         *IsQuoted = !(*IsQuoted);
         MoreToRead = FALSE;
      } // if/else second character is a quote
   } // if first character is a quote

   return MoreToRead;
} // BOOLEAN KeepReading()

//
// get a line of tokens from a file
//
UINTN ReadTokenLine (
    IN REFIT_FILE   *File,
    OUT CHAR16    ***TokenList
) {
    BOOLEAN          LineFinished, IsQuoted = FALSE;
    CHAR16          *Line, *Token, *p;
    UINTN            TokenCount = 0;

    *TokenList = NULL;

    while (TokenCount == 0) {
        Line = ReadLine (File);
        if (Line == NULL) {
            return 0;
        }

        p = Line;
        LineFinished = FALSE;
        while (!LineFinished) {
            // skip whitespace & find start of token
            while ((!IsQuoted)
                && (*p == ' ' || *p == '\t' || *p == '=' || *p == ',')
            ) {
                p++;
            } // while

            if (*p == 0 || *p == '#') {
                break;
            }

            if (*p == '"') {
               IsQuoted = !IsQuoted;
               p++;
            }

            Token = p;

            // find end of token
            while (KeepReading (p, &IsQuoted)) {
               if ((*p == L'/') && !IsQuoted) {
                   // Switch Unix-style to DOS-style directory separators
                   *p = L'\\';
               }
               p++;
            } // while
            if (*p == L'\0' || *p == L'#') {
                LineFinished = TRUE;
            }
            *p++ = 0;

            AddListElement ((VOID ***)TokenList, &TokenCount, (VOID *)StrDuplicate (Token));
        } // while

        MyFreePool (&Line);
    }

    return TokenCount;
} /* ReadTokenLine() */

VOID FreeTokenLine (
    IN OUT CHAR16 ***TokenList,
    IN OUT UINTN    *TokenCount
) {
    // TODO: also free the items
    FreeList ((VOID ***) TokenList, TokenCount);
}

// handle a parameter with a single integer argument (unsigned)
static
VOID HandleInt (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT UINTN   *Value
) {
    if (TokenCount == 2) {
       if (StrCmp (TokenList[1], L"-1") == 0) {
           *Value = -1;
       }
       else {
           *Value = Atoi (TokenList[1]);
       }
    }
}

// handle a parameter with a single integer argument (signed)
static
VOID HandleSignedInt (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT INTN    *Value
) {
    if (TokenCount == 2) {
       if (StrCmp (TokenList[1], L"-1") == 0) {
           *Value = -1;
       }
       else {
           *Value = Atoi (TokenList[1]);
       }
    }
}

// handle a parameter with a single string argument
static
VOID HandleString (
    IN  CHAR16  **TokenList,
    IN  UINTN     TokenCount,
    OUT CHAR16  **Target
) {
    if ((TokenCount == 2) && Target) {
        ReleasePtr (*Target);
        *Target = StrDuplicate (TokenList[1]);
    } // if
} // static VOID HandleString()

// Handle a parameter with a series of string arguments, to replace or be added to a
// comma-delimited list. Passes each token through the CleanUpPathNameSlashes() function
// to ensure consistency in subsequent comparisons of filenames. If the first
// non-keyword token is "+", the list is added to the existing target string; otherwise,
// the tokens replace the current string.
static
VOID HandleStrings (
    IN  CHAR16 **TokenList,
    IN  UINTN    TokenCount,
    OUT CHAR16 **Target
) {
    UINTN   i;
    BOOLEAN AddMode = FALSE;

    if (!Target) {
        return;
    }

    if ((TokenCount > 2) && (StrCmp (TokenList[1], L"+") == 0)) {
        AddMode = TRUE;
    }

    if ((*Target != NULL) && !AddMode) {
        ReleasePtr (*Target);
        *Target = NULL;
    }

    for (i = 1; i < TokenCount; i++) {
        if ((i != 1) || !AddMode) {
            CleanUpPathNameSlashes (TokenList[i]);
            MergeStrings (Target, TokenList[i], L',');
        }
    }
} // static VOID HandleStrings()

// Handle a parameter with a series of hexadecimal arguments, to replace or be added to a
// linked list of UINT32 values. Any item with a non-hexadecimal value is discarded, as is
// any value that exceeds MaxValue. If the first non-keyword token is "+", the new list is
// added to the existing Target; otherwise, the interpreted tokens replace the current
// Target.
static
VOID HandleHexes (
    IN  CHAR16       **TokenList,
    IN  UINTN          TokenCount,
    IN  UINTN          MaxValue,
    OUT UINT32_LIST  **Target
) {
    UINTN        InputIndex = 1, i;
    UINT32       Value;
    UINT32_LIST *EndOfList  = NULL;
    UINT32_LIST *NewEntry;

    if ((TokenCount > 2) && (StrCmp (TokenList[1], L"+") == 0)) {
        InputIndex = 2;
        EndOfList = *Target;
        while (EndOfList && (EndOfList->Next != NULL)) {
            EndOfList = EndOfList->Next;
        }
    }
    else {
        EraseUint32List (Target);
    }

    for (i = InputIndex; i < TokenCount; i++) {
        if (IsValidHex (TokenList[i])) {
            Value = (UINT32) StrToHex (TokenList[i], 0, 8);
            if (Value <= MaxValue) {
                NewEntry = AllocatePool (sizeof (UINT32_LIST));
                if (NewEntry) {
                    NewEntry->Value = Value;
                    NewEntry->Next = NULL;
                    if (EndOfList == NULL) {
                        EndOfList = NewEntry;
                        *Target = NewEntry;
                    }
                    else {
                        EndOfList->Next = NewEntry;
                        EndOfList = NewEntry;
                    } // if/else
                } // if allocated memory for NewEntry
            } // if (Value < MaxValue)
        } // if is valid hex value
    } // for
} // static VOID HandleHexes()

// Convert TimeString (in "HH:MM" format) to a pure-minute format. Values should be
// in the range from 0 (for 00:00, or midnight) to 1439 (for 23:59; aka LAST_MINUTE).
// Any value outside that range denotes an error in the specification. Note that if
// the input is a number that includes no colon, this function will return the original
// number in UINTN form.
static
UINTN HandleTime (
    IN CHAR16 *TimeString
) {
    UINTN Hour = 0, Minute = 0, TimeLength, i = 0;

    TimeLength = StrLen (TimeString);
    while (i < TimeLength) {
        if (TimeString[i] == L':') {
            Hour = Minute;
            Minute = 0;
        }

        if ((TimeString[i] >= L'0') && (TimeString[i] <= '9')) {
            Minute *= 10;
            Minute += (TimeString[i] - L'0');
        }

        i++;
    } // while

    return (Hour * 60 + Minute);
} // BOOLEAN HandleTime()

static
BOOLEAN HandleBoolean (
    IN CHAR16 **TokenList,
    IN UINTN    TokenCount
) {
    BOOLEAN TruthValue = TRUE;

    if ((TokenCount >= 2) &&
        (
            StrCmp (TokenList[1], L"0") == 0
            || MyStriCmp (TokenList[1], L"false")
            || MyStriCmp (TokenList[1], L"off")
        )
    ) {
        TruthValue = FALSE;
    }

    return TruthValue;
} // BOOLEAN HandleBoolean()

// Sets the default boot loader IF the current time is within the bounds
// defined by the third and fourth tokens in the TokenList.
static
VOID SetDefaultByTime (
    IN  CHAR16 **TokenList,
    OUT CHAR16 **Default
) {
    EFI_STATUS            Status;
    EFI_TIME              CurrentTime;
    UINTN                 StartTime, EndTime, Now;
    BOOLEAN               SetIt = FALSE;

    StartTime = HandleTime (TokenList[2]);
    EndTime   = HandleTime (TokenList[3]);

    if ((StartTime <= LAST_MINUTE) && (EndTime <= LAST_MINUTE)) {
        Status = REFIT_CALL_2_WRAPPER(GetTime, &CurrentTime, NULL);

        if (Status != EFI_SUCCESS) {
            return;
        }

        Now = CurrentTime.Hour * 60 + CurrentTime.Minute;

        if (Now > LAST_MINUTE) {
            // Should not happen ... just being paranoid
            #if REFIT_DEBUG > 0
            MsgLog ("  - WARN: Impossible System Time:- %d:%d\n", CurrentTime.Hour, CurrentTime.Minute);
            #endif

            Print (L"Warning: Impossible system time:- %d:%d\n", CurrentTime.Hour, CurrentTime.Minute);
            return;
        }

        if (StartTime < EndTime) {
            // Time range does NOT cross midnight
            if ((Now >= StartTime) && (Now <= EndTime)) {
                SetIt = TRUE;
            }
        }
        else {
            // Time range DOES cross midnight
            if ((Now >= StartTime) || (Now <= EndTime)) {
                SetIt = TRUE;
            }
        }

        if (SetIt) {
            ReleasePtr (*Default);
            *Default = StrDuplicate (TokenList[1]);
        }
    } // if ((StartTime <= LAST_MINUTE) && (EndTime <= LAST_MINUTE))
} // VOID SetDefaultByTime()

static
LOADER_ENTRY * AddPreparedLoaderEntry (
    LOADER_ENTRY *Entry
) {
    AddMenuEntry (&MainMenu, (REFIT_MENU_ENTRY *) Entry);

    return Entry;
} // LOADER_ENTRY * AddPreparedLoaderEntry()

// read config file
VOID ReadConfig (
    CHAR16 *FileName
) {
    EFI_STATUS        Status;
    REFIT_FILE        File;
    CHAR16          **TokenList;
    CHAR16           *Flag;
    CHAR16           *TempStr = NULL;
    CHAR16           *MsgStr  = NULL;
    UINTN             TokenCount, i;

    #if REFIT_DEBUG > 0
    MsgLog ("Read Config...\n");
    #endif

    // Set a few defaults only if we are loading the default file.
    if (MyStriCmp (FileName, GlobalConfig.ConfigFilename)) {
        MyFreePool (&GlobalConfig.AlsoScan);
        GlobalConfig.AlsoScan = StrDuplicate (ALSO_SCAN_DIRS);

        MyFreePool (&GlobalConfig.DontScanDirs);
        if (SelfVolume) {
            TempStr = GuidAsString (&(SelfVolume->PartGuid));
        }
        MergeStrings (&TempStr, SelfDirPath, L':');
        MergeStrings (&TempStr, MEMTEST_LOCATIONS, L',');
        GlobalConfig.DontScanDirs = TempStr;

        MyFreePool (&GlobalConfig.DontScanFiles);
        GlobalConfig.DontScanFiles = StrDuplicate (DONT_SCAN_FILES);

        MyFreePool (&GlobalConfig.DontScanTools);
        GlobalConfig.DontScanTools = NULL;

        MyFreePool (&GlobalConfig.DontScanFirmware);
        GlobalConfig.DontScanFirmware = NULL;
        MergeStrings (&(GlobalConfig.DontScanFiles), MOK_NAMES, L',');
        MergeStrings (&(GlobalConfig.DontScanFiles), FWUPDATE_NAMES, L',');

        MyFreePool (&GlobalConfig.DontScanVolumes);
        GlobalConfig.DontScanVolumes = StrDuplicate (DONT_SCAN_VOLUMES);

        MyFreePool (&GlobalConfig.WindowsRecoveryFiles);
        GlobalConfig.WindowsRecoveryFiles = StrDuplicate (WINDOWS_RECOVERY_FILES);

        MyFreePool (&GlobalConfig.MacOSRecoveryFiles);
        GlobalConfig.MacOSRecoveryFiles = StrDuplicate (MACOS_RECOVERY_FILES);

        MyFreePool (&GlobalConfig.DefaultSelection);
        GlobalConfig.DefaultSelection = StrDuplicate (L"+");
    } // if

    if (!FileExists (SelfDir, FileName)) {
        SwitchToText (FALSE);

        MsgStr = StrDuplicate (L"  - WARN: Cannot Find Configuration File ... Loading Defaults!!");
        PrintUglyText (MsgStr, NEXTLINE);

        #if REFIT_DEBUG > 0
        MsgLog ("%s\n", MsgStr);
        #endif

        if (!FileExists (SelfDir, L"icons")) {
            MyFreePool (&MsgStr);
            MsgStr = StrDuplicate (L"  - WARN: Cannot Find Icons Directory ... Switching to Text Mode!!");
            PrintUglyText (MsgStr, NEXTLINE);

            #if REFIT_DEBUG > 0
            MsgLog ("%s\n", MsgStr);
            #endif

            GlobalConfig.TextOnly = TRUE;
        }

        PauseForKey();
        SwitchToGraphics();
        MyFreePool (&MsgStr);

        return;
    }

    Status = RefitReadFile (SelfDir, FileName, &File, &i);
    if (EFI_ERROR(Status)) {
        return;
    }

    BOOLEAN DeclineSetting = FALSE;
    for (;;) {
        TokenCount = ReadTokenLine (&File, &TokenList);
        if (TokenCount == 0) {
            break;
        }

        if (MyStriCmp (TokenList[0], L"timeout")) {
            // Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.Timeout));
        }
        else if (MyStriCmp (TokenList[0], L"shutdown_after_timeout")) {
           GlobalConfig.ShutdownAfterTimeout = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"hideui")) {
            for (i = 1; i < TokenCount; i++) {
                Flag = TokenList[i];
                     if (MyStriCmp (Flag, L"all")       ) GlobalConfig.HideUIFlags  = HIDEUI_FLAG_ALL;
                else if (MyStriCmp (Flag, L"label")     ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_LABEL;
                else if (MyStriCmp (Flag, L"hints")     ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_HINTS;
                else if (MyStriCmp (Flag, L"banner")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_BANNER;
                else if (MyStriCmp (Flag, L"hwtest")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_HWTEST;
                else if (MyStriCmp (Flag, L"arrows")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_ARROWS;
                else if (MyStriCmp (Flag, L"editor")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_EDITOR;
                else if (MyStriCmp (Flag, L"badges")    ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_BADGES;
                else if (MyStriCmp (Flag, L"safemode")  ) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_SAFEMODE;
                else if (MyStriCmp (Flag, L"singleuser")) GlobalConfig.HideUIFlags |= HIDEUI_FLAG_SINGLEUSER;
                else {
                    SwitchToText (FALSE);

                    MsgStr = PoolPrint (
                        L"  - WARN: Invalid 'hideui' Flag:- '%s'",
                        Flag
                    );
                    PrintUglyText (MsgStr, NEXTLINE);

                    #if REFIT_DEBUG > 0
                    MsgLog ("%s\n", MsgStr);
                    #endif

                    PauseForKey();
                    MyFreePool (&MsgStr);
                }
            }
        }
        else if (MyStriCmp (TokenList[0], L"icons_dir")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.IconsDir));
        }
        else if (MyStriCmp (TokenList[0], L"set_boot_args")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SetBootArgs));
        }
        else if (MyStriCmp (TokenList[0], L"scanfor")) {
            for (i = 0; i < NUM_SCAN_OPTIONS; i++) {
                if (i < TokenCount) {
                    GlobalConfig.ScanFor[i] = TokenList[i][0];
                }
                else {
                    GlobalConfig.ScanFor[i] = ' ';
                }
            } // for
        }
        else if (MyStriCmp (TokenList[0], L"use_nvram")) {
            GlobalConfig.UseNvram = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"uefi_deep_legacy_scan")) {
            GlobalConfig.DeepLegacyScan = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"scan_delay") && (TokenCount == 2)) {
            HandleInt (TokenList, TokenCount, &(GlobalConfig.ScanDelay));
        }
        else if (MyStriCmp (TokenList[0], L"log_level") && (TokenCount == 2)) {
            // Signed integer as can have negative value (DA-TAG: negative disables logging - Not documented)
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.LogLevel));
            // Disable Logging on invalid max setting
            if (GlobalConfig.LogLevel > MAXLOGLEVEL) {
                GlobalConfig.LogLevel = -1;
            }
        }
        else if (MyStriCmp (TokenList[0], L"also_scan_dirs")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.AlsoScan));
        }
        else if (MyStriCmp (TokenList[0], L"don't_scan_dirs") || MyStriCmp (TokenList[0], L"dont_scan_dirs")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanDirs));
        }
        else if (MyStriCmp (TokenList[0], L"don't_scan_files") || MyStriCmp (TokenList[0], L"dont_scan_files")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanFiles));
        }
        else if (MyStriCmp (TokenList[0], L"don't_scan_tools") || MyStriCmp (TokenList[0], L"dont_scan_tools")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanTools));
        }
        else if (MyStriCmp (TokenList[0], L"don't_scan_firmware") || MyStriCmp (TokenList[0], L"dont_scan_firmware")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DontScanFirmware));
        }
        else if (MyStriCmp (TokenList[0], L"don't_scan_volumes") || MyStriCmp (TokenList[0], L"dont_scan_volumes")) {
            // Note: Do not use HandleStrings() because it modifies slashes, which might be present in volume name
            MyFreePool (&GlobalConfig.DontScanVolumes);
            GlobalConfig.DontScanVolumes = NULL;
            for (i = 1; i < TokenCount; i++) {
                MergeStrings (&GlobalConfig.DontScanVolumes, TokenList[i], L',');
            }
        }
        else if (MyStriCmp (TokenList[0], L"windows_recovery_files")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.WindowsRecoveryFiles));
        }
        else if (MyStriCmp (TokenList[0], L"scan_driver_dirs")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.DriverDirs));
        }
        else if (MyStriCmp (TokenList[0], L"showtools")) {
            SetMem (GlobalConfig.ShowTools, NUM_TOOLS * sizeof (UINTN), 0);
            GlobalConfig.HiddenTags = FALSE;
            for (i = 1; (i < TokenCount) && (i < NUM_TOOLS); i++) {
                Flag = TokenList[i];
                     if (MyStriCmp (Flag, L"exit")            ) GlobalConfig.ShowTools[i - 1] = TAG_EXIT;
                else if (MyStriCmp (Flag, L"shell")           ) GlobalConfig.ShowTools[i - 1] = TAG_SHELL;
                else if (MyStriCmp (Flag, L"gdisk")           ) GlobalConfig.ShowTools[i - 1] = TAG_GDISK;
                else if (MyStriCmp (Flag, L"about")           ) GlobalConfig.ShowTools[i - 1] = TAG_ABOUT;
                else if (MyStriCmp (Flag, L"reboot")          ) GlobalConfig.ShowTools[i - 1] = TAG_REBOOT;
                else if (MyStriCmp (Flag, L"gptsync")         ) GlobalConfig.ShowTools[i - 1] = TAG_GPTSYNC;
                else if (MyStriCmp (Flag, L"install")         ) GlobalConfig.ShowTools[i - 1] = TAG_INSTALL;
                else if (MyStriCmp (Flag, L"netboot")         ) GlobalConfig.ShowTools[i - 1] = TAG_NETBOOT;
                else if (MyStriCmp (Flag, L"memtest")         ) GlobalConfig.ShowTools[i - 1] = TAG_MEMTEST;
                else if (MyStriCmp (Flag, L"memtest86")       ) GlobalConfig.ShowTools[i - 1] = TAG_MEMTEST;
                else if (MyStriCmp (Flag, L"shutdown")        ) GlobalConfig.ShowTools[i - 1] = TAG_SHUTDOWN;
                else if (MyStriCmp (Flag, L"mok_tool")        ) GlobalConfig.ShowTools[i - 1] = TAG_MOK_TOOL;
                else if (MyStriCmp (Flag, L"firmware")        ) GlobalConfig.ShowTools[i - 1] = TAG_FIRMWARE;
                else if (MyStriCmp (Flag, L"bootorder")       ) GlobalConfig.ShowTools[i - 1] = TAG_BOOTORDER;
                else if (MyStriCmp (Flag, L"csr_rotate")      ) GlobalConfig.ShowTools[i - 1] = TAG_CSR_ROTATE;
                else if (MyStriCmp (Flag, L"fwupdate")        ) GlobalConfig.ShowTools[i - 1] = TAG_FWUPDATE_TOOL;
                else if (MyStriCmp (Flag, L"show_bootscreen") ) GlobalConfig.ShowTools[i - 1] = TAG_PRE_BOOTKICKER;
                else if (MyStriCmp (Flag, L"clean_nvram")     ) GlobalConfig.ShowTools[i - 1] = TAG_PRE_NVRAMCLEAN;
                else if (MyStriCmp (Flag, L"apple_recovery")  ) GlobalConfig.ShowTools[i - 1] = TAG_APPLE_RECOVERY;
                else if (MyStriCmp (Flag, L"windows_recovery")) GlobalConfig.ShowTools[i - 1] = TAG_WINDOWS_RECOVERY;
                else if (MyStriCmp (Flag, L"hidden_tags")) {
                    GlobalConfig.ShowTools[i - 1] = TAG_HIDDEN;
                    GlobalConfig.HiddenTags = TRUE;
                }
                else {
                    #if REFIT_DEBUG > 0
                    LOG(4, LOG_THREE_STAR_MID, L"Unknown Showtools Flag:- '%s'!!", Flag);
                    #endif
                }
            } // for
        }
        else if (MyStriCmp (TokenList[0], L"banner")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.BannerFileName));
        }
        else if (MyStriCmp (TokenList[0], L"banner_scale") && (TokenCount == 2)) {
            if (MyStriCmp (TokenList[1], L"noscale")) {
                GlobalConfig.BannerScale = BANNER_NOSCALE;
            }
            else if (MyStriCmp (TokenList[1], L"fillscreen") || MyStriCmp (TokenList[1], L"fullscreen")) {
                GlobalConfig.BannerScale = BANNER_FILLSCREEN;
            }
            else {
                MsgStr = PoolPrint (
                    L"  - WARN: Invalid 'banner_type' Flag:- '%s'",
                    TokenList[1]
                );
                PrintUglyText (MsgStr, NEXTLINE);

                #if REFIT_DEBUG > 0
                MsgLog ("%s\n", MsgStr);
                #endif

                PauseForKey();
                MyFreePool (&MsgStr);
            } // if/else MyStriCmp TokenList[0]
        }
        else if (MyStriCmp (TokenList[0], L"small_icon_size") && (TokenCount == 2)) {
            HandleInt (TokenList, TokenCount, &i);
            if (i >= 32) {
                GlobalConfig.IconSizes[ICON_SIZE_SMALL] = i;
            }
        }
        else if (MyStriCmp (TokenList[0], L"big_icon_size") && (TokenCount == 2)) {
            HandleInt (TokenList, TokenCount, &i);
            if (i >= 32) {
                GlobalConfig.IconSizes[ICON_SIZE_BIG] = i;
                GlobalConfig.IconSizes[ICON_SIZE_BADGE] = i / 4;
            }
        }
        else if (MyStriCmp (TokenList[0], L"mouse_size") && (TokenCount == 2)) {
            HandleInt (TokenList, TokenCount, &i);
            if (i >= DEFAULT_MOUSE_SIZE) {
                GlobalConfig.IconSizes[ICON_SIZE_MOUSE] = i;
            }
        }
        else if (MyStriCmp (TokenList[0], L"selection_small")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SelectionSmallFileName));
        }
        else if (MyStriCmp (TokenList[0], L"selection_big")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SelectionBigFileName));
        }
        else if (MyStriCmp (TokenList[0], L"default_selection")) {
            if (TokenCount == 4) {
                SetDefaultByTime (TokenList, &(GlobalConfig.DefaultSelection));
           }
           else {
               HandleString (TokenList, TokenCount, &(GlobalConfig.DefaultSelection));
           }
        }
        else if (MyStriCmp (TokenList[0], L"textonly")) {
            GlobalConfig.TextOnly = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"textmode")) {
            HandleInt (TokenList, TokenCount, &(GlobalConfig.RequestedTextMode));
        }
        else if (MyStriCmp (TokenList[0], L"resolution") && ((TokenCount == 2) || (TokenCount == 3))) {
            if (MyStriCmp(TokenList[1], L"max")) {
                // DA-TAG: has been set to 0 so as to ignore the 'max' setting
                //GlobalConfig.RequestedScreenWidth  = MAX_RES_CODE;
                //GlobalConfig.RequestedScreenHeight = MAX_RES_CODE;
                GlobalConfig.RequestedScreenWidth  = 0;
                GlobalConfig.RequestedScreenHeight = 0;
            }
            else {
                GlobalConfig.RequestedScreenWidth = Atoi(TokenList[1]);
                if (TokenCount == 3) {
                    GlobalConfig.RequestedScreenHeight = Atoi(TokenList[2]);
                }
                else {
                    GlobalConfig.RequestedScreenHeight = 0;
                }
            }
        }
        else if (MyStriCmp (TokenList[0], L"screensaver")) {
            // Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.ScreensaverTime));
        }
        else if (MyStriCmp (TokenList[0], L"use_graphics_for")) {
            if ((TokenCount == 2) || ((TokenCount > 2) && (!MyStriCmp (TokenList[1], L"+")))) {
                GlobalConfig.GraphicsFor = 0;
            }

            for (i = 1; i < TokenCount; i++) {
                     if (MyStriCmp (TokenList[i], L"osx")     ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_OSX;
                else if (MyStriCmp (TokenList[i], L"grub")    ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_GRUB;
                else if (MyStriCmp (TokenList[i], L"linux")   ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_LINUX;
                else if (MyStriCmp (TokenList[i], L"elilo")   ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_ELILO;
                else if (MyStriCmp (TokenList[i], L"clover")  ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_CLOVER;
                else if (MyStriCmp (TokenList[i], L"windows") ) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_WINDOWS;
                else if (MyStriCmp (TokenList[i], L"opencore")) GlobalConfig.GraphicsFor |= GRAPHICS_FOR_OPENCORE;
            } // for
        }
        else if (MyStriCmp (TokenList[0], L"font") && (TokenCount == 2)) {
            egLoadFont (TokenList[1]);
        }
        else if (MyStriCmp (TokenList[0], L"scan_all_linux_kernels")) {
            GlobalConfig.ScanAllLinux = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"fold_linux_kernels")) {
            GlobalConfig.FoldLinuxKernels = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"extra_kernel_version_strings")) {
            HandleStrings (TokenList, TokenCount, &(GlobalConfig.ExtraKernelVersionStrings));
        }
        else if (MyStriCmp (TokenList[0], L"max_tags")) {
            HandleInt (TokenList, TokenCount, &(GlobalConfig.MaxTags));
        }
        else if (MyStriCmp (TokenList[0], L"enable_and_lock_vmx")) {
            GlobalConfig.EnableAndLockVMX = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"spoof_osx_version")) {
            HandleString (TokenList, TokenCount, &(GlobalConfig.SpoofOSXVersion));
        }
        else if (MyStriCmp (TokenList[0], L"csr_values")) {
            HandleHexes (TokenList, TokenCount, CSR_MAX_LEGAL_VALUE, &(GlobalConfig.CsrValues));
        }
        else if (MyStriCmp (TokenList[0], L"include") &&
            (TokenCount == 2) &&
            MyStriCmp (FileName, GlobalConfig.ConfigFilename)
        ) {
            if (!MyStriCmp (TokenList[1], FileName)) {
                #if REFIT_DEBUG > 0
                MsgLog ("Detected Overrides - ");
                #endif

                ReadConfig (TokenList[1]);
            }
        }
        else if (MyStriCmp (TokenList[0], L"write_systemd_vars")) {
            GlobalConfig.WriteSystemdVars = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"enable_mouse")) {
            GlobalConfig.EnableMouse = HandleBoolean (TokenList, TokenCount);
            if (GlobalConfig.EnableMouse) {
                GlobalConfig.EnableTouch = FALSE;
            }
        }
        else if (MyStriCmp (TokenList[0], L"enable_touch")) {
            GlobalConfig.EnableTouch = HandleBoolean (TokenList, TokenCount);
            if (GlobalConfig.EnableTouch) {
                GlobalConfig.EnableMouse = FALSE;
            }
        }
        else if (MyStriCmp (TokenList[0], L"ignore_previous_boot")) {
            GlobalConfig.IgnorePreviousBoot = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"ignore_hidden_icons")) {
            GlobalConfig.IgnoreHiddenIcons = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"prefer_hidden_icons")) {
            GlobalConfig.PreferHiddenIcons = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"text_renderer")) {
            GlobalConfig.TextRenderer = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"uga_pass_through")) {
            GlobalConfig.UgaPassThrough = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"provide_console_gop")) {
            GlobalConfig.ProvideConsoleGOP = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"direct_gop_renderer")) {
            GlobalConfig.UseDirectGop = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"continue_on_warning")) {
            GlobalConfig.ContinueOnWarning = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"force_trim")) {
            GlobalConfig.ForceTRIM = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"disable_compat_check")) {
            GlobalConfig.DisableCompatCheck = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"disable_amfi")) {
            GlobalConfig.DisableAMFI = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"decline_reloadgop")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.ReloadGOP = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"decline_apfsload")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SupplyAPFS = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"decline_apfsmute")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SilenceAPFS = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"decline_apfssync")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.SyncAPFS = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"decline_nvramprotect")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.ProtectNVRAM = DeclineSetting ? FALSE : TRUE;

        }
        else if (MyStriCmp (TokenList[0], L"decline_espfilter")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.ScanAllESP = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"decline_tagshelp")) {
            DeclineSetting = HandleBoolean (TokenList, TokenCount);
            GlobalConfig.TagsHelp = DeclineSetting ? FALSE : TRUE;
        }
        else if (MyStriCmp (TokenList[0], L"normalise_csr")) {
            GlobalConfig.NormaliseCSR = HandleBoolean (TokenList, TokenCount);
        }
        else if (MyStriCmp (TokenList[0], L"scale_ui")) {
            // Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.ScaleUI));
        }
        else if (MyStriCmp (TokenList[0], L"active_csr")) {
            // Signed integer as can have negative value
            HandleSignedInt (TokenList, TokenCount, &(GlobalConfig.ActiveCSR));
        }
        else if (MyStriCmp (TokenList[0], L"mouse_speed") && (TokenCount == 2)) {
            HandleInt (TokenList, TokenCount, &i);
            if (i < 1)  i = 1;
            if (i > 32) i = 32;
            GlobalConfig.MouseSpeed = i;
        }

        FreeTokenLine (&TokenList, &TokenCount);
    } // for
    FreeTokenLine (&TokenList, &TokenCount);

    // "TagHelp" is active
    if (!GlobalConfig.TagsHelp) {
        // "TagHelp" feature is active ... Set "found" flag to false
        BOOLEAN HiddenTagsFlag = FALSE;
        // Loop through GlobalConfig.ShowTools list to check for "hidden_tags" tool
        for (i = 0; i < NUM_TOOLS; i++) {
            switch (GlobalConfig.ShowTools[i]) {
                case TAG_EXIT:
                case TAG_ABOUT:
                case TAG_SHELL:
                case TAG_GDISK:
                case TAG_REBOOT:
                case TAG_MEMTEST:
                case TAG_GPTSYNC:
                case TAG_NETBOOT:
                case TAG_INSTALL:
                case TAG_MOK_TOOL:
                case TAG_FIRMWARE:
                case TAG_SHUTDOWN:
                case TAG_BOOTORDER:
                case TAG_CSR_ROTATE:
                case TAG_FWUPDATE_TOOL:
                case TAG_PRE_BOOTKICKER:
                case TAG_PRE_NVRAMCLEAN:
                case TAG_APPLE_RECOVERY:
                case TAG_WINDOWS_RECOVERY:
                    // Continue checking
                    break;
                case TAG_HIDDEN:
                    // Tag to end search ... "hidden_tags" tool is already set
                    HiddenTagsFlag = TRUE;
                    break;
                default:
                    // Setup help needed ... "hidden_tags" tool is not set
                    GlobalConfig.ShowTools[i] = TAG_HIDDEN;
                    GlobalConfig.HiddenTags   = TRUE;

                    // Tag to end search ... "hidden_tags" tool is now set
                    HiddenTagsFlag = TRUE;
                    break;
            }

            if (HiddenTagsFlag) {
                // Halt search loop
                break;
            }
        } // for
    }

    if ((GlobalConfig.DontScanFiles) && (GlobalConfig.WindowsRecoveryFiles)) {
        MergeStrings (&(GlobalConfig.DontScanFiles), GlobalConfig.WindowsRecoveryFiles, L',');
    }
    MyFreePool (&File.Buffer);

    if (!FileExists (SelfDir, L"icons") && !FileExists (SelfDir, GlobalConfig.IconsDir)) {
        #if REFIT_DEBUG > 0
        MsgLog ("  - WARN: Cannot Find Icons Directory ... Switching to Text Mode\n");
        #endif

        GlobalConfig.TextOnly = TRUE;
    }

    // Disable ProtectNVRAM on Non-Apple Firmware
    if (MyStrStr (gST->FirmwareVendor, L"Apple") == NULL) {
        GlobalConfig.ProtectNVRAM = FALSE;
    }

    SilenceAPFS = GlobalConfig.SilenceAPFS;
} // VOID ReadConfig()

static
VOID AddSubmenu (
    LOADER_ENTRY *Entry,
    REFIT_FILE   *File,
    REFIT_VOLUME *Volume,
    CHAR16       *Title
) {
    REFIT_MENU_SCREEN   *SubScreen;
    LOADER_ENTRY        *SubEntry;
    UINTN                TokenCount;
    CHAR16             **TokenList;
    BOOLEAN              TitleVolume = FALSE;

    SubScreen = InitializeSubScreen (Entry);

    // Set defaults for the new entry
    // Will be modified based on lines read from the config file
    SubEntry = InitializeLoaderEntry (Entry);

    if ((SubEntry == NULL) || (SubScreen == NULL)) {
        return;
    }

    SubEntry->Enabled = TRUE;

    while (((TokenCount = ReadTokenLine (File, &TokenList)) > 0) &&
        (StrCmp (TokenList[0], L"}") != 0)
    ) {
        if (SubEntry->Enabled) {
            if (MyStriCmp (TokenList[0], L"loader") && (TokenCount > 1)) {
                // set the boot loader filename
                MyFreePool (&SubEntry->LoaderPath);
                SubEntry->LoaderPath = StrDuplicate (TokenList[1]);
                SubEntry->Volume     = Volume;
            }
            else if (MyStriCmp (TokenList[0], L"volume") && (TokenCount > 1)) {
                if (FindVolume (&Volume, TokenList[1])) {
                    if ((Volume != NULL) && (Volume->IsReadable) && (Volume->RootDir)) {
                        TitleVolume = TRUE;
                        SubEntry->me.BadgeImage = Volume->VolBadgeImage;
                        SubEntry->Volume        = Volume;
                    }
                }
            }
            else if (MyStriCmp (TokenList[0], L"initrd")) {
                MyFreePool (&SubEntry->InitrdPath);
                SubEntry->InitrdPath = NULL;
                if (TokenCount > 1) {
                    SubEntry->InitrdPath = StrDuplicate (TokenList[1]);
                }
            }
            else if (MyStriCmp (TokenList[0], L"options")) {
                MyFreePool (&SubEntry->LoadOptions);
                SubEntry->LoadOptions = NULL;
                if (TokenCount > 1) {
                    SubEntry->LoadOptions = StrDuplicate (TokenList[1]);
                }
            }
            else if (MyStriCmp (TokenList[0], L"add_options") && (TokenCount > 1)) {
                MergeStrings (&SubEntry->LoadOptions, TokenList[1], L' ');
            }
            else if (MyStriCmp (TokenList[0], L"graphics") && (TokenCount > 1)) {
                SubEntry->UseGraphicsMode = MyStriCmp (TokenList[1], L"on");
            }
            else if (MyStriCmp (TokenList[0], L"disabled")) {
                SubEntry->Enabled = FALSE;
            }
        } // if SubEntry->Enabled

        FreeTokenLine (&TokenList, &TokenCount);
    } // while
    FreeTokenLine (&TokenList, &TokenCount);

    if (!SubEntry->Enabled) {
        FreeLoaderEntry (SubEntry);

        return;
    }

    if (TitleVolume) {
        SubEntry->me.Title = PoolPrint (
            L"Boot %s from %s",
            (Title != NULL) ? Title : L"Unknown",
            Volume->VolName
        );
    }
    else {
        SubEntry->me.Title = StrDuplicate (Title);
    }

    if (SubEntry->InitrdPath != NULL) {
        MergeStrings (&SubEntry->LoadOptions, L"initrd=", L' ');
        MergeStrings (&SubEntry->LoadOptions, SubEntry->InitrdPath, 0);
        MyFreePool (&SubEntry->InitrdPath);
        SubEntry->InitrdPath = NULL;
    }

    AddMenuEntry (SubScreen, (REFIT_MENU_ENTRY *) SubEntry);

    Entry->me.SubScreen = SubScreen;
} // VOID AddSubmenu()

// Adds the options from a single config.conf stanza to a new loader entry and returns
// that entry. The calling function is then responsible for adding the entry to the
// list of entries.
static
LOADER_ENTRY * AddStanzaEntries (
    REFIT_FILE   *File,
    REFIT_VOLUME *Volume,
    CHAR16       *Title
) {
    UINTN           TokenCount;
    CHAR16         *OurEfiBootNumber;
    CHAR16        **TokenList;
    CHAR16         *LoadOptions       = NULL;
    BOOLEAN         HasPath           = FALSE;
    BOOLEAN         FirmwareBootNum   = FALSE;
    BOOLEAN         DefaultsSet       = FALSE;
    BOOLEAN         AddedSubmenu      = FALSE;
    REFIT_VOLUME   *CurrentVolume     = Volume;
    REFIT_VOLUME   *PreviousVolume;
    LOADER_ENTRY   *Entry;

    // prepare the menu entry
    Entry = InitializeLoaderEntry (NULL);
    if (Entry == NULL) {
        return NULL;
    }

    Entry->Title = (Title != NULL)
        ? StrDuplicate (Title)
        : StrDuplicate (L"Unknown");
    Entry->me.Row          = 0;
    Entry->Enabled         = TRUE;
    Entry->Volume          = CurrentVolume;
    Entry->me.BadgeImage   = CurrentVolume->VolBadgeImage;
    Entry->DiscoveryType   = DISCOVERY_TYPE_MANUAL;

    // Parse the config file to add options for a single stanza, terminating when the token
    // is "}" or when the end of file is reached.
    #if REFIT_DEBUG > 0
    CHAR16 *MsgStr = NULL;

    static BOOLEAN OtherCall;
    if (OtherCall) {
        /* Exception for LOG_THREE_STAR_SEP */
        LOG(3, LOG_THREE_STAR_SEP, L"NEXT STANZA");
    }
    OtherCall = TRUE;

    LOG(3, LOG_LINE_NORMAL, L"Adding User Configured Loader:- '%s'", Entry->Title);
    #endif

    while (((TokenCount = ReadTokenLine (File, &TokenList)) > 0) &&
        (StrCmp (TokenList[0], L"}") != 0)
    ) {
        if (Entry->Enabled) {
            if (MyStriCmp (TokenList[0], L"loader") && (TokenCount > 1)) {
                // set the boot loader filename
                Entry->LoaderPath = StrDuplicate (TokenList[1]);

                HasPath = (Entry->LoaderPath && StrLen (Entry->LoaderPath) > 0);
                if (HasPath) {
                    #if REFIT_DEBUG > 0
                    LOG(3, LOG_LINE_NORMAL, L"Adding Loader Path:- '%s'", Entry->LoaderPath);
                    #endif

                    SetLoaderDefaults (Entry, TokenList[1], CurrentVolume);

                    // Discard default options, if any
                    MyFreePool (&Entry->LoadOptions);
                    DefaultsSet = TRUE;
                }
            }
            else if (MyStriCmp (TokenList[0], L"volume") && (TokenCount > 1)) {
                PreviousVolume = CurrentVolume;
                if (!FindVolume (&CurrentVolume, TokenList[1])) {
                    #if REFIT_DEBUG > 0
                    LOG(4, LOG_THREE_STAR_MID, L"Could Not Find Volume for '%s'!!", Entry->Title);
                    #endif
                }
                else {
                    #if REFIT_DEBUG > 0
                    LOG(3, LOG_LINE_NORMAL, L"Adding Volume for '%s'", Entry->Title);
                    #endif

                    if ((CurrentVolume != NULL) &&
                        (CurrentVolume->IsReadable) &&
                        (CurrentVolume->RootDir)
                    ) {
                        Entry->Volume        = CurrentVolume;
                        Entry->me.BadgeImage = CurrentVolume->VolBadgeImage;
                    }
                    else {
                        // Will not work out ... reset to previous working volume
                        CurrentVolume = PreviousVolume;
                    }
                } // if/else !FindVolume
            }
            else if (MyStriCmp (TokenList[0], L"icon") && (TokenCount > 1)) {
                if (!AllowGraphicsMode) {
                    #if REFIT_DEBUG > 0
                    LOG(4, LOG_THREE_STAR_MID,
                        L"In AddStanzaEntries ... Skipped Loading Icon in Text Screen Mode"
                    );
                    #endif
                }
                else {
                    #if REFIT_DEBUG > 0
                    if (Entry->me.Image == NULL) {
                        MsgStr = PoolPrint (L"Adding Icon for '%s'", Entry->Title);
                    }
                    else {
                        MsgStr = PoolPrint (L"Overriding Previous Icon for '%s'", Entry->Title);
                    }
                    LOG(3, LOG_LINE_NORMAL, L"%s", MsgStr);
                    MyFreePool (&MsgStr);
                    #endif

                    egFreeImage (Entry->me.Image);
                    Entry->me.Image = egLoadIcon (
                        CurrentVolume->RootDir,
                        TokenList[1],
                        GlobalConfig.IconSizes[ICON_SIZE_BIG]
                    );

                    if (Entry->me.Image == NULL) {
                        // Set dummy image if icon was not found
                        Entry->me.Image = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BIG]);
                    }
                }
            }
            else if (MyStriCmp (TokenList[0], L"initrd") && (TokenCount > 1)) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL, L"Adding Initrd for '%s'", Entry->Title);
                #endif

                MyFreePool (&Entry->InitrdPath);
                Entry->InitrdPath = StrDuplicate (TokenList[1]);
            }
            else if (MyStriCmp (TokenList[0], L"options") && (TokenCount > 1)) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL, L"Adding Options for '%s'", Entry->Title);
                #endif

                LoadOptions = StrDuplicate (TokenList[1]);
            }
            else if (MyStriCmp (TokenList[0], L"ostype") && (TokenCount > 1)) {
                if (TokenCount > 1) {
                    #if REFIT_DEBUG > 0
                    LOG(3, LOG_LINE_NORMAL, L"Adding OS Type for '%s'", Entry->Title);
                    #endif

                    Entry->OSType = TokenList[1][0];
                }
            }
            else if (MyStriCmp (TokenList[0], L"graphics") && (TokenCount > 1)) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL,
                    L"Adding Graphics Mode for '%s'",
                    (HasPath) ? Entry->LoaderPath : Entry->Title
                );
                #endif

                Entry->UseGraphicsMode = MyStriCmp (TokenList[1], L"on");
            }
            else if (MyStriCmp (TokenList[0], L"disabled")) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL, L"Entry is Disabled!!");
                #endif

                Entry->Enabled = FALSE;
            }
            else if (MyStriCmp(TokenList[0], L"firmware_bootnum") && (TokenCount > 1)) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL, L"Adding Firmware Bootnum Entry for '%s'", Entry->Title);
                #endif

                Entry->me.Tag        = TAG_FIRMWARE_LOADER;
                Entry->me.BadgeImage = BuiltinIcon (BUILTIN_ICON_VOL_EFI);

                if (Entry->me.BadgeImage == NULL) {
                    // Set dummy image if badge was not found
                    Entry->me.BadgeImage = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BADGE]);
                }

                DefaultsSet      = TRUE;
                FirmwareBootNum  = TRUE;
                MyFreePool (&OurEfiBootNumber);
                OurEfiBootNumber = StrDuplicate (TokenList[1]);
            }
            else if (MyStriCmp (TokenList[0], L"submenuentry") && (TokenCount > 1)) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_LINE_NORMAL,
                    L"Adding Submenu Entry for '%s'",
                    (HasPath) ? Entry->LoaderPath : Entry->Title
                );
                #endif

                AddSubmenu (Entry, File, CurrentVolume, TokenList[1]);
                AddedSubmenu = TRUE;
            } // set options to pass to the loader program
        } // if Entry->Enabled

        FreeTokenLine (&TokenList, &TokenCount);
    } // while
    FreeTokenLine (&TokenList, &TokenCount);

    // Disabled entries are returned "as is" as will be discarded later
    if (Entry->Enabled) {
        // Set Screen Title
        if (!FirmwareBootNum && Entry->Volume->VolName) {
            Entry->me.Title = PoolPrint (
                L"Boot %s from %s",
                (Title != NULL) ? Title : L"Unknown",
                Entry->Volume->VolName
            );
        }
        else {
            if (FirmwareBootNum) {
                // Clear potentially wrongly set items
                MyFreePool (&Entry->LoaderPath);
                MyFreePool (&Entry->EfiLoaderPath);
                MyFreePool (&Entry->LoadOptions);
                MyFreePool (&Entry->InitrdPath);

                Entry->me.Title = PoolPrint (
                    L"Boot %s [Firmware Boot Number]",
                    (Title != NULL) ? Title : L"Unknown"
                );

                Entry->EfiBootNum = StrToHex (OurEfiBootNumber, 0, 16);
            }
            else {
                Entry->me.Title = PoolPrint (
                    L"Boot %s",
                    (Title != NULL) ? Title : L"Unknown"
                );
            }
        }

        // Set load options, if any
        if (LoadOptions && StrLen (LoadOptions) > 0) {
            MyFreePool (&Entry->LoadOptions);
            Entry->LoadOptions = StrDuplicate (LoadOptions);
        }

        if (AddedSubmenu) {
            AddMenuEntry (Entry->me.SubScreen, &MenuEntryReturn);
        }

        if (Entry->InitrdPath && StrLen (Entry->InitrdPath) > 0) {
            if (Entry->LoadOptions && StrLen (Entry->LoadOptions) > 0) {
                MergeStrings (&Entry->LoadOptions, L"initrd=", L' ');
                MergeStrings (&Entry->LoadOptions, Entry->InitrdPath, 0);
            }
            else {
                Entry->LoadOptions = PoolPrint (
                    L"initrd=%s",
                    Entry->InitrdPath
                );
            }
            MyFreePool (&Entry->InitrdPath);
            Entry->InitrdPath = NULL;
        }

        if (!DefaultsSet) {
            // No "loader" line ... use bogus one
            SetLoaderDefaults (Entry, L"\\EFI\\BOOT\\nemo.efi", CurrentVolume);
        }

        if (AllowGraphicsMode && Entry->me.Image == NULL) {
            // Still no icon ... set dummy image
            Entry->me.Image = DummyImage (GlobalConfig.IconSizes[ICON_SIZE_BIG]);
        }
    } // if Entry->Enabled

    MyFreePool (&OurEfiBootNumber);
    MyFreePool (&LoadOptions);

    return Entry;
} // static VOID AddStanzaEntries()

// Read the user-configured menu entries from config.conf and add or delete
// entries based on the contents of that file.
VOID ScanUserConfigured (
    CHAR16 *FileName
) {
    EFI_STATUS         Status;
    REFIT_FILE         File;
    CHAR16           **TokenList;
    UINTN              TokenCount, size;
    LOADER_ENTRY      *Entry;

    static UINTN EntryCount = 0;

    if (FileExists (SelfDir, FileName)) {
        Status = RefitReadFile (SelfDir, FileName, &File, &size);
        if (!EFI_ERROR(Status)) {
            while ((TokenCount = ReadTokenLine (&File, &TokenList)) > 0) {
                if (MyStriCmp (TokenList[0], L"menuentry") && (TokenCount > 1)) {
                    Entry = AddStanzaEntries (&File, SelfVolume, TokenList[1]);
                    if (Entry) {
                        EntryCount = EntryCount + 1;
                        if (!Entry->Enabled) {
                            FreeLoaderEntry (Entry);
                        }
                        else {
                            #if REFIT_DEBUG > 0
                            MsgLog ("\n");
                            MsgLog (
                                "  - Found '%s' on '%s'",
                                Entry->Title,
                                (SelfVolume->VolName) ? SelfVolume->VolName : Entry->LoaderPath
                            );
                            #endif

                            if (Entry->me.SubScreen == NULL) {
                                GenerateSubScreen (Entry, SelfVolume, TRUE);
                            }
                            AddPreparedLoaderEntry (Entry);
                        }
                    }
                }
                else if (MyStriCmp (TokenList[0], L"include") && (TokenCount == 2) &&
                    MyStriCmp (FileName, GlobalConfig.ConfigFilename)
                ) {
                    if (!MyStriCmp (TokenList[1], FileName)) {
                        InnerScan = TRUE;
                        ScanUserConfigured (TokenList[1]);
                    }
                }

                FreeTokenLine (&TokenList, &TokenCount);
            } // while

            FreeTokenLine (&TokenList, &TokenCount);
        }
    } // if FileExists

    #if REFIT_DEBUG > 0
    if (!InnerScan) {
        LOG(2, LOG_THREE_STAR_SEP, L"Processed %d User Defined Stanzas", EntryCount);
    }
    #endif

    InnerScan = FALSE;
} // VOID ScanUserConfigured()

// Create an options file based on /etc/fstab. The resulting file has two options
// lines, one of which boots the system with "ro root={rootfs}" and the other of
// which boots the system with "ro root={rootfs} single", where "{rootfs}" is the
// filesystem identifier associated with the "/" line in /etc/fstab.
static
REFIT_FILE * GenerateOptionsFromEtcFstab (
    REFIT_VOLUME *Volume
) {
    EFI_STATUS    Status;
    UINTN         TokenCount, i;
    CHAR16      **TokenList;
    CHAR16       *Line;
    CHAR16       *Root    = NULL;
    REFIT_FILE   *Options = NULL;
    REFIT_FILE   *Fstab   = NULL;

    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1 - START");

    if (FileExists(Volume->RootDir, L"\\etc\\fstab")) {
        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 1");
        Options = AllocateZeroPool (sizeof(REFIT_FILE));

        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 2");
        Fstab = AllocateZeroPool (sizeof(REFIT_FILE));

        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 3");
        Status = RefitReadFile (Volume->RootDir, L"\\etc\\fstab", Fstab, &i);

        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4");
        if (CheckError (Status, L"while reading /etc/fstab")) {
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 1");
            if (Options != NULL) {
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 1a 1");
                MyFreePool (&Options);
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 1a 2");
            }

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 2");
            if (Fstab != NULL) {
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 2a 1");
                MyFreePool (&Fstab);
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 2a 2");
            }

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 3");
            Options = NULL;
            Fstab   = NULL;
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4a 4");
        }
        else {
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 1");
            // File read; locate root fs and create entries
            Options->Encoding = ENCODING_UTF16_LE;

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2");
            while ((TokenCount = ReadTokenLine (Fstab, &TokenList)) > 0) {
                #if REFIT_DEBUG > 0
                LOG(3, LOG_THREE_STAR_MID,
                    L"Read Line Holding %d Token%s From '/etc/fstab'",
                    TokenCount,
                    (TokenCount == 1) ? L"" : L"s"
                );
                #endif

                LOG(5, LOG_BLANK_LINE_SEP, L"X");
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1  START WHILE LOOP");
                if (TokenCount > 2) {
                    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 1");
                    if (StrCmp (TokenList[1], L"\\") == 0) {
                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 1a 1");
                        Root = PoolPrint (L"%s", TokenList[0]);
                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 1a 2");
                    }
                    else if (StrCmp (TokenList[2], L"\\") == 0) {
                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 1b 1");
                        Root = PoolPrint (L"%s=%s", TokenList[0], TokenList[1]);
                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 1b 2");
                    }

                    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2");
                    if (Root && (Root[0] != L'\0')) {
                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 1");
                        for (i = 0; i < StrLen (Root); i++) {
                            LOG(5, LOG_BLANK_LINE_SEP, L"X");
                            LOG(5, LOG_LINE_FORENSIC,
                                L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 1  START FOR LOOP"
                            );
                            if (Root[i] == '\\') {
                                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 1a 1");
                                Root[i] = '/';
                                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 1a 2");
                            }
                            LOG(5, LOG_LINE_FORENSIC,
                                L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 2  END FOR LOOP"
                            );
                            LOG(5, LOG_BLANK_LINE_SEP, L"X");
                        }

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 2");
                        Line = PoolPrint (L"\"Boot with Normal Options\"    \"ro root=%s\"\n", Root);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 3");
                        MergeStrings ((CHAR16 **) &(Options->Buffer), Line, 0);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 4");
                        MyFreePool (&Line);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 5");
                        Line = PoolPrint (L"\"Boot into Single User Mode\"  \"ro root=%s single\"\n", Root);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 6");
                        MergeStrings ((CHAR16**) &(Options->Buffer), Line, 0);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 7");
                        MyFreePool (&Line);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 8");
                        Options->BufferSize = StrLen ((CHAR16*) Options->Buffer) * sizeof(CHAR16);

                        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 2a 9");
                    } // if

                    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 3");
                    MyFreePool (&Root);
                    Root = NULL;

                    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 1a 4");
                 } // if

                 LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 2");
                 FreeTokenLine (&TokenList, &TokenCount);

                 LOG(5, LOG_LINE_FORENSIC,
                     L"In GenerateOptionsFromEtcFstab ... 1a 4b 2a 3 END WHILE LOOP"
                 );
                 LOG(5, LOG_BLANK_LINE_SEP, L"X");
            } // while

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 3");
            if (Options->Buffer) {
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 3a 1");
                Options->Current8Ptr  = (CHAR8 *)Options->Buffer;
                Options->End8Ptr      = Options->Current8Ptr + Options->BufferSize;
                Options->Current16Ptr = (CHAR16 *)Options->Buffer;
                Options->End16Ptr     = Options->Current16Ptr + (Options->BufferSize >> 1);
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 3a 2");
            }
            else {
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 3b 1");
                MyFreePool (&Options);
                Options = NULL;
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 3b 2");
            }

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 4");
            MyFreePool (&Fstab->Buffer);
            MyFreePool (&Fstab);
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 4b 5");
        } // if/else file read error
        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 1a 5");
    } // if /etc/fstab exists

    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromEtcFstab ... 2 - END:- return REFIT_FILE *Options");
    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    return Options;
} // GenerateOptionsFromEtcFstab()


// Create options from partition type codes. Specifically, if the earlier
// partition scan found a partition with a type code corresponding to a root
// filesystem according to the Freedesktop.org Discoverable Partitions Spec
// (http://www.freedesktop.org/wiki/Specifications/DiscoverablePartitionsSpec/),
// this function returns an appropriate file with two lines, one with
// "ro root=/dev/disk/by-partuuid/{GUID}" and the other with that plus "single".
// Note that this function returns the LAST partition found with the
// appropriate type code, so this will work poorly on dual-boot systems or
// if the type code is set incorrectly.
static
REFIT_FILE * GenerateOptionsFromPartTypes (VOID) {
    REFIT_FILE   *Options = NULL;
    CHAR16       *Line, *GuidString, *WriteStatus;

    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1 - START");
    if (GlobalConfig.DiscoveredRoot) {
        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 1");
        Options = AllocateZeroPool (sizeof(REFIT_FILE));
        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2");
        if (Options) {
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 1");
            Options->Encoding = ENCODING_UTF16_LE;

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 2");
            GuidString = GuidAsString (&(GlobalConfig.DiscoveredRoot->PartGuid));

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 3");
            WriteStatus = GlobalConfig.DiscoveredRoot->IsMarkedReadOnly ? L"ro" : L"rw";

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 4");
            ToLower (GuidString);

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5");
            if (GuidString) {
                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 1");
                Line = PoolPrint (
                    L"\"Boot with Normal Options\"    \"%s root=/dev/disk/by-partuuid/%s\"\n",
                    WriteStatus, GuidString
                );

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 2");
                MergeStrings ((CHAR16 **) &(Options->Buffer), Line, 0);

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 3");
                MyFreePool (&Line);

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 4");
                Line = PoolPrint (
                    L"\"Boot into Single User Mode\"  \"%s root=/dev/disk/by-partuuid/%s single\"\n",
                    WriteStatus, GuidString
                );

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 5");
                MergeStrings ((CHAR16**) &(Options->Buffer), Line, 0);

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 6");
                MyFreePool (&Line);
                MyFreePool (&GuidString);

                LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 5a 7");
            } // if (GuidString)

            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 6");
            Options->BufferSize   = StrLen ((CHAR16*) Options->Buffer) * sizeof(CHAR16);
            Options->Current8Ptr  = (CHAR8 *) Options->Buffer;
            Options->End8Ptr      = Options->Current8Ptr + Options->BufferSize;
            Options->Current16Ptr = (CHAR16 *) Options->Buffer;
            Options->End16Ptr     = Options->Current16Ptr + (Options->BufferSize >> 1);
            LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 2a 7");
        } // if (Options allocated OK)
        LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 1a 3");
    } // if (partition has root GUID)

    LOG(5, LOG_LINE_FORENSIC, L"In GenerateOptionsFromPartTypes ... 2 - END:- return REFIT_FILE *Options");
    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    return Options;
} // REFIT_FILE * GenerateOptionsFromPartTypes()


// Read a Linux kernel options file for a Linux boot loader into memory. The LoaderPath
// and Volume variables identify the location of the options file, but not its name --
// you pass this function the filename of the Linux kernel, initial RAM disk, or other
// file in the target directory, and this function finds the file with a name in the
// comma-delimited list of names specified by LINUX_OPTIONS_FILENAMES within that
// directory and loads it. If a RefindPlus options file can't be found, try to generate
// minimal options from /etc/fstab on the same volume as the kernel. This typically
// works only if the kernel is being read from the Linux root filesystem.
//
// The return value is a pointer to the REFIT_FILE handle for the file, or NULL if
// it was not found.
REFIT_FILE * ReadLinuxOptionsFile (
    IN CHAR16       *LoaderPath,
    IN REFIT_VOLUME *Volume
) {
    EFI_STATUS   Status;
    CHAR16      *OptionsFilename;
    CHAR16      *FullFilename;
    UINTN        size;
    UINTN        i         = 0;
    BOOLEAN      GoOn      = TRUE;
    BOOLEAN      FileFound = FALSE;
    REFIT_FILE  *File      = NULL;

    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 1 - START");

    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2");
    do {
        LOG(5, LOG_BLANK_LINE_SEP, L"X");
        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 1  START DO LOOP");
        OptionsFilename = FindCommaDelimited (LINUX_OPTIONS_FILENAMES, i++);

        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 2");
        FullFilename = FindPath (LoaderPath);

        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3");
        if ((OptionsFilename != NULL) && (FullFilename != NULL)) {
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 1");
            MergeStrings (&FullFilename, OptionsFilename, '\\');

            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2");
            if (FileExists (Volume->RootDir, FullFilename)) {
                LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 1");
                File = AllocateZeroPool(sizeof(REFIT_FILE));

                LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 2");
                Status = RefitReadFile (Volume->RootDir, FullFilename, File, &size);

                LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3");
                if (CheckError(Status, L"while loading the Linux options file")) {
                    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3a 1");
                    if (File != NULL) {
                        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3a 1a 1");
                        MyFreePool (&File);
                        File = NULL;
                        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3a 1a 2");
                    }
                    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3a 2");
                }
                else {
                    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3b 1");
                    GoOn      = FALSE;
                    FileFound = TRUE;
                    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 3b 2");
                } // if/else error
                LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 2a 4");
            } // if file exists
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3a 3");
        }
        else {
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3b 1");
            GoOn = FALSE;
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 3b 2");
        } // if/else

        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 4");
        MyFreePool (&OptionsFilename);
        MyFreePool (&FullFilename);

        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 2a 5");
        OptionsFilename = FullFilename = NULL;

        LOG(5, LOG_LINE_FORENSIC,
            L"In ReadLinuxOptionsFile ... 2a 6 END WHILE LOOP"
        );
        LOG(5, LOG_BLANK_LINE_SEP, L"X");
    } while (GoOn);

    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3");
    if (!FileFound) {
        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3a 1");
        // No refind_linux.conf file; look for /etc/fstab and try to pull values from there
        File = GenerateOptionsFromEtcFstab(Volume);

        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3a 2");
        // If still no joy, try to use Freedesktop.org Discoverable Partitions Spec
        if (!File) {
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3a 2a 1");
            File = GenerateOptionsFromPartTypes ();
            LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3a 2a 2");
        }
        LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 3a 3");
    } // if

    LOG(5, LOG_LINE_FORENSIC, L"In ReadLinuxOptionsFile ... 4 - END:- return REFIT_FILE *File");
    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    return File;
} // static REFIT_FILE * ReadLinuxOptionsFile()

// Retrieve a single line of options from a Linux kernel options file
CHAR16 * GetFirstOptionsFromFile (
    IN CHAR16       *LoaderPath,
    IN REFIT_VOLUME *Volume
) {
    UINTN         TokenCount;
    CHAR16       *Options = NULL;
    CHAR16      **TokenList;
    REFIT_FILE   *File;

    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 1 - START");
    File = ReadLinuxOptionsFile (LoaderPath, Volume);

    LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2");
    if (File != NULL) {
        LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2a 1");
        TokenCount = ReadTokenLine(File, &TokenList);

        LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2a 2");
        if (TokenCount > 1) {
            Options = StrDuplicate(TokenList[1]);
        }

        LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2a 3");
        FreeTokenLine (&TokenList, &TokenCount);

        LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2a 3");
        MyFreePool (&File);

        LOG(5, LOG_LINE_FORENSIC, L"In GetFirstOptionsFromFile ... 2a 4");
    } // if

    LOG(5, LOG_LINE_FORENSIC,
        L"In GetFirstOptionsFromFile ... 3 - END:- return CHAR16 *Options = '%s'",
        Options ? Options : L"NULL"
    );
    LOG(5, LOG_BLANK_LINE_SEP, L"X");
    return Options;
} // static CHAR16 * GetOptionsFile()
