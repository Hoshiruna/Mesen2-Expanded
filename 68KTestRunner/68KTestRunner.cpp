#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "Genesis/GenesisNativeBackend.h"

using std::array;
using std::optional;
using std::string;
using std::string_view;
using std::vector;
namespace fs = std::filesystem;

namespace
{
	constexpr uint32_t FileMagic = 0x1A3F5D71u;
	constexpr uint32_t TestMagic = 0xABC12367u;
	constexpr uint32_t NameMagic = 0x89ABCDEFu;
	constexpr uint32_t StateMagic = 0x01234567u;
	constexpr uint32_t TransactionMagic = 0x456789ABu;

	struct MemoryByte
	{
		uint32_t Address = 0;
		uint8_t Value = 0;
	};

	struct TestState
	{
		array<uint32_t, 8> D = {};
		array<uint32_t, 7> A = {};
		uint32_t Usp = 0;
		uint32_t Ssp = 0;
		uint16_t Sr = 0;
		uint32_t Pc = 0;
		vector<MemoryByte> Ram;
	};

	struct TestCase
	{
		string Name;
		TestState Initial;
		TestState Final;
		uint32_t ExpectedCycles = 0;
	};

	struct FileSummary
	{
		fs::path Path;
		size_t TotalCases = 0;
		size_t PassedCases = 0;
		size_t FailedCases = 0;
		string FirstFailure;
	};

	struct Options
	{
		fs::path InputPath = fs::path("Core") / "Genesis" / "68Ktest" / "v1";
		string Filter;
		size_t LimitPerFile = 0;
		bool StopOnFail = false;
		bool Verbose = false;
	};

	class Reader
	{
	public:
		explicit Reader(vector<uint8_t> data)
			: _data(std::move(data))
		{
		}

		template<typename T>
		T Read()
		{
			if(_offset + sizeof(T) > _data.size()) {
				throw std::runtime_error("unexpected end of file");
			}

			T value {};
			memcpy(&value, _data.data() + _offset, sizeof(T));
			_offset += sizeof(T);
			return value;
		}

		string ReadString(size_t length)
		{
			if(_offset + length > _data.size()) {
				throw std::runtime_error("unexpected end of file");
			}

			string value(reinterpret_cast<const char*>(_data.data() + _offset), length);
			_offset += length;
			return value;
		}

		void Skip(size_t length)
		{
			if(_offset + length > _data.size()) {
				throw std::runtime_error("unexpected end of file");
			}
			_offset += length;
		}

	private:
		vector<uint8_t> _data;
		size_t _offset = 0;
	};

	string Hex(uint32_t value, int width)
	{
		std::ostringstream out;
		out << std::hex << std::uppercase << std::setfill('0') << std::setw(width) << value;
		return out.str();
	}

	uint32_t NormalizeCorpusPc(uint32_t pc)
	{
		// The corpus stores the 68000's internal prefetch PC, which points two
		// words past the opcode currently being executed. The native core does
		// not model the prefetch queue, so execute from the opcode address.
		return (pc - 4u) & 0x00FFFFFFu;
	}

	string FormatCpuState(const GenesisCpuState& state)
	{
		std::ostringstream out;
		out << "PC=0x" << Hex(state.PC & 0x00FFFFFFu, 6)
			<< " SR=0x" << Hex(state.SR, 4)
			<< " USP=0x" << Hex(state.USP, 8)
			<< " SSP=0x" << Hex(state.SP, 8);
		for(int i = 0; i < 8; i++) {
			out << " D" << i << "=0x" << Hex(state.D[i], 8);
		}
		for(int i = 0; i < 7; i++) {
			out << " A" << i << "=0x" << Hex(state.A[i], 8);
		}
		out << " A7=0x" << Hex(state.A[7], 8);
		return out.str();
	}

	[[noreturn]] void ThrowMagicError(string_view section, uint32_t actual, uint32_t expected)
	{
		throw std::runtime_error(string(section) + " magic mismatch: got 0x" + Hex(actual, 8) + ", expected 0x" + Hex(expected, 8));
	}

	void ExpectMagic(string_view section, uint32_t actual, uint32_t expected)
	{
		if(actual != expected) {
			ThrowMagicError(section, actual, expected);
		}
	}

	vector<uint8_t> ReadAllBytes(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if(!in) {
			throw std::runtime_error("failed to open file");
		}

		in.seekg(0, std::ios::end);
		std::streamsize size = in.tellg();
		in.seekg(0, std::ios::beg);
		if(size < 0) {
			throw std::runtime_error("failed to determine file size");
		}

		vector<uint8_t> data(static_cast<size_t>(size));
		if(size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
			throw std::runtime_error("failed to read file");
		}
		return data;
	}

	string ReadName(Reader& reader)
	{
		[[maybe_unused]] uint32_t numBytes = reader.Read<uint32_t>();
		uint32_t magic = reader.Read<uint32_t>();
		ExpectMagic("name", magic, NameMagic);
		uint32_t length = reader.Read<uint32_t>();
		return reader.ReadString(length);
	}

	TestState ReadState(Reader& reader)
	{
		[[maybe_unused]] uint32_t numBytes = reader.Read<uint32_t>();
		uint32_t magic = reader.Read<uint32_t>();
		ExpectMagic("state", magic, StateMagic);

		TestState state;
		for(uint32_t& reg : state.D) {
			reg = reader.Read<uint32_t>();
		}
		for(uint32_t& reg : state.A) {
			reg = reader.Read<uint32_t>();
		}
		state.Usp = reader.Read<uint32_t>();
		state.Ssp = reader.Read<uint32_t>();
		state.Sr = static_cast<uint16_t>(reader.Read<uint32_t>());
		state.Pc = reader.Read<uint32_t>() & 0x00FFFFFFu;

		reader.Skip(sizeof(uint32_t) * 2);

		uint32_t ramWordCount = reader.Read<uint32_t>();
		state.Ram.reserve(static_cast<size_t>(ramWordCount) * 2);
		for(uint32_t i = 0; i < ramWordCount; i++) {
			uint32_t address = reader.Read<uint32_t>();
			uint16_t data = reader.Read<uint16_t>();
			state.Ram.push_back({ address & 0x00FFFFFFu, static_cast<uint8_t>(data >> 8) });
			state.Ram.push_back({ (address | 1u) & 0x00FFFFFFu, static_cast<uint8_t>(data & 0xFFu) });
		}

		return state;
	}

	uint32_t SkipTransactions(Reader& reader)
	{
		[[maybe_unused]] uint32_t numBytes = reader.Read<uint32_t>();
		uint32_t magic = reader.Read<uint32_t>();
		ExpectMagic("transactions", magic, TransactionMagic);

		uint32_t expectedCycles = reader.Read<uint32_t>();
		uint32_t transactionCount = reader.Read<uint32_t>();
		for(uint32_t i = 0; i < transactionCount; i++) {
			uint8_t kind = reader.Read<uint8_t>();
			reader.Skip(sizeof(uint32_t));
			if(kind != 0) {
				reader.Skip(sizeof(uint32_t) * 5);
			}
		}
		return expectedCycles;
	}

	vector<TestCase> LoadCases(const fs::path& path)
	{
		Reader reader(ReadAllBytes(path));
		uint32_t magic = reader.Read<uint32_t>();
		ExpectMagic("file", magic, FileMagic);
		uint32_t testCount = reader.Read<uint32_t>();

		vector<TestCase> cases;
		cases.reserve(testCount);
		for(uint32_t i = 0; i < testCount; i++) {
			[[maybe_unused]] uint32_t numBytes = reader.Read<uint32_t>();
			uint32_t testMagic = reader.Read<uint32_t>();
			ExpectMagic("test", testMagic, TestMagic);

			TestCase entry;
			entry.Name = ReadName(reader);
			entry.Initial = ReadState(reader);
			entry.Final = ReadState(reader);
			entry.ExpectedCycles = SkipTransactions(reader);
			cases.push_back(std::move(entry));
		}

		return cases;
	}

	GenesisCpuState ToCpuState(const TestState& state)
	{
		GenesisCpuState cpu;
		cpu.CycleCount = 0;
		cpu.PC = NormalizeCorpusPc(state.Pc);
		cpu.SR = static_cast<uint16_t>(state.Sr & 0xA71Fu);
		memcpy(cpu.D, state.D.data(), sizeof(cpu.D));
		memcpy(cpu.A, state.A.data(), sizeof(uint32_t) * state.A.size());
		cpu.USP = state.Usp;
		if((cpu.SR & GenesisCpu68k::SR_S) != 0) {
			cpu.SP = state.Ssp;
			cpu.A[7] = state.Ssp;
		} else {
			cpu.SP = state.Ssp;
			cpu.A[7] = state.Usp;
		}
		return cpu;
	}

	optional<string> CompareState(const TestCase& testCase, const GenesisCpuState& actualState, const GenesisNativeBackend& backend)
	{
		for(size_t i = 0; i < testCase.Final.D.size(); i++) {
			if(actualState.D[i] != testCase.Final.D[i]) {
				return "D" + std::to_string(i) + ": expected 0x" + Hex(testCase.Final.D[i], 8) + ", got 0x" + Hex(actualState.D[i], 8);
			}
		}

		for(size_t i = 0; i < testCase.Final.A.size(); i++) {
			if(actualState.A[i] != testCase.Final.A[i]) {
				return "A" + std::to_string(i) + ": expected 0x" + Hex(testCase.Final.A[i], 8) + ", got 0x" + Hex(actualState.A[i], 8);
			}
		}

		if(actualState.USP != testCase.Final.Usp) {
			return "USP: expected 0x" + Hex(testCase.Final.Usp, 8) + ", got 0x" + Hex(actualState.USP, 8);
		}

		if(actualState.SP != testCase.Final.Ssp) {
			return "SSP: expected 0x" + Hex(testCase.Final.Ssp, 8) + ", got 0x" + Hex(actualState.SP, 8);
		}

		uint16_t expectedSr = static_cast<uint16_t>(testCase.Final.Sr & 0xA71Fu);
		if(actualState.SR != expectedSr) {
			return "SR: expected 0x" + Hex(expectedSr, 4) + ", got 0x" + Hex(actualState.SR, 4);
		}

		uint32_t expectedPc = NormalizeCorpusPc(testCase.Final.Pc);
		if((actualState.PC & 0x00FFFFFFu) != expectedPc) {
			return "PC: expected 0x" + Hex(expectedPc, 6) + ", got 0x" + Hex(actualState.PC & 0x00FFFFFFu, 6);
		}

		for(const MemoryByte& mem : testCase.Final.Ram) {
			uint8_t actualValue = backend.GetCpuTestBusByte(mem.Address);
			if(actualValue != mem.Value) {
				return "MEM[0x" + Hex(mem.Address & 0x00FFFFFFu, 6) + "]: expected 0x" + Hex(mem.Value, 2) + ", got 0x" + Hex(actualValue, 2);
			}
		}

		return std::nullopt;
	}

	void LoadInitialState(GenesisNativeBackend& backend, const TestCase& testCase)
	{
		backend.ClearCpuTestBus(0);
		for(const MemoryByte& mem : testCase.Initial.Ram) {
			backend.SetCpuTestBusByte(mem.Address, mem.Value);
		}
		backend.SetCpuStateForTest(ToCpuState(testCase.Initial));
	}

	FileSummary RunFile(GenesisNativeBackend& backend, const fs::path& path, const Options& options)
	{
		vector<TestCase> cases = LoadCases(path);
		FileSummary summary;
		summary.Path = path;
		summary.TotalCases = cases.size();

		size_t caseCount = cases.size();
		if(options.LimitPerFile > 0 && options.LimitPerFile < caseCount) {
			caseCount = options.LimitPerFile;
			summary.TotalCases = caseCount;
		}

		for(size_t i = 0; i < caseCount; i++) {
			const TestCase& testCase = cases[i];
			LoadInitialState(backend, testCase);

			GenesisCpuState injectedState {};
			backend.GetCpuStateForTest(injectedState);
			TestCase injectionCheck = testCase;
			injectionCheck.Final = testCase.Initial;
			if(optional<string> mismatch = CompareState(injectionCheck, injectedState, backend)) {
				summary.FailedCases++;
				if(summary.FirstFailure.empty()) {
					summary.FirstFailure = testCase.Name + " -> initial state injection mismatch: " + *mismatch;
				}
				if(options.Verbose) {
					std::cout << "  FAIL " << testCase.Name << ": initial state injection mismatch: " << *mismatch << '\n';
				}
				if(options.StopOnFail) {
					break;
				}
				continue;
			}

			int32_t cycles = backend.RunCpuInstructionForTest();
			(void)cycles;

			GenesisCpuState actualState {};
			backend.GetCpuStateForTest(actualState);
			if(optional<string> mismatch = CompareState(testCase, actualState, backend)) {
				summary.FailedCases++;
				if(summary.FirstFailure.empty()) {
					std::ostringstream failure;
					failure << testCase.Name << " -> " << *mismatch;
					if(testCase.ExpectedCycles != 0) {
						failure << " (expected cycles " << testCase.ExpectedCycles << ")";
					}
					summary.FirstFailure = failure.str();
				}

				if(options.Verbose) {
					std::cout << "  FAIL " << testCase.Name << ": " << *mismatch << '\n';
					std::cout << "    before: " << FormatCpuState(injectedState) << '\n';
					std::cout << "    after : " << FormatCpuState(actualState) << '\n';
				}

				if(options.StopOnFail) {
					break;
				}
			} else {
				summary.PassedCases++;
				if(options.Verbose) {
					std::cout << "  PASS " << testCase.Name << '\n';
				}
			}
		}

		summary.TotalCases = summary.PassedCases + summary.FailedCases;

		return summary;
	}

	vector<fs::path> EnumerateInputFiles(const Options& options)
	{
		vector<fs::path> files;
		if(fs::is_regular_file(options.InputPath)) {
			files.push_back(options.InputPath);
			return files;
		}

		for(const fs::directory_entry& entry : fs::directory_iterator(options.InputPath)) {
			if(!entry.is_regular_file()) {
				continue;
			}
			if(entry.path().extension() != ".bin") {
				continue;
			}
			if(entry.path().filename().string().find(".json.bin") == string::npos) {
				continue;
			}
			files.push_back(entry.path());
		}

		std::sort(files.begin(), files.end());
		return files;
	}

	bool MatchesFilter(const fs::path& path, const Options& options)
	{
		if(options.Filter.empty()) {
			return true;
		}

		string fileName = path.filename().string();
		return fileName.find(options.Filter) != string::npos;
	}

	Options ParseOptions(int argc, char** argv)
	{
		Options options;
		for(int i = 1; i < argc; i++) {
			string arg = argv[i];
			if(arg == "--filter" && i + 1 < argc) {
				options.Filter = argv[++i];
			} else if(arg == "--limit" && i + 1 < argc) {
				options.LimitPerFile = static_cast<size_t>(std::stoull(argv[++i]));
			} else if(arg == "--stop-on-fail") {
				options.StopOnFail = true;
			} else if(arg == "--verbose") {
				options.Verbose = true;
			} else if(arg == "--help" || arg == "-h") {
				std::cout
					<< "Usage: Genesis68KTestRunner [path] [--filter text] [--limit N] [--stop-on-fail] [--verbose]\n"
					<< "Default path: Core/Genesis/68Ktest/v1\n";
				std::exit(0);
			} else if(!arg.empty() && arg[0] == '-') {
				throw std::runtime_error("unknown option: " + arg);
			} else {
				options.InputPath = arg;
			}
		}
		return options;
	}

	vector<uint8_t> CreateDummyRom()
	{
		vector<uint8_t> rom(0x200, 0);
		return rom;
	}
}

int main(int argc, char** argv)
{
	try {
		Options options = ParseOptions(argc, argv);
		vector<fs::path> files = EnumerateInputFiles(options);
		if(files.empty()) {
			std::cerr << "No .json.bin files found under " << options.InputPath.string() << '\n';
			return 1;
		}

		GenesisNativeBackend backend(nullptr, nullptr);
		vector<uint8_t> rom = CreateDummyRom();
		if(!backend.LoadRom(rom, "ntsc", nullptr, 0, nullptr, 0)) {
			std::cerr << "Failed to initialize Genesis backend\n";
			return 1;
		}
		backend.EnableCpuTestBus();

		size_t totalFiles = 0;
		size_t passedFiles = 0;
		size_t totalCases = 0;
		size_t passedCases = 0;
		bool stoppedOnFailure = false;

		for(const fs::path& path : files) {
			if(!MatchesFilter(path, options)) {
				continue;
			}

			FileSummary summary = RunFile(backend, path, options);
			totalFiles++;
			totalCases += summary.TotalCases;
			passedCases += summary.PassedCases;

			if(summary.FailedCases == 0) {
				passedFiles++;
				std::cout << "[PASS] " << path.filename().string() << " (" << summary.PassedCases << '/' << summary.TotalCases << ")\n";
			} else {
				std::cout << "[FAIL] " << path.filename().string() << " (" << summary.PassedCases << '/' << summary.TotalCases << ")\n";
				std::cout << "  " << summary.FirstFailure << '\n';
				if(options.StopOnFail) {
					stoppedOnFailure = true;
					break;
				}
			}
		}

		if(totalFiles == 0) {
			std::cerr << "No files matched the requested filter\n";
			return 1;
		}

		std::cout << '\n'
		          << "Files: " << passedFiles << '/' << totalFiles << " passed\n"
		          << "Cases: " << passedCases << '/' << totalCases << " passed\n";

		return (passedFiles == totalFiles && !stoppedOnFailure) ? 0 : 1;
	} catch(const std::exception& ex) {
		std::cerr << "Error: " << ex.what() << '\n';
		return 1;
	}
}
