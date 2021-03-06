#include "StdAfx.h"
#include "Inject.h"

#include "Util.h"

// Ripped from Oblivion Script Extender 
// http://obse.silverlock.org/
// The sordid thread-suspending logic in DoInjectDLL is my own dismal contribution for ModelMod

extern BOOL SuspendProcessThreads(DWORD dwOwnerPID, bool suspend);

Inject::Inject(void)
{
}


Inject::~Inject(void)
{
}

bool Inject::InjectDLL(DWORD processId, const char * dllPath, bool processWasLaunched)
{
	bool	result = false;

	// wrap DLL injection in SEH, if it crashes print a message
	__try {
		result = DoInjectDLL(processId, dllPath, processWasLaunched);
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		_injectError = "DLL injection failed. In most cases, this is caused by an overly paranoid software firewall or antivirus package. Disabling either of these may solve the problem.";
		result = false;
	}

	return result;
}

/*** jmp hook layout
 *	E9 ## ## ## ##	jmp LoadLibraryA
 *						offset = LoadLibraryA - (base + 5)
 *	<string>		name of function
 ***/

typedef unsigned int UInt32;
typedef unsigned char UInt8;

bool Inject::DoInjectDLL(DWORD processId, const char * dllPath, bool processWasLaunched)
{
	bool result = false; // assume failure

	HMODULE k32 = GetModuleHandle("kernel32.dll");
	if (k32 == 0) {
		_injectError = "Failed to obtain handle for kernel32.dll";
		return result;
	}

	HANDLE process = OpenProcess(
		PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, processId);
	if(process)
	{
		UInt32	hookBase = (UInt32)VirtualAllocEx(process, NULL, 8192, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if(hookBase)
		{
			// safe because kernel32 is loaded at the same address in all processes
			// (can change across restarts)
			UInt32	loadLibraryAAddr = (UInt32)GetProcAddress(k32, "LoadLibraryA");

			//_MESSAGE("hookBase = %08X", hookBase);
			//_MESSAGE("loadLibraryAAddr = %08X", loadLibraryAAddr);

			SIZE_T	bytesWritten;
			WriteProcessMemory(process, (LPVOID)(hookBase + 5), dllPath, strlen(dllPath) + 1, &bytesWritten);

			UInt8	hookCode[5];

			hookCode[0] = 0xE9;
			*((UInt32 *)&hookCode[1]) = loadLibraryAAddr - (hookBase + 5);

			WriteProcessMemory(process, (LPVOID)(hookBase), hookCode, sizeof(hookCode), &bytesWritten);

			// So, if we are attaching to an existing process, all of its threads should have already been suspended by the loader.
			// however, its quite possible that we suspended the threads inside a critical section, and we'll have problems either
			// creating the hook thread or waiting for it to exit.
			// So what we'll do is wait for a bit on the hook thread, and if we timeout, resume all threads in the target process for a brief period,
			// then resuspend them.  Then resume our hook thread and try again.  Do this some number of times and hopefully we'll be successful.  
			// Its basically a jackhammer, and it can fail (especially if our hook thread does a bunch of slow initialization stuff), but it
			// usually succeeds.

			const int SuspendSleepTime = 1;

			int hook_thread_attempts = 3;
			HANDLE hookThread = NULL;
			bool hookThreadValid = false;
			while (!hookThreadValid && hook_thread_attempts > 0) {
				hook_thread_attempts--;
				hookThread = CreateRemoteThread(process, NULL, 0, (LPTHREAD_START_ROUTINE)hookBase, (void *)(hookBase + 5), 0, NULL);
				hookThreadValid = hookThread && hookThread != INVALID_HANDLE_VALUE;
				if (!hookThreadValid) {
					Util::Log("Failed to create hook thread (%d more attempts)\n", hook_thread_attempts);
					SuspendProcessThreads(processId, false);
					Sleep(SuspendSleepTime);
					SuspendProcessThreads(processId, true);
				}
			}
			if (hookThreadValid)
			{
				ResumeThread(hookThread);

				DWORD waitTimeout;
				int MaxHookAttempts;
				if (processWasLaunched) {
					waitTimeout = INFINITE;
					MaxHookAttempts = 1;
				}
				else {
					waitTimeout = 500;
					// Note, if you need to debug injection (via attaching during SpinWhileFileExists),
					// set this to a large number  
					// to prevent the loader from bailing and aborting
					// during the time it takes you to attach.
					// 100 attempts at 500 timeout = about 50 seconds of waiting.
					// Don't use waitTimeout INFINITE as above, it will deadlock.
					MaxHookAttempts = 25; 
				}
					 
				int attempt = 0;

				for (attempt = 0; !result && attempt < MaxHookAttempts; ++attempt) {
					Util::Log("Waiting for hook thread: attempt %d; timeout: %d\n", attempt, waitTimeout);

					switch(WaitForSingleObject(hookThread, waitTimeout))  // g_options.m_threadTimeout
					{
						case WAIT_OBJECT_0:
							Util::Log("Hook Thread complete\n");
							result = true;
							break;

						case WAIT_ABANDONED:
							_injectError = "Hook Thread WAIT_ABANDONED";
							break;

						case WAIT_TIMEOUT:
							// Possible lock contention.
							// Resume all threads, sleep for a bit then suspend them all again.  Then resume hook thread and retry.
							Util::Log("Timeout\n");
							SuspendProcessThreads(processId,false);
							Sleep(SuspendSleepTime);
							SuspendProcessThreads(processId,true);
							ResumeThread(hookThread);
							break;

						case WAIT_FAILED:
							_injectError = "Hook Thread WAIT_FAILED";
							break;
						default:
							_injectError = "Hook Thread Unknown wait state";

					}
				}

				if (!result) {
					_injectError = "Unable to complete hook thread after several attempts";
				}

				CloseHandle(hookThread);
			}
			else {
				//http://stackoverflow.com/questions/3006229/get-a-text-from-the-error-code-returns-from-the-getlasterror-function
				DWORD   dwLastError = ::GetLastError();
				const DWORD BufSize = 256;
				TCHAR   lpBuffer[BufSize] = _T("?");
				if(dwLastError != 0)    // Don't want to see a "operation done successfully" error ;-)
				::FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM,                 // It�s a system error
                     NULL,                                      // No string to be formatted needed
                     dwLastError,                               // Hey Windows: Please explain this error!
                     MAKELANGID(LANG_NEUTRAL,SUBLANG_DEFAULT),  // Do it in the standard language
                     lpBuffer,              // Put the message here
                     BufSize-1,                     // Number of bytes to store the message
                     NULL);

				_injectError = string("CreateRemoteThread failed: ") + string(lpBuffer);
			}
				

			// the size parameter must be 0 when using MEM_RELEASE (CodeAnalysis C6333)
			VirtualFreeEx(process, (LPVOID)hookBase, 0, MEM_RELEASE);
		}
		else
			_injectError = "Process::InstallHook: couldn't allocate memory in target process";

		CloseHandle(process);
	}
	else
		_injectError = "Process::InstallHook: couldn't get process handle.  You may need to run MMLoader as an adminstrator.";

	return result;
}
