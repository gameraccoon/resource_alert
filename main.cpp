#include <array>
#include <chrono>
#include <cstdio>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// need this while compilers align on how they support C++23 features
template<typename Deleter>
class FilePipe
{
public:
	explicit FilePipe(FILE* file, Deleter inDeleter) noexcept
		: mFile(file)
		, mDeleter(inDeleter)
	{
	}

	~FilePipe() noexcept
	{
		if (mFile)
		{
			mDeleter(mFile);
		}
	}

	bool operator()() noexcept
	{
		return mFile != nullptr;
	}

	bool operator!() noexcept
	{
		return mFile == nullptr;
	}

	FILE* operator*() noexcept
	{
		return mFile;
	}

private:
	FILE* mFile = nullptr;
	Deleter mDeleter;
};

// requires a null-terminated string
std::optional<int> parseInt(const char* str, const int base)
{
	char* end;
	errno = 0;
	const long l = std::strtol(str, &end, base);
	if ((errno == ERANGE && l == LONG_MAX) || l > INT_MAX)
	{
		return std::nullopt;
	}
	if ((errno == ERANGE && l == LONG_MIN) || l < INT_MIN)
	{
		return std::nullopt;
	}
	if (*str == '\0' || *end != '\0')
	{
		return std::nullopt;
	}
	return static_cast<int>(l);
}

bool readCommandOutput(std::string_view cmd, std::string& outResult) noexcept
{
	std::array<char, 128> buffer;
	auto pipe = FilePipe{popen(cmd.data(), "r"), [](FILE* f){ pclose(f); }};
	if (!pipe) {
		return false;
	}

	while (fgets(buffer.data(), static_cast<int>(buffer.size()), *pipe) != nullptr) {
		outResult += buffer.data();
	}

	return true;
}

bool saveCommandOutput(std::string_view cmd, const std::string_view& filePath) noexcept
{
	std::array<char, 128> buffer;
	auto pipe = FilePipe{popen(cmd.data(), "r"), [](FILE* f){ pclose(f); }};
	if (!pipe) {
		return false;
	}

	auto outFilePipe = FilePipe{fopen(filePath.data(), "w"), [](FILE* f){ fclose(f); }};

	while (fgets(buffer.data(), static_cast<int>(buffer.size()), *pipe) != nullptr) {
		const int bytesWritten = fputs(buffer.data(), *outFilePipe);
		if (bytesWritten == EOF)
		{
			return false;
		}
		// maybe we should also check if we wrote less bytes than read?
	}

	return true;
}

struct Args
{
	// [0.0, 100.0)
	float memThresholdPct = 70.0f;
	// [0.0, 100.0)
	float cpuThresholdPct = 70.0f;
	size_t timeBetweenChecksSec = 60;
	std::string runCustomScript;
	size_t notificationThrottleSec = 20 * 60;
};

struct AppState
{
	std::chrono::time_point<std::chrono::system_clock> lastMemAlertSentTime;
	std::chrono::time_point<std::chrono::system_clock> lastCpuAlertSentTime;
};

template<typename T>
bool readArgValue(T& dest, int argc, char** argv, int& i)
{
	if (i + 1 >= argc || argv[i + 1][0] == '-')
	{
		return false;
	}

	if constexpr (std::is_integral_v<T>)
	{
		std::optional<int> value = parseInt(argv[i + 1], 10);
		if (!value.has_value())
		{
			return false;
		}

		if constexpr (std::is_unsigned_v<T>)
		{
			if (*value < 0)
			{
				return false;
			}
		}

		if constexpr (sizeof(T) < sizeof(int))
		{
			if (*value < 0)
			{
				if (*value < std::numeric_limits<T>::lowest())
				{
					return false;
				}
			}
			else
			{
				if (*value > std::numeric_limits<T>::max())
				{
					return false;
				}
			}
		}

		dest = T(*value);
		++i;
		return true;
	}
	else if constexpr (std::is_floating_point_v<T>)
	{
		std::optional<int> value = parseInt(argv[i + 1], 10);
		if (value.has_value())
		{
			dest = T(*value);
			++i;
			return true;
		}
	}
	else if constexpr (std::is_same_v<T, std::string>)
	{
		dest = std::string(argv[i + 1]);
		++i;
		return true;
	}

	return false;
}

Args readArgs(int argc, char** argv)
{
	if (argc == 1)
	{
		return {};
	}

	Args args;
	for (int i = 1; i < argc; ++i)
	{
		bool isFound = false;
		bool isMissingValue = false;
		if (argv[i][0] == '-' && argv[i][1] != '\0')
		{
			// one letter args
			if (argv[i][2] == '\0')
			{
				switch (argv[i][1])
				{
				case 'm':
					isMissingValue = !readArgValue(args.memThresholdPct, argc, argv, i);
					isFound = true;
					break;
				case 'c':
					isMissingValue = !readArgValue(args.cpuThresholdPct, argc, argv, i);
					isFound = true;
					break;
				case 't':
					isMissingValue = !readArgValue(args.timeBetweenChecksSec, argc, argv, i);
					isFound = true;
					break;
				case 'r':
					isMissingValue = !readArgValue(args.runCustomScript, argc, argv, i);
					isFound = true;
					break;
				case 'n':
					isMissingValue = !readArgValue(args.notificationThrottleSec, argc, argv, i);
					isFound = true;
					break;
				}
			}
		}

		if (!isFound)
		{
			printf("Unknown argument '%s'\n", argv[i]);
			exit(1);
		}

		if (isMissingValue)
		{
			printf("Argument '%s' did not have a valid value\n", argv[i]);
			exit(2);
		}
	}

	return args;
}

void trySendNotification(const Args& args, auto& lastSendTime, std::string_view errorTitle, float consumptionPct)
{
	if (!args.runCustomScript.empty())
	{
		const auto timeNow = std::chrono::system_clock::now();
		if (timeNow > lastSendTime + std::chrono::seconds(args.notificationThrottleSec))
		{
			const std::string command = std::format("{} '{}. Consumption is {:.2f}%'", args.runCustomScript, errorTitle, consumptionPct);
			const int resultCode = std::system(command.data());
			if (resultCode != 0)
			{
				printf("Notification script exited with non-zero code %d\n", resultCode);
			}
			lastSendTime = timeNow;
		}
	}
}

int getFreePartValue(std::string& buffer, size_t partIndex)
{
	// free -L prints 4 blocks of the same size (usually 20 characters long each) + a '\n'
	// the size of the longest header + one space after
	const size_t longestHeaderLen = 8 + 1;
	const size_t startOffset = (buffer.size() - 1) / 4 * partIndex + longestHeaderLen;
	const size_t maxNumberLen = (buffer.size() - 1) / 4 - longestHeaderLen;

	const size_t end = std::min(startOffset + maxNumberLen, buffer.size());
	size_t numberStart = startOffset;
	size_t numberLen = maxNumberLen;
	for (size_t i = startOffset; i < end ; ++i)
	{
		--numberLen;
		if (buffer[i] != ' ')
		{
			numberStart = i;
			break;
		}
	}

	// unfortunate allocation because we want a null-terminated string without butchering the original string
	const std::string numberPart = buffer.substr(numberStart, numberLen);
	std::optional<int> parsedNumber = parseInt(numberPart.data(), 10);
	if (!parsedNumber.has_value())
	{
		printf("Failed to parse number '%s' from 'free -L' output '%s'\n", numberPart.data(), buffer.data());
		return 0;
	}

	return *parsedNumber;
}

float checkMemory(std::string& buffer)
{
	buffer.clear();
	const bool hasExecuted = readCommandOutput("free -L", buffer);
	if (!hasExecuted)
	{
		printf("Could not execute 'free -L'\n");
	}

	const int usedValue = getFreePartValue(buffer, 2);
	const int freeValue = getFreePartValue(buffer, 3);

	const float usedFraction = float(usedValue) / (float(freeValue) + float(usedValue));

	return usedFraction * 100.0;
}

bool isSubstr(std::string_view string, std::string_view substring, size_t position)
{
	if (position + substring.size() >= string.size())
	{
		return false;
	}

	for (size_t i = 0, size = substring.size(); i < size; ++i)
	{
		if (string[position + i] != substring[i])
		{
			return false;
		}
	}

	return true;
}

float checkCpu(std::string& buffer)
{
	buffer.clear();
	const bool hasExecuted = readCommandOutput("sar --dec=0 1 1 | tail -n 3", buffer);
	if (!hasExecuted)
	{
		printf("Could not execute 'sar --dec=0 1 1 | tail -n 3'\n");
	}

	// first find the offset of %idle column in the first line
	int idleOffset = -1;
	size_t secondLineStart = 0;
	for (size_t i = 0; i < buffer.size(); ++i)
	{
		if (isSubstr(buffer, "%idle", i))
		{
			idleOffset = int(i);
		}

		if (buffer[i] == '\n')
		{
			secondLineStart = i + 1;
			break;
		}
	}

	if (idleOffset == -1)
	{
		printf("Could not find idle column in 'sar --dec=0 1 1 | tail -n 3' output\n");
		return 0;
	}

	// unfortunate allocation because we want a null-terminated string without butchering the original string
	std::string numberPart = buffer.substr(secondLineStart + idleOffset + 2, 3);
	std::optional<int> parsedNumber = parseInt(numberPart.data(), 10);

	if (!parsedNumber.has_value())
	{
		printf("Failed to parse number '%s' from 'sar --dec=0 1 1 | tail -n 3' output '%s'\n", numberPart.data(), buffer.data());
		return 0;
	}

	return float(100 - *parsedNumber);
}

bool doPeriodicCheck(const Args& args, AppState& appState, std::string& readBuffer)
{
	const float memConsumptionPct = checkMemory(readBuffer);
	if (memConsumptionPct >= args.memThresholdPct)
	{
		const auto timeNow = std::chrono::system_clock::now();
		const bool couldSavePs = saveCommandOutput("ps aux --sort=-%mem", std::format("mem_report_ps_{:%y%m%d_%H%M%OS}_{}.txt", timeNow, int(memConsumptionPct)));
		if (!couldSavePs)
		{
			printf("Could not save mem report from ps to file\n");
		}

		const bool couldSaveTop = saveCommandOutput("top -b -n 1 -o =%MEM", std::format("mem_report_top_{:%y%m%d_%H%M%OS}_{}.txt", timeNow, int(memConsumptionPct)));
		if (!couldSaveTop)
		{
			printf("Could not save mem report from top to file\n");
		}

		trySendNotification(args, appState.lastMemAlertSentTime, "Memory consumption is high", memConsumptionPct);
	}

	const float cpuConsumptionPct = checkCpu(readBuffer);
	if (cpuConsumptionPct >= args.cpuThresholdPct)
	{
		const auto timeNow = std::chrono::system_clock::now();
		const bool couldSave = saveCommandOutput("ps aux --sort=-%cpu", std::format("cpu_report_{:%y%m%d_%H%M%OS}_{}.txt", timeNow, int(cpuConsumptionPct)));
		if (!couldSave)
		{
			printf("Could not save cpu report to file\n");
		}

		trySendNotification(args, appState.lastCpuAlertSentTime, "CPU consumption is high", cpuConsumptionPct);
	}

	return true;
}

int main(int argc, char** argv)
{
	const Args args = readArgs(argc, argv);
	AppState appState;

	std::string readBuffer;
	readBuffer.reserve(256);

	while (true)
	{
		doPeriodicCheck(args, appState, readBuffer);
		sleep(args.timeBetweenChecksSec);
	}
}
