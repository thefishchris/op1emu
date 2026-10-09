#include "headless.h"
#include "utils/log.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

HeadlessFrontend::HeadlessFrontend(const std::string& path, int inputFd) {
    std::ifstream config(path);
    if (!config) throw std::runtime_error("Cannot open key config: " + path);
    nlohmann::json json; config >> json;
    for (const auto& [name, button] : json.at("buttons").items()) {
        const auto& gpio = button.at("gpio");
        const int bank = gpio.at(0).get<int>(), pin = gpio.at(1).get<int>();
        if (bank < 0 || bank >= 8 || pin < 0 || pin >= 16) throw std::runtime_error("Invalid key wiring: " + name);
        keys_.emplace(name, Pin{bank, pin});
    }
    inputFd_ = dup(inputFd);
    if (inputFd_ < 0) throw std::runtime_error("Cannot duplicate input descriptor");
}
HeadlessFrontend::~HeadlessFrontend() { if (inputFd_ >= 0) close(inputFd_); }

bool HeadlessFrontend::ReadLine(std::string& line) {
    auto end = pending_.find('\n');
    if (end == std::string::npos && !eof_) {
        pollfd fd{inputFd_, POLLIN, 0};
        const int ready = poll(&fd, 1, 0);
        if (ready < 0 && errno != EINTR) throw std::runtime_error("Input poll failed");
        if (ready <= 0) return false;
        if (fd.revents & (POLLERR | POLLNVAL)) throw std::runtime_error("Invalid input descriptor");
        if (!(fd.revents & (POLLIN | POLLHUP))) return false;
        char buffer[4096];
        const auto count = read(inputFd_, buffer, sizeof(buffer));
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN) return false;
            throw std::runtime_error("Input read failed");
        }
        if (count == 0) eof_ = true;
        else pending_.append(buffer, static_cast<size_t>(count));
        end = pending_.find('\n');
    }
    if ((end == std::string::npos && pending_.size() > 4096) || (end != std::string::npos && end > 4096))
        throw std::runtime_error("Command exceeds 4096 bytes");
    if (end == std::string::npos) {
        if (!eof_ || pending_.empty()) return false;
        end = pending_.size();
    }
    line = pending_.substr(0, end);
    pending_.erase(0, end < pending_.size() ? end + 1 : end);
    return true;
}
static long Integer(const std::string& text, long low, long high) {
    const size_t first = !text.empty() && text[0] == '-' ? 1 : 0;
    if (text.size() == first || text.size() > first + 18 || text.find_first_not_of("0123456789", first) != std::string::npos)
        throw std::runtime_error("Invalid decimal integer: " + text);
    const long value = std::stol(text);
    if (value < low || value > high) throw std::runtime_error("Integer outside allowed range: " + text);
    return value;
}
void HeadlessFrontend::Initialize(int width, int height) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (width == width_ && height == height_) return;
    if (width < 0 || height < 0 || static_cast<uint64_t>(width) * static_cast<uint64_t>(height) > 16'777'216)
        throw std::runtime_error("Invalid or excessive host framebuffer size");
    width_ = width; height_ = height;
    work_.assign(static_cast<size_t>(width_) * static_cast<size_t>(height_), 0);
    seen_.assign(work_.size(), 0); latest_.clear(); remaining_ = work_.size(); started_ = false;
}
void HeadlessFrontend::UpdateRowBuffer(int x, int y, const void* data, int length) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (!data || length <= 0 || length % 2 || x < 0 || y < 0 || x >= width_ || y >= height_ || length / 2 > width_ - x) return;
    if (x == 0 && y == 0) { started_ = true; remaining_ = work_.size(); std::fill(seen_.begin(), seen_.end(), 0); }
    if (!started_) return;
    const size_t begin = static_cast<size_t>(y) * static_cast<size_t>(width_) + static_cast<size_t>(x);
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < static_cast<size_t>(length / 2); ++i) {
        work_[begin + i] = static_cast<uint16_t>(bytes[i * 2] | (uint16_t(bytes[i * 2 + 1]) << 8));
        if (!seen_[begin + i]) { seen_[begin + i] = 1; --remaining_; }
    }
    if (!remaining_) { latest_ = work_; started_ = false; ++frameCount_; }
}
void HeadlessFrontend::WriteScreenshot(const std::string& path) const {
    std::vector<uint16_t> frame; int width, height;
    { std::lock_guard<std::mutex> lock(frameMutex_); frame = latest_; width = width_; height = height_; }
    if (frame.empty()) throw std::runtime_error("No complete frame for screenshot");
    std::vector<uint8_t> rgb(frame.size() * 3);
    for (int yd = 0; yd < width; ++yd) for (int xd = 0; xd < height; ++xd) {
        const auto pixel = frame[static_cast<size_t>(height - 1 - xd) * static_cast<size_t>(width) + static_cast<size_t>(yd)];
        const unsigned r = (pixel >> 11) & 31, g = (pixel >> 5) & 63, b = pixel & 31;
        auto* out = &rgb[(static_cast<size_t>(yd) * static_cast<size_t>(height) + static_cast<size_t>(xd)) * 3];
        out[0] = static_cast<uint8_t>((r << 3) | (r >> 2)); out[1] = static_cast<uint8_t>((g << 2) | (g >> 4)); out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
    }
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) throw std::runtime_error("Screenshot output must be a new writable path: " + path);
    const std::string header = "P6\n" + std::to_string(height) + " " + std::to_string(width) + "\n255\n";
    auto writeAll = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        while (size) { const auto n = write(fd, bytes, size); if (n < 0 && errno == EINTR) continue; if (n <= 0) return false; bytes += n; size -= static_cast<size_t>(n); }
        return true;
    };
    const bool written = writeAll(header.data(), header.size()) && writeAll(rgb.data(), rgb.size());
    const int closed = close(fd);
    if (!written || closed != 0) throw std::runtime_error("Screenshot write failed: " + path);
    LogInfo("Headless screenshot %s (%dx%d)", path.c_str(), height, width);
}
void HeadlessFrontend::Execute(const std::string& line, Clock::time_point now) {
    std::istringstream stream(line); std::vector<std::string> args;
    for (std::string word; stream >> word;) args.push_back(word);
    if (args.empty() || args[0][0] == '#') return;
    const auto& command = args[0];
    if (command == "quit" && args.size() == 1) shouldClose_ = true;
    else if (command == "keys" && args.size() == 1) { for (const auto& [name, pin] : keys_) std::cout << name << '\n'; std::cout.flush(); }
    else if (command == "wait" && args.size() == 2) resumeAt_ = now + std::chrono::milliseconds(Integer(args[1], 1, 86400000));
    else if (command == "wait-frames" && (args.size() == 2 || args.size() == 3)) {
        frameTarget_ = FrameCount() + static_cast<uint64_t>(Integer(args[1], 1, 100000));
        frameDeadline_ = now + std::chrono::milliseconds(args.size() == 3 ? Integer(args[2], 1, 86400000) : 600000);
    } else if (command == "screenshot" && args.size() == 2) WriteScreenshot(args[1]);
    else if (command == "accel" && args.size() == 2 && args[1] == "random") { sensors_.accelFixed = false; ++sensorVersion_; }
    else if (command == "accel" && args.size() == 4) {
        const auto x = Integer(args[1], INT16_MIN, INT16_MAX), y = Integer(args[2], INT16_MIN, INT16_MAX), z = Integer(args[3], INT16_MIN, INT16_MAX);
        sensors_.accelFixed = true; sensors_.ax = static_cast<int16_t>(x); sensors_.ay = static_cast<int16_t>(y); sensors_.az = static_cast<int16_t>(z); ++sensorVersion_;
    } else if (command == "volume" && args.size() == 2) { sensors_.volume = static_cast<uint8_t>(Integer(args[1], 0, 255)); ++sensorVersion_; }
    else if (((command == "press" || command == "release") && args.size() == 2) || (command == "tap" && (args.size() == 2 || args.size() == 3))) {
        const auto key = keys_.find(args[1]); if (key == keys_.end()) throw std::runtime_error("Unknown key: " + args[1]);
        const auto duration = command == "tap" ? (args.size() == 3 ? Integer(args[2], 1, 86400000) : 100) : 0;
        const auto [bank, pin] = key->second;
        if (command == "release") OnKeyReleased(bank, pin); else OnKeyPressed(bank, pin);
        if (command == "tap") { releaseAtResume_ = key->second; resumeAt_ = now + std::chrono::milliseconds(duration); }
    } else throw std::runtime_error("Invalid headless command: " + line);
}
void HeadlessFrontend::PollEvents(Clock::time_point now) {
    if (frameCallback_) frameCallback_(*this);
    if (now < resumeAt_ || shouldClose_) return;
    if (releaseAtResume_) { OnKeyReleased(releaseAtResume_->first, releaseAtResume_->second); releaseAtResume_.reset(); return; }
    if (frameTarget_) {
        if (FrameCount() < *frameTarget_) { if (now >= frameDeadline_) throw std::runtime_error("Timed out waiting for frames"); return; }
        frameTarget_.reset();
    }
    for (unsigned budget = 0; budget < 64; ++budget) { std::string line; if (!ReadLine(line)) break; Execute(line, now); if (now < resumeAt_ || shouldClose_ || frameTarget_) break; }
}
