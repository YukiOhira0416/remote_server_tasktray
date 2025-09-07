#include "DebugLog.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>

// ログの呼び出し回数をカウントする静的変数
static std::atomic<int> logCounter(0);

// ログ書き込みの排他処理用のミューテックス
static std::mutex logMutex;

void InitializeLogger() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).remove_filename();
    std::filesystem::path logFilePath = exeDir / "debuglog_tasktray.log";
    std::string baseLogName = logFilePath.stem().string();
    std::string extension = ".log";
    std::string backup_extension = ".back";

    // 5番目のバックアップファイルを削除
    std::filesystem::path old_log_path = exeDir / (baseLogName + extension + backup_extension + ".5");
    if (std::filesystem::exists(old_log_path)) {
        std::filesystem::remove(old_log_path);
    }

    // 4番から1番のファイルをリネーム
    for (int i = 4; i >= 1; --i) {
        std::filesystem::path current_path = exeDir / (baseLogName + extension + backup_extension + "." + std::to_string(i));
        if (std::filesystem::exists(current_path)) {
            std::filesystem::path next_path = exeDir / (baseLogName + extension + backup_extension + "." + std::to_string(i + 1));
            std::filesystem::rename(current_path, next_path);
        }
    }

    // 現在のログファイルをリネーム
    if (std::filesystem::exists(logFilePath)) {
        std::filesystem::path new_log_path = exeDir / (baseLogName + extension + backup_extension + ".1");
        std::filesystem::rename(logFilePath, new_log_path);
    }
}

void DebugLog(const std::string& message) {
    // ログの呼び出し回数をインクリメント
    int logNumber = ++logCounter;

    // 番号付きのメッセージを作成
    std::string numberedMessage = std::to_string(logNumber) + ": " + message;

    // OutputDebugStringA にメッセージを出力
    OutputDebugStringA(numberedMessage.c_str());

    // 実行ファイルのパスを取得
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::filesystem::path exeDir = std::filesystem::path(exePath).remove_filename();
    std::filesystem::path logFilePath = exeDir / "debuglog_tasktray.log";

    // ログファイルにメッセージを出力（排他処理）
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream logFile(logFilePath, std::ios::app);
    if (logFile.is_open()) {
        logFile << numberedMessage << std::endl;
        logFile.close();
    }
    else {
        std::cerr << "Failed to open log file: " << logFilePath << std::endl;
    }
}
