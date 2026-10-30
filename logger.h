// logger.h
#ifndef LOGGER_H
#define LOGGER_H

#include <filesystem>
#include <plog/Log.h>
#include <plog/Init.h>
#include <plog/Formatters/TxtFormatter.h>
#include <plog/Appenders/RollingFileAppender.h>


void init_logger(const std::string& log_file) {
    // Create logs directory if it doesn't exist
    std::filesystem::path log_path(log_file);
    std::filesystem::create_directories(log_path.parent_path());

    static plog::RollingFileAppender<plog::TxtFormatter> fileAppender(log_file.c_str(), 2 * 1024 * 1024, 3);
    plog::init(plog::info, &fileAppender); // Initialize the logger with the both appenders.

    PLOG_INFO << "Logger initialized";
}

//inline void init_logger() {
//    static plog::RollingFileAppender<plog::TxtFormatter> fileAppender("stacking-insar.log", 2*1024 * 1024, 5); // 2MB per file, 5 files max
//    plog::init(plog::debug, &fileAppender); // Initialize the logger with file appender
//}

#endif // LOGGER_H