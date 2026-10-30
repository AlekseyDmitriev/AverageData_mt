// AverageData_mt.cpp
//
// Command line front-end of the averaging utility. The algorithm itself lives
// in average_data.cpp / average_data.h so that it can be covered by tests.
//
// Usage:
//   AverageData_mt -input_file_list <list.txt> -dst <output.tif> [-threads <n>]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gdal.h>
#include <plog/Log.h>
#include <logger.h>

#include "average_data.h"

namespace fs = std::filesystem;

using FileList = std::vector<std::string>;
using std::string;

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/
static void Usage()
{
    printf("Usage: AverageData -input_file_list <filename> -dst <output_filename> [-threads <number_of_threads>]\n");
    PLOG_FATAL << "Missing command line arguments. Shutting down.";
    exit(1);
}

static std::string format_duration(std::chrono::milliseconds milliseconds) {
    using namespace std::chrono;

    auto total_seconds = duration_cast<seconds>(milliseconds);
    auto mins = duration_cast<minutes>(total_seconds);
    auto secs = duration_cast<seconds>(total_seconds - mins);

    std::ostringstream oss;
    if (mins.count() > 0) {
        oss << mins.count() << "m " << secs.count() << "s";
    }
    else {
        oss << secs.count() << "s";
    }
    return oss.str();
}

// Trims surrounding whitespace (also strips CR left by CRLF file lists).
static std::string trim(const std::string& value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

int main(int argc, char* argv[])
{
    auto start_time = std::chrono::high_resolution_clock::now();
    // Configure the logger
    const string log_file = (fs::current_path() / "logs" / "averagedata.log").string();
    init_logger(log_file);

    PLOG_INFO << "The program has started!";

    std::unordered_map<string, string> namedArgs;

    std::stringstream msg;
    for (int i = 0; i < argc; ++i) {
        msg << argv[i] << " ";
    }
    PLOG_INFO << "Command line: " << msg.str();

    int i = 1;
    while (i < argc) {
        if (argv[i][0] == '-') {
            std::string arg = std::string(argv[i]).substr(1); // Remove the dash
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                namedArgs[arg] = argv[i + 1];
                i += 2;
            }
            else {
                // Value is missing, use empty string
                namedArgs[arg] = "";
                i++;
            }
        }
        else {
            i++;
        }
    }

    const auto input_list_arg = namedArgs.find("input_file_list");
    const auto dst_arg = namedArgs.find("dst");
    if (input_list_arg == namedArgs.end() || dst_arg == namedArgs.end() ||
        input_list_arg->second.empty() || dst_arg->second.empty()) {
        Usage();
    }

    string files_list = input_list_arg->second;
    string dst_file = dst_arg->second;

    int num_threads = 4; // default value
    unsigned int core_count = std::thread::hardware_concurrency();
    if (core_count == 0) {
        PLOG_WARNING << std::format("Could not detect core count, using {} as default.", num_threads);
    }
    else {
        PLOG_INFO << "Detected " << core_count << " CPU cores.";
    }

    if (const auto threads_arg = namedArgs.find("threads"); threads_arg != namedArgs.end()) {
        try {
            const long requested = std::stol(trim(threads_arg->second));
            if (requested <= 0) {
                PLOG_ERROR << std::format("Number of threads must be positive (got \"{}\"). Using default value {}.",
                    threads_arg->second, num_threads);
            }
            else {
                num_threads = static_cast<int>(std::min<long>(requested, std::numeric_limits<int>::max()));
            }
        }
        catch (const std::exception&) {
            PLOG_ERROR << std::format("Can't parse thread count \"{}\". Using default value {}.",
                threads_arg->second, num_threads);
        }
    }

    if (core_count > 0 && num_threads > static_cast<int>(core_count)) {
        PLOG_WARNING << "Requested " << num_threads << " threads, but detected only "
            << core_count << " CPU cores. Using " << core_count << " threads.";
        num_threads = static_cast<int>(core_count);
    }

    std::ifstream ifl(files_list, std::ios::in);
    if (!ifl.is_open()) {
        PLOG_ERROR << std::format("Can't open input file: {}! \nProgram aborted.", files_list);
        Usage();
        exit(-2);
    }

    string filename;
    FileList img_file_list;
    while (std::getline(ifl, filename)) {
        const string line = trim(filename);
        if (!line.empty()) {
            img_file_list.push_back(line);
        }
    }

    if (img_file_list.empty()) {
        PLOG_ERROR << "No input files found in the list.";
        exit(-1);
    }

    GDALAllRegister();

    AverageOptions options;
    options.threads = num_threads;
    AverageStats stats;
    int result = average_data(img_file_list, dst_file, options, &stats);

    GDALDestroyDriverManager();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start_time);

    PLOG_INFO << "Program execution time: " << format_duration(duration);
    PLOG_INFO << "Shutting down.\n\n";
    return result;
}
