// EventLog.h - console / events / debug log with levels, channels and filters.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <imgui.h>

namespace uilab {

enum class LogChannel : int { Console = 0, Event, Debug, Count };
enum class LogLevel : int { Trace = 0, Info, Success, Warning, Error, Count };

const char* LogChannelName(LogChannel c);
const char* LogLevelName(LogLevel l);

struct LogEntry {
    uint64_t id = 0;
    double wallTime = 0.0;   // seconds since process start
    int frame = 0;
    LogChannel channel = LogChannel::Console;
    LogLevel level = LogLevel::Info;
    std::string source;
    std::string message;

    std::string Timestamp() const; // mm:ss.mmm
};

class EventLog {
public:
    explicit EventLog(size_t capacity = 4000) : capacity_(capacity) {}

    void Push(LogChannel channel, LogLevel level, const std::string& source, const std::string& message);

    void ClearChannel(LogChannel channel);
    void ClearAll();

    const std::deque<LogEntry>& Entries() const { return entries_; }
    size_t TotalCount() const { return totalCount_; }

    int CountChannel(LogChannel channel) const;
    int CountLevel(LogLevel level) const;
    int CountFiltered(LogChannel channel) const; // respects the active filters
    int Unseen(LogChannel channel) const { return unseen_[static_cast<int>(channel)]; }
    void MarkSeen(LogChannel channel) { unseen_[static_cast<int>(channel)] = 0; }

    // Filters used by the console panel.
    bool showLevel[static_cast<int>(LogLevel::Count)] = { false, true, true, true, true };
    bool showChannel[static_cast<int>(LogChannel::Count)] = { true, true, true };
    bool autoScroll = true;
    bool wrapText = true;
    std::string filter;        // message / source substring
    std::string sourceFilter;  // exact source, empty = all
    bool PassesFilter(const LogEntry& e) const;

    std::vector<std::string> Sources() const;

    // Exports the (filtered) log as text. Returns false + error on failure.
    bool ExportToFile(const std::string& path, bool onlyFiltered, std::string& errorOut) const;

    // Test/diagnostic hooks used by the self-test.
    size_t Capacity() const { return capacity_; }

private:
    void PushInternal(LogEntry e);
    std::deque<LogEntry> entries_;
    size_t capacity_ = 4000;
    size_t totalCount_ = 0;
    uint64_t nextId_ = 1;
    int unseen_[static_cast<int>(LogChannel::Count)] = { 0, 0, 0 };
};

} // namespace uilab
