#include "core/EventLog.h"

#include "core/Serialize.h"
#include "core/Types.h"

#include <cstdio>
#include <sstream>

namespace uilab {

const char* LogChannelName(LogChannel c) {
    switch (c) {
    case LogChannel::Console: return "Console";
    case LogChannel::Event:   return "Events";
    case LogChannel::Debug:   return "Debug";
    default:                  return "?";
    }
}

const char* LogLevelName(LogLevel l) {
    switch (l) {
    case LogLevel::Trace:   return "Trace";
    case LogLevel::Info:    return "Info";
    case LogLevel::Success: return "Success";
    case LogLevel::Warning: return "Warning";
    case LogLevel::Error:   return "Error";
    default:                return "?";
    }
}

std::string LogEntry::Timestamp() const {
    const int totalMs = static_cast<int>(wallTime * 1000.0);
    const int minutes = totalMs / 60000;
    const int seconds = (totalMs / 1000) % 60;
    const int millis = totalMs % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d.%03d", minutes, seconds, millis);
    return std::string(buf);
}

void EventLog::Push(LogChannel channel, LogLevel level, const std::string& source, const std::string& message) {
    LogEntry e;
    e.channel = channel;
    e.level = level;
    e.source = source;
    e.message = message;
    // Timestamp() reads wallTime/frame, so a line that never had them printed as
    // 00:00.000. Headless (the self-test) has no context, and then they stay 0.
    if (ImGui::GetCurrentContext() != nullptr) {
        e.wallTime = ImGui::GetTime();
        e.frame = ImGui::GetFrameCount();
    }
    PushInternal(std::move(e));
}

void EventLog::PushInternal(LogEntry e) {
    e.id = nextId_++;
    entries_.push_back(std::move(e));
    ++totalCount_;
    const int ch = static_cast<int>(entries_.back().channel);
    if (ch >= 0 && ch < static_cast<int>(LogChannel::Count))
        ++unseen_[ch];

    while (entries_.size() > capacity_)
        entries_.pop_front();
}

void EventLog::ClearChannel(LogChannel channel) {
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->channel == channel)
            it = entries_.erase(it);
        else
            ++it;
    }
    unseen_[static_cast<int>(channel)] = 0;
}

void EventLog::ClearAll() {
    entries_.clear();
    for (int i = 0; i < static_cast<int>(LogChannel::Count); ++i)
        unseen_[i] = 0;
}

int EventLog::CountChannel(LogChannel channel) const {
    int n = 0;
    for (const LogEntry& e : entries_)
        if (e.channel == channel)
            ++n;
    return n;
}

int EventLog::CountLevel(LogLevel level) const {
    int n = 0;
    for (const LogEntry& e : entries_)
        if (e.level == level)
            ++n;
    return n;
}

bool EventLog::PassesFilter(const LogEntry& e) const {
    const int ch = static_cast<int>(e.channel);
    const int lv = static_cast<int>(e.level);
    if (ch < 0 || ch >= static_cast<int>(LogChannel::Count) || !showChannel[ch])
        return false;
    if (lv < 0 || lv >= static_cast<int>(LogLevel::Count) || !showLevel[lv])
        return false;
    if (!sourceFilter.empty() && e.source != sourceFilter)
        return false;
    if (!filter.empty() && !ContainsNoCase(e.message, filter) && !ContainsNoCase(e.source, filter))
        return false;
    return true;
}

int EventLog::CountFiltered(LogChannel channel) const {
    int n = 0;
    for (const LogEntry& e : entries_)
        if (e.channel == channel && PassesFilter(e))
            ++n;
    return n;
}

std::vector<std::string> EventLog::Sources() const {
    std::vector<std::string> out;
    for (const LogEntry& e : entries_) {
        bool found = false;
        for (const std::string& s : out)
            if (s == e.source) {
                found = true;
                break;
            }
        if (!found)
            out.push_back(e.source);
    }
    return out;
}

bool EventLog::ExportToFile(const std::string& path, bool onlyFiltered, std::string& errorOut) const {
    std::ostringstream os;
    os << "# Universal UI Components Lab - log export\n";
    for (const LogEntry& e : entries_) {
        if (onlyFiltered && !PassesFilter(e))
            continue;
        os << '[' << LogChannelName(e.channel) << "][" << LogLevelName(e.level) << "] "
           << e.Timestamp() << " f" << e.frame << " (" << e.source << "): " << e.message << "\n";
    }
    return WriteTextFile(path, os.str(), errorOut);
}

} // namespace uilab
