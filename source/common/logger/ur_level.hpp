// Copyright (C) 2023 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef UR_LEVEL_HPP
#define UR_LEVEL_HPP 1

#include <stdexcept>
#include <string>

namespace logger {

//enum class Level { DEBUG, INFO, WARN, ERR, QUIET };

inline constexpr auto level_to_str(ur_log_level_t level) {
    switch (level) {
    case ur_log_level_t::UR_LOG_LEVEL_DEBUG:
        return "DEBUG";
    case ur_log_level_t::UR_LOG_LEVEL_INFO:
        return "INFO";
    case ur_log_level_t::UR_LOG_LEVEL_WARN:
        return "WARNING";
    case ur_log_level_t::UR_LOG_LEVEL_ERR:
        return "ERROR";
    default:
        return "";
    }
}

inline auto str_to_level(std::string name) {
    struct lvl_name {
        std::string name;
        ur_log_level_t lvl;
    };

    const lvl_name lvl_names[] = {
        {"debug", ur_log_level_t::UR_LOG_LEVEL_DEBUG},
        {"info", ur_log_level_t::UR_LOG_LEVEL_INFO},
        {"warning", ur_log_level_t::UR_LOG_LEVEL_WARN},
        {"error", ur_log_level_t::UR_LOG_LEVEL_ERR}};

    for (auto const &item : lvl_names) {
        if (item.name.compare(name) == 0) {
            return item.lvl;
        }
    }
    throw std::invalid_argument(
        std::string("Parsing error: no valid log level for string '") + name +
        std::string("'.") +
        std::string(
            "\nValid log level names are: debug, info, warning and error"));
}

} // namespace logger

#endif /* UR_LEVEL_HPP */
