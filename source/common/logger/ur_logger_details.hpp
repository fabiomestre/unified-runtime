// Copyright (C) 2023 Intel Corporation
// Part of the Unified-Runtime Project, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE.TXT
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef UR_LOGGER_DETAILS_HPP
#define UR_LOGGER_DETAILS_HPP 1

#include "ur_api.h"
#include "ur_level.hpp"
#include "ur_sinks.hpp"

namespace logger {

struct LegacyMessage {
    LegacyMessage(const char *p) : message(p){};
    const char *message;
};

class Logger {
  public:
    Logger(std::unique_ptr<logger::Sink> sink)
        : sink(std::move(sink)), quiet(true) {}

    Logger(ur_log_level_t level, std::unique_ptr<logger::Sink> sink)
        : level(level), sink(std::move(sink)), quiet(false) {}

    ~Logger() = default;

    void setLevel(ur_log_level_t level) { this->level = level; }

    void setFlushLevel(ur_log_level_t level) {
        if (sink) {
            this->sink->setFlushLevel(level);
        }
    }

    /* Sets a callback that can be used to access logs at runtime. */
    void setLoggingCallback(ur_adapter_handle_t hAdapter,
                            ur_log_level_t levelThreshold,
                            ur_logger_callback_t loggerCallback,
                            void *callbackUserData) {
        callbackParams = {hAdapter, levelThreshold, loggerCallback,
                          callbackUserData};
    }

    template <typename... Args> void debug(const char *format, Args &&...args) {
        log(ur_log_level_t::UR_LOG_LEVEL_DEBUG, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args> void info(const char *format, Args &&...args) {
        log(ur_log_level_t::UR_LOG_LEVEL_INFO, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(const char *format, Args &&...args) {
        log(ur_log_level_t::UR_LOG_LEVEL_WARN, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args> void warn(const char *format, Args &&...args) {
        warning(format, std::forward<Args>(args)...);
    }

    template <typename... Args> void error(const char *format, Args &&...args) {
        log(ur_log_level_t::UR_LOG_LEVEL_ERR, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void always(const char *format, Args &&...args) {
        if (sink) {
            sink->logSimple(format, std::forward<Args>(args)...);
        }
    }

    template <typename... Args>
    void debug(const logger::LegacyMessage &p, const char *format,
               Args &&...args) {
        log(p, ur_log_level_t::UR_LOG_LEVEL_DEBUG, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void info(const logger::LegacyMessage &p, const char *format,
              Args &&...args) {
        log(p, ur_log_level_t::UR_LOG_LEVEL_INFO, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warning(const logger::LegacyMessage &p, const char *format,
                 Args &&...args) {
        log(p, ur_log_level_t::UR_LOG_LEVEL_WARN, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(const logger::LegacyMessage &p, const char *format,
               Args &&...args) {
        log(p, ur_log_level_t::UR_LOG_LEVEL_ERR, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void log(ur_log_level_t level, const char *format, Args &&...args) {
        log(logger::LegacyMessage(format), level, format,
            std::forward<Args>(args)...);
    }

    template <typename... Args>
    void log(const logger::LegacyMessage &p, ur_log_level_t level,
             const char *format, Args &&...args) {

        if (callbackParams.loggerCallback &&
            level >= callbackParams.levelThreshold) {
            callbackParams.loggerCallback(callbackParams.hAdapter, p.message,
                                          level,
                                          callbackParams.callbackUserData);
        }

        if (!sink || quiet) {
            return;
        }

        if (isLegacySink) {
            sink->log(level, p.message, std::forward<Args>(args)...);
            return;
        }
        if (level < this->level) {
            return;
        }

        sink->log(level, format, std::forward<Args>(args)...);
    }

    void setLegacySink(std::unique_ptr<logger::Sink> legacySink) {
        this->isLegacySink = true;
        this->sink = std::move(legacySink);
    }

  private:
    ur_log_level_t level;
    std::unique_ptr<logger::Sink> sink;
    bool isLegacySink = false;
    bool quiet = true;

    struct CallbackParams {
        ur_adapter_handle_t hAdapter = nullptr;
        ur_log_level_t levelThreshold = UR_LOG_LEVEL_DEBUG;
        ur_logger_callback_t loggerCallback = nullptr;
        void *callbackUserData = nullptr;
    } callbackParams;
};

} // namespace logger

#endif /* UR_LOGGER_DETAILS_HPP */
