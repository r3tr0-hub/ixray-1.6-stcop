#include "stdafx.h"
#include "AiBridgeProcess.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h>

namespace AiBridgePlatform
{
	static constexpr size_t MaxMessageBytes = 32 * 1024 * 1024;
	static constexpr size_t MaxQueuedBytes = 64 * 1024 * 1024;
	static constexpr size_t MaxQueuedMessages = 256;

	class NativeHandle final
	{
	public:
		NativeHandle() = default;
		~NativeHandle() { Reset(); }
		NativeHandle(const NativeHandle&) = delete;
		NativeHandle& operator=(const NativeHandle&) = delete;

		HANDLE Get() const { return Value; }
		HANDLE* Put()
		{
			Reset();
			return &Value;
		}
		void Reset(HANDLE NewValue = nullptr)
		{
			if (Value != nullptr && Value != INVALID_HANDLE_VALUE)
			{
				CloseHandle(Value);
			}
			Value = NewValue;
		}

	private:
		HANDLE Value = nullptr;
	};

	class ProcessAttributes final
	{
	public:
		ProcessAttributes() = default;
		~ProcessAttributes()
		{
			if (Value != nullptr)
			{
				DeleteProcThreadAttributeList(Value);
			}
			xr_free(Storage);
		}
		bool Initialize(HANDLE* Handles, size_t Count)
		{
			SIZE_T Size = 0;
			InitializeProcThreadAttributeList(nullptr, 1, 0, &Size);
			if (Size == 0)
			{
				return false;
			}
			Storage = xr_alloc<u8>(Size);
			Value = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Storage);
			if (!InitializeProcThreadAttributeList(Value, 1, 0, &Size))
			{
				Value = nullptr;
				return false;
			}
			return UpdateProcThreadAttribute(Value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Handles, Count * sizeof(HANDLE), nullptr, nullptr) != FALSE;
		}
		LPPROC_THREAD_ATTRIBUTE_LIST Get() const { return Value; }

	private:
		u8* Storage = nullptr;
		LPPROC_THREAD_ATTRIBUTE_LIST Value = nullptr;
	};

	static std::wstring QuoteArgument(const std::wstring& Argument)
	{
		std::wstring Result = L"\"";
		size_t Slashes = 0;
		for (wchar_t Character : Argument)
		{
			if (Character == L'\\')
			{
				++Slashes;
				continue;
			}
			if (Character == L'"')
			{
				Result.append(Slashes * 2 + 1, L'\\');
			}
			else
			{
				Result.append(Slashes, L'\\');
			}
			Slashes = 0;
			Result.push_back(Character);
		}
		Result.append(Slashes * 2, L'\\');
		Result.push_back(L'"');
		return Result;
	}

	static nlohmann::json ErrorEvent(const char* Code, const char* Message)
	{
		return {{"type", "ui.event"}, {"event", "error"}, {"params", {{"code", Code}, {"message", Message}}}};
	}
}

class AiBridgeProcessImpl final
{
public:
	AiBridgeProcessImpl() = default;
	~AiBridgeProcessImpl() { Stop(); }

	bool Start(xr_string& Error)
	{
		Stop();
		wchar_t ModuleName[32768] = {};
		const DWORD ModuleLength = GetModuleFileNameW(nullptr, ModuleName, DWORD(std::size(ModuleName)));
		if (ModuleLength == 0 || ModuleLength >= std::size(ModuleName))
		{
			Error = "Unable to locate Level Editor installation";
			return false;
		}
		const std::filesystem::path SdkRoot = std::filesystem::path(ModuleName).parent_path().parent_path();
		const std::filesystem::path ToolsRoot = SdkRoot / L"tools" / L"ai-assistant";
		const std::filesystem::path NodePath = ToolsRoot / L"runtime" / L"node.exe";
		const std::filesystem::path BridgePath = ToolsRoot / L"src" / L"bridge.mjs";
		std::error_code FileError;
		if (!std::filesystem::is_regular_file(NodePath, FileError) || !std::filesystem::is_regular_file(BridgePath, FileError))
		{
			Error = "Bundled AI runtime is missing: tools/ai-assistant/runtime/node.exe or src/bridge.mjs";
			return false;
		}

		SECURITY_ATTRIBUTES Security = {sizeof(Security), nullptr, TRUE};
		AiBridgePlatform::NativeHandle ChildInput;
		AiBridgePlatform::NativeHandle ChildOutput;
		AiBridgePlatform::NativeHandle ChildError;
		if (!CreatePipe(ChildInput.Put(), Input.Put(), &Security, 65536) ||
			!CreatePipe(Output.Put(), ChildOutput.Put(), &Security, 65536) ||
			!CreatePipe(ErrorOutput.Put(), ChildError.Put(), &Security, 65536) ||
			!SetHandleInformation(Input.Get(), HANDLE_FLAG_INHERIT, 0) ||
			!SetHandleInformation(Output.Get(), HANDLE_FLAG_INHERIT, 0) ||
			!SetHandleInformation(ErrorOutput.Get(), HANDLE_FLAG_INHERIT, 0))
		{
			Error = "Unable to create private AI process channels";
			Stop();
			return false;
		}
		WriteEvent.Reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
		Job.Reset(CreateJobObjectW(nullptr, nullptr));
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION JobLimits = {};
		JobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (WriteEvent.Get() == nullptr || Job.Get() == nullptr || !SetInformationJobObject(Job.Get(), JobObjectExtendedLimitInformation, &JobLimits, sizeof(JobLimits)))
		{
			Error = "Unable to create AI process lifetime guard";
			Stop();
			return false;
		}
		HANDLE InheritedHandles[] = {ChildInput.Get(), ChildOutput.Get(), ChildError.Get()};
		AiBridgePlatform::ProcessAttributes Attributes;
		if (!Attributes.Initialize(InheritedHandles, std::size(InheritedHandles)))
		{
			Error = "Unable to restrict inherited AI process handles";
			Stop();
			return false;
		}
		STARTUPINFOEXW Startup = {};
		Startup.StartupInfo.cb = sizeof(Startup);
		Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		Startup.StartupInfo.hStdInput = ChildInput.Get();
		Startup.StartupInfo.hStdOutput = ChildOutput.Get();
		Startup.StartupInfo.hStdError = ChildError.Get();
		Startup.lpAttributeList = Attributes.Get();
		std::wstring CommandLine = AiBridgePlatform::QuoteArgument(NodePath.native()) + L" " + AiBridgePlatform::QuoteArgument(BridgePath.native()) +
			L" --sdk-root " + AiBridgePlatform::QuoteArgument(SdkRoot.native()) +
			L" --runtime-root " + AiBridgePlatform::QuoteArgument((ToolsRoot / L"runtime").native());
		PROCESS_INFORMATION ProcessInfo = {};
		if (!CreateProcessW(NodePath.c_str(), CommandLine.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, SdkRoot.c_str(), &Startup.StartupInfo, &ProcessInfo))
		{
			Error = "Unable to start bundled AI runtime";
			Stop();
			return false;
		}
		Process.Reset(ProcessInfo.hProcess);
		AiBridgePlatform::NativeHandle MainThread;
		MainThread.Reset(ProcessInfo.hThread);
		if (!AssignProcessToJobObject(Job.Get(), Process.Get()))
		{
			TerminateProcess(Process.Get(), 1);
			Error = "Unable to attach AI runtime to editor lifetime guard";
			Stop();
			return false;
		}
		if (ResumeThread(MainThread.Get()) == DWORD(-1))
		{
			Error = "Unable to resume AI runtime";
			Stop();
			return false;
		}
		// Closing the parent's copies is necessary for EOF when the child exits.
		ChildInput.Reset();
		ChildOutput.Reset();
		ChildError.Reset();
		IsStopping.store(false);
		IsActive.store(true);
		Reader = std::thread(&AiBridgeProcessImpl::ReadLoop, this);
		Writer = std::thread(&AiBridgeProcessImpl::WriteLoop, this);
		ErrorReader = std::thread(&AiBridgeProcessImpl::DrainErrorLoop, this);
		return true;
	}

	void Stop()
	{
		IsStopping.store(true);
		IsActive.store(false);
		if (WriteEvent.Get() != nullptr)
		{
			SetEvent(WriteEvent.Get());
		}
		if (Job.Get() != nullptr)
		{
			TerminateJobObject(Job.Get(), 0);
		}
		// The job is terminated first, so pending writes/readers cannot wait on a live child.
		if (Reader.joinable())
		{
			CancelSynchronousIo(Reader.native_handle());
			Reader.join();
		}
		if (Writer.joinable())
		{
			CancelSynchronousIo(Writer.native_handle());
			Writer.join();
		}
		if (ErrorReader.joinable())
		{
			CancelSynchronousIo(ErrorReader.native_handle());
			ErrorReader.join();
		}
		Input.Reset();
		Output.Reset();
		ErrorOutput.Reset();
		Process.Reset();
		Job.Reset();
		WriteEvent.Reset();
		xrCriticalSectionGuard Guard(QueueLock);
		Incoming.clear();
		Outgoing.clear();
		IncomingBytes = 0;
		OutgoingBytes = 0;
	}

	bool Send(const nlohmann::json& Message)
	{
		if (!IsActive.load())
		{
			return false;
		}
		const auto Serialized = Message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
		if (Serialized.size() > AiBridgePlatform::MaxMessageBytes)
		{
			return false;
		}
		{
			xrCriticalSectionGuard Guard(QueueLock);
			if (Outgoing.size() >= AiBridgePlatform::MaxQueuedMessages || OutgoingBytes + Serialized.size() + 1 > AiBridgePlatform::MaxQueuedBytes)
			{
				return false;
			}
			Outgoing.emplace_back(Serialized.c_str());
			Outgoing.back().push_back('\n');
			OutgoingBytes += Outgoing.back().size();
		}
		SetEvent(WriteEvent.Get());
		return true;
	}

	bool Poll(nlohmann::json& Message)
	{
		xrCriticalSectionGuard Guard(QueueLock);
		if (Incoming.empty())
		{
			return false;
		}
		xr_string Line = std::move(Incoming.front());
		Incoming.pop_front();
		IncomingBytes -= Line.size();
		Message = nlohmann::json::parse(Line.begin(), Line.end(), nullptr, false);
		if (Message.is_discarded() || !Message.is_object())
		{
			Message = AiBridgePlatform::ErrorEvent("BRIDGE_PROTOCOL", "AI service returned an invalid JSON message");
		}
		return true;
	}

	bool IsRunning() const { return IsActive.load(); }

private:
	bool Enqueue(xr_string&& Line)
	{
		xrCriticalSectionGuard Guard(QueueLock);
		if (Incoming.size() >= AiBridgePlatform::MaxQueuedMessages || IncomingBytes + Line.size() > AiBridgePlatform::MaxQueuedBytes)
		{
			return false;
		}
		IncomingBytes += Line.size();
		Incoming.emplace_back(std::move(Line));
		return true;
	}

	void Fail(const char* Code, const char* Message)
	{
		if (IsActive.exchange(false) && !IsStopping.load())
		{
			const auto Serialized = AiBridgePlatform::ErrorEvent(Code, Message).dump();
			Enqueue(xr_string(Serialized.c_str()));
			SetEvent(WriteEvent.Get());
			// A failed channel must not leave an agent running with pending editor commands.
			TerminateJobObject(Job.Get(), 1);
		}
	}

	void ReadLoop()
	{
		xr_string Pending;
		char Buffer[16384];
		DWORD ReadSize = 0;
		while (!IsStopping.load() && ReadFile(Output.Get(), Buffer, sizeof(Buffer), &ReadSize, nullptr) && ReadSize != 0)
		{
			for (DWORD Index = 0; Index < ReadSize; ++Index)
			{
				if (Buffer[Index] == '\n')
				{
					if (!Pending.empty() && !Enqueue(std::move(Pending)))
					{
						Fail("BRIDGE_QUEUE_LIMIT", "AI incoming message queue exceeded its limit");
						return;
					}
					Pending.clear();
				}
				else if (Buffer[Index] != '\r')
				{
					Pending.push_back(Buffer[Index]);
					if (Pending.size() > AiBridgePlatform::MaxMessageBytes)
					{
						Fail("BRIDGE_MESSAGE_LIMIT", "AI service message exceeded its size limit");
						return;
					}
				}
			}
		}
		Fail("BRIDGE_EXITED", "AI service disconnected; reconnect from the assistant panel");
	}

	void WriteLoop()
	{
		while (!IsStopping.load() && IsActive.load())
		{
			WaitForSingleObject(WriteEvent.Get(), INFINITE);
			while (!IsStopping.load() && IsActive.load())
			{
				xr_string Line;
				{
					xrCriticalSectionGuard Guard(QueueLock);
					if (Outgoing.empty())
					{
						break;
					}
					Line = std::move(Outgoing.front());
					Outgoing.pop_front();
					OutgoingBytes -= Line.size();
				}
				size_t Offset = 0;
				while (Offset < Line.size() && !IsStopping.load())
				{
					DWORD Written = 0;
					const DWORD Chunk = DWORD(std::min<size_t>(65536, Line.size() - Offset));
					if (!WriteFile(Input.Get(), Line.data() + Offset, Chunk, &Written, nullptr) || Written == 0)
					{
						Fail("BRIDGE_WRITE", "AI service closed its command channel");
						return;
					}
					Offset += Written;
				}
			}
		}
	}

	void DrainErrorLoop()
	{
		// Discard stderr without retaining OAuth URLs, credentials, prompts or scene data.
		char Buffer[4096];
		DWORD ReadSize = 0;
		while (!IsStopping.load() && ReadFile(ErrorOutput.Get(), Buffer, sizeof(Buffer), &ReadSize, nullptr) && ReadSize != 0)
		{
		}
	}

	xrCriticalSection QueueLock;
	xr_deque<xr_string> Incoming;
	xr_deque<xr_string> Outgoing;
	size_t IncomingBytes = 0;
	size_t OutgoingBytes = 0;
	std::atomic_bool IsActive{false};
	std::atomic_bool IsStopping{true};
	std::thread Reader;
	std::thread Writer;
	std::thread ErrorReader;
	AiBridgePlatform::NativeHandle Input;
	AiBridgePlatform::NativeHandle Output;
	AiBridgePlatform::NativeHandle ErrorOutput;
	AiBridgePlatform::NativeHandle Process;
	AiBridgePlatform::NativeHandle Job;
	AiBridgePlatform::NativeHandle WriteEvent;
};

#else

class AiBridgeProcessImpl final
{
public:
	AiBridgeProcessImpl() = default;
	~AiBridgeProcessImpl() = default;
	bool Start(xr_string& Error)
	{
		Error = "This AI assistant package supports Windows x64 only";
		return false;
	}
	void Stop() {}
	bool Send(const nlohmann::json&) { return false; }
	bool Poll(nlohmann::json&) { return false; }
	bool IsRunning() const { return false; }
};

#endif

AiBridgeProcess::AiBridgeProcess()
	: Impl(new AiBridgeProcessImpl())
{
}

AiBridgeProcess::~AiBridgeProcess()
{
	xr_delete(Impl);
}

bool AiBridgeProcess::Start(xr_string& Error)
{
	return Impl->Start(Error);
}

void AiBridgeProcess::Stop()
{
	Impl->Stop();
}

bool AiBridgeProcess::Send(const nlohmann::json& Message)
{
	return Impl->Send(Message);
}

bool AiBridgeProcess::Poll(nlohmann::json& Message)
{
	return Impl->Poll(Message);
}

bool AiBridgeProcess::IsRunning() const
{
	return Impl->IsRunning();
}
