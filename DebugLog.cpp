#include "DebugLog.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <atomic>
#include <mutex>

// ログの呼び出し回数をカウントする静的変数
static std::atomic<int> logCounter(0);

// ログ書き込みの排他処理用のミューテックス
static std::mutex logMutex;

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
    std::filesystem::path logFilePath = std::filesystem::path(exePath).remove_filename() / "debuglog_tasktray.log";

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
